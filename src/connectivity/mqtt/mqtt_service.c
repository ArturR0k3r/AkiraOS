/**
 * @file mqtt_service.c
 * @brief AkiraOS MQTT client service implementation (Zephyr MQTT lib).
 */

#include "mqtt_service.h"
#include "../../settings/settings.h"

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_linkaddr.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(CONFIG_AKIRA_WIFI_MANAGER) && defined(CONFIG_AKIRA_MQTT_AUTO_CONNECT)
#include "../wifi/wifi_manager.h"
#endif

LOG_MODULE_REGISTER(mqtt_service, CONFIG_AKIRA_LOG_LEVEL);

/* ------------------------------------------------------------------------- */
#define RX_BUF_SIZE     256
#define TX_BUF_SIZE     256
#define MAX_SUBS        4
#define RECONNECT_MIN_MS 2000
#define RECONNECT_MAX_MS 30000
#define CONNACK_TRIES   20    /* × 100ms = 2s to receive CONNACK */

/* Broker configuration (loaded from NVS / Kconfig defaults) */
static char     s_host[64];
static uint16_t s_port;
static char     s_user[48];
static char     s_pass[64];

static char s_client_id[40];
static char s_node_id[24];

/* Zephyr MQTT client state */
static struct mqtt_client s_client;
static struct sockaddr_storage s_broker_addr;
static uint8_t s_rx_buf[RX_BUF_SIZE];
static uint8_t s_tx_buf[TX_BUF_SIZE];

static K_MUTEX_DEFINE(s_client_mutex);      /* guards all mqtt_* client calls */
static volatile mqtt_svc_state_t s_state = MQTT_SVC_DISCONNECTED;
static volatile bool s_want_connect;

/* Remembered subscriptions, replayed after every (re)connect */
static char s_subs[MAX_SUBS][AKIRA_MQTT_TOPIC_MAX];
static int  s_sub_count;

/* Inbound message queue */
static struct akira_mqtt_message s_rxq_buf[CONFIG_AKIRA_MQTT_RX_QUEUE_DEPTH];
static struct k_msgq s_rxq;

/* Service thread */
K_THREAD_STACK_DEFINE(s_thread_stack, CONFIG_AKIRA_MQTT_THREAD_STACK_SIZE);
static struct k_thread s_thread;
static bool s_thread_started;

static uint16_t next_msg_id(void)
{
    static uint16_t id;
    if (++id == 0) {
        id = 1;
    }
    return id;
}

/* ------------------------------------------------------------------------- */
/* Config                                                                    */
/* ------------------------------------------------------------------------- */
static void load_config(void)
{
    char val[MAX_VALUE_LEN];

    if (akira_settings_get("mqtt/host", val, sizeof(val)) == 0 && val[0]) {
        strncpy(s_host, val, sizeof(s_host) - 1);
    } else {
        strncpy(s_host, CONFIG_AKIRA_MQTT_BROKER_HOST, sizeof(s_host) - 1);
    }

    if (akira_settings_get("mqtt/port", val, sizeof(val)) == 0 && val[0]) {
        s_port = (uint16_t)atoi(val);
    } else {
        s_port = CONFIG_AKIRA_MQTT_BROKER_PORT;
    }

    if (akira_settings_get("mqtt/user", val, sizeof(val)) == 0) {
        strncpy(s_user, val, sizeof(s_user) - 1);
    } else {
        s_user[0] = '\0';
    }
    if (akira_settings_get("mqtt/pass", val, sizeof(val)) == 0) {
        strncpy(s_pass, val, sizeof(s_pass) - 1);
    } else {
        s_pass[0] = '\0';
    }
}

