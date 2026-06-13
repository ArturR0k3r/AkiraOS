/**
 * @file lr2021.c
 * @brief Semtech LR2021 LoRa Plus Transceiver Driver
 *
 * SPI protocol (datasheet DS.LR2021 Rev 1.1):
 *   Mode 0 (CPOL=0, CPHA=0), max 16 MHz.
 *   Write: host sends opcode(16-bit) + args.  BUSY↑ on NSS↓, BUSY↓ when done.
 *          MISO returns stat(16) + irq(32) + zeros during write.
 *   Read:  host sends opcode(16-bit) + args.  BUSY↑.  Wait BUSY↓.
 *          Host sends 0x00 bytes; MISO returns stat(16) + response bytes.
 *   Direct FIFO read/write: single SPI frame, no BUSY wait.
 */

#include "lr2021.h"
#include "connectivity/radio_interface.h"
#include <zephyr/logging/log.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(akira_lr2021, LOG_LEVEL_INF);

/* =========================================================================
 * LR2021 Command Opcodes (datasheet §5.6)
 * ========================================================================= */

/* Direct FIFO access */
#define LR2021_CMD_READ_RX_FIFO         0x0001
#define LR2021_CMD_WRITE_TX_FIFO        0x0002

/* System configuration (§5.6.3) */
#define LR2021_CMD_GET_STATUS           0x0100
#define LR2021_CMD_GET_VERSION          0x0101
#define LR2021_CMD_CLEAR_IRQ            0x0116
#define LR2021_CMD_GET_AND_CLEAR_IRQ    0x0117
#define LR2021_CMD_CLEAR_RX_FIFO        0x011E
#define LR2021_CMD_CLEAR_TX_FIFO        0x011F
#define LR2021_CMD_SET_SLEEP            0x0127
#define LR2021_CMD_SET_STANDBY          0x0128
#define LR2021_CMD_SET_FS               0x0129

/* Radio configuration (§5.6.4) */
#define LR2021_CMD_SET_RF_FREQUENCY     0x0200
#define LR2021_CMD_SET_RX_PATH          0x0201
#define LR2021_CMD_SET_PA_CONFIG        0x0202
#define LR2021_CMD_SET_TX_PARAMS        0x0203
#define LR2021_CMD_SET_RX_TX_FALLBACK   0x0206
#define LR2021_CMD_SET_PACKET_TYPE      0x0207
#define LR2021_CMD_GET_RX_FIFO_LEVEL    0x011C
#define LR2021_CMD_GET_RX_PKT_LENGTH    0x0212
#define LR2021_CMD_GET_RSSI_INST        0x020B
#define LR2021_CMD_SET_RX               0x020C
#define LR2021_CMD_SET_TX               0x020D
#define LR2021_CMD_SET_DEFAULT_TIMEOUT  0x0215

/* FSK packet radio (§5.6.4.3) */
#define LR2021_CMD_SET_FSK_MOD_PARAMS   0x0240
#define LR2021_CMD_SET_FSK_PKT_PARAMS   0x0241
#define LR2021_CMD_SET_FSK_SYNCWORD     0x0244

/* =========================================================================
 * Packet types
 * ========================================================================= */
#define LR2021_PKT_TYPE_FSK             0x00
#define LR2021_PKT_TYPE_LORA            0x01
#define LR2021_PKT_TYPE_BPSK            0x02
#define LR2021_PKT_TYPE_FLRC            0x03
#define LR2021_PKT_TYPE_BLE             0x04
#define LR2021_PKT_TYPE_OQPSK           0x05

/* Standby modes */
#define LR2021_STANDBY_RC               0x00
#define LR2021_STANDBY_XOSC             0x01

/* Sleep config: retain data RAM, wake on NSS */
#define LR2021_SLEEP_RAM_RETENTION      0x04

/* Fallback mode: return to Standby RC after Rx/Tx */
#define LR2021_FALLBACK_STDBY_RC        0x02

/* IRQ flags (datasheet Table 5-17: Host Interrupts, 32-bit IrqStatus).
 * Read via GetAndClearIrqStatus which returns 4 bytes after the 2-byte stat.
 * Bit positions are absolute across the 32-bit word (byte0 = bits 7:0). */
#define LR2021_IRQ_RX_FIFO             (1U << 0)
#define LR2021_IRQ_TX_FIFO             (1U << 1)
#define LR2021_IRQ_PREAMBLE_DETECTED   (1U << 5)
#define LR2021_IRQ_SYNCWORD_VALID      (1U << 6)
#define LR2021_IRQ_SYNC_FAIL           (1U << 13)
#define LR2021_IRQ_RX_DONE             (1U << 18)
#define LR2021_IRQ_TX_DONE             (1U << 19)
#define LR2021_IRQ_TIMEOUT             (1U << 21)
#define LR2021_IRQ_CRC_ERROR           (1U << 22)
#define LR2021_IRQ_LEN_ERROR           (1U << 23)
#define LR2021_IRQ_ADDR_ERROR          (1U << 24)
#define LR2021_IRQ_CMD_ERROR           (1U << 17)

/* =========================================================================
 * Timing
 * ========================================================================= */
#define LR2021_BUSY_TIMEOUT_MS          1000
#define LR2021_BOOT_TIMEOUT_MS          5000
#define LR2021_TX_TIMEOUT_3S            0x2DC6C0  /* 3 seconds in 32us units */

