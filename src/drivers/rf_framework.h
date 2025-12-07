/**
 * @file rf_framework.h
 * @brief AkiraOS Unified RF Driver Framework
 */

#ifndef AKIRA_RF_FRAMEWORK_H
#define AKIRA_RF_FRAMEWORK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief RF chip types
     */
    typedef enum
    {
        RF_CHIP_NONE = 0,
        RF_CHIP_NRF24L01,
        RF_CHIP_LR1121,
        RF_CHIP_CC1101,
        RF_CHIP_SX1276,
        RF_CHIP_RFM69,
        RF_CHIP_MAX
    } rf_chip_t;

    /**
     * @brief RF modulation types
     */
    typedef enum
    {
        RF_MOD_GFSK = 0,
        RF_MOD_FSK,
        RF_MOD_OOK,
        RF_MOD_MSK,
        RF_MOD_LORA
    } rf_modulation_t;

    /**
     * @brief RF mode
     */
    typedef enum
    {
        RF_MODE_SLEEP = 0,
        RF_MODE_STANDBY,
        RF_MODE_RX,
        RF_MODE_TX
    } rf_mode_t;

    /**
     * @brief RF packet received callback
     */
    typedef void (*rf_rx_callback_t)(const uint8_t *data, size_t len, int16_t rssi);

    /**
     * @brief RF driver interface
     */
    struct akira_rf_driver
    {
        const char *name;
        rf_chip_t type;

        /* Driver operations */
        int (*init)(void);
        int (*deinit)(void);
        int (*set_mode)(rf_mode_t mode);
        int (*set_frequency)(uint32_t freq_hz);
        int (*set_power)(int8_t dbm);
        int (*set_modulation)(rf_modulation_t mod);
        int (*set_bitrate)(uint32_t bps);
        int (*tx)(const uint8_t *data, size_t len);
        int (*rx)(uint8_t *buffer, size_t max_len, uint32_t timeout_ms);
        int (*get_rssi)(int16_t *rssi);
        void (*set_rx_callback)(rf_rx_callback_t callback);

        /* LoRa-specific (optional) */
        int (*set_spreading_factor)(uint8_t sf);
        int (*set_bandwidth)(uint32_t bw_hz);
        int (*set_coding_rate)(uint8_t cr);
    };

    /**
     * @brief Register RF driver
     * @param driver Driver interface
     * @return 0 on success
     */
    int rf_framework_register(const struct akira_rf_driver *driver);

    /**
     * @brief Get driver for chip type
     * @param chip Chip type
     * @return Driver pointer, or NULL if not registered
     */
    const struct akira_rf_driver *rf_framework_get_driver(rf_chip_t chip);

    /**
     * @brief Initialize RF framework
     * @return 0 on success
     */
    int rf_framework_init(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_RF_FRAMEWORK_H */