static void build_ids(void)
{
    uint8_t mac[6] = {0};
    struct net_if *iface = net_if_get_default();
    if (iface) {
        struct net_linkaddr *la = net_if_get_link_addr(iface);
        if (la && la->len >= 6) {
            memcpy(mac, la->addr, 6);
        }
    }
    snprintf(s_node_id, sizeof(s_node_id), "%s_%02x%02x%02x",
             CONFIG_AKIRA_MQTT_NODE_ID, mac[3], mac[4], mac[5]);
    snprintf(s_client_id, sizeof(s_client_id), "%s_%02x%02x%02x",
             CONFIG_AKIRA_MQTT_CLIENT_ID, mac[3], mac[4], mac[5]);
}

/* ------------------------------------------------------------------------- */
/* Inbound PUBLISH handling                                                  */
/* ------------------------------------------------------------------------- */
static void handle_publish(const struct mqtt_publish_param *pub)
{
    struct akira_mqtt_message msg;
    uint16_t tlen = MIN(pub->message.topic.topic.size,
                        (uint16_t)(sizeof(msg.topic) - 1));
    memcpy(msg.topic, pub->message.topic.topic.utf8, tlen);
    msg.topic[tlen] = '\0';

    uint32_t remaining = pub->message.payload.len;
    uint16_t stored = 0;
    uint8_t scratch[64];

    while (remaining > 0) {
        uint8_t *dst;
        uint32_t chunk;
        if (stored < sizeof(msg.payload)) {
            dst = &msg.payload[stored];
            chunk = MIN(remaining, (uint32_t)(sizeof(msg.payload) - stored));
        } else {
            dst = scratch;
            chunk = MIN(remaining, (uint32_t)sizeof(scratch));
        }
        int rc = mqtt_read_publish_payload(&s_client, dst, chunk);
        if (rc <= 0) {
            break;
        }
        if (dst != scratch) {
            stored += rc;
        }
        remaining -= rc;
    }
    msg.payload_len = stored;

    if (k_msgq_put(&s_rxq, &msg, K_NO_WAIT) != 0) {
        /* drop oldest, retry once */
        struct akira_mqtt_message discard;
        (void)k_msgq_get(&s_rxq, &discard, K_NO_WAIT);
        (void)k_msgq_put(&s_rxq, &msg, K_NO_WAIT);
    }

    if (pub->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
        struct mqtt_puback_param ack = { .message_id = pub->message_id };
        mqtt_publish_qos1_ack(&s_client, &ack);
    }
}

