/**
 * @file coap_client.c
 * @brief CoAP Client implementation for AkiraOS
 */

#include "coap_client.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/coap.h>
#include <zephyr/net/coap_client.h>
#include <zephyr/random/random.h>

#include <string.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(coap_client, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* Configuration                                                             */
/*===========================================================================*/

#define COAP_DEFAULT_PORT 5683
#define COAPS_DEFAULT_PORT 5684
#define COAP_MAX_RETRIES 4
#define COAP_ACK_TIMEOUT_MS 2000
#define COAP_MAX_OBSERVERS 8

/*===========================================================================*/
/* Private Types                                                             */
/*===========================================================================*/

struct observe_entry
{
    bool active;
    char url[COAP_CLIENT_MAX_URL_LEN];
    coap_observe_cb_t callback;
    void *user_data;
    uint8_t token[COAP_CLIENT_MAX_TOKEN_LEN];
    size_t token_len;
    int sock;
};

/*===========================================================================*/
/* Private Data                                                              */
/*===========================================================================*/

static bool initialized = false;
static struct k_mutex client_mutex;
static struct observe_entry observers[COAP_MAX_OBSERVERS];
static uint16_t message_id = 0;

/* DTLS PSK credentials */
static uint8_t psk_key[64];
static size_t psk_key_len = 0;
static char psk_identity[64];

/*===========================================================================*/
/* Private Functions                                                         */
/*===========================================================================*/

static uint16_t get_next_message_id(void)
{
    return ++message_id;
}

static void generate_token(uint8_t *token, size_t *len)
{
    *len = 4;
    sys_rand_get(token, *len);
}

static int parse_coap_url(const char *url, char *host, size_t host_len,
                          uint16_t *port, char *path, size_t path_len,
                          bool *secure)
{
    const char *p = url;

    /* Check protocol */
    if (strncmp(p, "coaps://", 8) == 0)
    {
        *secure = true;
        *port = COAPS_DEFAULT_PORT;
        p += 8;
    }
    else if (strncmp(p, "coap://", 7) == 0)
    {
        *secure = false;
        *port = COAP_DEFAULT_PORT;
        p += 7;
    }
    else
    {
        return -EINVAL;
    }

    /* Extract host */
    const char *host_end = strchr(p, '/');
    const char *port_start = strchr(p, ':');

    size_t host_copy_len;
    if (port_start && (!host_end || port_start < host_end))
    {
        host_copy_len = port_start - p;
        if (host_copy_len >= host_len)
        {
            return -ENOMEM;
        }
        memcpy(host, p, host_copy_len);
        host[host_copy_len] = '\0';

        /* Parse port */
        *port = (uint16_t)atoi(port_start + 1);
    }
    else if (host_end)
    {
        host_copy_len = host_end - p;
        if (host_copy_len >= host_len)
        {
            return -ENOMEM;
        }
        memcpy(host, p, host_copy_len);
        host[host_copy_len] = '\0';
    }
    else
    {
        size_t url_len = strlen(p);
        if (url_len >= host_len)
        {
            return -ENOMEM;
        }
        strcpy(host, p);
    }

    /* Extract path */
    if (host_end)
    {
        size_t path_copy_len = strlen(host_end);
        if (path_copy_len >= path_len)
        {
            return -ENOMEM;
        }
        strcpy(path, host_end);
    }
    else
    {
        strcpy(path, "/");
    }

    return 0;
}

static int create_socket(const char *host, uint16_t port, bool secure,
                         struct sockaddr *addr, socklen_t *addr_len)
{
    int sock;
    int ret;

    /* Resolve host - simplified for now, assume IPv4 */
    struct sockaddr_in *addr4 = (struct sockaddr_in *)addr;
    addr4->sin_family = AF_INET;
    addr4->sin_port = htons(port);

