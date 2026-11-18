/**
 * @file akira_mqtt_api.c
 * @brief MQTT / Home Assistant WASM native API — bridges apps to mqtt_service.
 *
 * mqtt_service owns one broker connection that the whole system shares, so
 * an app must not be able to break it or hold it:
 * - Topics are checked here (well-formed UTF-8, no control characters, no
 *   wildcards in a published topic, wildcards only where MQTT allows them in
 *   a filter). A broker drops the connection on a malformed topic, and a
 *   remembered bad subscription would be replayed on every reconnect.
 * - Sizes are capped and publishes rate-limited per app, and every call that
 *   holds the client mutex runs inside a native critical section so a stop
 *   never aborts the app thread while it holds that mutex.
 * - Waits (mqtt_poll, ha_light_poll) run in short slices and end when the
 *   app is stopped; mqtt_poll's negative timeout waits until a message or a
 *   stop.
 * - What an app subscribes to and the lights it registers are recorded per
 *   app slot (capped) and handed back when the app ends (mqtt_release(),
 *   through mqtt_service_unsubscribe(), which never does I/O).
 * No pointer into app memory is kept past the call.
 */

#include "akira_mqtt_api.h"
#include <connectivity/mqtt/mqtt_service.h>
#include <connectivity/mqtt/ha_discovery.h>
#include <runtime/security.h>
#include <runtime/akira_runtime.h> /* AKIRA_MAX_WASM_INSTANCES */
#include <akira_app_containment.h>
#include "mem_helper.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <wasm_export.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(akira_mqtt_api, CONFIG_AKIRA_LOG_LEVEL);

/* ---- Limits ------------------------------------------------------------- */

#define MQTT_API_PUB_TOPIC_MAX   128   /* published topic, bytes (no NUL)     */
#define MQTT_API_PUBLISH_MAX     1024  /* payload bytes per publish           */
#define MQTT_API_SUBS_PER_APP    4     /* distinct topic filters per app      */
#define MQTT_API_LIGHTS_PER_APP  4     /* distinct HA lights per app          */
#define MQTT_API_OBJECT_ID_MAX   32    /* HA object id, [A-Za-z0-9_-]         */
#define MQTT_API_NAME_MAX        48    /* HA display name, bytes              */
#define MQTT_API_PUB_BURST       20    /* publishes an app may send at once   */
#define MQTT_API_PUB_PER_SEC     10    /* ... and per second after that       */
#define MQTT_API_WAIT_SLICE_MS   50    /* waits re-check the stop flag        */
#define MQTT_API_LOCK_MS         2000  /* natives: -EBUSY after this          */
#define MQTT_API_RELEASE_LOCK_MS 500   /* release hook: give up after         */

/* What each app slot has subscribed to / registered, so it can be capped
 * and handed back when the app ends. Touched under s_api_lock. */
struct mqtt_app_state {
    uint8_t  sub_count;
    uint8_t  light_count;
    int32_t  pub_tokens;     /* publish budget, in 1/1000 publish          */
    int64_t  pub_last;       /* 0 = never: bucket full                     */
    char     subs[MQTT_API_SUBS_PER_APP][AKIRA_MQTT_TOPIC_MAX];
    char     lights[MQTT_API_LIGHTS_PER_APP][MQTT_API_OBJECT_ID_MAX + 1];
};

static struct mqtt_app_state AKIRA_BULK_BSS s_apps[AKIRA_MAX_WASM_INSTANCES];

/* Serialises the subscribe paths (mqtt_service keeps its replay list without
 * a lock) and guards s_apps. */
static K_MUTEX_DEFINE(s_api_lock);

/* ---- Helpers ------------------------------------------------------------ */

/* App-triggered warnings: at most one a second. */
static bool mqtt_api_log_allowed(void)
{
    static uint32_t last;
    uint32_t now = k_uptime_get_32();

    if (last != 0U && (now - last) < 1000U) {
        return false;
    }
    last = now ? now : 1U;
    return true;
}

#define MQTT_API_WRN(...)                                                      \
    do {                                                                       \
        if (mqtt_api_log_allowed()) {                                          \
            LOG_WRN(__VA_ARGS__);                                              \
        }                                                                      \
    } while (0)

static struct mqtt_app_state *app_state(wasm_exec_env_t exec_env)
{
    int slot = akira_runtime_slot_of_exec_env(exec_env);

    if (slot < 0 || slot >= AKIRA_MAX_WASM_INSTANCES) {
        return NULL;
    }
    return &s_apps[slot];
}

