/**
 * @file akira_matter_ipc.c
 * @brief Matter co-processor IPC transport — UART1 framing
 *
 * TX path: builds a frame, sends it over UART1, waits on a semaphore for the
 * matching response (matched by sequence number).
 *
 * RX path: a dedicated k_thread drains UART1 byte-by-byte using a state
 * machine, validates the CRC-16, and either posts the response to the
 * per-sequence semaphore or queues an async EVENT into g_event_msgq.
 */

#include "akira_matter_ipc.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>
#include <string.h>

LOG_MODULE_REGISTER(akira_matter_ipc, CONFIG_AKIRA_LOG_LEVEL);

/* -------------------------------------------------------------------------
 * Device binding — resolved at init from Kconfig alias
 * ---------------------------------------------------------------------- */
#define MATTER_COPROC_UART DEVICE_DT_GET(DT_ALIAS(matter_coproc_uart))

/* -------------------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------------- */
#define RX_THREAD_STACK_SIZE 1024
#define RX_THREAD_PRIORITY   7
#define MAX_INFLIGHT         4   /* max concurrent in-flight requests */
#define EVENT_QUEUE_DEPTH    8

K_THREAD_STACK_DEFINE(s_rx_stack, RX_THREAD_STACK_SIZE);
static struct k_thread s_rx_thread;

/* Per-sequence-number response slot */
typedef struct {
    uint8_t  seq;
    bool     used;
    int      status;
    uint8_t  payload[AKIRA_MATTER_IPC_MAX_PAYLOAD];
    uint16_t payload_len;
    struct k_sem sem;
} inflight_t;

static inflight_t s_inflight[MAX_INFLIGHT];
static K_MUTEX_DEFINE(s_tx_mutex);
static K_MUTEX_DEFINE(s_inflight_mutex);
static atomic_t s_seq_counter;

/* Event queue for async EVENT frames */
static struct akira_matter_event s_event_buf[EVENT_QUEUE_DEPTH];
static struct k_msgq s_event_msgq;

static bool s_initialised;

/* -------------------------------------------------------------------------
 * CRC-16/IBM  (poly 0x8005, init 0xFFFF, reflect in+out, xorout 0xFFFF)
 * Zephyr provides crc16_reflect() which matches CRC-16/IBM.
 * ---------------------------------------------------------------------- */
static uint16_t frame_crc(const uint8_t *hdr, uint8_t hdr_len,
                           const uint8_t *payload, uint16_t pay_len)
{
    uint16_t crc = crc16_reflect(0x8005, 0xFFFF, hdr, hdr_len);
    if (pay_len > 0) {
        crc = crc16_reflect(0x8005, crc, payload, pay_len);
    }
    return crc ^ 0xFFFF;
}

/* -------------------------------------------------------------------------
 * TX helpers
 * ---------------------------------------------------------------------- */
static void uart_write_byte(const struct device *dev, uint8_t b)
{
    uart_poll_out(dev, b);
}

static void uart_write_buf(const struct device *dev,
                           const uint8_t *buf, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        uart_poll_out(dev, buf[i]);
    }
}

static uint8_t alloc_seq(void)
{
    return (uint8_t)atomic_inc(&s_seq_counter);
}

static inflight_t *alloc_inflight(uint8_t seq)
{
    k_mutex_lock(&s_inflight_mutex, K_FOREVER);
    for (int i = 0; i < MAX_INFLIGHT; i++) {
        if (!s_inflight[i].used) {
            s_inflight[i].used    = true;
            s_inflight[i].seq     = seq;
            s_inflight[i].status  = -EIO;
            s_inflight[i].payload_len = 0;
            k_sem_init(&s_inflight[i].sem, 0, 1);
            k_mutex_unlock(&s_inflight_mutex);
            return &s_inflight[i];
        }
    }
    k_mutex_unlock(&s_inflight_mutex);
    return NULL;
}

static void free_inflight(inflight_t *slot)
{
    k_mutex_lock(&s_inflight_mutex, K_FOREVER);
    slot->used = false;
    k_mutex_unlock(&s_inflight_mutex);
}

