/**
 * @file cc1121.c
 * @brief TI CC1121RHB Sub-GHz transceiver driver
 *
 * All radio parameters (frequency, TX power, bitrate, crystal frequency)
 * come from the device-tree node — no code changes required when the board
 * hardware or target frequency changes.  Edit the DTS node only:
 *
 *   cc1121: cc1121@3 {
 *       compatible = "ti,cc1121";
 *       akira,default-frequency-hz  = <868000000>;
 *       akira,default-tx-power-dbm  = <14>;
 *       akira,default-bitrate-bps   = <4800>;
 *       akira,xosc-frequency-hz     = <32000000>;
 *       ...
 *   };
 *
 * The driver selects the correct FS_CFG synthesizer band register value
 * automatically from the DT frequency.
 */

#include "cc1121.h"
#include "rf_framework.h"
#include <zephyr/logging/log.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(akira_cc1121, LOG_LEVEL_INF);

/* =========================================================================
 * SPI header byte encoding (CC112x family)
 * ========================================================================= */
#define CC1121_READ        BIT(7)
#define CC1121_BURST       BIT(6)
#define CC1121_EXT_ADDR    0x2F   /* Extended register access prefix */

/* Standard register addresses (0x00–0x2E) */
#define CC1121_IOCFG3      0x00
#define CC1121_IOCFG2      0x01
#define CC1121_IOCFG1      0x02
#define CC1121_IOCFG0      0x03
#define CC1121_SYNC3       0x04
#define CC1121_SYNC2       0x05
#define CC1121_SYNC1       0x06
#define CC1121_SYNC0       0x07
#define CC1121_SYNC_CFG1   0x08
#define CC1121_SYNC_CFG0   0x09
#define CC1121_DEVIATION_M 0x0A
#define CC1121_MODCFG_DEV_E 0x0B
#define CC1121_DCFILT_CFG  0x0C
#define CC1121_PREAMBLE_CFG1 0x0D
#define CC1121_PREAMBLE_CFG0 0x0E
#define CC1121_IQIC        0x0F
#define CC1121_CHAN_BW     0x10
#define CC1121_MDMCFG1     0x11
#define CC1121_MDMCFG0     0x12
#define CC1121_SYMBOL_RATE2 0x13
#define CC1121_SYMBOL_RATE1 0x14
#define CC1121_SYMBOL_RATE0 0x15
#define CC1121_AGC_REF     0x16
#define CC1121_AGC_CS_THR  0x17
#define CC1121_AGC_GAIN_ADJUST 0x18
#define CC1121_AGC_CFG3    0x19
#define CC1121_AGC_CFG2    0x1A
#define CC1121_AGC_CFG1    0x1B
#define CC1121_AGC_CFG0    0x1C
#define CC1121_FIFO_CFG    0x1D
#define CC1121_DEV_ADDR    0x1E
#define CC1121_SETTLING_CFG 0x1F
#define CC1121_FS_CFG      0x20
#define CC1121_WOR_CFG1    0x21
#define CC1121_WOR_CFG0    0x22
#define CC1121_WOR_EVENT0_MSB 0x23
#define CC1121_WOR_EVENT0_LSB 0x24
#define CC1121_PKT_CFG2    0x26
#define CC1121_PKT_CFG1    0x27
#define CC1121_PKT_CFG0    0x28
#define CC1121_RFEND_CFG1  0x29
#define CC1121_RFEND_CFG0  0x2A
#define CC1121_PA_CFG1     0x2B
#define CC1121_PA_CFG0     0x2C

