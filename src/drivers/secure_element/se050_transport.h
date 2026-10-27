/**
 * @file se050_transport.h
 * @brief NXP SE050 link layer — ISO7816 T=1' block protocol over I2C.
 *
 * The SE050 is reached exclusively over I2C using the GlobalPlatform
 * "T=1 over I2C" (a.k.a. T=1', SCI2C) block protocol. Each APDU is carried
 * in an I-block; the SE answers with an I-block (its R-APDU). R-blocks handle
 * retransmission / chaining and S-blocks handle interface control (soft reset
 * to fetch the ATR, WTX time extension, IFS negotiation).
 *
 * Frame layout (prologue|information|epilogue):
 *   NAD(1) | PCB(1) | LEN(1) | INF(0..LEN) | CRC16(2, LSB first)
 *
 * This layer also owns the SE050 ENA (enable) GPIO: driving it high powers the
 * die, low puts it in deep power-down (~40 uA). The OS gates it for deep sleep.
 *
 * NOTE: implemented from the GP T=1' spec (no NXP plug-and-trust middleware is
 * vendored). Structurally complete and compiles; live ATR/APDU exchange and
 * WTX timing require the physical part to fully validate.
 *
 * @stability experimental
 * @since 1.7
 */

#ifndef AKIRA_SE050_TRANSPORT_H
#define AKIRA_SE050_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum INF (payload) bytes carried in one block. Bounds stack frames. */
#define SE050_MAX_INF        256
/** Full frame = NAD + PCB + LEN + INF + CRC(2). */
#define SE050_MAX_FRAME      (3 + SE050_MAX_INF + 2)
/** Longest ATR the interface soft-reset can return. */
#define SE050_ATR_MAX_LEN    64

/**
 * @brief Link-layer context. Embedded in the driver's per-device data.
 */
struct se050_transport {
    struct i2c_dt_spec  i2c;        /* SE050 I2C target (addr 0x48) */
    struct gpio_dt_spec ena;        /* ENA / enable pin (active high) */
    uint8_t             seq_tx;     /* host send-sequence bit N(S) */
    uint8_t             seq_rx;     /* expected SE send-sequence bit */
    bool                powered;    /* ENA currently driven high */
    uint8_t             atr_len;    /* last ATR length (0 if none); ATR discarded */
};

/**
 * @brief Configure the ENA GPIO and I2C spec. Does not power the part.
 * @return 0 on success, negative errno otherwise.
 */
int se050_transport_configure(struct se050_transport *t,
                              const struct i2c_dt_spec *i2c,
                              const struct gpio_dt_spec *ena);

/** @brief Drive ENA high and wait for the die to boot. */
int se050_transport_power_on(struct se050_transport *t);

/** @brief Drive ENA low (deep power-down). Safe to call when already off. */
int se050_transport_power_off(struct se050_transport *t);

/** @brief True if ENA is currently driven high. */
bool se050_transport_is_powered(const struct se050_transport *t);

/**
 * @brief Interface soft reset (S-block) — resets the T=1' state machine and
 *        retrieves the ATR. Call once after power-on before the first APDU.
 * @return 0 on success, negative errno otherwise.
 */
int se050_transport_reset(struct se050_transport *t);

/**
 * @brief Exchange one APDU: send @p tx as an I-block, return the SE's I-block
 *        payload (R-APDU incl. the trailing SW1SW2) in @p rx.
 *
 * @param t       Transport context.
 * @param tx      Command APDU bytes.
 * @param tx_len  Command APDU length.
 * @param rx      Response buffer.
 * @param rx_cap  Response buffer capacity.
 * @param rx_len  Out: bytes written to @p rx.
 * @return 0 on success, negative errno on transport failure.
 */
int se050_transport_apdu(struct se050_transport *t,
                        const uint8_t *tx, size_t tx_len,
                        uint8_t *rx, size_t rx_cap, size_t *rx_len);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_SE050_TRANSPORT_H */