static inflight_t *find_inflight(uint8_t seq)
{
    for (int i = 0; i < MAX_INFLIGHT; i++) {
        if (s_inflight[i].used && s_inflight[i].seq == seq) {
            return &s_inflight[i];
        }
    }
    return NULL;
}

/**
 * Send a frame and block until the matching response arrives or timeout.
 * @return 0 on success, negative errno on failure.
 */
static int send_and_wait(uint8_t cmd, uint8_t seq,
                         const uint8_t *payload, uint16_t payload_len,
                         uint8_t *resp_payload, uint16_t *resp_len)
{
    const struct device *dev = MATTER_COPROC_UART;
    uint8_t hdr[AKIRA_MATTER_IPC_FRAME_HDR_LEN];
    uint16_t crc;

    hdr[0] = AKIRA_MATTER_IPC_SYNC0;
    hdr[1] = AKIRA_MATTER_IPC_SYNC1;
    hdr[2] = cmd;
    hdr[3] = seq;
    hdr[4] = (uint8_t)(payload_len >> 8);
    hdr[5] = (uint8_t)(payload_len & 0xFF);

    crc = frame_crc(hdr, sizeof(hdr), payload, payload_len);

    k_mutex_lock(&s_tx_mutex, K_FOREVER);
    uart_write_buf(dev, hdr, sizeof(hdr));
    if (payload_len > 0) {
        uart_write_buf(dev, payload, payload_len);
    }
    uart_write_byte(dev, (uint8_t)(crc >> 8));
    uart_write_byte(dev, (uint8_t)(crc & 0xFF));
    k_mutex_unlock(&s_tx_mutex);

    /* Wait for response */
    inflight_t *slot = find_inflight(seq);
    if (!slot) {
        return -ENOMEM;
    }

    int rc = k_sem_take(&slot->sem,
                        K_MSEC(AKIRA_MATTER_IPC_TIMEOUT_MS));
    if (rc == -EAGAIN) {
        LOG_WRN("matter ipc: timeout seq=0x%02x cmd=0x%02x", seq, cmd);
        free_inflight(slot);
        return -ETIMEDOUT;
    }

    int status = slot->status;
    if (resp_payload && resp_len && slot->payload_len > 0) {
        uint16_t copy = MIN(slot->payload_len, *resp_len);
        memcpy(resp_payload, slot->payload, copy);
        *resp_len = copy;
    }
    free_inflight(slot);
    return status;
}

/* -------------------------------------------------------------------------
 * RX state machine
 * ---------------------------------------------------------------------- */
typedef enum {
    RX_SYNC0,
    RX_SYNC1,
    RX_CMD,
    RX_SEQ,
    RX_LEN_H,
    RX_LEN_L,
    RX_PAYLOAD,
    RX_CRC_H,
    RX_CRC_L,
} rx_state_t;