/* Extended register addresses (used with CC1121_EXT_ADDR prefix) */
#define CC1121_EXT_IF_MIX_CFG   0x00
#define CC1121_EXT_FREQOFF_CFG  0x01
#define CC1121_EXT_FREQ2        0x0C  /* Carrier frequency [23:16] */
#define CC1121_EXT_FREQ1        0x0D  /* Carrier frequency [15:8]  */
#define CC1121_EXT_FREQ0        0x0E  /* Carrier frequency [7:0]   */
#define CC1121_EXT_FS_DIG1      0x12
#define CC1121_EXT_FS_DIG0      0x13
#define CC1121_EXT_FS_CAL3      0x15
#define CC1121_EXT_FS_CAL2      0x16
#define CC1121_EXT_FS_CAL1      0x17
#define CC1121_EXT_FS_CAL0      0x18
#define CC1121_EXT_FS_CHP       0x19
#define CC1121_EXT_FS_VCDAC_START 0x1B
#define CC1121_EXT_FS_VCO4      0x1C
#define CC1121_EXT_FS_VCO3      0x1D
#define CC1121_EXT_FS_VCO2      0x1E
#define CC1121_EXT_FS_VCO1      0x1F
#define CC1121_EXT_FS_VCO0      0x20
#define CC1121_EXT_XOSC5        0x32
#define CC1121_EXT_XOSC1        0x36
#define CC1121_EXT_XOSC0        0x37
#define CC1121_EXT_PARTNUMBER   0x8F  /* Should read 0x20 for CC1121 */
#define CC1121_EXT_MARCSTATE    0xF5  /* Radio state machine status */
#define CC1121_EXT_NUM_TXBYTES  0xFA  /* Bytes in TX FIFO */
#define CC1121_EXT_NUM_RXBYTES  0xFB  /* Bytes in RX FIFO */

/* Command strobes */
#define CC1121_SRES    0x30  /* Reset */
#define CC1121_SFSTXON 0x31  /* Enable and calibrate frequency synthesizer */
#define CC1121_SXOFF   0x32  /* Crystal oscillator off */
#define CC1121_SCAL    0x33  /* Calibrate frequency synthesizer */
#define CC1121_SRX     0x34  /* Enable RX */
#define CC1121_STX     0x35  /* Enable TX */
#define CC1121_SIDLE   0x36  /* Exit RX/TX, turn off synthesizer */
#define CC1121_SPWD    0x39  /* Enter power down */
#define CC1121_SFRX    0x3A  /* Flush RX FIFO */
#define CC1121_SFTX    0x3B  /* Flush TX FIFO */
#define CC1121_SNOP    0x3D  /* No operation, read status byte */

/* FIFO access addresses */
#define CC1121_TXFIFO_BURST  0x7F  /* Burst write TX FIFO */
#define CC1121_RXFIFO_BURST  0xFF  /* Burst read RX FIFO */

/* MARCSTATE values */
#define CC1121_MARC_SLEEP    0x00
#define CC1121_MARC_IDLE     0x01
#define CC1121_MARC_CALIB    0x04
#define CC1121_MARC_FS_LOCK  0x0C
#define CC1121_MARC_TX       0x13
#define CC1121_MARC_TX_END   0x14
#define CC1121_MARC_RX       0x1D
#define CC1121_MARC_RX_END   0x1E

/* PA_CFG1 power levels (approximate dBm for 868 MHz) */
#define CC1121_PA_10DBM   0x43
#define CC1121_PA_14DBM   0x63

/* Timeout waiting for IDLE */
#define CC1121_IDLE_TIMEOUT_MS  500
#define CC1121_RX_TIMEOUT_MS    2000

/* Device tree node — all configurable parameters live here, not in C code */
#define CC1121_NODE DT_NODELABEL(cc1121)

/* Read DT properties; fall through to defaults if the property is absent */
#define CC1121_DT_FREQ_HZ    DT_PROP_OR(CC1121_NODE, akira_default_frequency_hz, 868000000)
#define CC1121_DT_POWER_DBM  DT_PROP_OR(CC1121_NODE, akira_default_tx_power_dbm, 14)
#define CC1121_DT_BITRATE    DT_PROP_OR(CC1121_NODE, akira_default_bitrate_bps,   4800)
#define CC1121_DT_XOSC_HZ    DT_PROP_OR(CC1121_NODE, akira_xosc_frequency_hz,     32000000)

static int cc1121_set_power(int8_t dbm);

static struct {
    bool initialized;
    struct spi_dt_spec spi;
    struct gpio_dt_spec cs;
    struct gpio_dt_spec reset;
    rf_mode_t current_mode;
    uint32_t frequency_hz;
    uint32_t xosc_hz;
    int8_t tx_power_dbm;
    rf_rx_callback_t rx_callback;
} g_cc1121 = {
    .initialized  = false,
    /* Populated from DT at init — these are just zero-init sentinels */
    .frequency_hz = 0,
    .xosc_hz      = 0,
    .tx_power_dbm = 0,
};

/* =========================================================================
 * Low-level SPI helpers
 * ========================================================================= */