    /* Try to parse as IP address first */
    ret = zsock_inet_pton(AF_INET, host, &addr4->sin_addr);
    if (ret != 1)
    {
        /* TODO: DNS resolution */
        LOG_ERR("DNS resolution not implemented, use IP address");
        return -ENOTSUP;
    }

    *addr_len = sizeof(struct sockaddr_in);

    /* Create UDP socket */
    sock = zsock_socket(AF_INET, SOCK_DGRAM, secure ? IPPROTO_DTLS_1_2 : IPPROTO_UDP);
    if (sock < 0)
    {
        LOG_ERR("Failed to create socket: %d", errno);
        return -errno;
    }

    /* Configure DTLS if secure */
    if (secure && psk_key_len > 0)
    {
#ifdef CONFIG_NET_SOCKETS_SOCKOPT_TLS
        sec_tag_t sec_tag_list[] = {1}; /* Use tag 1 for PSK */
        ret = zsock_setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST,
                               sec_tag_list, sizeof(sec_tag_list));
        if (ret < 0)
        {
            LOG_WRN("Failed to set DTLS sec tag: %d", errno);
        }
#endif
    }

    /* Connect socket */
    ret = zsock_connect(sock, addr, *addr_len);
    if (ret < 0)
    {
        LOG_ERR("Failed to connect: %d", errno);
        zsock_close(sock);
        return -errno;
    }

    return sock;
}

static int send_coap_request(int sock, const coap_request_t *request,
                             const char *path, coap_response_t *response)
{
    uint8_t tx_buf[512 + COAP_CLIENT_MAX_PAYLOAD];
    uint8_t rx_buf[512 + COAP_CLIENT_MAX_PAYLOAD];
    struct coap_packet pkt;
    uint8_t token[COAP_CLIENT_MAX_TOKEN_LEN];
    size_t token_len;
    int ret;

    /* Generate token */
    generate_token(token, &token_len);

    /* Initialize CoAP packet */
    ret = coap_packet_init(&pkt, tx_buf, sizeof(tx_buf),
                           COAP_VERSION_1,
                           request->type == COAP_TYPE_CON ? COAP_TYPE_CON : COAP_TYPE_NON,
                           token_len, token,
                           request->method, get_next_message_id());
    if (ret < 0)
    {
        LOG_ERR("Failed to init CoAP packet: %d", ret);
        return ret;
    }

    /* Add URI path options */
    if (path && path[0] == '/')
    {
        path++;
    }
    if (path && strlen(path) > 0)
    {
        char path_copy[256];
        strncpy(path_copy, path, sizeof(path_copy) - 1);
        path_copy[sizeof(path_copy) - 1] = '\0';

        char *segment = strtok(path_copy, "/");
        while (segment)
        {
            ret = coap_packet_append_option(&pkt, COAP_OPTION_URI_PATH,
                                            (uint8_t *)segment, strlen(segment));
            if (ret < 0)
            {
                LOG_ERR("Failed to add URI path: %d", ret);
                return ret;
            }
            segment = strtok(NULL, "/");
        }
    }

    /* Add content format if we have payload */
    if (request->payload && request->payload_len > 0)
    {
        uint8_t fmt_buf[2];
        size_t fmt_len = 1;
        fmt_buf[0] = request->format & 0xFF;
        if (request->format > 0xFF)
        {
            fmt_buf[0] = (request->format >> 8) & 0xFF;
            fmt_buf[1] = request->format & 0xFF;
            fmt_len = 2;
        }

        ret = coap_packet_append_option(&pkt, COAP_OPTION_CONTENT_FORMAT,
                                        fmt_buf, fmt_len);
        if (ret < 0)
        {
            LOG_ERR("Failed to add content format: %d", ret);
            return ret;
        }

        /* Add payload */
        ret = coap_packet_append_payload_marker(&pkt);
        if (ret < 0)
        {
            LOG_ERR("Failed to add payload marker: %d", ret);
            return ret;
        }

        ret = coap_packet_append_payload(&pkt, request->payload, request->payload_len);
        if (ret < 0)
        {
            LOG_ERR("Failed to add payload: %d", ret);
            return ret;
        }
    }

    /* Send request */
    ret = zsock_send(sock, pkt.data, pkt.offset, 0);
    if (ret < 0)
    {
        LOG_ERR("Failed to send: %d", errno);
        return -errno;
    }

    LOG_DBG("Sent CoAP request, %d bytes", ret);

    /* Set receive timeout */
    struct timeval tv = {
        .tv_sec = request->timeout_ms / 1000,
        .tv_usec = (request->timeout_ms % 1000) * 1000};
    if (tv.tv_sec == 0 && tv.tv_usec == 0)
    {
        tv.tv_sec = 5; /* Default 5 second timeout */
    }
    zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Receive response */
    ret = zsock_recv(sock, rx_buf, sizeof(rx_buf), 0);
    if (ret < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            LOG_WRN("CoAP request timeout");
            return -ETIMEDOUT;
        }
        LOG_ERR("Failed to receive: %d", errno);
        return -errno;
    }

    LOG_DBG("Received CoAP response, %d bytes", ret);

    /* Parse response */
    struct coap_packet resp_pkt;
    ret = coap_packet_parse(&resp_pkt, rx_buf, ret, NULL, 0);
    if (ret < 0)
    {
        LOG_ERR("Failed to parse response: %d", ret);
        return ret;
    }

    /* Extract response data */
    response->code = coap_header_get_code(&resp_pkt);

    /* Get token */
    const uint8_t *resp_token = coap_header_get_token(&resp_pkt, &response->token_len);
    if (resp_token && response->token_len <= COAP_CLIENT_MAX_TOKEN_LEN)
    {
        memcpy(response->token, resp_token, response->token_len);
    }

    /* Get payload */
    uint16_t payload_len;
    const uint8_t *payload = coap_packet_get_payload(&resp_pkt, &payload_len);
    if (payload && payload_len > 0)
    {
        /* Allocate and copy payload */
        uint8_t *payload_copy = k_malloc(payload_len);
        if (payload_copy)
        {
            memcpy(payload_copy, payload, payload_len);
            response->payload = payload_copy;
            response->payload_len = payload_len;
        }
        else
        {
            response->payload = NULL;
            response->payload_len = 0;
        }
    }
    else
    {
        response->payload = NULL;
        response->payload_len = 0;
    }

    /* TODO: Parse content format option */
    response->format = COAP_FORMAT_TEXT_PLAIN;

    return 0;
}