/* =========================================================================
 * Device tree
 * ========================================================================= */
#define LR2021_NODE DT_NODELABEL(lr2021)
#define LR2021_DT_FREQ_HZ   DT_PROP_OR(LR2021_NODE, akira_default_frequency_hz, 868000000)
#define LR2021_DT_BITRATE   DT_PROP_OR(LR2021_NODE, akira_default_bitrate_bps,   4800)

/* =========================================================================
 * Driver state
 * ========================================================================= */
static struct {
    bool initialized;
    struct spi_dt_spec spi;
    struct gpio_dt_spec cs;
    struct gpio_dt_spec reset;
    struct gpio_dt_spec busy;
    radio_mode_t current_mode;
    uint32_t frequency_hz;
    uint32_t bitrate_bps;
    int8_t tx_power_dbm;
    radio_event_cb_t event_cb;
    void *event_user_data;
} g_lr2021;

/* Forward declarations — called from init before their definitions */
static int lr2021_set_frequency(uint32_t freq_hz);
static int lr2021_set_modulation(radio_modulation_t mod);
static int lr2021_set_bitrate(uint32_t bps);

/* =========================================================================
 * Low-level helpers
 * ========================================================================= */

/** Wait for BUSY pin to go low, with timeout. */
static int lr2021_wait_busy(void)
{
    if (!gpio_is_ready_dt(&g_lr2021.busy)) {
        return -ENODEV;
    }

    int64_t deadline = k_uptime_get() + LR2021_BUSY_TIMEOUT_MS;
    while (gpio_pin_get_dt(&g_lr2021.busy)) {
        if (k_uptime_get() > deadline) {
            LOG_ERR("BUSY timeout");
            return -ETIMEDOUT;
        }
        k_usleep(100);
    }
    return 0;
}

/** Assert CS (active low) */
static inline void lr2021_cs_low(void)
{
    gpio_pin_set_dt(&g_lr2021.cs, 1);
    k_usleep(1);
}

/** Deassert CS */
static inline void lr2021_cs_high(void)
{
    k_usleep(1);
    gpio_pin_set_dt(&g_lr2021.cs, 0);
}

/** SPI write-only (ignore MISO) */
static int lr2021_spi_write(const uint8_t *data, size_t len)
{
    struct spi_buf tx = { .buf = (void *)data, .len = len };
    struct spi_buf_set tx_set = { .buffers = &tx, .count = 1 };
    return spi_write_dt(&g_lr2021.spi, &tx_set);
}

/** SPI transceive (equal-length tx/rx) */
static int lr2021_spi_transceive(const uint8_t *tx, uint8_t *rx, size_t len)
{
    struct spi_buf tx_buf = { .buf = (void *)tx, .len = len };
    struct spi_buf rx_buf = { .buf = rx, .len = len };
    struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };
    struct spi_buf_set rx_set = { .buffers = &rx_buf, .count = 1 };
    return spi_transceive_dt(&g_lr2021.spi, &tx_set, &rx_set);
}

/* =========================================================================
 * Command interface (datasheet §5.4.1)
 * ========================================================================= */

/**
 * @brief Send a write command (no response data expected).
 *
 * SPI frame: Op(16) | Arg0 | Arg1 | ... | ArgN
 * MISO:      Stat(16) | Irq(32) | 0...
 */
static int lr2021_write_command(uint16_t opcode, const uint8_t *args, size_t args_len)
{
    int ret;

    ret = lr2021_wait_busy();
    if (ret < 0) {
        return ret;
    }

    uint8_t op_buf[2] = { (opcode >> 8) & 0xFF, opcode & 0xFF };

    lr2021_cs_low();

    /* Send opcode + args in one SPI frame */
    if (args && args_len > 0) {
        struct spi_buf tx[2] = {
            { .buf = op_buf, .len = 2 },
            { .buf = (void *)args, .len = args_len },
        };
        struct spi_buf_set tx_set = { .buffers = tx, .count = 2 };
        ret = spi_write_dt(&g_lr2021.spi, &tx_set);
    } else {
        ret = lr2021_spi_write(op_buf, 2);
    }

    lr2021_cs_high();

    if (ret < 0) {
        LOG_ERR("SPI write cmd 0x%04X failed: %d", opcode, ret);
        return ret;
    }

    /* Wait for command to complete */
    ret = lr2021_wait_busy();
    if (ret < 0) {
        LOG_ERR("Cmd 0x%04X BUSY timeout", opcode);
    }
    return ret;
}

/**
 * @brief Send a read command and read back the response.
 *
 * Phase 1: Op(16) | Arg0 | ... — BUSY↑
 * Phase 2: Wait BUSY↓
 * Phase 3: Send 0x00 bytes, MISO returns Stat(16) | Rsp0 | Rsp1 | ...
 *          The first 2 response bytes are the status; actual data starts at rsp[2].
 */
