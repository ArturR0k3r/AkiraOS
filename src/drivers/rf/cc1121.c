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
#include <zephyr/init.h>
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
#define CC1121_FREQ_IF_CFG 0x0F
#define CC1121_IQIC        0x10
#define CC1121_CHAN_BW     0x11
#define CC1121_MDMCFG1     0x12
#define CC1121_MDMCFG0     0x13
#define CC1121_SYMBOL_RATE2 0x14
#define CC1121_SYMBOL_RATE1 0x15
#define CC1121_SYMBOL_RATE0 0x16
#define CC1121_AGC_REF     0x17
#define CC1121_AGC_CS_THR  0x18
#define CC1121_AGC_GAIN_ADJUST 0x19
#define CC1121_AGC_CFG3    0x1A
#define CC1121_AGC_CFG2    0x1B
#define CC1121_AGC_CFG1    0x1C
#define CC1121_AGC_CFG0    0x1D
#define CC1121_FIFO_CFG    0x1E
#define CC1121_DEV_ADDR    0x1F
#define CC1121_SETTLING_CFG 0x20
#define CC1121_FS_CFG      0x21
#define CC1121_WOR_CFG1    0x22
#define CC1121_WOR_CFG0    0x23
#define CC1121_WOR_EVENT0_MSB 0x24
#define CC1121_WOR_EVENT0_LSB 0x25
#define CC1121_PKT_CFG2    0x26
#define CC1121_PKT_CFG1    0x27
#define CC1121_PKT_CFG0    0x28
#define CC1121_RFEND_CFG1  0x29
#define CC1121_RFEND_CFG0  0x2A
#define CC1121_PA_CFG2     0x2B  /* PA_POWER_RAMP [5:0] */
#define CC1121_PA_CFG1     0x2C  /* FIRST_IPL / SECOND_IPL / RAMP_SHAPE */
#define CC1121_PA_CFG0     0x2D  /* Power ramp shape / ASK config */
#define CC1121_PKT_LEN      0x2E  /* Packet length (max in variable mode) */

/* Extended register addresses (used with CC1121_EXT_ADDR prefix) */
#define CC1121_EXT_IF_MIX_CFG   0x00
#define CC1121_EXT_FREQOFF_CFG  0x01
#define CC1121_EXT_EXT_CTRL     0x06  /* BURST_ADDR_INCR_EN at bit 0 */
#define CC1121_EXT_FREQ2        0x0C  /* Carrier frequency [23:16] */
#define CC1121_EXT_FREQ1        0x0D  /* Carrier frequency [15:8]  */
#define CC1121_EXT_FREQ0        0x0E  /* Carrier frequency [7:0]   */
#define CC1121_EXT_IF_ADC1      0x10
#define CC1121_EXT_IF_ADC0      0x11
#define CC1121_EXT_FS_DIG1      0x12
#define CC1121_EXT_FS_DIG0      0x13
#define CC1121_EXT_FS_CAL3      0x14
#define CC1121_EXT_FS_CAL2      0x15
#define CC1121_EXT_FS_CAL1      0x16
#define CC1121_EXT_FS_CAL0      0x17
#define CC1121_EXT_FS_CHP       0x18
#define CC1121_EXT_FS_DIVTWO    0x19
#define CC1121_EXT_FS_DSM1      0x1A
#define CC1121_EXT_FS_DSM0      0x1B
#define CC1121_EXT_FS_DVC1      0x1C
#define CC1121_EXT_FS_DVC0      0x1D
#define CC1121_EXT_FS_LBI       0x1E
#define CC1121_EXT_FS_PFD       0x1F
#define CC1121_EXT_FS_PRE       0x20
#define CC1121_EXT_FS_REG_DIV_CML 0x21
#define CC1121_EXT_FS_SPARE     0x22
#define CC1121_EXT_FS_VCO4      0x23
#define CC1121_EXT_FS_VCO3      0x24
#define CC1121_EXT_FS_VCO2      0x25
#define CC1121_EXT_FS_VCO1      0x26
#define CC1121_EXT_FS_VCO0      0x27
#define CC1121_EXT_XOSC5        0x32
#define CC1121_EXT_XOSC1        0x36
#define CC1121_EXT_XOSC0        0x37
#define CC1121_EXT_PARTNUMBER   0x8F  /* Reads 0x23 on CC1121RHB */
#define CC1121_EXT_RSSI1        0x71  /* RSSI upper 8 bits (signed, RSSI[11:4]) */
#define CC1121_EXT_RSSI0        0x72  /* RSSI lower nibble + status             */
#define CC1121_EXT_MARCSTATE    0x73  /* Radio state machine status */
#define CC1121_EXT_PQT_SYNC_ERR 0x75  /* bits[7:4]=PQT_ERROR (0=best), bits[3:0]=SYNC_ERROR (0=best, <SYNC_THR/2 → accepted) */
#define CC1121_EXT_FREQOFF_EST0 0x78  /* Frequency offset estimate LSB (signed, units = fxosc/2^18 ~122Hz) */
#define CC1121_EXT_MODEM_STATUS1 0x92 /* bit7=SYNC_FOUND bit1=PQT_REACHED bit0=PQT_VALID; RXFIFO status in bits[6:2] */
#define CC1121_EXT_NUM_TXBYTES  0xD6  /* Bytes in TX FIFO */
#define CC1121_EXT_NUM_RXBYTES  0xD7  /* Bytes in RX FIFO */

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
#define CC1121_MARC_FS_LOCK  0x0A  /* FS_LOCK MARC_STATE = 01010 */
#define CC1121_MARC_TX            0x13
#define CC1121_MARC_TX_END        0x14
#define CC1121_MARC_RX            0x0D
#define CC1121_MARC_RX_END        0x0E
#define CC1121_MARC_RXFIFO_ERROR  0x11
#define CC1121_MARC_FSTXON        0x12