/*===========================================================================*/
/* Public Functions                                                          */
/*===========================================================================*/

int coap_client_init(void)
{
    if (initialized)
    {
        return 0;
    }

    k_mutex_init(&client_mutex);
    memset(observers, 0, sizeof(observers));
    message_id = sys_rand32_get() & 0xFFFF;

    initialized = true;
    LOG_INF("CoAP client initialized");

    return 0;
}

int coap_client_deinit(void)
{
    if (!initialized)
    {
        return 0;
    }

    /* Stop all observers */
    for (int i = 0; i < COAP_MAX_OBSERVERS; i++)
    {
        if (observers[i].active)
        {
            coap_client_observe_stop(i);
        }
    }

    initialized = false;
    LOG_INF("CoAP client deinitialized");

    return 0;
}

int coap_client_request(const coap_request_t *request, coap_response_t *response)
{
    if (!initialized || !request || !response)
    {
        return -EINVAL;
    }

    char host[128];
    uint16_t port;
    char path[256];
    bool secure;
    struct sockaddr_storage addr;
    socklen_t addr_len;
    int ret;

    /* Parse URL */
    ret = parse_coap_url(request->url, host, sizeof(host),
                         &port, path, sizeof(path), &secure);
    if (ret < 0)
    {
        LOG_ERR("Failed to parse URL: %d", ret);
        return ret;
    }

    LOG_DBG("Request to %s:%d%s (secure=%d)", host, port, path, secure);

    k_mutex_lock(&client_mutex, K_FOREVER);

    /* Create socket */
    int sock = create_socket(host, port, secure,
                             (struct sockaddr *)&addr, &addr_len);
    if (sock < 0)
    {
        k_mutex_unlock(&client_mutex);
        return sock;
    }

    /* Send request and get response */
    ret = send_coap_request(sock, request, path, response);

    zsock_close(sock);
    k_mutex_unlock(&client_mutex);

    return ret;
}