/*
 * Well-formed UTF-8 that a broker accepts in a topic: no overlong forms,
 * surrogates or code points past U+10FFFF, and no control characters or
 * non-characters (Mosquitto, for one, drops the client on those).
 */
static bool topic_utf8_ok(const uint8_t *s, size_t len)
{
    static const uint32_t min_cp[5] = { 0, 0, 0x80, 0x800, 0x10000 };
    size_t i = 0;

    while (i < len) {
        uint8_t c = s[i];
        uint32_t cp;
        size_t n;

        if (c < 0x80) {
            cp = c;
            n = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            n = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            n = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            n = 4;
        } else {
            return false;
        }
        if (n > len - i) {
            return false;
        }
        for (size_t k = 1; k < n; k++) {
            if ((s[i + k] & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        if (cp < min_cp[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || (cp & 0xFFFE) == 0xFFFE) {
            return false;
        }
        i += n;
    }
    return true;
}

/*
 * Check a topic name (@p filter false: no wildcards) or a topic filter
 * (@p filter true: '+' fills a whole level, '#' only as the last level).
 * @return its length, or -EINVAL.
 */
static int topic_check(const char *t, size_t max, bool filter)
{
    size_t len = strnlen(t, max + 1);

    if (len == 0 || len > max || !topic_utf8_ok((const uint8_t *)t, len)) {
        return -EINVAL;
    }
    for (size_t i = 0; i < len; i++) {
        bool level_start = (i == 0) || (t[i - 1] == '/');
        bool level_end   = (i + 1 == len) || (t[i + 1] == '/');

        if (t[i] == '+') {
            if (!filter || !level_start || !level_end) {
                return -EINVAL;
            }
        } else if (t[i] == '#') {
            if (!filter || !level_start || i + 1 != len) {
                return -EINVAL;
            }
        }
    }
    return (int)len;
}

/* Home Assistant object ids: they become topic levels and JSON ids. */
static bool object_id_ok(const char *id)
{
    size_t len = strnlen(id, MQTT_API_OBJECT_ID_MAX + 1);

    if (len == 0 || len > MQTT_API_OBJECT_ID_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = id[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

/* The display name is pasted into the discovery JSON unescaped. */
static bool display_name_ok(const char *name)
{
    size_t len = strnlen(name, MQTT_API_NAME_MAX + 1);

    if (len == 0 || len > MQTT_API_NAME_MAX ||
        !topic_utf8_ok((const uint8_t *)name, len)) {
        return false;
    }
    return memchr(name, '"', len) == NULL && memchr(name, '\\', len) == NULL;
}

/* Token bucket per app: MQTT_API_PUB_BURST at once, then
 * MQTT_API_PUB_PER_SEC. Only the app's own thread touches its bucket. */
static bool publish_allowed(struct mqtt_app_state *a)
{
    const int32_t cap = MQTT_API_PUB_BURST * 1000;
    int64_t now = k_uptime_get();

    if (a->pub_last == 0) {
        a->pub_tokens = cap;
    } else {
        int64_t refill = (now - a->pub_last) * MQTT_API_PUB_PER_SEC;

        a->pub_tokens = (int32_t)MIN((int64_t)cap, a->pub_tokens + refill);
    }
    a->pub_last = now ? now : 1;

    if (a->pub_tokens < 1000) {
        return false;
    }
    a->pub_tokens -= 1000;
    return true;
}

/*
 * Wait for the next inbound message in slices so a stop ends the wait.
 * @p timeout_ms < 0 waits until a message arrives or the app is stopped.
 * @return 0, -EAGAIN on timeout, -ECANCELED if the app is being stopped.
 */
static int wait_message(wasm_exec_env_t exec_env, struct akira_mqtt_message *msg,
                        int timeout_ms)
{
    int64_t deadline = (timeout_ms < 0) ? INT64_MAX : k_uptime_get() + timeout_ms;

    do {
        if (akira_runtime_stop_requested(exec_env)) {
            return -ECANCELED;
        }

        int64_t left = deadline - k_uptime_get();
        int32_t slice = (int32_t)CLAMP(left, 0, MQTT_API_WAIT_SLICE_MS);

        if (mqtt_service_poll_message(msg, K_MSEC(slice)) == 0) {
            return 0;
        }
    } while (k_uptime_get() < deadline);

    return -EAGAIN;
}

/* Record @p topic for @p a if new. s_api_lock held.
 * @return 0, or -ENOSPC if the app holds MQTT_API_SUBS_PER_APP already. */
static int sub_record_locked(struct mqtt_app_state *a, const char *topic)
{
    for (int i = 0; i < a->sub_count; i++) {
        if (strcmp(a->subs[i], topic) == 0) {
            return 0;
        }
    }
    if (a->sub_count >= MQTT_API_SUBS_PER_APP) {
        return -ENOSPC;
    }
    strncpy(a->subs[a->sub_count], topic, AKIRA_MQTT_TOPIC_MAX - 1);
    a->subs[a->sub_count][AKIRA_MQTT_TOPIC_MAX - 1] = '\0';
    a->sub_count++;
    return 0;
}

static int light_record_locked(struct mqtt_app_state *a, const char *object_id)
{
    for (int i = 0; i < a->light_count; i++) {
        if (strcmp(a->lights[i], object_id) == 0) {
            return 0;
        }
    }
    if (a->light_count >= MQTT_API_LIGHTS_PER_APP) {
        return -ENOSPC;
    }
    strncpy(a->lights[a->light_count], object_id, MQTT_API_OBJECT_ID_MAX);
    a->lights[a->light_count][MQTT_API_OBJECT_ID_MAX] = '\0';
    a->light_count++;
    return 0;
}

/* ---- Generic MQTT ------------------------------------------------------- */

/* mqtt_publish — sig "($*~ii)i" */
int akira_native_mqtt_publish(wasm_exec_env_t exec_env, const char *topic,
                              const void *payload, int len, int qos, int retain)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MQTT, -EPERM);
    if (!topic || len < 0) {
        return -EINVAL;
    }
    /* "*~" validated [payload, payload + len); "$" is NUL-terminated. */
    if (len > MQTT_API_PUBLISH_MAX) {
        return -EMSGSIZE;
    }
    if (topic_check(topic, MQTT_API_PUB_TOPIC_MAX, false) < 0) {
        MQTT_API_WRN("mqtt_publish: malformed topic refused");
        return -EINVAL;
    }

    struct mqtt_app_state *a = app_state(exec_env);

    if (!a) {
        return -EACCES;
    }
    if (!publish_allowed(a)) {
        return -EBUSY;
    }

    /* Holds the shared client mutex across the socket send. */
    akira_native_critical_enter(exec_env);
    int rc = mqtt_service_publish(topic, payload, (size_t)len, qos >= 1 ? 1 : 0,
                                  retain != 0);
    akira_native_critical_exit(exec_env);
    return rc;
}

/* mqtt_subscribe — sig "($)i" */
int akira_native_mqtt_subscribe(wasm_exec_env_t exec_env, const char *topic)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MQTT, -EPERM);
    if (!topic) {
        return -EINVAL;
    }
    /* mqtt_service remembers filters shorter than AKIRA_MQTT_TOPIC_MAX. */
    if (topic_check(topic, AKIRA_MQTT_TOPIC_MAX - 1, true) < 0) {
        MQTT_API_WRN("mqtt_subscribe: malformed topic filter refused");
        return -EINVAL;
    }

    struct mqtt_app_state *a = app_state(exec_env);

    if (!a) {
        return -EACCES;
    }
    if (akira_runtime_stop_requested(exec_env)) {
        return -ECANCELED;
    }

    akira_native_critical_enter(exec_env);
    int rc = -EBUSY;

    if (k_mutex_lock(&s_api_lock, K_MSEC(MQTT_API_LOCK_MS)) == 0) {
        rc = sub_record_locked(a, topic);
        if (rc == 0) {
            rc = mqtt_service_subscribe(topic);
        }
        k_mutex_unlock(&s_api_lock);
    }
    akira_native_critical_exit(exec_env);
    return rc;
}

/* mqtt_poll — sig "(*~*~i)i" : topic, topic_cap, payload, payload_cap, timeout */
int akira_native_mqtt_poll(wasm_exec_env_t exec_env, char *topic, int topic_cap,
                           uint8_t *payload, int payload_cap, int timeout_ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MQTT, -EPERM);
    if (!topic || !payload || topic_cap <= 0 || payload_cap <= 0) {
        return -EINVAL;
    }

    /* "*~" validated both buffers for their full capacity. The message is
     * copied into this frame first, so nothing writes into app memory
     * outside this call. */
    struct akira_mqtt_message msg;
    int rc = wait_message(exec_env, &msg, timeout_ms);

    if (rc != 0) {
        return rc;
    }

    int tn = MIN((int)strnlen(msg.topic, sizeof(msg.topic) - 1), topic_cap - 1);
    memcpy(topic, msg.topic, tn);
    topic[tn] = '\0';

    int pn = MIN(payload_cap, (int)MIN(msg.payload_len, sizeof(msg.payload)));
    if (pn > 0) {
        memcpy(payload, msg.payload, pn);
    }
    return pn;
}

/* mqtt_connected — sig "()i" */
int akira_native_mqtt_connected(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MQTT, -EPERM);
    return mqtt_service_is_connected() ? 1 : 0;
}

/* ---- Home Assistant light helpers --------------------------------------- */

/* ha_light_register — sig "($$)i" */
int akira_native_ha_light_register(wasm_exec_env_t exec_env,
                                   const char *object_id, const char *name)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MQTT, -EPERM);
    if (!object_id || !name) {
        return -EINVAL;
    }
    if (!object_id_ok(object_id) || !display_name_ok(name)) {
        MQTT_API_WRN("ha_light_register: bad object id or name refused");
        return -EINVAL;
    }

    struct mqtt_app_state *a = app_state(exec_env);

    if (!a) {
        return -EACCES;
    }
    if (akira_runtime_stop_requested(exec_env)) {
        return -ECANCELED;
    }
    /* Publishes a retained discovery config: counts as a publish. */
    if (!publish_allowed(a)) {
        return -EBUSY;
    }

    akira_native_critical_enter(exec_env);
    int rc = -EBUSY;

    if (k_mutex_lock(&s_api_lock, K_MSEC(MQTT_API_LOCK_MS)) == 0) {
        /* Each new object id is a new retained entity on the broker. */
        rc = light_record_locked(a, object_id);
        if (rc == 0) {
            rc = ha_light_register(object_id, name);
        }
        k_mutex_unlock(&s_api_lock);
    }
    akira_native_critical_exit(exec_env);
    return rc;
}