static int cc1121_spi_write(const uint8_t *hdr, size_t hdr_len,
                             const uint8_t *data, size_t data_len)
{
    struct spi_buf tx[2] = {
        { .buf = (void *)hdr,  .len = hdr_len  },
        { .buf = (void *)data, .len = data_len },
    };
    struct spi_buf_set tx_set = {
        .buffers = tx,
        .count   = data ? 2 : 1,
    };

    gpio_pin_set_dt(&g_cc1121.cs, 0);
    k_usleep(1);
    int ret = spi_write_dt(&g_cc1121.spi, &tx_set);
    k_usleep(1);
    gpio_pin_set_dt(&g_cc1121.cs, 1);
    return ret;
}

static int cc1121_spi_transceive(const uint8_t *tx_buf, uint8_t *rx_buf, size_t len)
{
    struct spi_buf tx = { .buf = (void *)tx_buf, .len = len };
    struct spi_buf rx = { .buf = rx_buf,          .len = len };
    struct spi_buf_set tx_set = { .buffers = &tx, .count = 1 };
    struct spi_buf_set rx_set = { .buffers = &rx, .count = 1 };

    gpio_pin_set_dt(&g_cc1121.cs, 0);
    k_usleep(1);
    int ret = spi_transceive_dt(&g_cc1121.spi, &tx_set, &rx_set);
    k_usleep(1);
    gpio_pin_set_dt(&g_cc1121.cs, 1);
    return ret;
}

/* =========================================================================
 * Register access (public)
 * ========================================================================= */

int cc1121_write_reg(uint8_t addr, uint8_t value)
{
    uint8_t hdr[2] = { addr & 0x3F, value };
    return cc1121_spi_write(hdr, 2, NULL, 0);
}

int cc1121_read_reg(uint8_t addr, uint8_t *value)
{
    uint8_t tx[2] = { CC1121_READ | (addr & 0x3F), 0x00 };
    uint8_t rx[2] = { 0 };
    int ret = cc1121_spi_transceive(tx, rx, 2);
    if (ret == 0) {
        *value = rx[1];
    }
    return ret;
}

int cc1121_write_ext_reg(uint8_t ext_addr, uint8_t value)
{
    uint8_t hdr[3] = { CC1121_EXT_ADDR, ext_addr, value };
    return cc1121_spi_write(hdr, 3, NULL, 0);
}

int cc1121_read_ext_reg(uint8_t ext_addr, uint8_t *value)
{
    uint8_t tx[3] = { CC1121_READ | CC1121_EXT_ADDR, ext_addr, 0x00 };
    uint8_t rx[3] = { 0 };
    int ret = cc1121_spi_transceive(tx, rx, 3);
    if (ret == 0) {
        *value = rx[2];
    }
    return ret;
}

static int cc1121_strobe(uint8_t cmd)
{
    return cc1121_spi_write(&cmd, 1, NULL, 0);
}

int cc1121_get_marcstate(uint8_t *state)
{
    return cc1121_read_ext_reg(CC1121_EXT_MARCSTATE, state);
}

bool cc1121_is_ready(void)
{
    return g_cc1121.initialized;
}

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

static int wait_for_idle(void)
{
    int64_t deadline = k_uptime_get() + CC1121_IDLE_TIMEOUT_MS;
    uint8_t state;

    while (k_uptime_get() < deadline) {
        if (cc1121_get_marcstate(&state) < 0) {
            return -EIO;
        }
        if (state == CC1121_MARC_IDLE || state == CC1121_MARC_SLEEP) {
            return 0;
        }
        k_msleep(1);
    }
    LOG_ERR("IDLE timeout (MARCSTATE=0x%02X)", state);
    return -ETIMEDOUT;
}

/* Frequency register encoding: FREQ[23:0] = f_carrier * 2^16 / f_xosc
 * Crystal frequency comes from DT (akira,xosc-frequency-hz).
 */
static void freq_to_regs(uint32_t freq_hz, uint32_t xosc_hz,
                          uint8_t *f2, uint8_t *f1, uint8_t *f0)
{
    uint64_t freq_word = ((uint64_t)freq_hz << 16) / xosc_hz;
    *f2 = (freq_word >> 16) & 0xFF;
    *f1 = (freq_word >>  8) & 0xFF;
    *f0 =  freq_word        & 0xFF;
}

/* Symbol rate encoding: SR[23:0] = bps * 2^20 / f_xosc
 * Exponent E in bits [23:20], mantissa M in bits [19:0].
 * Crystal frequency comes from DT (akira,xosc-frequency-hz).
 */