/* PA_CFG2 bit 6 (RESERVED6) must be 1 — SmartRF Studio always sets it. */
#define CC1121_PA_CFG2_BIT6  0x40

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

static const struct spi_dt_spec g_spi = SPI_DT_SPEC_GET(
    CC1121_NODE,
    SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8),
    0);

static struct {
    bool initialized;
    struct gpio_dt_spec reset;
    rf_mode_t current_mode;
    uint32_t frequency_hz;
    uint32_t xosc_hz;
    int8_t tx_power_dbm;
    rf_rx_callback_t rx_callback;
} g_cc1121;

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

    return spi_write_dt(&g_spi, &tx_set);
}

static int cc1121_spi_transceive(const uint8_t *tx_buf, uint8_t *rx_buf, size_t len)
{
    struct spi_buf tx = { .buf = (void *)tx_buf, .len = len };
    struct spi_buf rx = { .buf = rx_buf,          .len = len };
    struct spi_buf_set tx_set = { .buffers = &tx, .count = 1 };
    struct spi_buf_set rx_set = { .buffers = &rx, .count = 1 };

    return spi_transceive_dt(&g_spi, &tx_set, &rx_set);
}

/* =========================================================================
 * Register access (public)
 * ========================================================================= */

static int cc1121_write_reg(uint8_t addr, uint8_t value)
{
    uint8_t hdr[2] = { addr & 0x3F, value };
    return cc1121_spi_write(hdr, 2, NULL, 0);
}

static int cc1121_read_reg(uint8_t addr, uint8_t *value)
{
    uint8_t tx[2] = { CC1121_READ | (addr & 0x3F), 0x00 };
    uint8_t rx[2] = { 0 };
    int ret = cc1121_spi_transceive(tx, rx, 2);
    if (ret == 0) {
        *value = rx[1];
    }
    return ret;
}

static int cc1121_write_ext_reg(uint8_t ext_addr, uint8_t value)
{
    uint8_t hdr[3] = { CC1121_EXT_ADDR, ext_addr, value };
    return cc1121_spi_write(hdr, 3, NULL, 0);
}

static int cc1121_read_ext_reg(uint8_t ext_addr, uint8_t *value)
{
    /* CC112x returns a chip status byte for every byte on SI.
     * Extended register read requires 4-byte burst:
     *   tx: cmd | ext_addr | dummy | dummy
     *   rx: status | status | status | data
     * BURST is required — B=0 paths return 0x00 on this chip revision. */
    uint8_t hdr = CC1121_READ | CC1121_BURST | CC1121_EXT_ADDR;
    uint8_t tx[4] = { hdr, ext_addr, 0x00, 0x00 };
    uint8_t rx[4] = { 0 };
    int ret = cc1121_spi_transceive(tx, rx, 4);
    if (ret == 0) {
        *value = rx[3];
    }
    return ret;
}

static int cc1121_strobe(uint8_t cmd)
{
    return cc1121_spi_write(&cmd, 1, NULL, 0);
}

static int cc1121_get_marcstate(uint8_t *state)
{
    return cc1121_read_ext_reg(CC1121_EXT_MARCSTATE, state);
}

static bool cc1121_is_ready(void)
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
        /* MARCSTATE register includes MARC_2PIN_STATE [6:5];
         * only compare MARC_STATE [4:0] */
        state &= 0x1F;
        if (state == CC1121_MARC_IDLE || state == CC1121_MARC_SLEEP) {
            return 0;
        }
        k_msleep(1);
    }
    LOG_ERR("IDLE timeout (MARCSTATE=0x%02X)", state);
    return -ETIMEDOUT;
}