static int lr2021_read_command(uint16_t opcode, const uint8_t *args,
                                size_t args_len, uint8_t *rsp, size_t rsp_len)
{
    int ret;

    if (!rsp || rsp_len == 0) {
        return -EINVAL;
    }

    ret = lr2021_wait_busy();
    if (ret < 0) {
        return ret;
    }

    /* Phase 1: send opcode + args */
    uint8_t op_buf[2] = { (opcode >> 8) & 0xFF, opcode & 0xFF };

    lr2021_cs_low();

    if (args && args_len > 0) {
        struct spi_buf tx[2] = {
            { .buf = op_buf, .len = 2 },
            { .buf = (void *)args, .len = args_len },
        };
        struct spi_buf_set tx_set = { .buffers = tx, .count = 2 };
        ret = spi_write_dt(&g_lr2021.spi, &tx_set);
    } else {
        ret = lr2021_spi_write(op_buf, 2);
    }

    lr2021_cs_high();

    if (ret < 0) {
        LOG_ERR("SPI read cmd 0x%04X phase1 failed: %d", opcode, ret);
        return ret;
    }

    /* Wait for data ready */
    ret = lr2021_wait_busy();
    if (ret < 0) {
        LOG_ERR("Cmd 0x%04X BUSY timeout (phase2)", opcode);
        return ret;
    }

    /* Phase 2: read response — send dummy bytes, read MISO.
     * MISO returns Stat(16) + data(rsp_len).  Read into a local buffer
     * so we don't overflow the caller's rsp. */
    size_t total = rsp_len + 2;
    if (total > 128) {
        LOG_ERR("Cmd 0x%04X response too large: %u", opcode, total);
        return -EINVAL;
    }

    uint8_t rx_local[128];
    uint8_t tx_dummy[128];
    memset(tx_dummy, 0, total);

    lr2021_cs_low();
    ret = lr2021_spi_transceive(tx_dummy, rx_local, total);
    lr2021_cs_high();

    if (ret < 0) {
        LOG_ERR("SPI read cmd 0x%04X phase3 failed: %d", opcode, ret);
        return ret;
    }

    /* First 2 bytes are stat; copy the actual data to caller's buffer */
    memcpy(rsp, rx_local + 2, rsp_len);

    return 0;
}

/**
 * @brief Direct FIFO read — single SPI frame, data starts immediately.
 *
 * SPI frame: Op(16) | 0x00... 
 * MISO:      Stat(16) | Data0 | Data1 | ...
 */
static int lr2021_read_fifo(uint8_t *data, size_t len)
{
    int ret = lr2021_wait_busy();
    if (ret < 0) {
        return ret;
    }

    uint8_t op_buf[2] = { (LR2021_CMD_READ_RX_FIFO >> 8) & 0xFF,
                           LR2021_CMD_READ_RX_FIFO & 0xFF };

    /* Need to send opcode + len dummy bytes, receive stat(2) + data(len) */
    size_t tx_len = 2 + len;
    uint8_t tx[258];  /* max opcode(2) + 256 data */
    uint8_t rx[258];
    memset(tx, 0, tx_len);
    tx[0] = op_buf[0];
    tx[1] = op_buf[1];

    lr2021_cs_low();
    ret = lr2021_spi_transceive(tx, rx, tx_len);
    lr2021_cs_high();

    if (ret < 0) {
        return ret;
    }

    /* First 2 rx bytes are stat; data follows */
    memcpy(data, rx + 2, len);
    return 0;
}

/* =========================================================================
 * RF framework operations
 * ========================================================================= */

