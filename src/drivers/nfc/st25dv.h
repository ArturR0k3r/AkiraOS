/**
 * @file st25dv.h
 * @brief ST25DV64K Dynamic NFC/RFID Tag driver (STMicroelectronics)
 *
 * The ST25DV64K is a 64-Kbit EEPROM with a dual interface:
 *   - I2C (addr 0x53 / 0x57) for MCU access to user and system memory
 *   - ISO 15693 NFC RF interface for contactless read/write
 *
 * This driver handles the I2C side only.  RF operations happen
 * autonomously when an NFC reader comes within range.
 *
 * I2C addresses (7-bit):
 *   0x53 — user memory, FTM (Fast Transfer Mode), NDEF mailbox
 *   0x57 — system configuration registers (GPO, EH, RF, lock bits)
 *
 * GPO interrupt → NFC_INT → GPIO21 (fires on RF field detect, write
 * completion, or mailbox full/empty, depending on GPO_CTRL register).
 * @stability experimental
 * @since 1.5
 */

#ifndef AKIRA_ST25DV_H
#define AKIRA_ST25DV_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief GPO interrupt source selection (GPO_CTRL_Dyn register bits).
 * OR together to enable multiple sources.
 */
#define ST25DV_GPO_RFUSERWRITE    BIT(0)  /* RF write to user memory */
#define ST25DV_GPO_RFACTIVITY     BIT(1)  /* Any RF activity */
#define ST25DV_GPO_RFINTERRUPT    BIT(2)  /* RF interrupt command */
#define ST25DV_GPO_FIELDRISING    BIT(3)  /* RF field appears */
#define ST25DV_GPO_FIELDFALLING   BIT(4)  /* RF field disappears */
#define ST25DV_GPO_RFPUTMSG       BIT(5)  /* RF writes FTM mailbox */
#define ST25DV_GPO_RFGETMSG       BIT(6)  /* RF reads FTM mailbox */
#define ST25DV_GPO_ENABLE         BIT(7)  /* Master GPO enable */

/** @brief Maximum EEPROM read/write chunk (I2C buffer limited). */
#define ST25DV_MAX_XFER_LEN  256

/** @brief Maximum FTM mailbox message length (bytes). */
#define ST25DV_MAILBOX_MAX_LEN  256

/**
 * @brief Read bytes from user EEPROM.
 *
 * @param dev   Driver device pointer.
 * @param addr  Byte address within user memory (0x0000 – 0x1FFF for 64 Kbit).
 * @param buf   Destination buffer.
 * @param len   Number of bytes to read.
 * @return 0 on success, negative errno on failure.
 */
int st25dv_read_user_mem(const struct device *dev,
                          uint16_t addr, uint8_t *buf, size_t len);

/**
 * @brief Write bytes to user EEPROM (page-aligned internally).
 *
 * @param dev   Driver device pointer.
 * @param addr  Byte address within user memory.
 * @param buf   Source buffer.
 * @param len   Number of bytes to write.
 * @return 0 on success, negative errno on failure.
 */
int st25dv_write_user_mem(const struct device *dev,
                           uint16_t addr, const uint8_t *buf, size_t len);

/**
 * @brief Read a system configuration register (addr in 0x0000–0x00FF range).
 */
int st25dv_read_sys_reg(const struct device *dev, uint16_t addr, uint8_t *val);

/**
 * @brief Write a system configuration register.
 * Some registers require I2C security session (write password) first.
 */
int st25dv_write_sys_reg(const struct device *dev, uint16_t addr, uint8_t val);

/**
 * @brief Configure GPO interrupt sources.
 *
 * @param dev     Driver device pointer.
 * @param sources Bitmask of ST25DV_GPO_* flags.  Pass 0 to disable GPO.
 * @return 0 on success, negative errno on failure.
 */
int st25dv_set_gpo(const struct device *dev, uint8_t sources);

/**
 * @brief Check if RF field is currently present.
 */
int st25dv_rf_field_present(const struct device *dev, bool *present);

/**
 * @brief Read the 8-byte ISO 15693 UID of this tag.
 *
 * @param dev Driver device pointer.
 * @param uid 8-byte buffer for the UID (UID[0] = MSB).
 * @return 0 on success, negative errno on failure.
 */
int st25dv_read_uid(const struct device *dev, uint8_t uid[8]);

/**
 * @brief Present the I2C security session password to unlock write-protected
 *        system registers.  Default password is all zeros.
 *
 * @param dev      Driver device pointer.
 * @param password 8-byte password.
 * @return 0 on success, -EACCES if password rejected, negative errno otherwise.
 */
int st25dv_open_i2c_session(const struct device *dev, const uint8_t password[8]);

/**
 * @brief Enable or disable Fast Transfer Mode (FTM) — the mailbox that lets
 *        an RF reader and this I2C host exchange dynamic messages.
 *
 * Requires an open I2C security session (see st25dv_open_i2c_session) —
 * the FTM authorization register is write-protected like other static regs.
 *
 * @param dev    Driver device pointer.
 * @param enable true to enable FTM, false to disable.
 * @param wdg    Watchdog setting 0-7: duration = 2^(wdg-1) x 30ms, 0 = infinite.
 *               Ignored when disabling.
 * @return 0 on success, negative errno on failure.
 */
int st25dv_mailbox_enable(const struct device *dev, bool enable, uint8_t wdg);

/**
 * @brief Put a message in the FTM mailbox for an RF reader to read.
 *
 * @param dev Driver device pointer.
 * @param buf Source buffer.
 * @param len Message length, 1-256 bytes.
 * @return 0 on success, negative errno on failure.
 */
int st25dv_mailbox_put_msg(const struct device *dev, const uint8_t *buf, size_t len);

/**
 * @brief Get the message an RF reader put in the FTM mailbox.
 *
 * @param dev     Driver device pointer.
 * @param buf     Destination buffer.
 * @param cap     Destination buffer capacity.
 * @param len_out Set to the number of bytes actually read.
 * @return 0 on success, negative errno on failure.
 */
int st25dv_mailbox_get_msg(const struct device *dev, uint8_t *buf, size_t cap, size_t *len_out);

/**
 * @brief Query FTM mailbox control/status bits and current message length.
 *
 * @param dev     Driver device pointer.
 * @param ctrl    Set to the raw MB_CTRL_Dyn register value.
 * @param msg_len Set to the current message length in the mailbox.
 * @return 0 on success, negative errno on failure.
 */
int st25dv_mailbox_status(const struct device *dev, uint8_t *ctrl, size_t *msg_len);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_ST25DV_H */