static void bitrate_to_regs(uint32_t bps, uint32_t xosc_hz,
                              uint8_t *sr2, uint8_t *sr1, uint8_t *sr0)
{
    uint32_t sr = (uint32_t)(((uint64_t)bps << 20) / xosc_hz);
    uint8_t e = 0;
    while (sr > 0xFFFFF) { sr >>= 1; e++; }
    *sr2 = (e << 4) | ((sr >> 16) & 0x0F);
    *sr1 = (sr >>  8) & 0xFF;
    *sr0 =  sr        & 0xFF;
}

/* FS_CFG register value — selects the synthesizer band.
 * Derived from the DT carrier frequency so no code change is needed
 * when a board targets a different band.
 */
static uint8_t freq_to_fs_cfg(uint32_t freq_hz)
{
    if (freq_hz < 400000000UL) {
        return 0x02; /* 315 MHz band (290–350 MHz) */
    } else if (freq_hz < 600000000UL) {
        return 0x04; /* 433 MHz band (410–475 MHz) */
    } else {
        return 0x14; /* 868/915 MHz band (820–960 MHz) */
    }
}

/* =========================================================================
 * Base register configuration — band-independent modulation settings.
 * FS_CFG (synthesizer band) and SYMBOL_RATE / PA_CFG1 are NOT in this
 * table; they are computed at init from DT values so no rebuild is
 * needed when frequency, bitrate, or TX power changes.
 * ========================================================================= */
static const struct { uint8_t addr; uint8_t val; } k_cc1121_base_cfg[] = {
    { CC1121_IOCFG0,       0x06 },  /* GDO0: sync word detect */
    { CC1121_SYNC_CFG1,    0x0B },  /* 16/16 bits, 30-bit tolerance */
    { CC1121_SYNC_CFG0,    0x17 },
    { CC1121_DEVIATION_M,  0x48 },  /* ±25 kHz deviation (2-FSK default) */
    { CC1121_MODCFG_DEV_E, 0x05 },
    { CC1121_DCFILT_CFG,   0x1C },
    { CC1121_IQIC,         0xC6 },
    { CC1121_CHAN_BW,       0x08 },  /* ~100 kHz RX filter BW */
    { CC1121_MDMCFG1,      0x46 },  /* 2-FSK modulation */
    { CC1121_MDMCFG0,      0x05 },
    /* SYMBOL_RATE2/1/0 written separately from DT akira,default-bitrate-bps */
    { CC1121_AGC_REF,      0x3C },
    { CC1121_AGC_CS_THR,   0xEF },
    { CC1121_AGC_CFG3,     0x83 },
    { CC1121_AGC_CFG2,     0x00 },
    { CC1121_AGC_CFG1,     0xA9 },
    { CC1121_AGC_CFG0,     0xCF },
    { CC1121_FIFO_CFG,     0x00 },  /* Variable-length packets */
    /* FS_CFG written separately — derived from DT akira,default-frequency-hz */
    { CC1121_PKT_CFG2,     0x00 },  /* Normal CRC, packet mode */
    { CC1121_PKT_CFG1,     0x05 },  /* Status bytes appended */
    { CC1121_PKT_CFG0,     0x20 },  /* Variable packet length */
    /* PA_CFG1 written separately — derived from DT akira,default-tx-power-dbm */
};

/* Extended register defaults */
static const struct { uint8_t ext; uint8_t val; } k_cc1121_868_ext[] = {
    { CC1121_EXT_IF_MIX_CFG,  0x00 },
    { CC1121_EXT_FREQOFF_CFG, 0x22 },
    { CC1121_EXT_FS_DIG1,     0x00 },
    { CC1121_EXT_FS_DIG0,     0xAF },
    { CC1121_EXT_FS_CAL1,     0x40 },
    { CC1121_EXT_FS_CAL0,     0x0E },
    { CC1121_EXT_FS_CHP,      0x28 },
    { CC1121_EXT_FS_VCDAC_START, 0x0B },
    { CC1121_EXT_FS_VCO0,     0xB9 },
    { CC1121_EXT_XOSC5,       0x0E },
    { CC1121_EXT_XOSC1,       0x03 },
    { CC1121_EXT_XOSC0,       0x04 },
};

/* =========================================================================
 * RF framework operations
 * ========================================================================= */

