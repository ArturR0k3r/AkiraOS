/**
 * @file cc1101.c
 * @brief CC1101 Sub-GHz Transceiver Driver Implementation
 */

#include "cc1101.h"
#include <zephyr/logging/log.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>

LOG_MODULE_REGISTER(akira_cc1101, LOG_LEVEL_INF);

// TODO: Implement CC1101 register access
// TODO: Implement FSK/GFSK/MSK/OOK modulations
// TODO: Implement packet handling
// TODO: Implement WOR (Wake-on-Radio)
// TODO: Implement frequency calibration

/* CC1101 Registers */
#define CC1101_REG_IOCFG2 0x00
#define CC1101_REG_IOCFG1 0x01
#define CC1101_REG_IOCFG0 0x02
#define CC1101_REG_FIFOTHR 0x03
#define CC1101_REG_SYNC1 0x04
#define CC1101_REG_SYNC0 0x05
#define CC1101_REG_PKTLEN 0x06
#define CC1101_REG_PKTCTRL1 0x07
#define CC1101_REG_PKTCTRL0 0x08
#define CC1101_REG_ADDR 0x09
#define CC1101_REG_CHANNR 0x0A
#define CC1101_REG_FSCTRL1 0x0B
#define CC1101_REG_FSCTRL0 0x0C
#define CC1101_REG_FREQ2 0x0D
#define CC1101_REG_FREQ1 0x0E
#define CC1101_REG_FREQ0 0x0F
#define CC1101_REG_MDMCFG4 0x10
#define CC1101_REG_MDMCFG3 0x11
#define CC1101_REG_MDMCFG2 0x12
#define CC1101_REG_MDMCFG1 0x13
#define CC1101_REG_MDMCFG0 0x14
#define CC1101_REG_DEVIATN 0x15
#define CC1101_REG_MCSM2 0x16
#define CC1101_REG_MCSM1 0x17
#define CC1101_REG_MCSM0 0x18
#define CC1101_REG_FOCCFG 0x19
#define CC1101_REG_BSCFG 0x1A
#define CC1101_REG_AGCCTRL2 0x1B
#define CC1101_REG_AGCCTRL1 0x1C
#define CC1101_REG_AGCCTRL0 0x1D
#define CC1101_REG_WOREVT1 0x1E
#define CC1101_REG_WOREVT0 0x1F
#define CC1101_REG_WORCTRL 0x20

/* Command strobes */
#define CC1101_CMD_SRES 0x30
#define CC1101_CMD_SFSTXON 0x31
#define CC1101_CMD_SXOFF 0x32
#define CC1101_CMD_SCAL 0x33
#define CC1101_CMD_SRX 0x34
#define CC1101_CMD_STX 0x35
#define CC1101_CMD_SIDLE 0x36
#define CC1101_CMD_SWOR 0x38
#define CC1101_CMD_SPWD 0x39
#define CC1101_CMD_SFRX 0x3A
#define CC1101_CMD_SFTX 0x3B
#define CC1101_CMD_SWORRST 0x3C
#define CC1101_CMD_SNOP 0x3D

static struct
{
    bool initialized;
    struct cc1101_config config;
    rf_mode_t current_mode;
    uint32_t frequency;
    int8_t tx_power;
    rf_rx_callback_t rx_callback;
} g_cc1101 = {0};

static int cc1101_init(void)
{
    // TODO: Reset chip (SRES strobe)
    // TODO: Configure default registers
    // TODO: Calibrate oscillator

    LOG_INF("CC1101 init (stub)");
    return -3; // Not implemented
}

static int cc1101_deinit(void)
{
    // TODO: Power down (SPWD strobe)
    g_cc1101.initialized = false;
    return 0;
}

static int cc1101_set_mode(rf_mode_t mode)
{
    // TODO: Send mode command strobe

    switch (mode)
    {
    case RF_MODE_SLEEP:
        // CC1101_CMD_SPWD
        break;
    case RF_MODE_STANDBY:
        // CC1101_CMD_SIDLE
        break;
    case RF_MODE_RX:
        // CC1101_CMD_SRX
        break;
    case RF_MODE_TX:
        // CC1101_CMD_STX
        break;
    }

    g_cc1101.current_mode = mode;
    return -3; // Not implemented
}

static int cc1101_set_frequency(uint32_t freq_hz)
{
    // TODO: Calculate FREQ registers from frequency
    // TODO: Recalibrate after frequency change

    LOG_DBG("CC1101 set freq: %u Hz", freq_hz);
    g_cc1101.frequency = freq_hz;
    return -3; // Not implemented
}

static int cc1101_set_power(int8_t dbm)
{
    // TODO: Set PATABLE value based on dbm

    LOG_DBG("CC1101 set power: %d dBm", dbm);
    g_cc1101.tx_power = dbm;
    return -3; // Not implemented
}

static int cc1101_set_modulation(rf_modulation_t mod)
{
    // TODO: Configure MDMCFG2 register
    LOG_DBG("CC1101 set modulation: %d", mod);
    return -3; // Not implemented
}

static int cc1101_set_bitrate(uint32_t bps)
{
    // TODO: Configure MDMCFG4/MDMCFG3 registers
    LOG_DBG("CC1101 set bitrate: %u bps", bps);
    return -3; // Not implemented
}

static int cc1101_tx(const uint8_t *data, size_t len)
{
    // TODO: Write to TX FIFO
    // TODO: Send STX strobe
    // TODO: Wait for TX complete (GDO)

    if (!data || len == 0 || len > 64)
    {
        return -1;
    }

    LOG_DBG("CC1101 TX: %zu bytes", len);
    return -3; // Not implemented
}

static int cc1101_rx(uint8_t *buffer, size_t max_len, uint32_t timeout_ms)
{
    // TODO: Set RX mode
    // TODO: Wait for packet (GDO0) or timeout
    // TODO: Read from RX FIFO

    if (!buffer || max_len == 0)
    {
        return -1;
    }

    LOG_DBG("CC1101 RX: max=%zu, timeout=%u", max_len, timeout_ms);
    return -3; // Not implemented
}

static int cc1101_get_rssi(int16_t *rssi)
{
    // TODO: Read RSSI status register
    // TODO: Convert to dBm

    if (!rssi)
    {
        return -1;
    }

    *rssi = -100; // Placeholder
    return -3;    // Not implemented
}

static void cc1101_set_rx_callback(rf_rx_callback_t callback)
{
    g_cc1101.rx_callback = callback;
}

static const struct akira_rf_driver cc1101_driver = {
    .name = "CC1101",
    .type = RF_CHIP_CC1101,
    .init = cc1101_init,
    .deinit = cc1101_deinit,
    .set_mode = cc1101_set_mode,
    .set_frequency = cc1101_set_frequency,
    .set_power = cc1101_set_power,
    .set_modulation = cc1101_set_modulation,
    .set_bitrate = cc1101_set_bitrate,
    .tx = cc1101_tx,
    .rx = cc1101_rx,
    .get_rssi = cc1101_get_rssi,
    .set_rx_callback = cc1101_set_rx_callback,
    .set_spreading_factor = NULL, // Not applicable
    .set_bandwidth = NULL,
    .set_coding_rate = NULL,
};

int cc1101_init_with_config(const struct cc1101_config *config)
{
    if (!config)
    {
        return -1;
    }

    memcpy(&g_cc1101.config, config, sizeof(g_cc1101.config));
    return cc1101_init();
}

const struct akira_rf_driver *cc1101_get_driver(void)
{
    return &cc1101_driver;
}
