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
 *   0x01 COMMISSION_REQ   → 0x81 COMMISSION_RESP
 *   0x02 SEND_REQ         → 0x82 SEND_RESP
 *   0x03 SUBSCRIBE_REQ    → 0x83 SUBSCRIBE_RESP
 *   0x04 EVENT            (unsolicited, co-processor → host)
 *   0x05 STATUS_REQ       → 0x85 STATUS_RESP
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
#define AKIRA_MATTER_IPC_EUI64_LEN      8
#define AKIRA_MATTER_IPC_PASSCODE_LEN   16
#define AKIRA_MATTER_IPC_TIMEOUT_MS     3000

/* CMD byte values */
#define AKIRA_MATTER_CMD_COMMISSION_REQ  0x01
#define AKIRA_MATTER_CMD_COMMISSION_RESP 0x81
#define AKIRA_MATTER_CMD_SEND_REQ        0x02
#define AKIRA_MATTER_CMD_SEND_RESP       0x82
#define AKIRA_MATTER_CMD_SUBSCRIBE_REQ   0x03
#define AKIRA_MATTER_CMD_SUBSCRIBE_RESP  0x83
#define AKIRA_MATTER_CMD_EVENT           0x04
#define AKIRA_MATTER_CMD_STATUS_REQ      0x05
#define AKIRA_MATTER_CMD_STATUS_RESP     0x85

/* Async event descriptor filled by the RX thread */
struct akira_matter_event {
    uint8_t  src_eui64[AKIRA_MATTER_IPC_EUI64_LEN];
    uint16_t attr_id;
    uint8_t  value[AKIRA_MATTER_IPC_MAX_PAYLOAD];
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

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_MATTER_IPC_H */
