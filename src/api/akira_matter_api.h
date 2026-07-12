/**
 * @file akira_matter_api.h
 * @brief Matter/Thread co-processor WASM native API
 *
 * Exposes matter_commission / matter_send / matter_subscribe / matter_poll
 * to WASM apps over the IPC bridge to the Thread co-processor.
 *
 * Gated behind AKIRA_CAP_MATTER ("matter" in manifest capabilities).
 *
 * @stability experimental
 * @since 1.6
 */

#ifndef AKIRA_MATTER_API_H
#define AKIRA_MATTER_API_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#else
typedef void *wasm_exec_env_t;
#endif

#define MATTER_OK               0
#define MATTER_ERR_NO_COPROC   -1  /* co-processor not responding */
#define MATTER_ERR_TIMEOUT     -2  /* commission/response timed out */
#define MATTER_ERR_INVALID     -3  /* bad argument */
#define MATTER_ERR_NOPERM      -4  /* capability denied */
#define MATTER_ERR_IO          -5  /* UART transport error */

/**
 * Commission a Matter device into the fabric.
 *
 * Sends the passcode to the co-processor which performs BLE commissioning.
 * On success, writes the 8-byte EUI-64 of the commissioned device into
 * eui64_out (WASM pointer, 8 bytes minimum).
 *
 * @param exec_env       WAMR execution environment
 * @param passcode_ptr   WASM pointer to NUL-terminated passcode string
 * @param eui64_ptr      WASM pointer to 8-byte output buffer
 * @return 0 on success, negative MATTER_ERR_* on failure
 *
 * Required manifest capability: "matter"
 */
int akira_native_matter_commission(wasm_exec_env_t exec_env,
                                   const char *passcode,
                                   uint8_t *eui64_out);

/**
 * Send a raw payload to a commissioned Matter device.
 *
 * @param exec_env      WAMR execution environment
 * @param eui64         WASM pointer to 8-byte device EUI-64
 * @param payload       WASM pointer to payload bytes
 * @param len           Payload length in bytes
 * @return 0 on success, negative MATTER_ERR_* on failure
 *
 * Required manifest capability: "matter"
 */
int akira_native_matter_send(wasm_exec_env_t exec_env,
                             const uint8_t *eui64,
                             const uint8_t *payload,
                             int len);

/**
 * Subscribe to attribute change events for a Matter device.
 *
 * Events are queued and retrieved via matter_poll().
 *
 * @param exec_env      WAMR execution environment
 * @param eui64         WASM pointer to 8-byte device EUI-64
 * @param attr_id       Matter cluster/attribute ID (16-bit)
 * @return 0 on success, negative MATTER_ERR_* on failure
 *
 * Required manifest capability: "matter"
 */
int akira_native_matter_subscribe(wasm_exec_env_t exec_env,
                                  const uint8_t *eui64,
                                  int attr_id);

/**
 * Poll for the next incoming Matter event (blocking).
 *
 * Fills src_eui64_ptr (8 bytes), attr_id_ptr (4 bytes, int), and buf.
 * Returns the number of value bytes written into buf, or a negative error.
 *
 * @param exec_env       WAMR execution environment
 * @param src_eui64      WASM pointer to 8-byte source EUI-64 output
 * @param attr_id_ptr    WASM pointer to int32 attribute ID output
 * @param buf            WASM pointer to value output buffer
 * @param buf_len        Size of buf in bytes
 * @param timeout_ms     Milliseconds to wait; -1 = forever
 * @return Number of value bytes on success, negative MATTER_ERR_* on failure
 *
 * Required manifest capability: "matter"
 */
int akira_native_matter_poll(wasm_exec_env_t exec_env,
                             uint8_t *src_eui64,
                             int *attr_id_ptr,
                             uint8_t *buf,
                             int buf_len,
                             int timeout_ms);

/* =========================================================================
 * Accessory direction — expose AkiraOS's own hardware as a Matter device.
 * Gated behind CONFIG_AKIRA_MATTER_ACCESSORY (compiled out otherwise).
 * All require manifest capability "matter".
 * ========================================================================= */

/**
 * Register a local Matter endpoint (device type + server clusters).
 *
 * @param exec_env     WAMR execution environment
 * @param device_type  Matter device type ID (e.g. 0x0102 color light)
 * @param clusters     WASM pointer to an array of uint32 cluster IDs
 * @param n_clusters   Number of cluster IDs (max 8)
 * @return Assigned endpoint ID (>= 0) on success, negative MATTER_ERR_* on failure
 */
int akira_native_matter_endpoint_add(wasm_exec_env_t exec_env,
                                     int device_type,
                                     const uint32_t *clusters,
                                     int n_clusters);

/**
 * Report a local attribute value outward to the fabric.
 *
 * @param exec_env  WAMR execution environment
 * @param endpoint  Local endpoint ID (from matter_endpoint_add)
 * @param cluster   Cluster ID
 * @param attr      Attribute ID
 * @param val       WASM pointer to value bytes
 * @param len       Value length in bytes (1..255)
 * @return 0 on success, negative MATTER_ERR_* on failure
 */
int akira_native_matter_report_attr(wasm_exec_env_t exec_env,
                                    int endpoint, int cluster, int attr,
                                    const void *val, int len);

/**
 * Poll for the next inbound command targeting a local endpoint (blocking).
 *
 * Fills endpoint/cluster/cmd and copies the command payload into buf.
 *
 * @param exec_env    WAMR execution environment
 * @param endpoint    WASM pointer to int32 endpoint output
 * @param cluster     WASM pointer to int32 cluster output
 * @param cmd         WASM pointer to int32 command-id output
 * @param buf         WASM pointer to value output buffer
 * @param buf_len     Size of buf in bytes
 * @param timeout_ms  Milliseconds to wait; -1 = forever
 * @return Number of value bytes on success, negative MATTER_ERR_* on failure
 */
int akira_native_matter_cmd_poll(wasm_exec_env_t exec_env,
                                 int *endpoint, int *cluster, int *cmd,
                                 uint8_t *buf, int buf_len, int timeout_ms);

/**
 * Open this node's commissioning window so a controller can adopt it.
 *
 * @param exec_env     WAMR execution environment
 * @param timeout_sec  Window timeout in seconds (0 = co-processor default)
 * @return 0 on success, negative MATTER_ERR_* on failure
 */
int akira_native_matter_open_pairing(wasm_exec_env_t exec_env, int timeout_sec);

/**
 * Fetch this node's onboarding payload (QR string + manual pairing code).
 *
 * @param exec_env    WAMR execution environment
 * @param qr          WASM pointer to QR output buffer
 * @param qr_len      Size of qr buffer
 * @param manual      WASM pointer to manual-code output buffer
 * @param manual_len  Size of manual buffer
 * @return 0 on success, negative MATTER_ERR_* on failure
 */
int akira_native_matter_get_pairing(wasm_exec_env_t exec_env,
                                    char *qr, int qr_len,
                                    char *manual, int manual_len);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_MATTER_API_H */
