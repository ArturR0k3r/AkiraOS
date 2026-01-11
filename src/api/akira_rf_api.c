/**
 * @file akira_rf_api.c
 * @brief RF API implementation for WASM exports
 */

#include "akira_api.h"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(akira_rf_api, LOG_LEVEL_INF);

#ifdef CONFIG_AKIRA_RF_MODULE

static akira_rf_chip_t g_active_chip = AKIRA_RF_CHIP_NONE;

// TODO: Add capability check before each API call
// TODO: Add mutex for thread safety
// TODO: Add TX/RX queue for async operations
// TODO: Add frequency hopping support
// TODO: Add channel scanning

int akira_rf_init(akira_rf_chip_t chip)
{
    // TODO: Check CAP_RF_TRANSCEIVE capability
    // TODO: Initialize selected RF chip driver
    // TODO: Configure default parameters

    LOG_INF("RF init: chip=%d", chip);

    switch (chip)
    {
    case AKIRA_RF_CHIP_NRF24L01:
        // TODO: Call nrf24_init()
        break;
    case AKIRA_RF_CHIP_LR1121:
        // TODO: Call lr1121_init()
        break;
    case AKIRA_RF_CHIP_CC1101:
        // TODO: Call cc1101_init()
        break;
    case AKIRA_RF_CHIP_SX1276:
        // TODO: Call sx1276_init()
        break;
    case AKIRA_RF_CHIP_RFM69:
        // TODO: Call rfm69_init()
        break;
    default:
        LOG_ERR("Unknown RF chip: %d", chip);
        return -1;
    }

    g_active_chip = chip;
    return 0;
}

int akira_rf_deinit(void)
{
    // TODO: Deinitialize active RF chip
    // TODO: Put chip in low power mode
    LOG_INF("RF deinit");
    g_active_chip = AKIRA_RF_CHIP_NONE;
    return 0;
}

int akira_rf_send(const uint8_t *data, size_t len)
{
    // TODO: Check CAP_RF_TRANSCEIVE capability
    // TODO: Check if chip is initialized
    // TODO: Call driver tx function
    // TODO: Handle ACK/NACK

    if (g_active_chip == AKIRA_RF_CHIP_NONE)
    {
        LOG_ERR("RF not initialized");
        return -1;
    }

    if (!data || len == 0)
    {
        return -2;
    }

    LOG_DBG("RF send: %zu bytes", len);

    // TODO: Dispatch to active driver
    return -1; // Not implemented
}

int akira_rf_receive(uint8_t *buffer, size_t max_len, uint32_t timeout_ms)
{
    // TODO: Check CAP_RF_TRANSCEIVE capability
    // TODO: Call driver rx function with timeout
    // TODO: Copy received data to buffer

    if (g_active_chip == AKIRA_RF_CHIP_NONE)
    {
        LOG_ERR("RF not initialized");
        return -1;
    }

    if (!buffer || max_len == 0)
    {
        return -2;
    }

    LOG_DBG("RF receive: max=%zu, timeout=%u", max_len, timeout_ms);

    // TODO: Dispatch to active driver
    return -1; // Not implemented
}

int akira_rf_set_frequency(uint32_t freq_hz)
{
    // TODO: Check CAP_RF_TRANSCEIVE capability
    // TODO: Validate frequency for selected chip
    // TODO: Call driver set_frequency

    LOG_INF("RF set frequency: %u Hz", freq_hz);

    // TODO: Dispatch to active driver
    return -1; // Not implemented
}

int akira_rf_set_power(int8_t dbm)
{
    // TODO: Check CAP_RF_TRANSCEIVE capability
    // TODO: Clamp to chip-specific limits
    // TODO: Call driver set_power

    LOG_INF("RF set power: %d dBm", dbm);

    // TODO: Dispatch to active driver
    return -1; // Not implemented
}

int akira_rf_get_rssi(int16_t *rssi)
{
    // TODO: Check CAP_RF_TRANSCEIVE capability
    // TODO: Read RSSI from driver

    if (!rssi)
    {
        return -1;
    }

    // TODO: Dispatch to active driver
    *rssi = -100; // Placeholder
    return -1;    // Not implemented
}

#endif /* CONFIG_AKIRA_RF_MODULE */