/* ha_light_report — sig "($iiiii)i" */
int akira_native_ha_light_report(wasm_exec_env_t exec_env, const char *object_id,
                                 int on, int brightness, int r, int g, int b)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MQTT, -EPERM);
    if (!object_id) {
        return -EINVAL;
    }
    if (!object_id_ok(object_id)) {
        return -EINVAL;
    }

    struct mqtt_app_state *a = app_state(exec_env);

    if (!a) {
        return -EACCES;
    }
    if (!publish_allowed(a)) {
        return -EBUSY;
    }

    akira_native_critical_enter(exec_env);
    int rc = ha_light_report(object_id, on, brightness, r, g, b);
    akira_native_critical_exit(exec_env);
    return rc;
}

/* ha_light_poll — sig "($*****i)i" */
int akira_native_ha_light_poll(wasm_exec_env_t exec_env, const char *object_id,
                               int *on, int *brightness, int *r, int *g, int *b,
                               int timeout_ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MQTT, -EPERM);
    if (!object_id || !on || !brightness || !r || !g || !b) {
        return -EINVAL;
    }
    if (!object_id_ok(object_id)) {
        return -EINVAL;
    }

    /* "*" only validates one byte per pointer; check each full int. */
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!inst ||
        !wasm_runtime_validate_native_addr(inst, on, sizeof(*on)) ||
        !wasm_runtime_validate_native_addr(inst, brightness, sizeof(*brightness)) ||
        !wasm_runtime_validate_native_addr(inst, r, sizeof(*r)) ||
        !wasm_runtime_validate_native_addr(inst, g, sizeof(*g)) ||
        !wasm_runtime_validate_native_addr(inst, b, sizeof(*b))) {
        return -EFAULT;
    }

    /* Work on copies: app pointers may be unaligned (an unaligned int access
     * faults on Xtensa), and a field the command leaves out keeps its value. */
    int v[5];
    int *out[5] = { on, brightness, r, g, b };

    for (int i = 0; i < 5; i++) {
        memcpy(&v[i], out[i], sizeof(int));
    }

    /* ha_light_poll() waits up to its timeout: call it in slices so a stop
     * ends the wait. A negative timeout does not wait, as before. */
    int64_t deadline = k_uptime_get() + MAX(timeout_ms, 0);
    int fields = 0;

    do {
        if (akira_runtime_stop_requested(exec_env)) {
            return -ECANCELED;
        }

        int64_t left = deadline - k_uptime_get();
        int slice = (int)CLAMP(left, 0, MQTT_API_WAIT_SLICE_MS);

        fields = ha_light_poll(object_id, &v[0], &v[1], &v[2], &v[3], &v[4], slice);
    } while (fields == 0 && k_uptime_get() < deadline);

    if (fields > 0) {
        for (int i = 0; i < 5; i++) {
            memcpy(out[i], &v[i], sizeof(int));
        }
    }
    return fields;
}

