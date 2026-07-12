/**
 * @file akira_matter_ipc.h
 * @brief Matter co-processor IPC transport (UART framing)
 *
 * AkiraOS runs on the HP core and communicates with a Thread/Matter
 * co-processor (e.g. ESP32-H2 flashed with esp-matter) over UART1 using a
 * lightweight TLV frame protocol.
 *
 * Frame layout:
 *   [0xAC][0xCE][CMD:1][SEQ:1][LEN_H:1][LEN_L:1][PAYLOAD:LEN][CRC16_H:1][CRC16_L:1]
 *
 * CMD encoding:
 *   Controller direction (AkiraOS adopts / talks to remote Matter devices):
 *   0x01 COMMISSION_REQ   → 0x81 COMMISSION_RESP
 *   0x02 SEND_REQ         → 0x82 SEND_RESP
 *   0x03 SUBSCRIBE_REQ    → 0x83 SUBSCRIBE_RESP
 *   0x04 EVENT            (unsolicited, co-processor → host: remote attr changed)
 *   0x05 STATUS_REQ       → 0x85 STATUS_RESP
 *
 *   Accessory direction (AkiraOS exposes its OWN hardware as a Matter device):
 *   0x06 EP_ADD_REQ       → 0x86 EP_ADD_RESP     (register a local endpoint)
 *   0x07 ATTR_REPORT_REQ  → 0x87 ATTR_REPORT_RESP(report a local attribute value)
 *   0x08 PAIR_OPEN_REQ    → 0x88 PAIR_OPEN_RESP  (open local commissioning window)
 *   0x09 QR_GET_REQ       → 0x89 QR_GET_RESP     (fetch this node's onboarding codes)
 *   0x0A ACC_CMD_EVENT    (unsolicited, co-processor → host: inbound command to a
 *                          local endpoint, e.g. On/Off from Home Assistant)
 *
 * All multi-byte fields are big-endian.
 */

#ifndef AKIRA_MATTER_IPC_H
#define AKIRA_MATTER_IPC_H

#include <stdint.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AKIRA_MATTER_IPC_SYNC0          0xAC
#define AKIRA_MATTER_IPC_SYNC1          0xCE
#define AKIRA_MATTER_IPC_FRAME_HDR_LEN  6    /* 2 sync + cmd + seq + len(2) */
#define AKIRA_MATTER_IPC_FRAME_CRC_LEN  2
#define AKIRA_MATTER_IPC_MAX_PAYLOAD    256
/* Attribute/command values carried in async events and response payloads are
 * small (an On/Off byte, a level, a short color/measurement). Cap their buffers
 * well below MAX_PAYLOAD to keep the event queue and in-flight slots compact —
 * these live in .bss and matter on RAM-tight targets (e.g. ESP32-S3). */
#define AKIRA_MATTER_IPC_EVENT_VALUE_LEN 64
#define AKIRA_MATTER_IPC_RESP_LEN        64
#define AKIRA_MATTER_IPC_EUI64_LEN      8
#define AKIRA_MATTER_IPC_PASSCODE_LEN   16
#define AKIRA_MATTER_IPC_TIMEOUT_MS     3000
#define AKIRA_MATTER_IPC_QR_LEN         48   /* max onboarding QR string */
#define AKIRA_MATTER_IPC_MANUAL_LEN     16   /* "1234-5678-901" + NUL */

/* CMD byte values — controller direction */
#define AKIRA_MATTER_CMD_COMMISSION_REQ  0x01
#define AKIRA_MATTER_CMD_COMMISSION_RESP 0x81
#define AKIRA_MATTER_CMD_SEND_REQ        0x02
#define AKIRA_MATTER_CMD_SEND_RESP       0x82
#define AKIRA_MATTER_CMD_SUBSCRIBE_REQ   0x03
#define AKIRA_MATTER_CMD_SUBSCRIBE_RESP  0x83
#define AKIRA_MATTER_CMD_EVENT           0x04
#define AKIRA_MATTER_CMD_STATUS_REQ      0x05
#define AKIRA_MATTER_CMD_STATUS_RESP     0x85

/* CMD byte values — accessory direction */
#define AKIRA_MATTER_CMD_EP_ADD_REQ      0x06
#define AKIRA_MATTER_CMD_EP_ADD_RESP     0x86
#define AKIRA_MATTER_CMD_ATTR_REPORT_REQ  0x07
#define AKIRA_MATTER_CMD_ATTR_REPORT_RESP 0x87
#define AKIRA_MATTER_CMD_PAIR_OPEN_REQ   0x08
#define AKIRA_MATTER_CMD_PAIR_OPEN_RESP  0x88
#define AKIRA_MATTER_CMD_QR_GET_REQ      0x09
#define AKIRA_MATTER_CMD_QR_GET_RESP     0x89
#define AKIRA_MATTER_CMD_ACC_CMD_EVENT   0x0A

/* Event kinds — discriminate the two async event directions */
#define AKIRA_MATTER_EVT_ATTR    0  /* controller: remote device attribute changed */
#define AKIRA_MATTER_EVT_ACC_CMD 1  /* accessory: inbound command to a local endpoint */