int coap_client_get(const char *url, coap_response_t *response)
{
    coap_request_t req = {
        .url = url,
        .method = COAP_METHOD_GET,
        .type = COAP_TYPE_CON,
        .format = COAP_FORMAT_TEXT_PLAIN,
        .payload = NULL,
        .payload_len = 0,
        .timeout_ms = 5000};

    return coap_client_request(&req, response);
}

int coap_client_post(const char *url, const uint8_t *payload, size_t payload_len,
                     coap_content_format_t format, coap_response_t *response)
{
    coap_request_t req = {
        .url = url,
        .method = COAP_METHOD_POST,
        .type = COAP_TYPE_CON,
        .format = format,
        .payload = payload,
        .payload_len = payload_len,
        .timeout_ms = 5000};

    return coap_client_request(&req, response);
}

int coap_client_put(const char *url, const uint8_t *payload, size_t payload_len,
                    coap_content_format_t format, coap_response_t *response)
{
    coap_request_t req = {
        .url = url,
        .method = COAP_METHOD_PUT,
        .type = COAP_TYPE_CON,
        .format = format,
        .payload = payload,
        .payload_len = payload_len,
        .timeout_ms = 5000};

    return coap_client_request(&req, response);
}

int coap_client_delete(const char *url, coap_response_t *response)
{
    coap_request_t req = {
        .url = url,
        .method = COAP_METHOD_DELETE,
        .type = COAP_TYPE_CON,
        .format = COAP_FORMAT_TEXT_PLAIN,
        .payload = NULL,
        .payload_len = 0,
        .timeout_ms = 5000};

    return coap_client_request(&req, response);
}

coap_observe_handle_t coap_client_observe(const char *url,
                                          coap_observe_cb_t callback,
                                          void *user_data)
{
    if (!initialized || !url || !callback)
    {
        return -EINVAL;
    }

    k_mutex_lock(&client_mutex, K_FOREVER);

    /* Find free observer slot */
    int handle = -1;
    for (int i = 0; i < COAP_MAX_OBSERVERS; i++)
    {
        if (!observers[i].active)
        {
            handle = i;
            break;
        }
    }

    if (handle < 0)
    {
        k_mutex_unlock(&client_mutex);
        LOG_ERR("No free observer slots");
        return -ENOMEM;
    }

    /* Set up observer */
    struct observe_entry *obs = &observers[handle];
    strncpy(obs->url, url, sizeof(obs->url) - 1);
    obs->url[sizeof(obs->url) - 1] = '\0';
    obs->callback = callback;
    obs->user_data = user_data;
    generate_token(obs->token, &obs->token_len);

    /* TODO: Send observe request and start background receive thread */
    /* For now, this is a stub - real implementation would need a dedicated
     * thread or work queue to handle incoming notifications */

    obs->active = true;

    k_mutex_unlock(&client_mutex);

    LOG_INF("Started observing: %s (handle=%d)", url, handle);

    return handle;
}

int coap_client_observe_stop(coap_observe_handle_t handle)
{
    if (!initialized || handle < 0 || handle >= COAP_MAX_OBSERVERS)
    {
        return -EINVAL;
    }

    k_mutex_lock(&client_mutex, K_FOREVER);

    struct observe_entry *obs = &observers[handle];
    if (!obs->active)
    {
        k_mutex_unlock(&client_mutex);
        return -ENOENT;
    }

    /* TODO: Send cancel observe request */

    if (obs->sock >= 0)
    {
        zsock_close(obs->sock);
    }

    obs->active = false;

    k_mutex_unlock(&client_mutex);

    LOG_INF("Stopped observing (handle=%d)", handle);

    return 0;
}