static int lr2021_init(void)
{
    int ret;

    if (g_lr2021.initialized) {
        return 0;
    }

    /* --- SPI bus --------------------------------------------------------- */
    const struct device *spi_dev = DEVICE_DT_GET(DT_BUS(LR2021_NODE));
    if (!device_is_ready(spi_dev)) {
        LOG_ERR("SPI bus not ready");
        return -ENODEV;
    }

    g_lr2021.spi.bus = spi_dev;
    g_lr2021.spi.config.frequency  = DT_PROP(LR2021_NODE, spi_max_frequency);
    g_lr2021.spi.config.operation  = SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB
                                     | SPI_WORD_SET(8);  /* SPI mode 0 (CPOL=0,CPHA=0) */

    /* --- CS GPIO --------------------------------------------------------- */
    g_lr2021.cs = (struct gpio_dt_spec)GPIO_DT_SPEC_GET(LR2021_NODE, cs_gpios);
    if (!gpio_is_ready_dt(&g_lr2021.cs)) {
        LOG_ERR("CS GPIO not ready");
        return -ENODEV;
    }
    gpio_pin_configure_dt(&g_lr2021.cs, GPIO_OUTPUT_INACTIVE);
    gpio_pin_set_dt(&g_lr2021.cs, 0); /* Deassert CS */

    /* --- RESET GPIO ------------------------------------------------------ */
    g_lr2021.reset = (struct gpio_dt_spec)GPIO_DT_SPEC_GET(LR2021_NODE, reset_gpios);
    if (!gpio_is_ready_dt(&g_lr2021.reset)) {
        LOG_ERR("RESET GPIO not ready");
        return -ENODEV;
    }
    gpio_pin_configure_dt(&g_lr2021.reset, GPIO_OUTPUT_INACTIVE);

    /* --- BUSY GPIO (input) ----------------------------------------------- */
    g_lr2021.busy = (struct gpio_dt_spec)GPIO_DT_SPEC_GET(LR2021_NODE, busy_gpios);
    if (!gpio_is_ready_dt(&g_lr2021.busy)) {
        LOG_ERR("BUSY GPIO not ready");
        return -ENODEV;
    }
    gpio_pin_configure_dt(&g_lr2021.busy, GPIO_INPUT);

    /* --- Hardware reset -------------------------------------------------- */
    LOG_INF("Resetting LR2021...");
    gpio_pin_set_dt(&g_lr2021.reset, 1);
    k_msleep(10);
    gpio_pin_set_dt(&g_lr2021.reset, 0);
    k_msleep(10);

    /* Wait for boot calibration to finish (BUSY goes low) */
    ret = lr2021_wait_busy();
    if (ret < 0) {
        LOG_ERR("Boot timeout (BUSY stuck high)");
        return ret;
    }

    /* --- Read version ---------------------------------------------------- */
    {
        uint8_t ver[4] = { 0 };
        ret = lr2021_read_command(LR2021_CMD_GET_VERSION, NULL, 0, ver, 4);
        if (ret == 0) {
            LOG_INF("LR2021 version: [0x%02X 0x%02X 0x%02X 0x%02X]",
                    ver[0], ver[1], ver[2], ver[3]);
        } else {
            LOG_WRN("Failed to read version: %d", ret);
        }
    }

    /* --- Set standby (XOSC) ---------------------------------------------- */
    {
        uint8_t mode = LR2021_STANDBY_XOSC;
        ret = lr2021_write_command(LR2021_CMD_SET_STANDBY, &mode, 1);
        if (ret < 0) {
            LOG_ERR("SetStandby failed: %d", ret);
            return ret;
        }
    }

    g_lr2021.current_mode = RADIO_MODE_STANDBY;

    /* --- Set defaults from DT --- */
    g_lr2021.frequency_hz = LR2021_DT_FREQ_HZ;
    g_lr2021.bitrate_bps  = LR2021_DT_BITRATE;

    /* Calibrate front-end first, then set frequency (datasheet §6.4.2).
     * Boot calibrates at 915 MHz; frequencies >10 MHz away need recal.
     * CalibFE exits to Standby RC — set frequency AFTER calibration. */
    {
        uint32_t freq_step = g_lr2021.frequency_hz / 4000000;
        uint16_t freq1 = (uint16_t)(freq_step | (1U << 15)); /* HF path */
        uint8_t cal_args[6] = {
            (freq1 >> 8) & 0xFF, freq1 & 0xFF,
            0x00, 0x00,
            0x00, 0x00
        };
        int cal_ret = lr2021_write_command(0x0123, cal_args, 6);
        if (cal_ret < 0) {
            LOG_WRN("CalibFE failed: %d", cal_ret);
        } else {
            LOG_INF("CalibFE done for %u MHz", g_lr2021.frequency_hz / 1000000);
        }
    }

    lr2021_set_frequency(g_lr2021.frequency_hz);

    lr2021_set_modulation(RADIO_MOD_FSK);
    lr2021_set_bitrate(g_lr2021.bitrate_bps);

    /* --- Set default syncword (same as CC1121 for cross-compat) --- */
    {
        uint8_t sync_args[10];
        sync_args[0] = 0x93; sync_args[1] = 0x0B;
        sync_args[2] = 0x51; sync_args[3] = 0xDE;
        sync_args[4] = 0x00; sync_args[5] = 0x00;
        sync_args[6] = 0x00; sync_args[7] = 0x00;
        sync_args[8] = 0x00;  /* MSB first */
        sync_args[9] = 32;    /* 32-bit syncword */
        lr2021_write_command(LR2021_CMD_SET_FSK_SYNCWORD, sync_args, 10);
    }

    /* --- Default FSK packet params (variable length, 8-bit header, no CRC) --- */
    {
        uint8_t pkt_args[8];
        memset(pkt_args, 0, sizeof(pkt_args));
        pkt_args[0] = 0x00;
        pkt_args[1] = 0x08;  /* preamble: 8 bytes */
        pkt_args[2] = 0x08;  /* preamble detect: 8 bytes */
        pkt_args[3] = 0x01;  /* pkt_format=1: variable length (8-bit header) */
        pkt_args[4] = 0x00;
        pkt_args[5] = 0xFF;  /* max payload length */
        pkt_args[6] = 0x00;  /* CRC off, dc_free off */
        pkt_args[7] = 0x00;
        lr2021_write_command(LR2021_CMD_SET_FSK_PKT_PARAMS, pkt_args, 8);
    }

    g_lr2021.initialized = true;
    LOG_INF("LR2021 ready: %u Hz, %u bps",
            g_lr2021.frequency_hz, g_lr2021.bitrate_bps);
    return 0;
}

static int lr2021_deinit(void)
{
    if (!g_lr2021.initialized) {
        return 0;
    }

    uint8_t sleep_cfg[5] = { LR2021_SLEEP_RAM_RETENTION, 0, 0, 0, 0 };
    lr2021_write_command(LR2021_CMD_SET_SLEEP, sleep_cfg, 5);

    g_lr2021.initialized = false;
    g_lr2021.current_mode = RADIO_MODE_SLEEP;
    return 0;
}