static int cc1121_init(void)
{
    int ret;

    if (g_cc1121.initialized) {
        return 0;
    }

    /* --- SPI bus --------------------------------------------------------- */
    const struct device *spi_dev = DEVICE_DT_GET(DT_BUS(CC1121_NODE));
    if (!device_is_ready(spi_dev)) {
        LOG_ERR("SPI bus not ready");
        return -ENODEV;
    }
    g_cc1121.spi.bus = spi_dev;
    g_cc1121.spi.config.frequency  = DT_PROP(CC1121_NODE, spi_max_frequency);
    g_cc1121.spi.config.operation  = SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB
                                     | SPI_WORD_SET(8);

    /* --- CS GPIO --------------------------------------------------------- */
    g_cc1121.cs = (struct gpio_dt_spec)GPIO_DT_SPEC_GET(CC1121_NODE, cs_gpios);
    if (!gpio_is_ready_dt(&g_cc1121.cs)) {
        LOG_ERR("CS GPIO not ready");
        return -ENODEV;
    }
    ret = gpio_pin_configure_dt(&g_cc1121.cs, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        return ret;
    }
    gpio_pin_set_dt(&g_cc1121.cs, 1); /* Deassert CS */

    /* --- RESET GPIO (optional, shared with LR2021) ----------------------- */
    if (DT_NODE_HAS_PROP(CC1121_NODE, reset_gpios)) {
        g_cc1121.reset = (struct gpio_dt_spec)GPIO_DT_SPEC_GET(CC1121_NODE, reset_gpios);
        if (gpio_is_ready_dt(&g_cc1121.reset)) {
            gpio_pin_configure_dt(&g_cc1121.reset, GPIO_OUTPUT_INACTIVE);
            /* Pulse reset */
            gpio_pin_set_dt(&g_cc1121.reset, 1);
            k_msleep(5);
            gpio_pin_set_dt(&g_cc1121.reset, 0);
            k_msleep(5);
        }
    }

    /* --- Software reset -------------------------------------------------- */
    ret = cc1121_strobe(CC1121_SRES);
    if (ret < 0) {
        LOG_ERR("SRES failed: %d", ret);
        return ret;
    }
    k_msleep(10);

    /* --- Verify part number ---------------------------------------------- */
    uint8_t pn = 0;
    ret = cc1121_read_ext_reg(CC1121_EXT_PARTNUMBER, &pn);
    if (ret < 0 || pn != 0x20) {
        LOG_ERR("CC1121 not found (PARTNUMBER=0x%02X, expected 0x20)", pn);
        return -ENODEV;
    }

    /* --- Populate runtime state from device-tree -------------------------- */
    g_cc1121.frequency_hz = CC1121_DT_FREQ_HZ;
    g_cc1121.xosc_hz      = CC1121_DT_XOSC_HZ;
    g_cc1121.tx_power_dbm = (int8_t)CC1121_DT_POWER_DBM;

    /* --- Apply base register configuration ------------------------------- */
    for (size_t i = 0; i < ARRAY_SIZE(k_cc1121_base_cfg); i++) {
        ret = cc1121_write_reg(k_cc1121_base_cfg[i].addr, k_cc1121_base_cfg[i].val);
        if (ret < 0) {
            LOG_ERR("reg 0x%02X write failed: %d", k_cc1121_base_cfg[i].addr, ret);
            return ret;
        }
    }
    for (size_t i = 0; i < ARRAY_SIZE(k_cc1121_868_ext); i++) {
        ret = cc1121_write_ext_reg(k_cc1121_868_ext[i].ext, k_cc1121_868_ext[i].val);
        if (ret < 0) {
            LOG_ERR("ext reg 0x%02X write failed: %d", k_cc1121_868_ext[i].ext, ret);
            return ret;
        }
    }

    /* --- FS_CFG: synthesizer band — derived from DT frequency ------------ */
    cc1121_write_reg(CC1121_FS_CFG, freq_to_fs_cfg(g_cc1121.frequency_hz));

    /* --- Symbol rate — derived from DT bitrate and crystal frequency ------ */
    uint8_t sr2, sr1, sr0;
    bitrate_to_regs(CC1121_DT_BITRATE, g_cc1121.xosc_hz, &sr2, &sr1, &sr0);
    cc1121_write_reg(CC1121_SYMBOL_RATE2, sr2);
    cc1121_write_reg(CC1121_SYMBOL_RATE1, sr1);
    cc1121_write_reg(CC1121_SYMBOL_RATE0, sr0);

    /* --- PA power — derived from DT tx-power-dbm ------------------------- */
    cc1121_set_power(g_cc1121.tx_power_dbm);

    /* --- Frequency registers — derived from DT frequency and crystal ------ */
    uint8_t f2, f1, f0;
    freq_to_regs(g_cc1121.frequency_hz, g_cc1121.xosc_hz, &f2, &f1, &f0);
    cc1121_write_ext_reg(CC1121_EXT_FREQ2, f2);
    cc1121_write_ext_reg(CC1121_EXT_FREQ1, f1);
    cc1121_write_ext_reg(CC1121_EXT_FREQ0, f0);

    /* --- Calibrate ------------------------------------------------------- */
    ret = cc1121_strobe(CC1121_SCAL);
    if (ret < 0) {
        LOG_ERR("SCAL strobe failed: %d", ret);
        return ret;
    }
    ret = wait_for_idle();
    if (ret < 0) {
        return ret;
    }

    g_cc1121.initialized = true;
    g_cc1121.current_mode = RF_MODE_STANDBY;
    LOG_INF("CC1121 ready: %u Hz, %d dBm", g_cc1121.frequency_hz, g_cc1121.tx_power_dbm);
    return 0;
}