/* LO divider for the band that contains freq_hz.
 * Must match the FSD_BANDSELECT value chosen by freq_to_fs_cfg().
 * Datasheet SWRU295E Table 30. */
static uint32_t freq_to_lo_div(uint32_t freq_hz)
{
    if (freq_hz < 370000000UL) {
        return 12; /* 273–320 MHz band */
    } else if (freq_hz < 550000000UL) {
        return 8;  /* 410–480 MHz band */
    } else {
        return 4;  /* 820–960 MHz band */
    }
}

/* Frequency register encoding (datasheet SWRU295E Eq. 26/27):
 *   f_RF  = f_VCO / LO_Divider
 *   f_VCO = (FREQ / 2^16) * f_xosc
 *   => FREQ[23:0] = f_RF * LO_Divider * 2^16 / f_xosc
 * Crystal frequency comes from DT (akira,xosc-frequency-hz).
 */
static void freq_to_regs(uint32_t freq_hz, uint32_t xosc_hz,
                          uint8_t *f2, uint8_t *f1, uint8_t *f0)
{
    uint64_t freq_word =
        (((uint64_t)freq_hz * freq_to_lo_div(freq_hz)) << 16) / xosc_hz;
    *f2 = (freq_word >> 16) & 0xFF;
    *f1 = (freq_word >>  8) & 0xFF;
    *f0 =  freq_word        & 0xFF;
}

/* CC112x symbol rate: f_sym = (2^20 + M) × 2^(E−39) × fxosc
 * Compute val = bps × 2^39 / fxosc, then right-shift until val < 2^21.
 * Shift count becomes E; M = val − 2^20.
 */
static void bitrate_to_regs(uint32_t bps, uint32_t xosc_hz,
                              uint8_t *sr2, uint8_t *sr1, uint8_t *sr0)
{
    uint64_t val = ((uint64_t)bps << 39) / xosc_hz;
    uint8_t e = 0;
    while (val >= (1ULL << 21)) { val >>= 1; e++; }
    uint32_t m = (uint32_t)(val - (1ULL << 20));
    *sr2 = ((e & 0x0F) << 4) | ((m >> 16) & 0x0F);
    *sr1 = (m >> 8) & 0xFF;
    *sr0 =  m       & 0xFF;
}

/* FS_CFG register value — selects the synthesizer band.
 * Derived from the DT carrier frequency so no code change is needed
 * when a board targets a different band.
 */
static uint8_t freq_to_fs_cfg(uint32_t freq_hz)
{
    /* FS_CFG.FSD_BANDSELECT [3:0] selects the LO divider for the band.
     * Bit 4 (FS_LOCK_EN) is set to 1 to enable the out-of-lock detector.
     * Datasheet Table: SWRU295E page 83. */
    if (freq_hz < 370000000UL) {
        return 0x16; /* 273–320 MHz band (LO div = 12) */
    } else if (freq_hz < 550000000UL) {
        return 0x14; /* 410–480 MHz band (LO div = 8) */
    } else {
        return 0x12; /* 820–960 MHz band (LO div = 4) */
    }
}

/* PA_CFG2 value for a target output power (datasheet SWRU295E p.88):
 *   P_out[dBm] = (PA_POWER_RAMP + 1) / 2 - 18
 *   => PA_POWER_RAMP = 2 * (dBm + 18) - 1
 * PA_POWER_RAMP is 6 bits, valid range 0x03..0x3F (~ -16..+14 dBm).
 * Bit 6 (RESERVED6) must be set.
 */
static uint8_t dbm_to_pa_cfg2(int8_t dbm)
{
    int ramp = 2 * ((int)dbm + 18) - 1;
    if (ramp < 0x03) ramp = 0x03;
    if (ramp > 0x3F) ramp = 0x3F;
    return CC1121_PA_CFG2_BIT6 | (uint8_t)(ramp & 0x3F);
}

/* =========================================================================
 * Base register configuration — band-independent modulation settings.
 * FS_CFG (synthesizer band), SYMBOL_RATE, and PA_CFG2 are NOT in this
 * table; they are computed at init from DT values so no rebuild is
 * needed when frequency, bitrate, or TX power changes.
 * ========================================================================= */