int coap_client_download(const char *url, uint8_t *buffer, size_t buffer_len,
                         size_t *received_len)
{
    if (!initialized || !url || !buffer || !received_len)
    {
        return -EINVAL;
    }

    /* TODO: Implement block transfer (RFC 7959) */
    /* For now, do a simple GET */

    coap_response_t response = {0};
    int ret = coap_client_get(url, &response);
    if (ret < 0)
    {
        return ret;
    }

    if (response.code != COAP_CODE_CONTENT)
    {
        coap_client_free_response(&response);
        return -ENOENT;
    }

    size_t copy_len = response.payload_len;
    if (copy_len > buffer_len)
    {
        copy_len = buffer_len;
    }

    if (response.payload && copy_len > 0)
    {
        memcpy(buffer, response.payload, copy_len);
    }
    *received_len = copy_len;

    coap_client_free_response(&response);

    return 0;
}

int coap_client_upload(const char *url, const uint8_t *data, size_t data_len,
                       coap_content_format_t format)
{
    if (!initialized || !url || !data)
    {
        return -EINVAL;
    }

    /* TODO: Implement block transfer for large uploads */
    /* For now, do a simple PUT */

    coap_response_t response = {0};
    int ret = coap_client_put(url, data, data_len, format, &response);
    if (ret < 0)
    {
        return ret;
    }

    int result = 0;
    if (response.code != COAP_CODE_CHANGED && response.code != COAP_CODE_CREATED)
    {
        result = -EIO;
    }

    coap_client_free_response(&response);

    return result;
}

void coap_client_free_response(coap_response_t *response)
{
    if (response && response->payload)
    {
        k_free((void *)response->payload);
        response->payload = NULL;
        response->payload_len = 0;
    }
}

const char *coap_code_to_str(coap_code_t code)
{
    switch (code)
    {
    case COAP_CODE_CREATED:
        return "2.01 Created";
    case COAP_CODE_DELETED:
        return "2.02 Deleted";
    case COAP_CODE_VALID:
        return "2.03 Valid";
    case COAP_CODE_CHANGED:
        return "2.04 Changed";
    case COAP_CODE_CONTENT:
        return "2.05 Content";
    case COAP_CODE_BAD_REQUEST:
        return "4.00 Bad Request";
    case COAP_CODE_UNAUTHORIZED:
        return "4.01 Unauthorized";
    case COAP_CODE_FORBIDDEN:
        return "4.03 Forbidden";
    case COAP_CODE_NOT_FOUND:
        return "4.04 Not Found";
    case COAP_CODE_NOT_ALLOWED:
        return "4.05 Method Not Allowed";
    case COAP_CODE_INTERNAL_ERR:
        return "5.00 Internal Server Error";
    case COAP_CODE_NOT_IMPL:
        return "5.01 Not Implemented";
    case COAP_CODE_UNAVAILABLE:
        return "5.03 Service Unavailable";
    default:
        return "Unknown";
    }
}

int coap_client_set_psk(const uint8_t *psk, size_t psk_len, const char *psk_id)
{
    if (!psk || psk_len == 0 || !psk_id)
    {
        return -EINVAL;
    }

    if (psk_len > sizeof(psk_key))
    {
        return -ENOMEM;
    }

    memcpy(psk_key, psk, psk_len);
    psk_key_len = psk_len;
    strncpy(psk_identity, psk_id, sizeof(psk_identity) - 1);
    psk_identity[sizeof(psk_identity) - 1] = '\0';

    LOG_INF("CoAP DTLS PSK configured");

    return 0;
}