static int cc1121_deinit(void)
{
    cc1121_strobe(CC1121_SPWD);
    g_cc1121.initialized = false;
    g_cc1121.current_mode = RF_MODE_SLEEP;
    return 0;
}

static int cc1121_set_mode(rf_mode_t mode)
{
    if (!g_cc1121.initialized) {
        return -ENODEV;
    }

    int ret = 0;
    switch (mode) {
    case RF_MODE_SLEEP:
        ret = cc1121_strobe(CC1121_SPWD);
        break;
    case RF_MODE_STANDBY:
        ret = cc1121_strobe(CC1121_SIDLE);
        if (ret == 0) {
            ret = wait_for_idle();
        }
        break;
    case RF_MODE_RX:
        cc1121_strobe(CC1121_SFRX);
        ret = cc1121_strobe(CC1121_SRX);
        break;
    case RF_MODE_TX:
        cc1121_strobe(CC1121_SFTX);
        ret = cc1121_strobe(CC1121_STX);
        break;
    default:
        return -EINVAL;
    }

    if (ret == 0) {
        g_cc1121.current_mode = mode;
    }
    return ret;
}

static int cc1121_set_frequency(uint32_t freq_hz)
{
    if (!g_cc1121.initialized) {
        return -ENODEV;
    }

    cc1121_strobe(CC1121_SIDLE);
    wait_for_idle();

    uint8_t f2, f1, f0;
    freq_to_regs(freq_hz, g_cc1121.xosc_hz, &f2, &f1, &f0);

    /* Update FS_CFG band if crossing a band boundary */
    cc1121_write_reg(CC1121_FS_CFG, freq_to_fs_cfg(freq_hz));
    cc1121_write_ext_reg(CC1121_EXT_FREQ2, f2);
    cc1121_write_ext_reg(CC1121_EXT_FREQ1, f1);
    cc1121_write_ext_reg(CC1121_EXT_FREQ0, f0);

    int ret = cc1121_strobe(CC1121_SCAL);
    if (ret == 0) {
        ret = wait_for_idle();
    }
    if (ret == 0) {
        g_cc1121.frequency_hz = freq_hz;
        LOG_INF("CC1121 frequency: %u Hz", freq_hz);
    }
    return ret;
}

static int cc1121_set_power(int8_t dbm)
{
    if (!g_cc1121.initialized) {
        return -ENODEV;
    }

    /* PA_CFG1: coarse power setting. Values are approximate for 868 MHz.
     * See CC1121 datasheet Table 10. */
    uint8_t pa_val;
    if (dbm <= 0) {
        pa_val = 0x03;
    } else if (dbm <= 5) {
        pa_val = 0x1B;
    } else if (dbm <= 10) {
        pa_val = CC1121_PA_10DBM;
    } else {
        pa_val = CC1121_PA_14DBM; /* 14 dBm max */
        dbm = 14;
    }

    int ret = cc1121_write_reg(CC1121_PA_CFG1, pa_val);
    if (ret == 0) {
        g_cc1121.tx_power_dbm = dbm;
    }
    return ret;
}

