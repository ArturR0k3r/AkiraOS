/**
 * @file akira_matter_api.c
 * @brief Matter/Thread WASM native API — bridges WASM apps to IPC transport
 */

#include "akira_matter_api.h"
#include <runtime/akira_matter_ipc.h>
#include <runtime/security.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <wasm_export.h>
#include <string.h>

LOG_MODULE_REGISTER(akira_matter_api, CONFIG_AKIRA_LOG_LEVEL);

static bool s_ipc_ready;

static int ensure_ipc(void)
{
    if (s_ipc_ready) {
        return 0;
    }
    int rc = akira_matter_ipc_init();
    if (rc == 0) {
        s_ipc_ready = true;
    }
    return rc;
}

/* --------------------------------------------------------------------------
 * Pointer validation helpers
 * ------------------------------------------------------------------------- */
static const char *validated_str(wasm_exec_env_t env, uint32_t app_ptr,
                                  uint32_t max_len)
{
    wasm_module_inst_t mi = wasm_runtime_get_module_inst(env);
    if (!mi || !wasm_runtime_validate_app_str_addr(mi, app_ptr)) {
        return NULL;
    }
    return (const char *)wasm_runtime_addr_app_to_native(mi, app_ptr);
}

/* --------------------------------------------------------------------------
 * matter_commission
 * ------------------------------------------------------------------------- */
int akira_native_matter_commission(wasm_exec_env_t exec_env,
                                   const char *passcode,
                                   uint8_t *eui64_out)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MATTER, MATTER_ERR_NOPERM);

    if (!passcode || !eui64_out) {
        return MATTER_ERR_INVALID;
    }

    int rc = ensure_ipc();
    if (rc != 0) {
        LOG_ERR("matter: IPC init failed (%d)", rc);
        return MATTER_ERR_NO_COPROC;
    }

    rc = akira_matter_ipc_commission(passcode, eui64_out);
    if (rc != 0) {
        LOG_WRN("matter: commission failed (%d)", rc);
        return (rc == -ETIMEDOUT) ? MATTER_ERR_TIMEOUT : MATTER_ERR_IO;
    }
    return MATTER_OK;
}

/* --------------------------------------------------------------------------
 * matter_send
 * ------------------------------------------------------------------------- */
int akira_native_matter_send(wasm_exec_env_t exec_env,
                             const uint8_t *eui64,
                             const uint8_t *payload,
                             int len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MATTER, MATTER_ERR_NOPERM);

    if (!eui64 || !payload || len <= 0 ||
        len > AKIRA_MATTER_IPC_MAX_PAYLOAD - AKIRA_MATTER_IPC_EUI64_LEN) {
        return MATTER_ERR_INVALID;
    }

    int rc = ensure_ipc();
    if (rc != 0) { return MATTER_ERR_NO_COPROC; }

    rc = akira_matter_ipc_send(eui64, payload, (uint16_t)len);
    if (rc != 0) {
        return (rc == -ETIMEDOUT) ? MATTER_ERR_TIMEOUT : MATTER_ERR_IO;
    }
    return MATTER_OK;
}

/* --------------------------------------------------------------------------
 * matter_subscribe
 * ------------------------------------------------------------------------- */
int akira_native_matter_subscribe(wasm_exec_env_t exec_env,
                                  const uint8_t *eui64,
                                  int attr_id)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MATTER, MATTER_ERR_NOPERM);

    if (!eui64 || attr_id < 0 || attr_id > 0xFFFF) {
        return MATTER_ERR_INVALID;
    }

    int rc = ensure_ipc();
    if (rc != 0) { return MATTER_ERR_NO_COPROC; }

    rc = akira_matter_ipc_subscribe(eui64, (uint16_t)attr_id);
    if (rc != 0) {
        return (rc == -ETIMEDOUT) ? MATTER_ERR_TIMEOUT : MATTER_ERR_IO;
    }
    return MATTER_OK;
}

/* --------------------------------------------------------------------------
 * matter_poll
 * ------------------------------------------------------------------------- */
int akira_native_matter_poll(wasm_exec_env_t exec_env,
                             uint8_t *src_eui64,
                             int *attr_id_ptr,
                             uint8_t *buf,
                             int buf_len,
                             int timeout_ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MATTER, MATTER_ERR_NOPERM);

    if (!src_eui64 || !attr_id_ptr || !buf || buf_len <= 0) {
        return MATTER_ERR_INVALID;
    }

    int rc = ensure_ipc();
    if (rc != 0) { return MATTER_ERR_NO_COPROC; }

    struct akira_matter_event evt;
    k_timeout_t to = (timeout_ms < 0) ? K_FOREVER : K_MSEC(timeout_ms);

    rc = akira_matter_ipc_poll(&evt, to);
    if (rc == -EAGAIN) {
        return MATTER_ERR_TIMEOUT;
    }
    if (rc != 0) {
        return MATTER_ERR_IO;
    }

    memcpy(src_eui64, evt.src_eui64, AKIRA_MATTER_IPC_EUI64_LEN);
    *attr_id_ptr = (int)evt.attr_id;

    uint16_t copy = MIN((uint16_t)buf_len, evt.value_len);
    if (copy > 0) {
        memcpy(buf, evt.value, copy);
    }
    return (int)copy;
}

