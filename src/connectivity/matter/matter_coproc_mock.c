/**
 * @file matter_coproc_mock.c
 * @brief In-firmware mock Matter co-processor — answers IPC frames locally.
 *
 * Enabled by CONFIG_AKIRA_MATTER_COPROC_MOCK. Parses the request frames that
 * akira_matter_ipc.c writes and synthesises plausible responses, so the whole
 * accessory (device-as-endpoint) path runs without real co-processor hardware.
 *
 * Development / CI only — never enable in production.
 */

#include "matter_coproc_mock.h"

#include <runtime/akira_matter_ipc.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/ring_buffer.h>
#include <string.h>

LOG_MODULE_REGISTER(matter_coproc_mock, CONFIG_AKIRA_LOG_LEVEL);

/* Bytes the mock sends back to AkiraOS (co-processor → host). */
RING_BUF_DECLARE(s_out_ring, 256);
static K_MUTEX_DEFINE(s_out_mutex);

/* Assigned-endpoint counter (first endpoint_add returns 1). */
static uint8_t s_next_endpoint = 1;

/* ------------------------------------------------------------------------- */
static uint16_t frame_crc(const uint8_t *hdr, uint8_t hdr_len,
                          const uint8_t *payload, uint16_t pay_len)
{
    uint16_t crc = crc16_reflect(0x8005, 0xFFFF, hdr, hdr_len);
    if (pay_len > 0) {
        crc = crc16_reflect(0x8005, crc, payload, pay_len);
    }
    return crc ^ 0xFFFF;
}

/* Serialise a full frame into the outbound ring buffer. */
static void emit_frame(uint8_t cmd, uint8_t seq,
                       const uint8_t *payload, uint16_t len)
{
    uint8_t hdr[AKIRA_MATTER_IPC_FRAME_HDR_LEN];
    hdr[0] = AKIRA_MATTER_IPC_SYNC0;
    hdr[1] = AKIRA_MATTER_IPC_SYNC1;
    hdr[2] = cmd;
    hdr[3] = seq;
    hdr[4] = (uint8_t)(len >> 8);
    hdr[5] = (uint8_t)(len & 0xFF);

    uint16_t crc = frame_crc(hdr, sizeof(hdr), payload, len);
    uint8_t crc_bytes[2] = { (uint8_t)(crc >> 8), (uint8_t)(crc & 0xFF) };

    k_mutex_lock(&s_out_mutex, K_FOREVER);
    ring_buf_put(&s_out_ring, hdr, sizeof(hdr));
    if (len > 0) {
        ring_buf_put(&s_out_ring, payload, len);
    }
    ring_buf_put(&s_out_ring, crc_bytes, sizeof(crc_bytes));
    k_mutex_unlock(&s_out_mutex);
}

/* Build a response payload: [status:4 big-endian][extra...]. */
static void emit_response(uint8_t req_cmd, uint8_t seq, int32_t status,
                          const uint8_t *extra, uint16_t extra_len)
{
    uint8_t payload[AKIRA_MATTER_IPC_MAX_PAYLOAD];
    payload[0] = (uint8_t)(status >> 24);
    payload[1] = (uint8_t)(status >> 16);
    payload[2] = (uint8_t)(status >> 8);
    payload[3] = (uint8_t)(status & 0xFF);
    uint16_t len = 4;
    if (extra && extra_len > 0) {
        extra_len = MIN(extra_len, (uint16_t)(sizeof(payload) - 4));
        memcpy(&payload[4], extra, extra_len);
        len += extra_len;
    }
    emit_frame((uint8_t)(req_cmd | 0x80), seq, payload, len);
}

/* --------------------------------------------------------------------------
 * Request dispatch
 * ------------------------------------------------------------------------- */
static void handle_request(uint8_t cmd, uint8_t seq,
                           const uint8_t *payload, uint16_t len)
{
    switch (cmd) {
    case AKIRA_MATTER_CMD_STATUS_REQ:
    case AKIRA_MATTER_CMD_SEND_REQ:
    case AKIRA_MATTER_CMD_SUBSCRIBE_REQ:
    case AKIRA_MATTER_CMD_ATTR_REPORT_REQ:
    case AKIRA_MATTER_CMD_PAIR_OPEN_REQ:
        emit_response(cmd, seq, 0, NULL, 0);
        break;

    case AKIRA_MATTER_CMD_COMMISSION_REQ: {
        /* Return a deterministic fake EUI-64. */
        static const uint8_t eui64[AKIRA_MATTER_IPC_EUI64_LEN] = {
            0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
        };
        emit_response(cmd, seq, 0, eui64, sizeof(eui64));
        break;
    }

    case AKIRA_MATTER_CMD_EP_ADD_REQ: {
        uint8_t ep = s_next_endpoint++;
        emit_response(cmd, seq, 0, &ep, 1);
        LOG_INF("mock: endpoint %u registered", ep);
        /* Kick off the demo: pretend a controller turns the endpoint ON. */
        matter_coproc_mock_inject_command(ep, 0x0006 /*OnOff*/,
                                          0x0001 /*On*/, NULL, 0);
        break;
    }

    case AKIRA_MATTER_CMD_QR_GET_REQ: {
        /* Two NUL-terminated strings: QR then manual code. */
        static const char qr[] = "MT:MOCK.AKIRA000MATTER01";
        static const char manual[] = "3497-011-2332";
        uint8_t extra[sizeof(qr) + sizeof(manual)];
        memcpy(extra, qr, sizeof(qr));                 /* includes NUL */
        memcpy(extra + sizeof(qr), manual, sizeof(manual));
        emit_response(cmd, seq, 0, extra, sizeof(extra));
        break;
    }

    default:
        LOG_WRN("mock: unknown request cmd 0x%02x", cmd);
        emit_response(cmd, seq, -3 /*invalid*/, NULL, 0);
        break;
    }
}

