/**
 * @file rf_framework.h
 * @brief Unified RF Driver Framework for AkiraOS
 * 
 * Provides a common interface for different RF transceiver chips (NRF24L01, CC1121, LR2021)
 * allowing applications and WASM modules to interact with any RF chip through a consistent API.
 * @stability experimental
 * @since 1.4
 */

#ifndef AKIRA_RF_FRAMEWORK_H
#define AKIRA_RF_FRAMEWORK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Supported RF chip types
 */
typedef enum {
    RF_CHIP_NONE = 0,      /**< No chip / uninitialized */
    RF_CHIP_NRF24L01 = 1,  /**< Nordic nRF24L01+ 2.4GHz transceiver */
    RF_CHIP_CC1121 = 2,    /**< TI CC1121 Sub-GHz transceiver */
    RF_CHIP_LR2021 = 3,    /**< Semtech LR2021 LoRa Plus transceiver */
} rf_chip_type_t;

/**
 * @brief RF operating modes
 */
typedef enum {
    RF_MODE_SLEEP,     /**< Low power sleep mode */
    RF_MODE_STANDBY,   /**< Standby mode (ready to TX/RX) */
    RF_MODE_RX,        /**< Receive mode */
    RF_MODE_TX,        /**< Transmit mode */
} rf_mode_t;

/**
 * @brief RF modulation types
 */
typedef enum {
    RF_MOD_FSK,    /**< Frequency Shift Keying */
    RF_MOD_GFSK,   /**< Gaussian FSK */
    RF_MOD_OOK,    /**< On-Off Keying */
    RF_MOD_MSK,    /**< Minimum Shift Keying */
    RF_MOD_LORA,   /**< LoRa spread spectrum */
} rf_modulation_t;

/**
 * @brief RX callback function signature
 * 
 * Called when a packet is received (interrupt-driven mode)
 * 
 * @param data Pointer to received data buffer
 * @param len Length of received data
 * @param rssi Received Signal Strength Indicator (dBm)
 */
typedef void (*rf_rx_callback_t)(const uint8_t *data, size_t len, int16_t rssi);

/**
 * @brief Unified RF driver interface
 * 
 * All RF chip drivers must implement this interface. Optional functions
 * (e.g., LoRa-specific) can be NULL if not supported.
 */
struct akira_rf_driver {
    const char *name;           /**< Driver name (e.g., "LR2021") */
    rf_chip_type_t type;        /**< Chip type identifier */
    
    /* === Core Operations (REQUIRED) === */
    
    /**
     * @brief Initialize the RF driver
     * @return 0 on success, negative errno on failure
     */
    int (*init)(void);
    
    /**
     * @brief Deinitialize the RF driver
     * @return 0 on success, negative errno on failure
     */
    int (*deinit)(void);
    
    /**
     * @brief Set RF operating mode
     * @param mode Target operating mode
     * @return 0 on success, negative errno on failure
     */
    int (*set_mode)(rf_mode_t mode);
    
    /**
     * @brief Set RF frequency
     * @param freq_hz Frequency in Hz
     * @return 0 on success, negative errno on failure
     */
    int (*set_frequency)(uint32_t freq_hz);
    
    /**
     * @brief Set TX power level
     * @param dbm Power in dBm
     * @return 0 on success, negative errno on failure
     */
    int (*set_power)(int8_t dbm);
    
    /**
     * @brief Set modulation type
     * @param mod Modulation type
     * @return 0 on success, negative errno on failure
     */
    int (*set_modulation)(rf_modulation_t mod);
    
    /**
     * @brief Set bitrate (for FSK/GFSK modes)
     * @param bps Bitrate in bits per second
     * @return 0 on success, negative errno on failure
     */
    int (*set_bitrate)(uint32_t bps);
    
    /**
     * @brief Transmit data packet
     * @param data Pointer to data buffer
     * @param len Length of data to transmit
     * @return 0 on success, negative errno on failure
     */
    int (*tx)(const uint8_t *data, size_t len);
    
    /**
     * @brief Receive data packet (blocking with timeout)
     * @param buffer Buffer to store received data
     * @param max_len Maximum buffer size
     * @param timeout_ms Timeout in milliseconds (0 = no wait)
     * @return Number of bytes received, or negative errno on error
     */
    int (*rx)(uint8_t *buffer, size_t max_len, uint32_t timeout_ms);
    
    /**
     * @brief Get current RSSI value
     * @param rssi Pointer to store RSSI value (dBm)
     * @return 0 on success, negative errno on failure
     */
    int (*get_rssi)(int16_t *rssi);
    
    /**
     * @brief Set RX callback for interrupt-driven operation
     * @param callback Callback function pointer (NULL to disable)
     */
    void (*set_rx_callback)(rf_rx_callback_t callback);
    
    /* === LoRa-Specific Operations (OPTIONAL) === */
    
    /**
     * @brief Set LoRa spreading factor
     * @param sf Spreading factor (5-12)
     * @return 0 on success, negative errno on failure
     */
    int (*set_spreading_factor)(uint8_t sf);
    
    /**
     * @brief Set LoRa bandwidth
     * @param bw_hz Bandwidth in Hz (e.g., 125000, 250000, 500000)
     * @return 0 on success, negative errno on failure
     */
    int (*set_bandwidth)(uint32_t bw_hz);
    
    /**
     * @brief Set LoRa coding rate
     * @param cr Coding rate denominator (5-8 for 4/5 to 4/8)
     * @return 0 on success, negative errno on failure
     */
    int (*set_coding_rate)(uint8_t cr);
};

/* === Framework Management Functions === */

/**
 * @brief Initialize the RF framework
 * 
 * Must be called before any RF operations. Safe to call multiple times.
 * 
 * @return 0 on success, negative errno on failure
 */
int rf_framework_init(void);

/**
 * @brief Register an RF driver with the framework
 * 
 * @param driver Pointer to driver interface (must remain valid)
 * @return 0 on success, negative errno on failure
 */
int rf_framework_register_driver(const struct akira_rf_driver *driver);

/**
 * @brief Get a registered RF driver by chip type
 * 
 * @param type Chip type to lookup
 * @return Pointer to driver interface, or NULL if not found
 */
const struct akira_rf_driver *rf_framework_get_driver(rf_chip_type_t type);

/**
 * @brief Get currently active RF driver
 * 
 * @return Pointer to active driver interface, or NULL if none active
 */
const struct akira_rf_driver *rf_framework_get_active_driver(void);

/**
 * @brief Set the active RF driver
 * 
 * @param type Chip type to make active
 * @return 0 on success, negative errno on failure
 */
int rf_framework_set_active_driver(rf_chip_type_t type);

/**
 * @brief Get number of registered drivers
 *
 * @return Number of registered drivers
 */
int rf_framework_get_driver_count(void);

/**
 * @brief Claim the shared RF hardware reset line (RF_RST).
 *
 * On boards where multiple transceivers share one physical RESET pin (e.g.
 * CC1121 and LR2021 both on TCA6408 P4), pulsing reset resets ALL of them.
 * The first driver to init claims the reset and performs the pulse; later
 * inits must skip their own pulse so they don't wipe an already-configured
 * sibling. Returns true exactly once per boot, to the first caller.
 *
 * @return true if the caller should perform the reset pulse, false otherwise
 */
bool rf_framework_claim_shared_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_RF_FRAMEWORK_H */