#ifdef CONFIG_AKIRA_MATTER_ACCESSORY
/* --------------------------------------------------------------------------
 * Accessory direction — expose AkiraOS's own hardware as a Matter device.
 * ------------------------------------------------------------------------- */

/* matter_endpoint_add — sig "(i*~)i": device_type, clusters, n_clusters */
int akira_native_matter_endpoint_add(wasm_exec_env_t exec_env,
                                     int device_type,
                                     const uint32_t *clusters,
                                     int n_clusters)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MATTER, MATTER_ERR_NOPERM);

    if (!clusters || device_type < 0 || device_type > 0xFFFF ||
        n_clusters <= 0 || n_clusters > 8) {
        return MATTER_ERR_INVALID;
    }

    int rc = ensure_ipc();
    if (rc != 0) { return MATTER_ERR_NO_COPROC; }

    uint8_t endpoint = 0;
    rc = akira_matter_ipc_endpoint_add((uint16_t)device_type, clusters,
                                       (uint8_t)n_clusters, &endpoint);
    if (rc != 0) {
        return (rc == -ETIMEDOUT) ? MATTER_ERR_TIMEOUT : MATTER_ERR_IO;
    }
    return (int)endpoint;
}

/* matter_report_attr — sig "(iii*~)i": endpoint, cluster, attr, val, len */
int akira_native_matter_report_attr(wasm_exec_env_t exec_env,
                                    int endpoint, int cluster, int attr,
                                    const void *val, int len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MATTER, MATTER_ERR_NOPERM);

    if (!val || endpoint < 0 || endpoint > 0xFF || len <= 0 || len > 0xFF) {
        return MATTER_ERR_INVALID;
    }

    int rc = ensure_ipc();
    if (rc != 0) { return MATTER_ERR_NO_COPROC; }

    rc = akira_matter_ipc_report((uint8_t)endpoint, (uint32_t)cluster,
                                 (uint32_t)attr, (const uint8_t *)val,
                                 (uint16_t)len);
    if (rc != 0) {
        return (rc == -ETIMEDOUT) ? MATTER_ERR_TIMEOUT : MATTER_ERR_IO;
    }
    return MATTER_OK;
}

/* matter_cmd_poll — sig "(****~i)i": endpoint*, cluster*, cmd*, buf, buf_len, timeout */
int akira_native_matter_cmd_poll(wasm_exec_env_t exec_env,
                                 int *endpoint, int *cluster, int *cmd,
                                 uint8_t *buf, int buf_len, int timeout_ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MATTER, MATTER_ERR_NOPERM);

    if (!endpoint || !cluster || !cmd || !buf || buf_len <= 0) {
        return MATTER_ERR_INVALID;
    }

    int rc = ensure_ipc();
    if (rc != 0) { return MATTER_ERR_NO_COPROC; }

    struct akira_matter_event evt;
    k_timeout_t to = (timeout_ms < 0) ? K_FOREVER : K_MSEC(timeout_ms);

    rc = akira_matter_ipc_poll(&evt, to);
    if (rc == -EAGAIN) {
        return MATTER_ERR_TIMEOUT;
    }
    if (rc != 0) {
        return MATTER_ERR_IO;
    }
    /* This poll only serves accessory-direction commands. A controller-attr
     * event here means the app mixed both roles; drop it and report timeout. */
    if (evt.kind != AKIRA_MATTER_EVT_ACC_CMD) {
        return MATTER_ERR_TIMEOUT;
    }

    *endpoint = (int)evt.endpoint_id;
    *cluster = (int)evt.cluster_id;
    *cmd = (int)evt.cmd_id;

    uint16_t copy = MIN((uint16_t)buf_len, evt.value_len);
    if (copy > 0) {
        memcpy(buf, evt.value, copy);
    }
    return (int)copy;
}

/* matter_open_pairing — sig "(i)i": timeout_sec */
int akira_native_matter_open_pairing(wasm_exec_env_t exec_env, int timeout_sec)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MATTER, MATTER_ERR_NOPERM);

    if (timeout_sec < 0 || timeout_sec > 0xFFFF) {
        return MATTER_ERR_INVALID;
    }

    int rc = ensure_ipc();
    if (rc != 0) { return MATTER_ERR_NO_COPROC; }

    rc = akira_matter_ipc_open_pairing((uint16_t)timeout_sec);
    if (rc != 0) {
        return (rc == -ETIMEDOUT) ? MATTER_ERR_TIMEOUT : MATTER_ERR_IO;
    }
    return MATTER_OK;
}

/* matter_get_pairing — sig "(*~*~)i": qr, qr_len, manual, manual_len */
int akira_native_matter_get_pairing(wasm_exec_env_t exec_env,
                                    char *qr, int qr_len,
                                    char *manual, int manual_len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MATTER, MATTER_ERR_NOPERM);

    if (!qr || qr_len <= 0 || !manual || manual_len <= 0) {
        return MATTER_ERR_INVALID;
    }

    int rc = ensure_ipc();
    if (rc != 0) { return MATTER_ERR_NO_COPROC; }

    rc = akira_matter_ipc_get_qr(qr, (size_t)qr_len, manual, (size_t)manual_len);
    if (rc != 0) {
        return (rc == -ETIMEDOUT) ? MATTER_ERR_TIMEOUT : MATTER_ERR_IO;
    }
    return MATTER_OK;
}
#endif /* CONFIG_AKIRA_MATTER_ACCESSORY */