/* Async event descriptor filled by the RX thread.
 * `kind` selects which fields are valid:
 *   AKIRA_MATTER_EVT_ATTR    → src_eui64, attr_id, value
 *   AKIRA_MATTER_EVT_ACC_CMD → endpoint_id, cluster_id, cmd_id, value
 */
struct akira_matter_event {
    uint8_t  kind;
    /* controller-direction fields */
    uint8_t  src_eui64[AKIRA_MATTER_IPC_EUI64_LEN];
    uint16_t attr_id;
    /* accessory-direction fields */
    uint8_t  endpoint_id;
    uint32_t cluster_id;
    uint32_t cmd_id;
    /* shared payload (small — see AKIRA_MATTER_IPC_EVENT_VALUE_LEN) */
    uint8_t  value[AKIRA_MATTER_IPC_EVENT_VALUE_LEN];
    uint16_t value_len;
};

/**
 * Initialise the Matter IPC subsystem.
 * Opens UART1, starts the RX thread, and sends a STATUS_REQ ping.
 * @return 0 on success, negative errno on failure.
 */
int akira_matter_ipc_init(void);

/**
 * Commission a Matter device into the fabric.
 * @param passcode    NUL-terminated commission passcode string (up to 15 chars).
 * @param eui64_out   8-byte buffer to receive the device EUI-64.
 * @return 0 on success, negative errno on failure.
 */
int akira_matter_ipc_commission(const char *passcode,
                                uint8_t eui64_out[AKIRA_MATTER_IPC_EUI64_LEN]);

/**
 * Send a raw payload to a commissioned device.
 * @param eui64    Target device EUI-64 (8 bytes).
 * @param payload  Payload bytes.
 * @param len      Payload length (max AKIRA_MATTER_IPC_MAX_PAYLOAD).
 * @return 0 on success, negative errno on failure.
 */
int akira_matter_ipc_send(const uint8_t eui64[AKIRA_MATTER_IPC_EUI64_LEN],
                          const uint8_t *payload, uint16_t len);

/**
 * Subscribe to attribute change events for a device.
 * @param eui64    Target device EUI-64 (8 bytes).
 * @param attr_id  Matter cluster/attribute ID.
 * @return 0 on success, negative errno on failure.
 */
int akira_matter_ipc_subscribe(const uint8_t eui64[AKIRA_MATTER_IPC_EUI64_LEN],
                               uint16_t attr_id);

/**
 * Poll for the next incoming event (blocking).
 * @param evt      Output event descriptor.
 * @param timeout  Wait timeout (use K_MSEC(n) or K_FOREVER).
 * @return 0 on success, -EAGAIN on timeout, negative errno on error.
 */
int akira_matter_ipc_poll(struct akira_matter_event *evt, k_timeout_t timeout);

/**
 * Query co-processor readiness.
 * @return 0 if co-processor ack'd, -EIO if unreachable.
 */
int akira_matter_ipc_status(void);

#ifdef CONFIG_AKIRA_MATTER_ACCESSORY
/* -------------------------------------------------------------------------
 * Accessory direction — expose AkiraOS's own hardware as a Matter device.
 * ---------------------------------------------------------------------- */

/**
 * Register a local Matter endpoint (device type + clusters) on the co-processor.
 * @param device_type   Matter device type ID (e.g. 0x0102 color light).
 * @param clusters      Array of server cluster IDs to instantiate.
 * @param n_clusters    Number of entries in @p clusters (max 8).
 * @param endpoint_out  Receives the endpoint ID assigned by the co-processor.
 * @return 0 on success, negative errno on failure.
 */
int akira_matter_ipc_endpoint_add(uint16_t device_type, const uint32_t *clusters,
                                  uint8_t n_clusters, uint8_t *endpoint_out);

/**
 * Report a local attribute value outward (co-processor pushes it to the fabric).
 * @param endpoint  Local endpoint ID (from endpoint_add).
 * @param cluster   Cluster ID.
 * @param attr      Attribute ID.
 * @param val       Attribute value bytes.
 * @param len       Value length in bytes.
 * @return 0 on success, negative errno on failure.
 */
int akira_matter_ipc_report(uint8_t endpoint, uint32_t cluster, uint32_t attr,
                            const uint8_t *val, uint16_t len);

/**
 * Open this node's commissioning window so a controller (Home Assistant,
 * Google Home, ...) can adopt it.
 * @param timeout_sec  Window timeout in seconds (0 = co-processor default).
 * @return 0 on success, negative errno on failure.
 */
int akira_matter_ipc_open_pairing(uint16_t timeout_sec);

/**
 * Fetch this node's onboarding payload from the co-processor.
 * @param qr          Buffer for the "MT:..." QR string.
 * @param qr_len      Size of @p qr.
 * @param manual      Buffer for the 11-digit manual pairing code.
 * @param manual_len  Size of @p manual.
 * @return 0 on success, negative errno on failure.
 */
int akira_matter_ipc_get_qr(char *qr, size_t qr_len,
                            char *manual, size_t manual_len);
#endif /* CONFIG_AKIRA_MATTER_ACCESSORY */

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_MATTER_IPC_H */