/* ---- App teardown ------------------------------------------------------- */

/* Does another slot than @p slot hold @p topic? s_api_lock held. */
static bool topic_held_elsewhere(int slot, const char *topic)
{
    for (int s = 0; s < AKIRA_MAX_WASM_INSTANCES; s++) {
        if (s == slot) {
            continue;
        }
        for (int i = 0; i < s_apps[s].sub_count; i++) {
            if (strcmp(s_apps[s].subs[i], topic) == 0) {
                return true;
            }
        }
    }
    return false;
}

static bool light_held_elsewhere(int slot, const char *object_id)
{
    for (int s = 0; s < AKIRA_MAX_WASM_INSTANCES; s++) {
        if (s == slot) {
            continue;
        }
        for (int i = 0; i < s_apps[s].light_count; i++) {
            if (strcmp(s_apps[s].lights[i], object_id) == 0) {
                return true;
            }
        }
    }
    return false;
}

/*
 * Hand back what the app subscribed to (topics no other app holds) and the
 * command topics of its lights, then forget its records and publish budget.
 * The lights' retained discovery configs stay: HA keeps the entity across
 * app restarts. Bounded: the lock wait is capped, and
 * mqtt_service_unsubscribe() only queues the UNSUBSCRIBE for the service
 * thread.
 */
