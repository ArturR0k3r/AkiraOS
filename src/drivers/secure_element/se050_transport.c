/**
 * @file se050_transport.c
 * @brief NXP SE050 T=1' (T=1 over I2C) block-protocol link layer.
 *
 * See se050_transport.h for the frame layout and scope caveats.
 */

#include "se050_transport.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(se050_link, CONFIG_I2C_LOG_LEVEL);

/* Node address bytes */
#define NAD_H2SE            0x5A  /* host  -> secure element */
#define NAD_SE2H           0xA5  /* secure element -> host */

/* PCB (protocol control byte) encodings */
#define PCB_I_BLOCK        0x00  /* bit7=0                              */
#define PCB_I_SEQ          0x40  /* I-block N(S) sequence bit           */
#define PCB_I_MORE         0x20  /* I-block "more data" (chaining) bit  */
#define PCB_R_BLOCK        0x80  /* bit7=1,bit6=0                       */
#define PCB_R_SEQ          0x10  /* R-block N(R) sequence bit           */
#define PCB_R_ERR_MASK     0x03  /* R-block error code                  */
#define PCB_S_BLOCK        0xC0  /* bit7=1,bit6=1                       */

/* S-block request/response codes (low bits of PCB) */
#define S_RESYNCH_REQ      0xC0
#define S_RESYNCH_RESP     0xE0
#define S_IFS_REQ          0xC1
#define S_IFS_RESP         0xE1
#define S_ABORT_REQ        0xC2
#define S_ABORT_RESP       0xE2
#define S_WTX_REQ          0xC3
#define S_WTX_RESP         0xE3
#define S_SOFT_RESET_REQ   0xC4  /* interface soft reset -> ATR in resp */
#define S_SOFT_RESET_RESP  0xE4

/* Timing / retry budget (no HW spec value tuning done — conservative) */
#define BOOT_DELAY_MS      3     /* ENA high -> die ready */
#define POLL_DELAY_MS      1
#define READ_RETRIES       200   /* ~ up to READ_RETRIES * POLL_DELAY_MS */
#define WTX_MAX            10     /* max consecutive WTX grants honoured */

/* =========================================================================
 * CRC-16/X-25 (poly 0x1021 reflected, init 0xFFFF, xorout 0xFFFF) — the
 * epilogue checksum used by GP T=1'. Transmitted LSB first.
 * ========================================================================= */
static uint16_t crc16_x25(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : (crc >> 1);
        }
    }
    return crc ^ 0xFFFF;
}

/* =========================================================================
 * Raw frame TX/RX over I2C
 * ========================================================================= */

/* Build NAD|PCB|LEN|INF|CRC into @out and write it as one I2C transaction. */
static int frame_send(struct se050_transport *t, uint8_t pcb,
                      const uint8_t *inf, size_t inf_len)
{
    uint8_t out[SE050_MAX_FRAME];

    if (inf_len > SE050_MAX_INF) {
        return -EINVAL;
    }

    out[0] = NAD_H2SE;
    out[1] = pcb;
    out[2] = (uint8_t)inf_len;
    if (inf_len && inf) {
        memcpy(&out[3], inf, inf_len);
    }
    uint16_t crc = crc16_x25(out, 3 + inf_len);
    out[3 + inf_len]     = crc & 0xFF;        /* LSB first */
    out[3 + inf_len + 1] = (crc >> 8) & 0xFF;

    /* The SE may NACK its address while busy; retry the write briefly. */
    int ret = -EIO;
    for (int i = 0; i < READ_RETRIES; i++) {
        ret = i2c_write_dt(&t->i2c, out, 3 + inf_len + 2);
        if (ret == 0) {
            return 0;
        }
        k_msleep(POLL_DELAY_MS);
    }
    LOG_ERR("frame_send failed: %d", ret);
    return ret;
}

/* Read one full frame. Returns PCB in @pcb, INF into @inf (<= inf_cap). */
static int frame_recv(struct se050_transport *t, uint8_t *pcb,
                      uint8_t *inf, size_t inf_cap, size_t *inf_len)
{
    uint8_t buf[SE050_MAX_FRAME];

    /* Poll the read until the SE returns a well-formed frame. While the SE
     * is still processing it NACKs (i2c_read fails) or returns a header of
     * all-0xFF / all-0x00 padding — both are retried. */
    for (int i = 0; i < READ_RETRIES; i++) {
        int ret = i2c_read_dt(&t->i2c, buf, sizeof(buf));
        if (ret != 0) {
            k_msleep(POLL_DELAY_MS);
            continue;
        }
        if (buf[0] != NAD_SE2H) {
            /* padding / not-ready */
            k_msleep(POLL_DELAY_MS);
            continue;
        }

        size_t len = buf[2];
        if (len > SE050_MAX_INF || (3 + len + 2) > sizeof(buf)) {
            return -EMSGSIZE;
        }
        uint16_t rx_crc = buf[3 + len] | ((uint16_t)buf[3 + len + 1] << 8);
        if (rx_crc != crc16_x25(buf, 3 + len)) {
            LOG_WRN("SE050 frame CRC mismatch");
            return -EBADMSG;
        }

        *pcb = buf[1];
        if (inf) {
            if (len > inf_cap) {
                return -ENOSPC;
            }
            memcpy(inf, &buf[3], len);
        }
        if (inf_len) {
            *inf_len = len;
        }
        return 0;
    }
    return -ETIMEDOUT;
}