static int lr2021_set_mode(radio_mode_t mode)
{
    if (!g_lr2021.initialized) {
        return -ENODEV;
    }

    int ret = 0;

    switch (mode) {
    case RADIO_MODE_SLEEP: {
        uint8_t args[5] = { LR2021_SLEEP_RAM_RETENTION, 0, 0, 0, 0 };
        ret = lr2021_write_command(LR2021_CMD_SET_SLEEP, args, 5);
        break;
    }
    case RADIO_MODE_STANDBY: {
        uint8_t m = LR2021_STANDBY_XOSC;
        ret = lr2021_write_command(LR2021_CMD_SET_STANDBY, &m, 1);
        break;
    }
    case RADIO_MODE_RX: {
        /* Select Rx path based on frequency: HF (>=500MHz) or LF (<500MHz) */
        uint8_t rx_path = (g_lr2021.frequency_hz >= 500000000) ? 1 : 0;
        uint8_t rx_boost = rx_path ? 4 : 0;  /* datasheet recommended defaults */
        uint8_t path_args[2] = { rx_path, rx_boost };
        ret = lr2021_write_command(LR2021_CMD_SET_RX_PATH, path_args, 2);
        if (ret < 0) {
            break;
        }
        uint8_t timeout[3] = { 0x00, 0x00, 0x00 };  /* continuous */
        ret = lr2021_write_command(LR2021_CMD_SET_RX, timeout, 3);
        break;
    }
    case RADIO_MODE_TX:
        /* TX mode is entered during lr2021_tx() */
        break;
    default:
        return -EINVAL;
    }

    if (ret == 0 && mode != RADIO_MODE_TX) {
        g_lr2021.current_mode = mode;
    }
    return ret;
}

static int lr2021_set_frequency(uint32_t freq_hz)
{
    if (!g_lr2021.initialized) {
        return -ENODEV;
    }

    if (freq_hz < 150000000 || freq_hz > 960000000) {
        LOG_ERR("Frequency %u Hz out of range (150-960 MHz)", freq_hz);
        return -EINVAL;
    }

    uint8_t args[4];
    args[0] = (freq_hz >> 24) & 0xFF;
    args[1] = (freq_hz >> 16) & 0xFF;
    args[2] = (freq_hz >> 8) & 0xFF;
    args[3] = freq_hz & 0xFF;

    int ret = lr2021_write_command(LR2021_CMD_SET_RF_FREQUENCY, args, 4);
    if (ret == 0) {
        g_lr2021.frequency_hz = freq_hz;
        LOG_INF("LR2021 frequency: %u Hz", freq_hz);
    }
    return ret;
}

static int lr2021_set_power(int8_t dbm)
{
    if (!g_lr2021.initialized) {
        return -ENODEV;
    }

    /* Clamp to chip limits (-17 to +22 dBm sub-GHz) */
    if (dbm < -17) dbm = -17;
    if (dbm > 22)  dbm = 22;

    /* SetTxParams: tx_power(8) | ramp_time(8)
     *   tx_power is the target power in dBm (chip handles PA config internally
     *   for LR2021 when using default PA settings). */
    uint8_t args[2];
    args[0] = (uint8_t)dbm;
    args[1] = 0x02;  /* ramp_time: 40 µs */

    int ret = lr2021_write_command(LR2021_CMD_SET_TX_PARAMS, args, 2);
    if (ret == 0) {
        g_lr2021.tx_power_dbm = dbm;
        LOG_INF("LR2021 power: %d dBm", dbm);
    }
    return ret;
}

static int lr2021_set_modulation(radio_modulation_t mod)
{
    if (!g_lr2021.initialized) {
        return -ENODEV;
    }

    /* Select packet type based on modulation */
    uint8_t pkt_type;
    switch (mod) {
    case RADIO_MOD_FSK:
    case RADIO_MOD_GFSK:
        pkt_type = LR2021_PKT_TYPE_FSK;
        break;
    case RADIO_MOD_LORA:
        pkt_type = LR2021_PKT_TYPE_LORA;
        break;
    default:
        LOG_WRN("Modulation %d not supported, using FSK", mod);
        pkt_type = LR2021_PKT_TYPE_FSK;
        break;
    }

    return lr2021_write_command(LR2021_CMD_SET_PACKET_TYPE, &pkt_type, 1);
}

static int lr2021_set_bitrate(uint32_t bps)
{
    if (!g_lr2021.initialized) {
        return -ENODEV;
    }

    /* FSK modulation params: bitrate(32) | pulse_shape(8) | rx_bw(8) | fdev(24)
     *   pulse_shape: 0x00=none, 0x05=Gaussian BT=0.5 (datasheet §11.3.1)
     *   rx_bw: 0xFF=auto (datasheet: optimal BW from bitrate/deviation) */
    uint32_t fdev = bps * 5;  /* deviation = 5× bitrate for 2-FSK */

    uint8_t args[10];
    args[0] = (bps >> 24) & 0xFF;
    args[1] = (bps >> 16) & 0xFF;
    args[2] = (bps >> 8) & 0xFF;
    args[3] = bps & 0xFF;
    args[4] = 0x00;  /* pulse_shape: no shaping */
    args[5] = 0xFF;  /* rx_bw: auto */
    args[6] = (fdev >> 16) & 0xFF;
    args[7] = (fdev >> 8) & 0xFF;
    args[8] = fdev & 0xFF;

    int ret = lr2021_write_command(LR2021_CMD_SET_FSK_MOD_PARAMS, args, 9);
    if (ret == 0) {
        g_lr2021.bitrate_bps = bps;
        LOG_INF("LR2021 FSK bitrate: %u bps, fdev: %u Hz", bps, fdev);
    }
    return ret;
}