/* --------------------------------------------------------------------------
 * Inbound request parser (mirrors the akira_matter_ipc RX state machine)
 * ------------------------------------------------------------------------- */
static struct {
    enum {
        M_SYNC0, M_SYNC1, M_CMD, M_SEQ, M_LEN_H, M_LEN_L,
        M_PAYLOAD, M_CRC_H, M_CRC_L,
    } state;
    uint8_t  cmd, seq;
    uint16_t len, received, crc_recv;
    uint8_t  hdr[AKIRA_MATTER_IPC_FRAME_HDR_LEN];
    uint8_t  payload[AKIRA_MATTER_IPC_MAX_PAYLOAD];
} s_rx;

void matter_coproc_mock_rx_byte(uint8_t b)
{
    switch (s_rx.state) {
    case M_SYNC0:
        if (b == AKIRA_MATTER_IPC_SYNC0) { s_rx.state = M_SYNC1; }
        break;
    case M_SYNC1:
        s_rx.state = (b == AKIRA_MATTER_IPC_SYNC1) ? M_CMD : M_SYNC0;
        break;
    case M_CMD:
        s_rx.cmd = b;
        s_rx.hdr[0] = AKIRA_MATTER_IPC_SYNC0;
        s_rx.hdr[1] = AKIRA_MATTER_IPC_SYNC1;
        s_rx.hdr[2] = b;
        s_rx.state = M_SEQ;
        break;
    case M_SEQ:
        s_rx.seq = b;
        s_rx.hdr[3] = b;
        s_rx.state = M_LEN_H;
        break;
    case M_LEN_H:
        s_rx.len = (uint16_t)b << 8;
        s_rx.hdr[4] = b;
        s_rx.state = M_LEN_L;
        break;
    case M_LEN_L:
        s_rx.len |= b;
        s_rx.hdr[5] = b;
        s_rx.received = 0;
        if (s_rx.len > AKIRA_MATTER_IPC_MAX_PAYLOAD) {
            s_rx.state = M_SYNC0;
        } else {
            s_rx.state = (s_rx.len > 0) ? M_PAYLOAD : M_CRC_H;
        }
        break;
    case M_PAYLOAD:
        s_rx.payload[s_rx.received++] = b;
        if (s_rx.received >= s_rx.len) { s_rx.state = M_CRC_H; }
        break;
    case M_CRC_H:
        s_rx.crc_recv = (uint16_t)b << 8;
        s_rx.state = M_CRC_L;
        break;
    case M_CRC_L: {
        s_rx.crc_recv |= b;
        uint16_t calc = frame_crc(s_rx.hdr, sizeof(s_rx.hdr),
                                  s_rx.payload, s_rx.len);
        if (calc == s_rx.crc_recv) {
            handle_request(s_rx.cmd, s_rx.seq, s_rx.payload, s_rx.len);
        } else {
            LOG_WRN("mock: CRC mismatch (got 0x%04x want 0x%04x)",
                    s_rx.crc_recv, calc);
        }
        s_rx.state = M_SYNC0;
        break;
    }
    default:
        s_rx.state = M_SYNC0;
        break;
    }
}

int matter_coproc_mock_tx_byte(uint8_t *out)
{
    int rc = -1;
    k_mutex_lock(&s_out_mutex, K_FOREVER);
    if (ring_buf_get(&s_out_ring, out, 1) == 1) {
        rc = 0;
    }
    k_mutex_unlock(&s_out_mutex);
    return rc;
}

void matter_coproc_mock_inject_command(uint8_t endpoint, uint32_t cluster,
                                       uint32_t cmd, const uint8_t *val,
                                       uint16_t len)
{
    /* ACC_CMD_EVENT payload: [ep:1][cluster:4 BE][cmd:4 BE][value:N] */
    uint8_t payload[9 + AKIRA_MATTER_IPC_MAX_PAYLOAD];
    payload[0] = endpoint;
    payload[1] = (uint8_t)(cluster >> 24);
    payload[2] = (uint8_t)(cluster >> 16);
    payload[3] = (uint8_t)(cluster >> 8);
    payload[4] = (uint8_t)(cluster & 0xFF);
    payload[5] = (uint8_t)(cmd >> 24);
    payload[6] = (uint8_t)(cmd >> 16);
    payload[7] = (uint8_t)(cmd >> 8);
    payload[8] = (uint8_t)(cmd & 0xFF);
    uint16_t total = 9;
    if (val && len > 0) {
        len = MIN(len, (uint16_t)AKIRA_MATTER_IPC_MAX_PAYLOAD);
        memcpy(&payload[9], val, len);
        total += len;
    }
    /* seq is irrelevant for unsolicited events. */
    emit_frame(AKIRA_MATTER_CMD_ACC_CMD_EVENT, 0, payload, total);
}