static int cc1121_set_modulation(rf_modulation_t mod)
{
    if (!g_cc1121.initialized) {
        return -ENODEV;
    }

    uint8_t mod_reg;
    switch (mod) {
    case RF_MOD_FSK:
    case RF_MOD_GFSK:
        mod_reg = 0x46; /* 2-FSK */
        break;
    case RF_MOD_OOK:
        mod_reg = 0x36; /* ASK/OOK */
        break;
    default:
        LOG_WRN("CC1121 does not support modulation %d, using 2-FSK", mod);
        mod_reg = 0x46;
        break;
    }

    return cc1121_write_reg(CC1121_MDMCFG1, mod_reg);
}

static int cc1121_set_bitrate(uint32_t bps)
{
    if (!g_cc1121.initialized) {
        return -ENODEV;
    }

    if (bps < 600 || bps > 500000) {
        return -EINVAL;
    }
    /* Uses xosc_hz from DT (akira,xosc-frequency-hz) */
    uint8_t sr2, sr1, sr0;
    bitrate_to_regs(bps, g_cc1121.xosc_hz, &sr2, &sr1, &sr0);
    cc1121_write_reg(CC1121_SYMBOL_RATE2, sr2);
    cc1121_write_reg(CC1121_SYMBOL_RATE1, sr1);
    cc1121_write_reg(CC1121_SYMBOL_RATE0, sr0);
    LOG_INF("CC1121 bitrate: %u bps (SYMBOL_RATE=0x%02X%02X%02X)", bps, sr2, sr1, sr0);
    return 0;
}

static int cc1121_tx(const uint8_t *data, size_t len)
{
    if (!g_cc1121.initialized) {
        return -ENODEV;
    }
    if (!data || len == 0 || len > 127) {
        return -EINVAL;
    }

    /* Ensure idle */
    /* Ensure idle */
    cc1121_strobe(CC1121_SIDLE);
    cc1121_strobe(CC1121_SFTX);

    /* Write length byte + payload to TX FIFO */
    uint8_t hdr  = CC1121_TXFIFO_BURST;
    uint8_t lbuf = (uint8_t)len;

    gpio_pin_set_dt(&g_cc1121.cs, 0);
    k_usleep(1);
    struct spi_buf tx_bufs[] = {
        { .buf = &hdr,         .len = 1   },
        { .buf = &lbuf,        .len = 1   },
        { .buf = (void *)data, .len = len },
    };
    struct spi_buf_set tx_set = { .buffers = tx_bufs, .count = 3 };
    int ret = spi_write_dt(&g_cc1121.spi, &tx_set);
    k_usleep(1);
    gpio_pin_set_dt(&g_cc1121.cs, 1);

    if (ret < 0) {
        LOG_ERR("TX FIFO write failed: %d", ret);
        return ret;
    }

    /* Trigger TX */
    ret = cc1121_strobe(CC1121_STX);
    if (ret < 0) {
        return ret;
    }

    /* Wait for TX completion (MARCSTATE returns to IDLE/RX after TX_END) */
    int64_t deadline = k_uptime_get() + 2000;
    uint8_t state;
    do {
        k_msleep(1);
        cc1121_get_marcstate(&state);
    } while (state == CC1121_MARC_TX && k_uptime_get() < deadline);

    if (state == CC1121_MARC_TX) {
        LOG_ERR("TX timeout");
        cc1121_strobe(CC1121_SIDLE);
        return -ETIMEDOUT;
    }

    g_cc1121.current_mode = RF_MODE_STANDBY;
    return 0;
}