static void mqtt_release(int slot, const char *app_name)
{
    ARG_UNUSED(app_name);

    if (slot < 0 || slot >= AKIRA_MAX_WASM_INSTANCES) {
        return;
    }

    struct mqtt_app_state *a = &s_apps[slot];

    if (k_mutex_lock(&s_api_lock, K_MSEC(MQTT_API_RELEASE_LOCK_MS)) != 0) {
        /* A holder died with it: forget this slot's records anyway (nobody
         * else writes them), but leave the broker side alone. */
        memset(a, 0, sizeof(*a));
        return;
    }

    for (int i = 0; i < a->sub_count; i++) {
        if (!topic_held_elsewhere(slot, a->subs[i])) {
            (void)mqtt_service_unsubscribe(a->subs[i]);
        }
    }
    for (int i = 0; i < a->light_count; i++) {
        if (!light_held_elsewhere(slot, a->lights[i])) {
            char cmd[AKIRA_MQTT_TOPIC_MAX];

            snprintf(cmd, sizeof(cmd), "%s/light/%s/set",
                     mqtt_service_node_id(), a->lights[i]);
            (void)mqtt_service_unsubscribe(cmd);
        }
    }
    memset(a, 0, sizeof(*a));
    k_mutex_unlock(&s_api_lock);
}

AKIRA_APP_RESOURCE_DEFINE(mqtt, 50, NULL, mqtt_release);

/* ===== WASM exports =====
 * Registered with WAMR by the native API registry (akira_native_registry.h).
 * Import names and signatures are WASM ABI: see docs/api-stability-policy.md. */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && (defined(CONFIG_AKIRA_WASM_MQTT))
#include <akira_native_registry.h>

/* mqtt: MQTT client + Home Assistant discovery */
static const NativeSymbol akira_mqtt_natives[] = {
    {"mqtt_publish",       (void *)akira_native_mqtt_publish,       "($*~ii)i",  NULL},
    {"mqtt_subscribe",     (void *)akira_native_mqtt_subscribe,     "($)i",      NULL},
    {"mqtt_poll",          (void *)akira_native_mqtt_poll,          "(*~*~i)i",  NULL},
    {"mqtt_connected",     (void *)akira_native_mqtt_connected,     "()i",       NULL},
    {"ha_light_register",  (void *)akira_native_ha_light_register,  "($$)i",     NULL},
    {"ha_light_report",    (void *)akira_native_ha_light_report,    "($iiiii)i", NULL},
    {"ha_light_poll",      (void *)akira_native_ha_light_poll,      "($*****i)i", NULL},
};

AKIRA_NATIVE_API_DEFINE(akira_mqtt_api, "env", akira_mqtt_natives);
#endif