static void rx_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    const struct device *dev = MATTER_COPROC_UART;
    rx_state_t state = RX_SYNC0;
    uint8_t  rx_cmd, rx_seq;
    uint16_t rx_len, rx_received;
    uint8_t  rx_payload[AKIRA_MATTER_IPC_MAX_PAYLOAD];
    uint16_t rx_crc_recv;
    uint8_t  rx_hdr[AKIRA_MATTER_IPC_FRAME_HDR_LEN];
    uint8_t  b;

    while (true) {
        while (uart_poll_in(dev, &b) != 0) {
            k_sleep(K_USEC(200));
        }

        switch (state) {
        case RX_SYNC0:
            if (b == AKIRA_MATTER_IPC_SYNC0) { state = RX_SYNC1; }
            break;
        case RX_SYNC1:
            state = (b == AKIRA_MATTER_IPC_SYNC1) ? RX_CMD : RX_SYNC0;
            break;
        case RX_CMD:
            rx_cmd = b;
            rx_hdr[0] = AKIRA_MATTER_IPC_SYNC0;
            rx_hdr[1] = AKIRA_MATTER_IPC_SYNC1;
            rx_hdr[2] = b;
            state = RX_SEQ;
            break;
        case RX_SEQ:
            rx_seq = b;
            rx_hdr[3] = b;
            state = RX_LEN_H;
            break;
        case RX_LEN_H:
            rx_len = (uint16_t)b << 8;
            rx_hdr[4] = b;
            state = RX_LEN_L;
            break;
        case RX_LEN_L:
            rx_len |= b;
            rx_hdr[5] = b;
            rx_received = 0;
            if (rx_len > AKIRA_MATTER_IPC_MAX_PAYLOAD) {
                LOG_ERR("matter ipc: rx frame too large (%u)", rx_len);
                state = RX_SYNC0;
            } else {
                state = (rx_len > 0) ? RX_PAYLOAD : RX_CRC_H;
            }
            break;
        case RX_PAYLOAD:
            rx_payload[rx_received++] = b;
            if (rx_received >= rx_len) { state = RX_CRC_H; }
            break;
        case RX_CRC_H:
            rx_crc_recv = (uint16_t)b << 8;
            state = RX_CRC_L;
            break;
        case RX_CRC_L: {
            rx_crc_recv |= b;
            uint16_t crc_calc = frame_crc(rx_hdr, sizeof(rx_hdr),
                                          rx_payload, rx_len);
            if (crc_calc != rx_crc_recv) {
                LOG_WRN("matter ipc: CRC mismatch (got 0x%04x, want 0x%04x)",
                        rx_crc_recv, crc_calc);
                state = RX_SYNC0;
                break;
            }

            /* Dispatch */
            if (rx_cmd == AKIRA_MATTER_CMD_EVENT) {
                /* Async event — push to event queue */
                if (rx_len >= AKIRA_MATTER_IPC_EUI64_LEN + 2) {
                    struct akira_matter_event evt;
                    memcpy(evt.src_eui64, rx_payload,
                           AKIRA_MATTER_IPC_EUI64_LEN);
                    evt.attr_id = (uint16_t)(rx_payload[8] << 8) |
                                  rx_payload[9];
                    uint16_t vlen = rx_len - AKIRA_MATTER_IPC_EUI64_LEN - 2;
                    evt.value_len = MIN(vlen,
                                       AKIRA_MATTER_IPC_MAX_PAYLOAD);
                    if (evt.value_len > 0) {
                        memcpy(evt.value, &rx_payload[10], evt.value_len);
                    }
                    if (k_msgq_put(&s_event_msgq, &evt, K_NO_WAIT) != 0) {
                        LOG_WRN("matter ipc: event queue full");
                    }
                }
            } else if (rx_cmd & 0x80) {
                /* Response — wake up waiting sender */
                inflight_t *slot = find_inflight(rx_seq);
                if (slot) {
                    /* First 4 bytes of response payload = int32 status */
                    int32_t st = 0;
                    if (rx_len >= 4) {
                        st = (int32_t)((rx_payload[0] << 24) |
                                       (rx_payload[1] << 16) |
                                       (rx_payload[2] << 8)  |
                                        rx_payload[3]);
                        if (rx_len > 4) {
                            slot->payload_len = MIN(rx_len - 4,
                                                    AKIRA_MATTER_IPC_MAX_PAYLOAD);
                            memcpy(slot->payload, &rx_payload[4],
                                   slot->payload_len);
                        }
                    }
                    slot->status = (int)st;
                    k_sem_give(&slot->sem);
                } else {
                    LOG_WRN("matter ipc: unmatched response seq=0x%02x", rx_seq);
                }
            }
            state = RX_SYNC0;
            break;
        }
        default:
            state = RX_SYNC0;
            break;
        }
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

int akira_matter_ipc_init(void)
{
    const struct device *dev = MATTER_COPROC_UART;

    if (s_initialised) {
        return 0;
    }

    if (!device_is_ready(dev)) {
        LOG_ERR("matter coproc uart not ready");
        return -ENODEV;
    }

    k_msgq_init(&s_event_msgq, (char *)s_event_buf,
                sizeof(struct akira_matter_event), EVENT_QUEUE_DEPTH);

    memset(s_inflight, 0, sizeof(s_inflight));

    k_thread_create(&s_rx_thread, s_rx_stack, RX_THREAD_STACK_SIZE,
                    rx_thread_fn, NULL, NULL, NULL,
                    RX_THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&s_rx_thread, "matter_ipc_rx");

    s_initialised = true;

    int rc = akira_matter_ipc_status();
    if (rc != 0) {
        LOG_WRN("matter coproc not responding (rc=%d); "
                "will retry on first API call", rc);
    }
    return 0;
}

int akira_matter_ipc_commission(const char *passcode,
                                uint8_t eui64_out[AKIRA_MATTER_IPC_EUI64_LEN])
{
    if (!s_initialised) { return -EAGAIN; }
    if (!passcode || strlen(passcode) == 0) { return -EINVAL; }

    uint8_t seq = alloc_seq();
    inflight_t *slot = alloc_inflight(seq);
    if (!slot) { return -ENOMEM; }

    uint8_t payload[AKIRA_MATTER_IPC_PASSCODE_LEN] = {0};
    strncpy((char *)payload, passcode, AKIRA_MATTER_IPC_PASSCODE_LEN - 1);

    uint16_t rlen = AKIRA_MATTER_IPC_EUI64_LEN;
    int rc = send_and_wait(AKIRA_MATTER_CMD_COMMISSION_REQ, seq,
                           payload, AKIRA_MATTER_IPC_PASSCODE_LEN,
                           eui64_out, &rlen);
    return rc;
}

int akira_matter_ipc_send(const uint8_t eui64[AKIRA_MATTER_IPC_EUI64_LEN],
                          const uint8_t *payload, uint16_t len)
{
    if (!s_initialised) { return -EAGAIN; }
    if (!eui64 || !payload || len == 0 ||
        len > AKIRA_MATTER_IPC_MAX_PAYLOAD - AKIRA_MATTER_IPC_EUI64_LEN) {
        return -EINVAL;
    }

    uint8_t frame[AKIRA_MATTER_IPC_EUI64_LEN + AKIRA_MATTER_IPC_MAX_PAYLOAD];
    memcpy(frame, eui64, AKIRA_MATTER_IPC_EUI64_LEN);
    memcpy(frame + AKIRA_MATTER_IPC_EUI64_LEN, payload, len);

    uint8_t seq = alloc_seq();
    inflight_t *slot = alloc_inflight(seq);
    if (!slot) { return -ENOMEM; }

    return send_and_wait(AKIRA_MATTER_CMD_SEND_REQ, seq,
                         frame, AKIRA_MATTER_IPC_EUI64_LEN + len,
                         NULL, NULL);
}

int akira_matter_ipc_subscribe(const uint8_t eui64[AKIRA_MATTER_IPC_EUI64_LEN],
                               uint16_t attr_id)
{
    if (!s_initialised) { return -EAGAIN; }
    if (!eui64) { return -EINVAL; }

    uint8_t payload[AKIRA_MATTER_IPC_EUI64_LEN + 2];
    memcpy(payload, eui64, AKIRA_MATTER_IPC_EUI64_LEN);
    payload[AKIRA_MATTER_IPC_EUI64_LEN]     = (uint8_t)(attr_id >> 8);
    payload[AKIRA_MATTER_IPC_EUI64_LEN + 1] = (uint8_t)(attr_id & 0xFF);

    uint8_t seq = alloc_seq();
    inflight_t *slot = alloc_inflight(seq);
    if (!slot) { return -ENOMEM; }

    return send_and_wait(AKIRA_MATTER_CMD_SUBSCRIBE_REQ, seq,
                         payload, sizeof(payload), NULL, NULL);
}

int akira_matter_ipc_poll(struct akira_matter_event *evt, k_timeout_t timeout)
{
    if (!s_initialised) { return -EAGAIN; }
    if (!evt) { return -EINVAL; }

    return k_msgq_get(&s_event_msgq, evt, timeout);
}

int akira_matter_ipc_status(void)
{
    if (!s_initialised) { return -EAGAIN; }

    uint8_t seq = alloc_seq();
    inflight_t *slot = alloc_inflight(seq);
    if (!slot) { return -ENOMEM; }

    return send_and_wait(AKIRA_MATTER_CMD_STATUS_REQ, seq,
                         NULL, 0, NULL, NULL);
}
