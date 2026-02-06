/**
 * @file lr1121.c
 * @brief LR1121 LoRa/GFSK Transceiver Driver Implementation
 */

#include "lr1121.h"
#include <zephyr/logging/log.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>

LOG_MODULE_REGISTER(akira_lr1121, LOG_LEVEL_INF);

// TODO: Implement LR1121 register access
// TODO: Implement LoRa mode
// TODO: Implement GFSK mode
// TODO: Implement frequency hopping
// TODO: Implement LBT (Listen Before Talk)
// TODO: Implement satellite band support

/* LR1121 Commands */
#define LR1121_CMD_GET_STATUS 0x0100
#define LR1121_CMD_SET_SLEEP 0x0200
#define LR1121_CMD_SET_STANDBY 0x0201
#define LR1121_CMD_SET_FS 0x0202
#define LR1121_CMD_SET_TX 0x0203
#define LR1121_CMD_SET_RX 0x0204
#define LR1121_CMD_SET_RF_FREQUENCY 0x0304
#define LR1121_CMD_SET_TX_PARAMS 0x0305
#define LR1121_CMD_SET_PACKET_TYPE 0x0306
#define LR1121_CMD_SET_MODULATION_PARAMS 0x0307
#define LR1121_CMD_SET_PACKET_PARAMS 0x0308
#define LR1121_CMD_WRITE_BUFFER 0x0309
#define LR1121_CMD_READ_BUFFER 0x030A
#define LR1121_CMD_GET_RSSI_INST 0x030B

static struct
{
    bool initialized;
    struct lr1121_config config;
    rf_mode_t current_mode;
    uint32_t frequency;
    int8_t tx_power;
    rf_rx_callback_t rx_callback;
} g_lr1121 = {0};

static int lr1121_init(void)
{
    // TODO: Implement SPI initialization
    // TODO: Reset chip
    // TODO: Configure default parameters

    LOG_INF("LR1121 init (stub)");
    return -3; // Not implemented
}

static int lr1121_deinit(void)
{
    // TODO: Put chip in sleep mode
    g_lr1121.initialized = false;
    return 0;
}

static int lr1121_set_mode(rf_mode_t mode)
{
    // TODO: Send mode change command

    switch (mode)
    {
    case RF_MODE_SLEEP:
        // LR1121_CMD_SET_SLEEP
        break;
    case RF_MODE_STANDBY:
        // LR1121_CMD_SET_STANDBY
        break;
    case RF_MODE_RX:
        // LR1121_CMD_SET_RX
        break;
    case RF_MODE_TX:
        // LR1121_CMD_SET_TX
        break;
    }

    g_lr1121.current_mode = mode;
    return -3; // Not implemented
}

static int lr1121_set_frequency(uint32_t freq_hz)
{
    // TODO: Validate frequency range (150-960 MHz)
    // TODO: Send SET_RF_FREQUENCY command

    LOG_DBG("LR1121 set freq: %u Hz", freq_hz);
    g_lr1121.frequency = freq_hz;
    return -3; // Not implemented
}

static int lr1121_set_power(int8_t dbm)
{
    // TODO: Clamp to chip limits
    // TODO: Send SET_TX_PARAMS command

    LOG_DBG("LR1121 set power: %d dBm", dbm);
    g_lr1121.tx_power = dbm;
    return -3; // Not implemented
}

static int lr1121_set_modulation(rf_modulation_t mod)
{
    // TODO: Configure modulation parameters
    LOG_DBG("LR1121 set modulation: %d", mod);
    return -3; // Not implemented
}

static int lr1121_set_bitrate(uint32_t bps)
{
    // TODO: Configure GFSK bitrate
    LOG_DBG("LR1121 set bitrate: %u bps", bps);
    return -3; // Not implemented
}

static int lr1121_tx(const uint8_t *data, size_t len)
{
    // TODO: Write to FIFO buffer
    // TODO: Set TX mode
    // TODO: Wait for TX done

    if (!data || len == 0)
    {
        return -1;
    }

    LOG_DBG("LR1121 TX: %zu bytes", len);
    return -3; // Not implemented
}

static int lr1121_rx(uint8_t *buffer, size_t max_len, uint32_t timeout_ms)
{
    // TODO: Set RX mode
    // TODO: Wait for packet or timeout
    // TODO: Read from FIFO buffer

    if (!buffer || max_len == 0)
    {
        return -1;
    }

    LOG_DBG("LR1121 RX: max=%zu, timeout=%u", max_len, timeout_ms);
    return -3; // Not implemented
}

static int lr1121_get_rssi(int16_t *rssi)
{
    // TODO: Read RSSI register

    if (!rssi)
    {
        return -1;
    }

    *rssi = -100; // Placeholder
    return -3;    // Not implemented
}

static void lr1121_set_rx_callback(rf_rx_callback_t callback)
{
    g_lr1121.rx_callback = callback;
}

static int lr1121_set_spreading_factor(uint8_t sf)
{
    // TODO: Configure LoRa SF (5-12)
    LOG_DBG("LR1121 set SF: %d", sf);
    return -3; // Not implemented
}

static int lr1121_set_bandwidth(uint32_t bw_hz)
{
    // TODO: Configure LoRa bandwidth
    LOG_DBG("LR1121 set BW: %u Hz", bw_hz);
    return -3; // Not implemented
}

static int lr1121_set_coding_rate(uint8_t cr)
{
    // TODO: Configure LoRa coding rate (4/5 to 4/8)
    LOG_DBG("LR1121 set CR: 4/%d", cr);
    return -3; // Not implemented
}

static const struct akira_rf_driver lr1121_driver = {
    .name = "LR1121",
    .type = RF_CHIP_LR1121,
    .init = lr1121_init,
    .deinit = lr1121_deinit,
    .set_mode = lr1121_set_mode,
    .set_frequency = lr1121_set_frequency,
    .set_power = lr1121_set_power,
    .set_modulation = lr1121_set_modulation,
    .set_bitrate = lr1121_set_bitrate,
    .tx = lr1121_tx,
    .rx = lr1121_rx,
    .get_rssi = lr1121_get_rssi,
    .set_rx_callback = lr1121_set_rx_callback,
    .set_spreading_factor = lr1121_set_spreading_factor,
    .set_bandwidth = lr1121_set_bandwidth,
    .set_coding_rate = lr1121_set_coding_rate,
};

int lr1121_init_with_config(const struct lr1121_config *config)
{
    if (!config)
    {
        return -1;
    }

    memcpy(&g_lr1121.config, config, sizeof(g_lr1121.config));
    return lr1121_init();
}

const struct akira_rf_driver *lr1121_get_driver(void)
{
    return &lr1121_driver;
}