static int lr2021_tx(const uint8_t *data, size_t len)
{
    if (!g_lr2021.initialized) {
        return -ENODEV;
    }
    if (!data || len == 0 || len > 255) {
        return -EINVAL;
    }

    /* Update FSK packet params: max length = this payload. */
    {
        uint8_t pkt_args[8];
        memset(pkt_args, 0, sizeof(pkt_args));
        pkt_args[0] = 0x00;
        pkt_args[1] = 0x08;  /* preamble: 8 bytes */
        pkt_args[2] = 0x08;  /* preamble detect: 8 bytes */
        pkt_args[3] = 0x01;  /* pkt_format=1: variable length */
        pkt_args[4] = (uint8_t)((len + 1) >> 8);  /* +1 for length byte */
        pkt_args[5] = (uint8_t)(len + 1);
        pkt_args[6] = 0x00;
        pkt_args[7] = 0x00;

        lr2021_write_command(LR2021_CMD_SET_FSK_PKT_PARAMS, pkt_args, 8);
    }

    /* --- Write [length byte | payload] to TX FIFO.
     * In variable-length mode (pkt_format=1), the first byte in the TX FIFO
     * is the length byte (datasheet: 8-bit header). */
    {
        int ret = lr2021_wait_busy();
        if (ret < 0) {
            return ret;
        }

        uint8_t op_buf[2] = { (LR2021_CMD_WRITE_TX_FIFO >> 8) & 0xFF,
                               LR2021_CMD_WRITE_TX_FIFO & 0xFF };
        uint8_t lbuf = (uint8_t)len;

        lr2021_cs_low();

        struct spi_buf tx[3] = {
            { .buf = op_buf, .len = 2 },
            { .buf = &lbuf, .len = 1 },
            { .buf = (void *)data, .len = len },
        };
        struct spi_buf_set tx_set = { .buffers = tx, .count = 3 };
        ret = spi_write_dt(&g_lr2021.spi, &tx_set);

        lr2021_cs_high();

        if (ret < 0) {
            LOG_ERR("TX FIFO write failed: %d", ret);
            return ret;
        }
    }

    /* --- Trigger TX --- */
    {
        uint8_t timeout[3] = { (LR2021_TX_TIMEOUT_3S >> 16) & 0xFF,
                               (LR2021_TX_TIMEOUT_3S >> 8) & 0xFF,
                                LR2021_TX_TIMEOUT_3S & 0xFF };
        int ret = lr2021_write_command(LR2021_CMD_SET_TX, timeout, 3);
        if (ret < 0) {
            return ret;
        }
    }

    g_lr2021.current_mode = RADIO_MODE_STANDBY;  /* Auto fallback to standby */
    LOG_DBG("LR2021 TX: %zu bytes at %u Hz, %d dBm",
            len, g_lr2021.frequency_hz, g_lr2021.tx_power_dbm);
    return 0;
}