/* =========================================================================
 * ENA power control
 * ========================================================================= */
int se050_transport_configure(struct se050_transport *t,
                              const struct i2c_dt_spec *i2c,
                              const struct gpio_dt_spec *ena)
{
    memset(t, 0, sizeof(*t));
    t->i2c = *i2c;
    if (ena && ena->port) {
        t->ena = *ena;
    }
    if (!device_is_ready(t->i2c.bus)) {
        return -ENODEV;
    }
    if (t->ena.port) {
        if (!gpio_is_ready_dt(&t->ena)) {
            return -ENODEV;
        }
        /* Start powered-down; power_on() drives it high. */
        int ret = gpio_pin_configure_dt(&t->ena, GPIO_OUTPUT_INACTIVE);
        if (ret < 0) {
            return ret;
        }
    }
    return 0;
}

int se050_transport_power_on(struct se050_transport *t)
{
    if (t->powered) {
        return 0;
    }
    if (t->ena.port) {
        int ret = gpio_pin_set_dt(&t->ena, 1);
        if (ret < 0) {
            return ret;
        }
    }
    t->powered = true;
    k_msleep(BOOT_DELAY_MS);
    return 0;
}

int se050_transport_power_off(struct se050_transport *t)
{
    if (!t->powered) {
        return 0;
    }
    if (t->ena.port) {
        int ret = gpio_pin_set_dt(&t->ena, 0);
        if (ret < 0) {
            return ret;
        }
    }
    t->powered = false;
    return 0;
}

bool se050_transport_is_powered(const struct se050_transport *t)
{
    return t->powered;
}

/* =========================================================================
 * Interface soft reset — fetch ATR, reset sequence bits
 * ========================================================================= */
int se050_transport_reset(struct se050_transport *t)
{
    int ret = frame_send(t, S_SOFT_RESET_REQ, NULL, 0);
    if (ret < 0) {
        return ret;
    }

    uint8_t pcb = 0;
    size_t atr_len = 0;
    ret = frame_recv(t, &pcb, t->atr, sizeof(t->atr), &atr_len);
    if (ret < 0) {
        return ret;
    }
    if (pcb != S_SOFT_RESET_RESP) {
        LOG_ERR("SE050 soft-reset: unexpected PCB 0x%02X", pcb);
        return -EPROTO;
    }

    t->atr_len = atr_len;
    t->seq_tx = 0;
    t->seq_rx = 0;
    LOG_INF("SE050 ATR: %u bytes", (unsigned)atr_len);
    return 0;
}

/* =========================================================================
 * APDU exchange (I-block send, handle R/S-blocks, I-block receive)
 * ========================================================================= */
int se050_transport_apdu(struct se050_transport *t,
                        const uint8_t *tx, size_t tx_len,
                        uint8_t *rx, size_t rx_cap, size_t *rx_len)
{
    if (!t->powered) {
        return -EIO;
    }
    if (tx_len > SE050_MAX_INF) {
        /* APDU chaining across multiple I-blocks not implemented — our
         * commands (random, ECDSA, 32-byte objects) fit one block. */
        return -ENOTSUP;
    }

    /* Send the command APDU as a single (non-chained) I-block. */
    uint8_t pcb_tx = PCB_I_BLOCK | (t->seq_tx ? PCB_I_SEQ : 0);
    int ret = frame_send(t, pcb_tx, tx, tx_len);
    if (ret < 0) {
        return ret;
    }
    t->seq_tx ^= 1;

    /* Collect the response, honouring WTX (time-extension) S-blocks and
     * reassembling any SE->host I-block chaining. */
    size_t total = 0;
    int wtx_grants = 0;

    for (;;) {
        uint8_t pcb = 0;
        uint8_t inf[SE050_MAX_INF];
        size_t inf_len = 0;

        ret = frame_recv(t, &pcb, inf, sizeof(inf), &inf_len);
        if (ret < 0) {
            return ret;
        }

        if ((pcb & 0xC0) == PCB_S_BLOCK) {
            if (pcb == S_WTX_REQ) {
                if (++wtx_grants > WTX_MAX) {
                    return -ETIMEDOUT;
                }
                /* Grant the wait-time extension (echo INF back as response). */
                ret = frame_send(t, S_WTX_RESP, inf, inf_len);
                if (ret < 0) {
                    return ret;
                }
                continue;
            }
            LOG_ERR("SE050 unexpected S-block 0x%02X", pcb);
            return -EPROTO;
        }

        if ((pcb & 0x80) == PCB_R_BLOCK) {
            /* SE requested retransmission — not expected for our single
             * short block; treat as protocol error. */
            LOG_ERR("SE050 unexpected R-block 0x%02X", pcb);
            return -EPROTO;
        }

        /* I-block: append payload. */
        if (total + inf_len > rx_cap) {
            return -ENOSPC;
        }
        memcpy(rx + total, inf, inf_len);
        total += inf_len;
        t->seq_rx ^= 1;

        if (pcb & PCB_I_MORE) {
            /* Acknowledge the chunk with an R-block and keep reading. */
            uint8_t r = PCB_R_BLOCK | (t->seq_rx ? PCB_R_SEQ : 0);
            ret = frame_send(t, r, NULL, 0);
            if (ret < 0) {
                return ret;
            }
            continue;
        }
        break; /* last (or only) I-block */
    }

    if (rx_len) {
        *rx_len = total;
    }
    return 0;
}