static const struct { uint8_t addr; uint8_t val; } k_cc1121_base_cfg[] = {
    { CC1121_IOCFG0,       0x06 },  /* GDO0: sync word detect */
    { CC1121_SYNC3,        0x55 },  /* Sync word [31:24] — SmartRF values */
    { CC1121_SYNC2,        0x55 },  /* Sync word [23:16] */
    { CC1121_SYNC1,        0x7A },  /* Sync word [15:8]  */
    { CC1121_SYNC0,        0x0E },  /* Sync word [7:0]   */
    { CC1121_SYNC_CFG1,    0x0B },  /* 30/32 sync (SmartRF/reset default) */
    { CC1121_SYNC_CFG0,    0x0B },  /* SmartRF default */
    { CC1121_DEVIATION_M,  0x48 },  /* ±25 kHz deviation (2-FSK default) */
    { CC1121_MODCFG_DEV_E, 0x05 },
    { CC1121_DCFILT_CFG,   0x1C },
    { CC1121_PREAMBLE_CFG1, 0x18 },  /* 4 bytes preamble (0xAA pattern); reset=3 bytes
                                         too short for AGC+FOC settling (§6.8) */
    { CC1121_PREAMBLE_CFG0, 0x2A },  /* PQT_EN=1, 16-symbol timeout, PQT=10 */
    { CC1121_IQIC,         0x00 },  /* disabled — reset default; enabling without cal corrupts RX */
    { CC1121_FREQ_IF_CFG,   0x40 },  /* 62.5 kHz digital IF (SmartRF/reset default) */
    { CC1121_CHAN_BW,       0x01 },  /* 200 kHz RX filter — widest valid for CC1121 (BB_CIC=1, ADC_CIC=20). 0x08=BB_CIC=8 was OUT OF RANGE for CC1121 (max 4). */
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
    { CC1121_PA_CFG0,      0x79 },  /* SmartRF: PA ramp shape */
    /* FS_CFG written separately — derived from DT akira,default-frequency-hz */
    { CC1121_PKT_CFG2,     0x00 },  /* Normal CRC, packet mode */
    { CC1121_PKT_CFG1,     0x01 },  /* CRC disabled (bits[3:2]=00), APPEND_STATUS=1 — TEST: isolate CRC mismatch */
    { CC1121_PKT_CFG0,     0x20 },  /* Variable packet length */
    { CC1121_PKT_LEN,      0xFF },  /* Max allowed packet length (datasheet §8.2.2);
                                       255 − 2 APPEND_STATUS bytes = 253 usable */
    /* PA_CFG2 written separately — derived from DT akira,default-tx-power-dbm */
};

/* Extended register defaults.
 * The FS_* synthesizer registers are the TI-recommended values that MUST be
 * changed from reset for the PLL to lock (SmartRF Studio export; these values
 * are frequency-independent across the CC112x family). Without them the VCO
 * free-runs and the chip transmits at a fixed wrong frequency. */