static void mqtt_evt_handler(struct mqtt_client *c, const struct mqtt_evt *evt)
{
    ARG_UNUSED(c);
    switch (evt->type) {
    case MQTT_EVT_CONNACK:
        if (evt->result == 0) {
            s_state = MQTT_SVC_CONNECTED;
            LOG_INF("connected to broker %s:%u as %s", s_host, s_port, s_client_id);
        } else {
            LOG_WRN("CONNACK error %d", evt->result);
        }
        break;
    case MQTT_EVT_DISCONNECT:
        LOG_WRN("broker disconnected");
        s_state = MQTT_SVC_DISCONNECTED;
        break;
    case MQTT_EVT_PUBLISH:
        handle_publish(&evt->param.publish);
        break;
    case MQTT_EVT_PUBACK:
    case MQTT_EVT_SUBACK:
    case MQTT_EVT_PINGRESP:
    default:
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* Connection lifecycle                                                      */
/* ------------------------------------------------------------------------- */
static int resolve_broker(void)
{
    struct zsock_addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct zsock_addrinfo *res = NULL;
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%u", s_port);

    int rc = zsock_getaddrinfo(s_host, portstr, &hints, &res);
    if (rc != 0 || !res) {
        LOG_ERR("DNS resolve '%s' failed (%d)", s_host, rc);
        return -EHOSTUNREACH;
    }
    memcpy(&s_broker_addr, res->ai_addr, res->ai_addrlen);
    ((struct sockaddr_in *)&s_broker_addr)->sin_port = htons(s_port);
    zsock_freeaddrinfo(res);
    return 0;
}

static int client_sock(void)
{
#if defined(CONFIG_AKIRA_MQTT_TLS)
    return s_client.transport.tls.sock;
#else
    return s_client.transport.tcp.sock;
#endif
}

static void setup_client(void)
{
    mqtt_client_init(&s_client);

    s_client.broker = &s_broker_addr;
    s_client.evt_cb = mqtt_evt_handler;
    s_client.client_id.utf8 = (uint8_t *)s_client_id;
    s_client.client_id.size = strlen(s_client_id);
    s_client.protocol_version = MQTT_VERSION_3_1_1;
    s_client.keepalive = CONFIG_AKIRA_MQTT_KEEPALIVE_SEC;

    static struct mqtt_utf8 user_utf8, pass_utf8;
    if (s_user[0]) {
        user_utf8.utf8 = (uint8_t *)s_user;
        user_utf8.size = strlen(s_user);
        pass_utf8.utf8 = (uint8_t *)s_pass;
        pass_utf8.size = strlen(s_pass);
        s_client.user_name = &user_utf8;
        s_client.password = &pass_utf8;
    } else {
        s_client.user_name = NULL;
        s_client.password = NULL;
    }

    s_client.rx_buf = s_rx_buf;
    s_client.rx_buf_size = sizeof(s_rx_buf);
    s_client.tx_buf = s_tx_buf;
    s_client.tx_buf_size = sizeof(s_tx_buf);

#if defined(CONFIG_AKIRA_MQTT_TLS)
    static const sec_tag_t sec_tags[] = { CONFIG_AKIRA_MQTT_TLS_SEC_TAG };
    s_client.transport.type = MQTT_TRANSPORT_SECURE;
    struct mqtt_sec_config *tls = &s_client.transport.tls.config;
    tls->peer_verify = TLS_PEER_VERIFY_REQUIRED;
    tls->cipher_list = NULL;
    tls->sec_tag_list = sec_tags;
    tls->sec_tag_count = ARRAY_SIZE(sec_tags);
    tls->hostname = s_host;
#else
    s_client.transport.type = MQTT_TRANSPORT_NON_SECURE;
#endif
}

/* Replay remembered subscriptions after a (re)connect. */
static void replay_subscriptions(void)
{
    for (int i = 0; i < s_sub_count; i++) {
        struct mqtt_topic t = {
            .topic = { .utf8 = (uint8_t *)s_subs[i], .size = strlen(s_subs[i]) },
            .qos = MQTT_QOS_0_AT_MOST_ONCE,
        };
        struct mqtt_subscription_list sub = {
            .list = &t, .list_count = 1, .message_id = next_msg_id(),
        };
        (void)mqtt_subscribe(&s_client, &sub);
    }
}

static int connect_broker(void)
{
    load_config();
    build_ids();

    int rc = resolve_broker();
    if (rc != 0) {
        return rc;
    }

    k_mutex_lock(&s_client_mutex, K_FOREVER);
    setup_client();
    s_state = MQTT_SVC_CONNECTING;
    rc = mqtt_connect(&s_client);
    k_mutex_unlock(&s_client_mutex);
    if (rc != 0) {
        LOG_ERR("mqtt_connect failed (%d)", rc);
        s_state = MQTT_SVC_DISCONNECTED;
        return rc;
    }

    /* Pump input until CONNACK (or give up). */
    struct zsock_pollfd fds = { .fd = client_sock(), .events = ZSOCK_POLLIN };
    for (int i = 0; i < CONNACK_TRIES && s_state == MQTT_SVC_CONNECTING; i++) {
        zsock_poll(&fds, 1, 100);
        k_mutex_lock(&s_client_mutex, K_FOREVER);
        (void)mqtt_input(&s_client);
        k_mutex_unlock(&s_client_mutex);
    }

    if (s_state != MQTT_SVC_CONNECTED) {
        LOG_ERR("no CONNACK from broker");
        k_mutex_lock(&s_client_mutex, K_FOREVER);
        mqtt_abort(&s_client);
        k_mutex_unlock(&s_client_mutex);
        s_state = MQTT_SVC_DISCONNECTED;
        return -ETIMEDOUT;
    }

    k_mutex_lock(&s_client_mutex, K_FOREVER);
    replay_subscriptions();
    k_mutex_unlock(&s_client_mutex);
    return 0;
}

static void abort_connection(void)
{
    k_mutex_lock(&s_client_mutex, K_FOREVER);
    mqtt_abort(&s_client);
    k_mutex_unlock(&s_client_mutex);
    s_state = MQTT_SVC_DISCONNECTED;
}

/* ------------------------------------------------------------------------- */
/* Service thread                                                            */
/* ------------------------------------------------------------------------- */
static void mqtt_thread_fn(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    uint32_t backoff = RECONNECT_MIN_MS;

    while (true) {
        if (!s_want_connect) {
            if (s_state != MQTT_SVC_DISCONNECTED) {
                abort_connection();
            }
            k_sleep(K_MSEC(500));
            continue;
        }

        if (s_state == MQTT_SVC_DISCONNECTED) {
            if (connect_broker() != 0) {
                k_sleep(K_MSEC(backoff));
                backoff = MIN(backoff * 2, RECONNECT_MAX_MS);
                continue;
            }
            backoff = RECONNECT_MIN_MS;
        }

        struct zsock_pollfd fds = { .fd = client_sock(), .events = ZSOCK_POLLIN };
        int pr = zsock_poll(&fds, 1, 500);

        k_mutex_lock(&s_client_mutex, K_FOREVER);
        int rc = 0;
        if (pr > 0 && (fds.revents & ZSOCK_POLLIN)) {
            rc = mqtt_input(&s_client);
        }
        if (rc == 0) {
            rc = mqtt_live(&s_client);
            if (rc == -EAGAIN) {
                rc = 0;   /* nothing to send yet */
            }
        }
        k_mutex_unlock(&s_client_mutex);

        if (rc != 0 || (pr > 0 && (fds.revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP)))) {
            LOG_WRN("connection error (%d) — reconnecting", rc);
            abort_connection();
        }
    }
}

static void ensure_thread(void)
{
    if (!s_thread_started) {
        k_thread_create(&s_thread, s_thread_stack,
                        K_THREAD_STACK_SIZEOF(s_thread_stack),
                        mqtt_thread_fn, NULL, NULL, NULL,
                        7, 0, K_NO_WAIT);
        k_thread_name_set(&s_thread, "mqtt_svc");
        s_thread_started = true;
    }
}

/* ------------------------------------------------------------------------- */
/* WiFi hook                                                                 */
/* ------------------------------------------------------------------------- */
#if defined(CONFIG_AKIRA_WIFI_MANAGER) && defined(CONFIG_AKIRA_MQTT_AUTO_CONNECT)
static void on_wifi_event(wifi_mgr_event_t event, void *user)
{
    ARG_UNUSED(user);
    if (event == WIFI_MGR_EVT_CONNECTED) {
        LOG_INF("WiFi up — starting MQTT");
        mqtt_service_start();
    } else if (event == WIFI_MGR_EVT_DISCONNECTED) {
        s_want_connect = false;
    }
}
#endif

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */
int mqtt_service_init(void)
{
    static bool inited;
    if (inited) {
        return 0;
    }
    k_msgq_init(&s_rxq, (char *)s_rxq_buf, sizeof(struct akira_mqtt_message),
                CONFIG_AKIRA_MQTT_RX_QUEUE_DEPTH);
    build_ids();
    ensure_thread();
#if defined(CONFIG_AKIRA_WIFI_MANAGER) && defined(CONFIG_AKIRA_MQTT_AUTO_CONNECT)
    wifi_manager_register_cb(on_wifi_event, NULL);
#endif
    inited = true;
    LOG_INF("MQTT service ready (node id %s)", s_node_id);
    return 0;
}

int mqtt_service_start(void)
{
    mqtt_service_init();
    s_want_connect = true;
    return 0;
}

int mqtt_service_stop(void)
{
    s_want_connect = false;
    return 0;
}

mqtt_svc_state_t mqtt_service_get_state(void)
{
    return s_state;
}

bool mqtt_service_is_connected(void)
{
    return s_state == MQTT_SVC_CONNECTED;
}

int mqtt_service_publish(const char *topic, const void *payload, size_t len,
                         int qos, bool retain)
{
    if (!topic) {
        return -EINVAL;
    }
    if (s_state != MQTT_SVC_CONNECTED) {
        return -ENOTCONN;
    }

    struct mqtt_publish_param param = {0};
    param.message.topic.topic.utf8 = (uint8_t *)topic;
    param.message.topic.topic.size = strlen(topic);
    param.message.topic.qos = (qos >= 1) ? MQTT_QOS_1_AT_LEAST_ONCE
                                         : MQTT_QOS_0_AT_MOST_ONCE;
    param.message.payload.data = (uint8_t *)payload;
    param.message.payload.len = len;
    param.message_id = next_msg_id();
    param.dup_flag = 0;
    param.retain_flag = retain ? 1 : 0;

    k_mutex_lock(&s_client_mutex, K_FOREVER);
    int rc = mqtt_publish(&s_client, &param);
    k_mutex_unlock(&s_client_mutex);
    return rc;
}

int mqtt_service_subscribe(const char *topic)
{
    if (!topic || strlen(topic) >= AKIRA_MQTT_TOPIC_MAX) {
        return -EINVAL;
    }

    /* Remember it so it survives reconnects. */
    bool known = false;
    for (int i = 0; i < s_sub_count; i++) {
        if (strcmp(s_subs[i], topic) == 0) {
            known = true;
            break;
        }
    }
    if (!known && s_sub_count < MAX_SUBS) {
        strncpy(s_subs[s_sub_count++], topic, AKIRA_MQTT_TOPIC_MAX - 1);
    }

    if (s_state != MQTT_SVC_CONNECTED) {
        return -ENOTCONN;   /* will be replayed on connect */
    }

    struct mqtt_topic t = {
        .topic = { .utf8 = (uint8_t *)topic, .size = strlen(topic) },
        .qos = MQTT_QOS_0_AT_MOST_ONCE,
    };
    struct mqtt_subscription_list sub = {
        .list = &t, .list_count = 1, .message_id = next_msg_id(),
    };
    k_mutex_lock(&s_client_mutex, K_FOREVER);
    int rc = mqtt_subscribe(&s_client, &sub);
    k_mutex_unlock(&s_client_mutex);
    return rc;
}

int mqtt_service_poll_message(struct akira_mqtt_message *out, k_timeout_t timeout)
{
    if (!out) {
        return -EINVAL;
    }
    int rc = k_msgq_get(&s_rxq, out, timeout);
    return (rc == 0) ? 0 : -EAGAIN;
}

int mqtt_service_set_broker(const char *host, uint16_t port,
                            const char *user, const char *pass)
{
    if (host && host[0]) {
        akira_settings_set("mqtt/host", host, 0);
    }
    if (port) {
        char pbuf[8];
        snprintf(pbuf, sizeof(pbuf), "%u", port);
        akira_settings_set("mqtt/port", pbuf, 0);
    }
    if (user) {
        akira_settings_set("mqtt/user", user, 0);
    }
    if (pass) {
        akira_settings_set("mqtt/pass", pass, 1 /* encrypted */);
    }
    return 0;
}

const char *mqtt_service_node_id(void)
{
    if (s_node_id[0] == '\0') {
        build_ids();
    }
    return s_node_id;
}

/* ------------------------------------------------------------------------- */
static int mqtt_service_sys_init(void)
{
    return mqtt_service_init();
}
SYS_INIT(mqtt_service_sys_init, APPLICATION, 99);