static int lr2021_rx(uint8_t *buffer, size_t max_len, uint32_t timeout_ms)
{
    if (!g_lr2021.initialized) {
        return -ENODEV;
    }
    if (!buffer || max_len == 0) {
        return -EINVAL;
    }

    /* --- Ensure standby, clear FIFO, set RX path, start RX --- */
    {
        uint8_t mode = LR2021_STANDBY_XOSC;
        lr2021_write_command(LR2021_CMD_SET_STANDBY, &mode, 1);
    }
    {
        int ret = lr2021_write_command(LR2021_CMD_CLEAR_RX_FIFO, NULL, 0);
        if (ret < 0) {
            LOG_ERR("ClearRxFifo failed: %d", ret);
            return ret;
        }
    }
    {
        uint8_t rx_path = (g_lr2021.frequency_hz >= 500000000) ? 1 : 0;
        uint8_t rx_boost = rx_path ? 4 : 0;
        uint8_t path_args[2] = { rx_path, rx_boost };
        int ret = lr2021_write_command(LR2021_CMD_SET_RX_PATH, path_args, 2);
        if (ret < 0) {
            LOG_ERR("SetRxPath failed: %d", ret);
            return ret;
        }
    }
    {
        uint32_t timeout_units = timeout_ms * 1000 / 32;  /* ms → 32 µs units */
        if (timeout_units == 0) timeout_units = 0xFFFFFF; /* continuous */
        uint8_t rx_args[3];
        rx_args[0] = (timeout_units >> 16) & 0xFF;
        rx_args[1] = (timeout_units >> 8) & 0xFF;
        rx_args[2] = timeout_units & 0xFF;

        int ret = lr2021_write_command(LR2021_CMD_SET_RX, rx_args, 3);
        if (ret < 0) {
            LOG_ERR("SetRx failed: %d", ret);
            return ret;
        }
    }

    g_lr2021.current_mode = RADIO_MODE_RX;

    /* Poll IRQ for RX_DONE or TIMEOUT.
     * GetAndClearIrqStatus returns: Stat(16) | IrqStatus(31:24..7:0) = 6 bytes.
     * lr2021_read_command strips Stat(16) → caller gets 4 bytes of IrqStatus. */
    int64_t deadline = k_uptime_get() + (timeout_ms ? timeout_ms + 500 : 2500);
    uint32_t irq = 0;
    bool rx_done = false;
    int poll_count = 0;
    uint32_t last_irq = 0;

    while (k_uptime_get() < deadline) {
        uint8_t irq_buf[4] = { 0 };
        int ret = lr2021_read_command(LR2021_CMD_GET_AND_CLEAR_IRQ,
                                       NULL, 0, irq_buf, 4);
        if (ret < 0) {
            LOG_ERR("GetAndClearIrqStatus failed: %d", ret);
            lr2021_write_command(LR2021_CMD_SET_STANDBY,
                                 (uint8_t[]){ LR2021_STANDBY_RC }, 1);
            g_lr2021.current_mode = RADIO_MODE_STANDBY;
            return ret;
        }
        /* Response byte order: IrqStatus(31:24) | (23:16) | (15:8) | (7:0) */
        irq = ((uint32_t)irq_buf[0] << 24) | ((uint32_t)irq_buf[1] << 16) |
              ((uint32_t)irq_buf[2] << 8)  |  (uint32_t)irq_buf[3];

        if (poll_count % 25 == 0 || irq != last_irq) {
            last_irq = irq;
        }

        if (irq & LR2021_IRQ_RX_DONE) {
            rx_done = true;
            break;
        }
        if (irq & LR2021_IRQ_TIMEOUT) {
            LOG_DBG("LR2021 RX timeout IRQ (no packet detected)");
            lr2021_write_command(LR2021_CMD_SET_STANDBY,
                                 (uint8_t[]){ LR2021_STANDBY_RC }, 1);
            g_lr2021.current_mode = RADIO_MODE_STANDBY;
            return 0;
        }
        if (irq & LR2021_IRQ_CRC_ERROR) {
            LOG_WRN("LR2021 CRC error during poll");
        }
        if (irq & LR2021_IRQ_LEN_ERROR) {
            LOG_WRN("LR2021 packet length error");
        }

        k_msleep(20);
    }

    if (!rx_done) {
        LOG_WRN("LR2021 RX poll timeout — no RxDone IRQ after %d polls, last irq=0x%08X",
                poll_count, irq);
        lr2021_write_command(LR2021_CMD_SET_STANDBY,
                             (uint8_t[]){ LR2021_STANDBY_RC }, 1);
        g_lr2021.current_mode = RADIO_MODE_STANDBY;
        return 0;
    }

    if (irq & LR2021_IRQ_CRC_ERROR) {
        LOG_WRN("LR2021 CRC error");
    }

    /* LR2021 variable-length mode stores [length_byte][payload…] in RX FIFO.
     * Read the length byte first, then exactly that many payload bytes. */
    {
        uint8_t lbuf = 0;
        int ret = lr2021_read_fifo(&lbuf, 1);
        if (ret < 0) {
            LOG_ERR("RX FIFO length read failed: %d", ret);
            lr2021_write_command(LR2021_CMD_SET_STANDBY,
                                 (uint8_t[]){ LR2021_STANDBY_RC }, 1);
            g_lr2021.current_mode = RADIO_MODE_STANDBY;
            return ret;
        }

        size_t pkt_len = lbuf;
        if (pkt_len == 0 || pkt_len > max_len) {
            LOG_WRN("LR2021 bad pkt_len=%zu (max=%zu)", pkt_len, max_len);
            lr2021_write_command(LR2021_CMD_CLEAR_RX_FIFO, NULL, 0);
            lr2021_write_command(LR2021_CMD_SET_STANDBY,
                                 (uint8_t[]){ LR2021_STANDBY_RC }, 1);
            g_lr2021.current_mode = RADIO_MODE_STANDBY;
            return 0;
        }

        ret = lr2021_read_fifo(buffer, pkt_len);
        if (ret < 0) {
            LOG_ERR("RX FIFO read failed: %d", ret);
            lr2021_write_command(LR2021_CMD_SET_STANDBY,
                                 (uint8_t[]){ LR2021_STANDBY_RC }, 1);
            g_lr2021.current_mode = RADIO_MODE_STANDBY;
            return ret;
        }

        lr2021_write_command(LR2021_CMD_CLEAR_RX_FIFO, NULL, 0);
        lr2021_write_command(LR2021_CMD_SET_STANDBY,
                             (uint8_t[]){ LR2021_STANDBY_RC }, 1);
        g_lr2021.current_mode = RADIO_MODE_STANDBY;

        if (g_lr2021.event_cb) {
            radio_event_t ev = {
                .type = RADIO_EVENT_RX_DONE,
                .data = buffer,
                .len = pkt_len,
                .rssi = 0,
                .user_data = g_lr2021.event_user_data,
            };
            g_lr2021.event_cb(&ev, g_lr2021.event_user_data);
        }

        return (int)pkt_len;
    }
}