static const struct { uint8_t ext; uint8_t val; } k_cc1121_868_ext[] = {
    { CC1121_EXT_IF_MIX_CFG,    0x00 },
    { CC1121_EXT_FREQOFF_CFG,   0x22 },  /* FOC_EN=1 (was 0x20=disabled!); FOC after
                                             channel filter; compensates crystal offset
                                             that was corrupting data bytes after sync */
    { CC1121_EXT_IF_ADC1,       0xEE },
    { CC1121_EXT_IF_ADC0,       0x10 },
    { CC1121_EXT_FS_DIG1,       0x00 },
    { CC1121_EXT_FS_DIG0,       0x5F },
    { CC1121_EXT_FS_CAL1,       0x40 },
    { CC1121_EXT_FS_CAL0,       0x0E },
    { CC1121_EXT_FS_DIVTWO,     0x03 },
    { CC1121_EXT_FS_DSM0,       0x33 },
    { CC1121_EXT_FS_DVC0,       0x17 },
    { CC1121_EXT_FS_PFD,        0x50 },
    { CC1121_EXT_FS_PRE,        0x6E },
    { CC1121_EXT_FS_REG_DIV_CML, 0x14 },
    { CC1121_EXT_FS_SPARE,      0xAC },
    { CC1121_EXT_FS_VCO0,       0xB4 },
    { CC1121_EXT_XOSC5,         0x0E },
    { CC1121_EXT_XOSC1,         0x03 },
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
    if (!spi_is_ready_dt(&g_spi)) {
        LOG_ERR("SPI bus not ready");
        return -ENODEV;
    }

    /* --- RESET GPIO (optional, shared with LR2021) ----------------------- */
    if (DT_NODE_HAS_PROP(CC1121_NODE, reset_gpios)) {
        g_cc1121.reset = (struct gpio_dt_spec)GPIO_DT_SPEC_GET(CC1121_NODE, reset_gpios);
        if (gpio_is_ready_dt(&g_cc1121.reset)) {
            gpio_pin_configure_dt(&g_cc1121.reset, GPIO_OUTPUT_INACTIVE);
            /* RF_RST is shared with LR2021 — only pulse it if no sibling has
             * already claimed it, otherwise we would reset a configured LR2021.
             * The SRES software strobe below resets this chip either way. */
            if (rf_framework_claim_shared_reset()) {
                gpio_pin_set_dt(&g_cc1121.reset, 1);
                k_msleep(5);
                gpio_pin_set_dt(&g_cc1121.reset, 0);
                k_msleep(5);
            }
        }
    }

    /* --- Software reset -------------------------------------------------- */
    ret = cc1121_strobe(CC1121_SRES);
    if (ret < 0) {
        LOG_ERR("SRES failed: %d", ret);
        return ret;
    }
    k_msleep(10);

    /* --- SPI sanity check: read a standard register with known reset --- */
    {
        uint8_t iocfg0 = 0;
        ret = cc1121_read_reg(CC1121_IOCFG0, &iocfg0);
        LOG_INF("SPI check: IOCFG0=0x%02X (expect ~0x3C) ret=%d", iocfg0, ret);
    }

    /* --- Verify part number ---------------------------------------------- */
    {
        uint8_t pn = 0;
        /* Try burst-read method first (used by TI's own reference code for
         * status registers), fall back to single-read */
        uint8_t hdr = CC1121_READ | CC1121_BURST | CC1121_EXT_ADDR;
        uint8_t tx[4] = { hdr, CC1121_EXT_PARTNUMBER, 0x00, 0x00 };
        uint8_t rx[4] = { 0 };
        ret = cc1121_spi_transceive(tx, rx, 4);
        LOG_INF("PARTNUMBER burst-read: rx=[0x%02X 0x%02X 0x%02X 0x%02X] ret=%d",
                rx[0], rx[1], rx[2], rx[3], ret);
        /* rx[0]=status, rx[1]=status, rx[2]=status, rx[3]=data */
        pn = rx[3];

        if (ret < 0 || pn != 0x23) {
            /* Try single-read as fallback */
            uint8_t pn2 = 0;
            int ret2 = cc1121_read_ext_reg(CC1121_EXT_PARTNUMBER, &pn2);
            LOG_ERR("CC1121 not found (PARTNUMBER=0x%02X burst, 0x%02X single, expected 0x23)",
                    pn, pn2);
            return -ENODEV;
        }
    }

    /* --- Disable burst address auto-increment (EXT_CTRL.BURST_ADDR_INCR_EN).
     * When enabled (reset default), consecutive burst reads of extended
     * registers return data from auto-incremented addresses because the
     * chip returns a status byte for the first dummy, then increments
     * the address counter before the actual data byte. */
    cc1121_write_ext_reg(CC1121_EXT_EXT_CTRL, 0x00);

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
    {
        uint8_t pa_val = dbm_to_pa_cfg2(g_cc1121.tx_power_dbm);
        cc1121_write_reg(CC1121_PA_CFG2, pa_val);
        LOG_INF("PA_CFG2=0x%02X for %d dBm", pa_val, g_cc1121.tx_power_dbm);
    }

    /* --- Frequency registers — derived from DT frequency and crystal ------ */
    uint8_t f2, f1, f0;
    freq_to_regs(g_cc1121.frequency_hz, g_cc1121.xosc_hz, &f2, &f1, &f0);
    cc1121_write_ext_reg(CC1121_EXT_FREQ2, f2);
    cc1121_write_ext_reg(CC1121_EXT_FREQ1, f1);
    cc1121_write_ext_reg(CC1121_EXT_FREQ0, f0);

    /* Read back FREQ regs to confirm SPI writes landed and band is correct */
    {
        uint8_t rb2 = 0, rb1 = 0, rb0 = 0;
        cc1121_read_ext_reg(CC1121_EXT_FREQ2, &rb2);
        cc1121_read_ext_reg(CC1121_EXT_FREQ1, &rb1);
        cc1121_read_ext_reg(CC1121_EXT_FREQ0, &rb0);
        LOG_INF("FREQ regs: wrote %02X%02X%02X read %02X%02X%02X (LO_div=%u)",
                f2, f1, f0, rb2, rb1, rb0,
                freq_to_lo_div(g_cc1121.frequency_hz));
        if (rb2 != f2 || rb1 != f1 || rb0 != f0) {
            LOG_ERR("FREQ readback mismatch — SPI write to ext regs failing");
        }
    }

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

    /* --- Readback key modem registers to confirm SPI writes landed ---------- */
    {
        uint8_t v08=0, v0d=0, v0f=0, v10=0, v11=0, v14=0, v27=0, v28=0, v1e=0, v2e=0;
        cc1121_read_reg(0x08, &v08);  /* SYNC_CFG1     expect 0x0B */
        cc1121_read_reg(0x0D, &v0d);  /* PREAMBLE_CFG1 expect 0x18 */
        cc1121_read_reg(0x0F, &v0f);  /* FREQ_IF_CFG   expect 0x40 */
        cc1121_read_reg(0x10, &v10);  /* IQIC          expect 0x00 */
        cc1121_read_reg(0x11, &v11);  /* CHAN_BW       expect 0x01 */
        cc1121_read_reg(0x14, &v14);  /* SYMBOL_RATE2 */
        cc1121_read_reg(0x1E, &v1e);  /* FIFO_CFG      expect 0x00 */
        cc1121_read_reg(0x27, &v27);  /* PKT_CFG1      expect 0x01 */
        cc1121_read_reg(0x28, &v28);  /* PKT_CFG0      expect 0x20 */
        cc1121_read_reg(0x2E, &v2e);  /* PKT_LEN       expect 0xFF */
        LOG_INF("REG_RB: SYNC_CFG1=%02X PRE_CFG1=%02X FREQ_IF=%02X IQIC=%02X CHAN_BW=%02X"
                " SR2=%02X FIFO_CFG=%02X PKT_CFG1=%02X PKT_CFG0=%02X PKT_LEN=%02X",
                v08, v0d, v0f, v10, v11, v14, v1e, v27, v28, v2e);
        if (v08 != 0x0B) LOG_WRN("SYNC_CFG1 mismatch: got %02X want 0x0B", v08);
        if (v0d != 0x18) LOG_WRN("PREAMBLE_CFG1 mismatch: got %02X want 0x18", v0d);
        if (v0f != 0x40) LOG_WRN("FREQ_IF_CFG mismatch: got %02X want 0x40", v0f);
        if (v10 != 0x00) LOG_WRN("IQIC mismatch: got %02X want 0x00", v10);
        if (v1e != 0x00) LOG_WRN("FIFO_CFG mismatch: got %02X want 0x00 (CRC_AUTOFLUSH?)", v1e);
        if (v27 != 0x01) LOG_WRN("PKT_CFG1 mismatch: got %02X want 0x01", v27);
        if (v28 != 0x20) LOG_WRN("PKT_CFG0 mismatch: got %02X want 0x20", v28);
        if (v2e != 0xFF) LOG_WRN("PKT_LEN mismatch: got %02X want 0xFF - packet filtering broken!", v2e);
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
        cc1121_strobe(CC1121_SIDLE);
        wait_for_idle();
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

    if (dbm > 14) {
        dbm = 14; /* PA_POWER_RAMP saturates at 0x3F (~14 dBm) */
    }

    int ret = cc1121_write_reg(CC1121_PA_CFG2, dbm_to_pa_cfg2(dbm));
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
    cc1121_strobe(CC1121_SIDLE);
    wait_for_idle();
    cc1121_strobe(CC1121_SFTX);

    /* Write length byte + payload to TX FIFO */
    uint8_t hdr  = CC1121_TXFIFO_BURST;
    uint8_t lbuf = (uint8_t)len;

    struct spi_buf tx_bufs[] = {
        { .buf = &hdr,         .len = 1   },
        { .buf = &lbuf,        .len = 1   },
        { .buf = (void *)data, .len = len },
    };
    struct spi_buf_set tx_set = { .buffers = tx_bufs, .count = 3 };
    int ret = spi_write_dt(&g_spi, &tx_set);

    if (ret < 0) {
        LOG_ERR("TX FIFO write failed: %d", ret);
        return ret;
    }

    /* Verify the FIFO actually holds length byte + payload before firing TX */
    {
        uint8_t txbytes = 0;
        cc1121_read_ext_reg(CC1121_EXT_NUM_TXBYTES, &txbytes);
        if (txbytes != len + 1) {
            LOG_ERR("TX FIFO load mismatch: NUM_TXBYTES=%u expected=%zu",
                    txbytes, len + 1);
            cc1121_strobe(CC1121_SFTX);
            return -EIO;
        }
    }

    /* Trigger TX */
    ret = cc1121_strobe(CC1121_STX);
    if (ret < 0) {
        return ret;
    }

    /* Wait for calibration to finish and TX to start, then for TX to end.
     * After STX the chip goes IDLE→SETTLING→...→TX→TX_END→IDLE.
     * Phase 1: wait for MARC_STATE to reach TX (0x13).
     * Phase 2: wait for MARC_STATE to leave TX (TX complete). */
    {
        int64_t deadline = k_uptime_get() + 2000;
        uint8_t st = 0;

        /* Phase 1: wait to enter TX */
        while (k_uptime_get() < deadline) {
            if (cc1121_get_marcstate(&st) < 0) {
                cc1121_strobe(CC1121_SIDLE);
                return -EIO;
            }
            st &= 0x1F;
            if (st == CC1121_MARC_TX) break;
            if (st == CC1121_MARC_TX_END) break;  /* already finishing */
            k_msleep(1);
        }
        if (st != CC1121_MARC_TX && st != CC1121_MARC_TX_END) {
            LOG_ERR("TX never started (MARCSTATE=0x%02X, freq=%u pwr=%d)",
                    st, g_cc1121.frequency_hz, g_cc1121.tx_power_dbm);
            cc1121_strobe(CC1121_SIDLE);
            return -EIO;
        }

        /* Phase 2: wait for TX to complete */
        while (st == CC1121_MARC_TX && k_uptime_get() < deadline) {
            k_msleep(1);
            if (cc1121_get_marcstate(&st) < 0) {
                cc1121_strobe(CC1121_SIDLE);
                return -EIO;
            }
            st &= 0x1F;
        }
        if (st == CC1121_MARC_TX) {
            LOG_ERR("TX timeout");
            cc1121_strobe(CC1121_SIDLE);
            return -ETIMEDOUT;
        }
    }

    g_cc1121.current_mode = RF_MODE_STANDBY;
    LOG_INF("TX done: %zu bytes at %u Hz, %d dBm", len,
            g_cc1121.frequency_hz, g_cc1121.tx_power_dbm);
    return 0;
}

static int cc1121_rx(uint8_t *buffer, size_t max_len, uint32_t timeout_ms)
{
    if (!g_cc1121.initialized) {
        return -ENODEV;
    }

    cc1121_strobe(CC1121_SIDLE);
    wait_for_idle();
    cc1121_strobe(CC1121_SFRX);
    cc1121_strobe(CC1121_SRX);
    g_cc1121.current_mode = RF_MODE_RX;
    LOG_DBG("RX started: freq=%u timeout=%u ms", g_cc1121.frequency_hz, timeout_ms);

    int64_t deadline = k_uptime_get() + (timeout_ms ? timeout_ms : CC1121_RX_TIMEOUT_MS);
    uint8_t last_marc = CC1121_MARC_RX;

    while (k_uptime_get() < deadline) {
        uint8_t rx_bytes = 0;
        cc1121_read_ext_reg(CC1121_EXT_NUM_RXBYTES, &rx_bytes);

        uint8_t marc = 0;
        cc1121_get_marcstate(&marc);
        marc &= 0x1F;
        last_marc = marc;

        /* Recover from RX FIFO overflow error */
        if (marc == CC1121_MARC_RXFIFO_ERROR) {
            LOG_WRN("RX FIFO overflow — flushing and restarting RX");
            cc1121_strobe(CC1121_SIDLE);
            k_msleep(1);
            cc1121_strobe(CC1121_SFRX);
            cc1121_strobe(CC1121_SRX);
            last_marc = CC1121_MARC_RX;
            k_msleep(1);
            continue;
        }

        /* Only process the FIFO when the chip signals packet completion.
         * While MARC=RX (0x0D) the packet handler is still receiving —
         * reading mid-packet returns truncated data (datasheet §9.4.1). */
        bool packet_done = (marc == CC1121_MARC_IDLE ||
                            marc == CC1121_MARC_RX_END);
        if (!packet_done) {
            /* Adaptive polling: relax when idle, speed up when data arrives.
             * A 14-byte packet at 4800 bps takes ~29 ms on-air. */
            k_msleep(rx_bytes > 0 ? 2 : 50);
            continue;
        }

        /* Packet is complete.  FIFO empty with IDLE means noise/timeout. */
        if (rx_bytes == 0) {
            cc1121_strobe(CC1121_SFRX);
            cc1121_strobe(CC1121_SRX);
            last_marc = CC1121_MARC_RX;
            k_msleep(10);
            continue;
        }

        /* Spurious 1-2 bytes on completion are noise — flush and restart */
        if (rx_bytes < 3) {
            cc1121_strobe(CC1121_SFRX);
            cc1121_strobe(CC1121_SRX);
            last_marc = CC1121_MARC_RX;
            k_msleep(1);
            continue;
        }

        /* Valid completed packet */
        {
            /* CC1121 variable-length mode stores [length_byte][payload…]
             * in the RX FIFO, then appends 2 status bytes (APPEND_STATUS=1).
             * payload_len = NUM_RXBYTES − 2 (subtract status bytes).
             * The first byte is the over-the-air length byte — caller sees it. */
            size_t payload_len = (size_t)(rx_bytes - 2);

            if (payload_len > max_len) {
                LOG_WRN("RX: payload_len=%zu > max=%zu — flush+restart",
                        payload_len, max_len);
                cc1121_strobe(CC1121_SIDLE);
                cc1121_strobe(CC1121_SFRX);
                cc1121_strobe(CC1121_SRX);
                last_marc = CC1121_MARC_RX;
                k_msleep(1);
                continue;
            }

            /* Burst-read the entire RX FIFO in one SPI transaction.
             * The first byte on SO is the chip status (during the hdr byte
             * on SI); subsequent bytes are FIFO data. */
            uint8_t burst = CC1121_RXFIFO_BURST;
            uint8_t rx_buf[130] = { 0 };  /* 128 payload + 2 status */
            uint8_t tx_dummy[130] = { 0 };
            struct spi_buf tx_bufs[] = {
                { .buf = &burst,   .len = 1          },
                { .buf = tx_dummy, .len = rx_bytes   },
            };
            struct spi_buf rx_bufs_spi[] = {
                { .buf = &burst,  .len = 1          },
                { .buf = rx_buf,  .len = rx_bytes   },
            };
            struct spi_buf_set tx_set = { .buffers = tx_bufs, .count = 2 };
            struct spi_buf_set rx_set = { .buffers = rx_bufs_spi, .count = 2 };
            int ret = spi_transceive_dt(&g_spi, &tx_set, &rx_set);

            if (ret < 0) {
                LOG_ERR("RX FIFO read failed: %d", ret);
                cc1121_strobe(CC1121_SIDLE);
                return ret;
            }

            /* rx_buf[0..payload_len-1] = payload, rx_buf[payload_len..rx_bytes-1] = status */
            memcpy(buffer, rx_buf, payload_len);

            cc1121_strobe(CC1121_SIDLE);
            cc1121_strobe(CC1121_SFRX);
            g_cc1121.current_mode = RF_MODE_STANDBY;

            if (g_cc1121.rx_callback) {
                g_cc1121.rx_callback(buffer, payload_len, 0);
            }
            return (int)payload_len;
        }

    }

    /* Timeout */
    {
        uint8_t marc = 0;
        cc1121_get_marcstate(&marc);
        LOG_DBG("RX timeout: MARCSTATE=0x%02X NUM_RXBYTES=0", marc & 0x1F);
    }
    cc1121_strobe(CC1121_SIDLE);
    g_cc1121.current_mode = RF_MODE_STANDBY;
    return 0;
}

static int cc1121_get_rssi(int16_t *rssi)
{
    if (!g_cc1121.initialized) {
        return -ENODEV;
    }

    /* Enter RX so the AGC runs and RSSI reflects real signal level. */
    cc1121_strobe(CC1121_SIDLE);
    wait_for_idle();
    cc1121_strobe(CC1121_SFRX);
    cc1121_strobe(CC1121_SRX);
    k_msleep(10);

    /* Read full 12-bit RSSI: RSSI1[7:0] = RSSI[11:4], RSSI0[6:3] = RSSI[3:0].
     * CC112x: RSSI[11:0] is two's complement, 0.0625 dB/LSB.
     * RSSI_dBm = (int12_t)RSSI[11:0] / 16 - offset.
     *
     * AGC_GAIN_ADJUST is at reset (0x00), so offset is applied in software.
     * Typical offset for CC112x at 868 MHz is ~82-86 dB. */
    uint8_t rssi1 = 0, rssi0 = 0;
    cc1121_read_ext_reg(CC1121_EXT_RSSI1, &rssi1);
    cc1121_read_ext_reg(CC1121_EXT_RSSI0, &rssi0);

    /* DEBUG: log raw bytes to diagnose erratic readings */
    LOG_INF("RSSI raw: RSSI1=0x%02X RSSI0=0x%02X", rssi1, rssi0);

    cc1121_strobe(CC1121_SIDLE);

    /* Build signed 12-bit value */
    int16_t raw = ((int16_t)(int8_t)rssi1) << 4;
    raw |= (rssi0 >> 3) & 0x0F;
    if (raw & 0x0800) {
        raw |= 0xF000;  /* sign-extend to 16-bit */
    }

    /* 0.0625 dB/LSB → raw/16 converts to dB, then subtract offset (~84 dB at 868 MHz) */
    *rssi = raw / 16 - 84;
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
    .type              = RF_CHIP_CC1121,
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

/**
 * @brief Auto-register CC1121 driver at boot
 */
static int cc1121_auto_register(void)
{
    int ret = rf_framework_register_driver(&cc1121_driver);
    if (ret < 0 && ret != -EEXIST) {
        LOG_ERR("Failed to auto-register CC1121 driver: %d", ret);
        return ret;
    }
    LOG_INF("CC1121 driver registered with RF framework");
    return 0;
}

/* Register driver during APPLICATION initialization */
SYS_INIT(cc1121_auto_register, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