static int cc1121_rx(uint8_t *buffer, size_t max_len, uint32_t timeout_ms)
{
    if (!g_cc1121.initialized) {
        return -ENODEV;
    }

    cc1121_strobe(CC1121_SFRX);
    cc1121_strobe(CC1121_SRX);
    g_cc1121.current_mode = RF_MODE_RX;

    int64_t deadline = k_uptime_get() + (timeout_ms ? timeout_ms : CC1121_RX_TIMEOUT_MS);
    uint8_t rx_bytes = 0;

    while (k_uptime_get() < deadline) {
        cc1121_read_ext_reg(CC1121_EXT_NUM_RXBYTES, &rx_bytes);
        if (rx_bytes > 0) {
            break;
        }
        k_msleep(1);
    }

    if (rx_bytes == 0) {
        cc1121_strobe(CC1121_SIDLE);
        g_cc1121.current_mode = RF_MODE_STANDBY;
        return 0;
    }

    /* Read length byte first */
    uint8_t pkt_len = 0;
    uint8_t hdr = CC1121_RXFIFO_BURST;

    gpio_pin_set_dt(&g_cc1121.cs, 0);
    k_usleep(1);
    struct spi_buf tx_bufs[] = { { .buf = &hdr, .len = 1 } };
    struct spi_buf rx_bufs[] = {
        { .buf = &hdr,    .len = 1 },       /* status byte */
        { .buf = &pkt_len,.len = 1 },        /* length byte */
    };
    struct spi_buf_set tx_set = { .buffers = tx_bufs, .count = 1 };
    struct spi_buf_set rx_set = { .buffers = rx_bufs, .count = 2 };
    spi_transceive_dt(&g_cc1121.spi, &tx_set, &rx_set);
    k_usleep(1);
    gpio_pin_set_dt(&g_cc1121.cs, 1);

    size_t read_len = MIN(pkt_len, max_len);
    if (read_len == 0) {
        cc1121_strobe(CC1121_SFRX);
        return 0;
    }

    /* Read payload */
    uint8_t burst = CC1121_RXFIFO_BURST;
    gpio_pin_set_dt(&g_cc1121.cs, 0);
    k_usleep(1);
    struct spi_buf tx2 = { .buf = &burst, .len = 1 };
    struct spi_buf rx2[2] = {
        { .buf = &burst, .len = 1       }, /* status */
        { .buf = buffer, .len = read_len },
    };
    struct spi_buf_set tx2_set = { .buffers = &tx2,  .count = 1 };
    struct spi_buf_set rx2_set = { .buffers = rx2,   .count = 2 };
    int ret = spi_transceive_dt(&g_cc1121.spi, &tx2_set, &rx2_set);
    k_usleep(1);
    gpio_pin_set_dt(&g_cc1121.cs, 1);

    cc1121_strobe(CC1121_SIDLE);
    g_cc1121.current_mode = RF_MODE_STANDBY;

    if (ret < 0) {
        LOG_ERR("RX FIFO read failed: %d", ret);
        return ret;
    }

    if (g_cc1121.rx_callback) {
        g_cc1121.rx_callback(buffer, read_len, 0);
    }

    return (int)read_len;
}

static int cc1121_get_rssi(int16_t *rssi)
{
    if (!g_cc1121.initialized) {
        return -ENODEV;
    }

    /* CC1121 reports RSSI as a 2's-complement 12-bit value in 0.0625 dBm steps.
     * For simplicity, read RSSI1 (MSB) as an 8-bit signed estimate. */
    uint8_t rssi1 = 0;
    int ret = cc1121_read_ext_reg(0xF6, &rssi1); /* RSSI1 extended reg */
    if (ret < 0) {
        return ret;
    }
    /* Convert: value is (RSSI / 16) - offset.  Offset ≈ 74 dB for most configs. */
    *rssi = (int16_t)((int8_t)rssi1) - 74;
    return 0;
}

static void cc1121_set_rx_callback(rf_rx_callback_t cb)
{
    g_cc1121.rx_callback = cb;
}

/* =========================================================================
 * RF framework driver struct
 * ========================================================================= */

static const struct akira_rf_driver cc1121_driver = {
    .name              = "CC1121",
    .type              = RF_CHIP_CC1101, /* Reuse enum slot; CC1121 replaces CC1101 */
    .init              = cc1121_init,
    .deinit            = cc1121_deinit,
    .set_mode          = cc1121_set_mode,
    .set_frequency     = cc1121_set_frequency,
    .set_power         = cc1121_set_power,
    .set_modulation    = cc1121_set_modulation,
    .set_bitrate       = cc1121_set_bitrate,
    .tx                = cc1121_tx,
    .rx                = cc1121_rx,
    .get_rssi          = cc1121_get_rssi,
    .set_rx_callback   = cc1121_set_rx_callback,
    /* LoRa-specific ops not applicable */
    .set_spreading_factor = NULL,
    .set_bandwidth        = NULL,
    .set_coding_rate      = NULL,
};

const struct akira_rf_driver *cc1121_get_driver(void)
{
    return &cc1121_driver;
}