static int lr2021_get_rssi(int16_t *rssi)
{
    if (!g_lr2021.initialized || !rssi) {
        return -ENODEV;
    }

    if (g_lr2021.frequency_hz == 0) {
        LOG_ERR("Frequency not set");
        return -EINVAL;
    }

    /* Ensure chip is in RX (PLL locked) before reading RSSI.
     * rx_timeout=0 means single mode: stays in RX until packet or mode change. */
    radio_mode_t prev_mode = g_lr2021.current_mode;

    if (prev_mode != RADIO_MODE_RX) {
        int ret = lr2021_set_mode(RADIO_MODE_RX);
        if (ret < 0) {
            return ret;
        }
        k_msleep(50);
    }

    /* GetRssiInst is a read command (datasheet §7.2.3): opcode→BUSY→read.
     * Response after Stat(16): Rssi(8:1) | Rssi(0)+rfu(6:0).
     * Read 4 bytes from lr2021_read_command (which strips the 2 Stat bytes). */
    uint8_t rsp[2] = { 0 };
    int ret = lr2021_read_command(LR2021_CMD_GET_RSSI_INST, NULL, 0, rsp, 2);

    if (prev_mode != RADIO_MODE_RX) {
        lr2021_set_mode(prev_mode);
    }

    if (ret < 0) {
        LOG_ERR("GetRssiInst failed: %d", ret);
        return ret;
    }

    uint16_t rssi_9bit = ((uint16_t)rsp[0] << 1) | (rsp[1] >> 7);
    *rssi = -(int16_t)rssi_9bit / 2;
    return 0;
}

static int lr2021_set_event_callback(radio_handle_t *handle, radio_event_cb_t cb, void *user_data)
{
    ARG_UNUSED(handle);
    g_lr2021.event_cb = cb;
    g_lr2021.event_user_data = user_data;
    return 0;
}

/* =========================================================================
 * radio_ops_t vtable shims (handle param unused — driver uses global state)
 * ========================================================================= */

static int lr2021_ops_init(radio_handle_t *h)        { ARG_UNUSED(h); return lr2021_init(); }
static int lr2021_ops_deinit(radio_handle_t *h)      { ARG_UNUSED(h); return lr2021_deinit(); }
static int lr2021_ops_send(radio_handle_t *h, const uint8_t *d, size_t l) { ARG_UNUSED(h); return lr2021_tx(d, l); }
static int lr2021_ops_recv(radio_handle_t *h, uint8_t *b, size_t l, uint32_t t) { ARG_UNUSED(h); return lr2021_rx(b, l, t); }
static int lr2021_ops_set_frequency(radio_handle_t *h, uint32_t hz) { ARG_UNUSED(h); return lr2021_set_frequency(hz); }
static int lr2021_ops_set_power(radio_handle_t *h, int8_t dbm)      { ARG_UNUSED(h); return lr2021_set_power(dbm); }
static int lr2021_ops_get_rssi(radio_handle_t *h, int16_t *r)       { ARG_UNUSED(h); return lr2021_get_rssi(r); }
static int lr2021_ops_set_mode(radio_handle_t *h, radio_mode_t m)   { ARG_UNUSED(h); return lr2021_set_mode(m); }
static int lr2021_ops_set_modulation(radio_handle_t *h, radio_modulation_t m) { ARG_UNUSED(h); return lr2021_set_modulation(m); }
static int lr2021_ops_set_bitrate(radio_handle_t *h, uint32_t bps)  { ARG_UNUSED(h); return lr2021_set_bitrate(bps); }

static const radio_ops_t lr2021_ops = {
    .init               = lr2021_ops_init,
    .deinit             = lr2021_ops_deinit,
    .send               = lr2021_ops_send,
    .recv               = lr2021_ops_recv,
    .set_event_callback = lr2021_set_event_callback,
    .set_frequency      = lr2021_ops_set_frequency,
    .set_power          = lr2021_ops_set_power,
    .get_rssi           = lr2021_ops_get_rssi,
    .set_mode           = lr2021_ops_set_mode,
    .set_modulation     = lr2021_ops_set_modulation,
    .set_bitrate        = lr2021_ops_set_bitrate,
};

static radio_handle_t lr2021_handle = {
    .type         = RADIO_TYPE_SUBGHZ,
    .name         = "LR2021",
    .capabilities = RADIO_CAP_TX | RADIO_CAP_RX | RADIO_CAP_CCA | RADIO_CAP_RAW_MODE |
                    RADIO_CAP_LOW_POWER | RADIO_CAP_BAND_SUBGHZ | RADIO_CAP_BAND_2GHZ4 |
                    RADIO_CAP_MOD_FSK | RADIO_CAP_MOD_LORA | RADIO_CAP_MOD_BPSK |
                    RADIO_CAP_MOD_FLRC | RADIO_CAP_MOD_BLE_PHY | RADIO_CAP_MOD_OQPSK,
    .ops          = &lr2021_ops,
};

radio_handle_t *lr2021_get_handle(void)
{
    return &lr2021_handle;
}

/* =========================================================================
 * Auto-register at boot
 * ========================================================================= */

#ifdef CONFIG_AKIRA_LR2021
static int lr2021_auto_register(void)
{
    int ret = radio_manager_register(&lr2021_handle);
    if (ret < 0 && ret != -EALREADY) {
        LOG_ERR("Failed to register LR2021: %d", ret);
        return ret;
    }
    LOG_INF("LR2021 registered with radio_manager");
    return 0;
}

SYS_INIT(lr2021_auto_register, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
#endif /* CONFIG_AKIRA_LR2021 */
