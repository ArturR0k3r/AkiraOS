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
#define LR2021_CMD_SET_DIO_FUNC         0x0112
#define LR2021_CMD_SET_DIO_IRQ_CFG      0x0115
#define LR2021_CMD_SET_SLEEP            0x0127
#define LR2021_CMD_SET_STANDBY          0x0128
#define LR2021_CMD_SET_FS               0x0129

/* DIO function: (func<<4 | drive); func 0x1 = IRQ output, drive 0x2 = PULL_UP.
 * Datasheet §6.8.1: "For DIO5, only DIO_SLEEP_PULL_UP is accepted. Otherwise
 * the command returns FAIL." — 0x10 (PULL_NONE) silently fails, leaving DIO5
 * at its default hardware pull-up (always HIGH), so 0x12 is mandatory. */
#define LR2021_DIO_FUNC_IRQ             0x12
/* DIO pin (5..11) carrying the host IRQ; board-defined via `irq-dio` DT prop. */
#define LR2021_IRQ_DIO                  DT_PROP_OR(LR2021_NODE, irq_dio, 5)

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
#define LR2021_CMD_SET_FSK_CRC_PARAMS   0x0243
#define LR2021_CMD_SET_FSK_SYNCWORD     0x0244

/* =========================================================================
 * Packet types (datasheet Table 8-1)
 * ========================================================================= */
#define LR2021_PKT_TYPE_LORA            0x00
#define LR2021_PKT_TYPE_FSK             0x02
#define LR2021_PKT_TYPE_BLE             0x03
#define LR2021_PKT_TYPE_RTTOF           0x04
#define LR2021_PKT_TYPE_FLRC            0x05
#define LR2021_PKT_TYPE_BPSK            0x06

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
    struct gpio_dt_spec irq;        /* optional (irq-gpios); {0} if absent */
    struct gpio_callback irq_cb;
    struct k_sem irq_sem;
    bool use_irq;                   /* true => interrupt-driven RX, no polling */
    bool rx_armed;                  /* true => chip held in continuous RX */
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

static void lr2021_irq_handler(const struct device *port, struct gpio_callback *cb,
                               gpio_port_pins_t pins)
{
    ARG_UNUSED(port);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);
    k_sem_give(&g_lr2021.irq_sem);
}

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

static inline void lr2021_cs_low(void)
{
    gpio_pin_set_dt(&g_lr2021.cs, 1);
    k_usleep(1);
}

static inline void lr2021_cs_high(void)
{
    k_usleep(1);
    gpio_pin_set_dt(&g_lr2021.cs, 0);
}

static int lr2021_spi_write(const uint8_t *data, size_t len)
{
    struct spi_buf tx = { .buf = (void *)data, .len = len };
    struct spi_buf_set tx_set = { .buffers = &tx, .count = 1 };
    return spi_write_dt(&g_lr2021.spi, &tx_set);
}

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

    ret = lr2021_wait_busy();
    if (ret < 0) {
        LOG_ERR("Cmd 0x%04X BUSY timeout (phase2)", opcode);
        return ret;
    }

    /* Read response: send dummy bytes, MISO returns Stat(16) + data.
     * Read into local buffer to avoid overflowing the caller's rsp. */
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

    size_t tx_len = 2 + len;  /* opcode(2) + dummy(len) → stat(2) + data(len) */
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

    memcpy(data, rx + 2, len);  /* skip stat(2), copy data */
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

    /* --- IRQ GPIO (optional) --------------------------------------------- */
    g_lr2021.irq = (struct gpio_dt_spec)GPIO_DT_SPEC_GET_OR(LR2021_NODE, irq_gpios, {0});
    if (g_lr2021.irq.port && gpio_is_ready_dt(&g_lr2021.irq)) {
        ret = gpio_pin_configure_dt(&g_lr2021.irq, GPIO_INPUT);
        if (ret < 0) {
            LOG_ERR("IRQ GPIO config failed: %d", ret);
            return ret;
        }

        /* Init sem and register the callback BEFORE enabling the interrupt,
         * so a spurious edge can never hit an unready sem/callback. */
        k_sem_init(&g_lr2021.irq_sem, 0, 1);

        gpio_init_callback(&g_lr2021.irq_cb, lr2021_irq_handler,
                           BIT(g_lr2021.irq.pin));
        ret = gpio_add_callback(g_lr2021.irq.port, &g_lr2021.irq_cb);
        if (ret < 0) {
            LOG_ERR("IRQ callback add failed: %d", ret);
            return ret;
        }

        ret = gpio_pin_interrupt_configure_dt(&g_lr2021.irq,
                                              GPIO_INT_EDGE_TO_ACTIVE);
        if (ret < 0) {
            LOG_ERR("IRQ interrupt config failed: %d", ret);
            return ret;
        }

        /* Map DIO pin to IRQ output function.
         * SetDioFunc args: dio(8) | func(4)+drive(4); func 0x1 = IRQ output. */
        {
            uint8_t dio_args[2] = { LR2021_IRQ_DIO, LR2021_DIO_FUNC_IRQ };
            ret = lr2021_write_command(LR2021_CMD_SET_DIO_FUNC, dio_args, 2);
            if (ret < 0) {
                LOG_ERR("SetDioFunc failed: %d", ret);
                return ret;
            }
        }

        /* Configure which IRQ events assert the DIO pin.
         * SetDioIrqCfg args: dio(8) | irq_en(32) — mask of IRQ flags to unmask.
         * ONLY RX_DONE: in continuous RX (SetRx 0xFFFFFF) the chip fires an
         * internal TIMEOUT every ~1 s when no preamble is detected. Routing
         * TIMEOUT to DIO would lock DIO HIGH (host never clears it between
         * timeouts) and prevent the RX_DONE rising edge from firing the ISR.
         * The daemon already wakes every 1 s via k_sem_take timeout, so
         * TIMEOUT does not need to assert DIO. */
        {
            uint32_t irq_mask = LR2021_IRQ_RX_DONE;
            uint8_t irq_args[5] = {
                LR2021_IRQ_DIO,
                (irq_mask >> 24) & 0xFF,
                (irq_mask >> 16) & 0xFF,
                (irq_mask >>  8) & 0xFF,
                 irq_mask        & 0xFF,
            };
            ret = lr2021_write_command(LR2021_CMD_SET_DIO_IRQ_CFG, irq_args, 5);
            if (ret < 0) {
                LOG_ERR("SetDioIrqCfg failed: %d", ret);
                return ret;
            }
        }

        g_lr2021.use_irq = true;
        LOG_INF("LR2021 IRQ on DIO%u (GPIO port %s pin %u)",
                LR2021_IRQ_DIO, g_lr2021.irq.port->name, g_lr2021.irq.pin);
    } else {
        LOG_INF("LR2021: no IRQ GPIO — using polling mode");
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
        /* CalibrateFrontEnd (0x0123): value = (rx_path==HF ? 0x8000 : 0) |
         * ceil(freq/4MHz). 868MHz is LF => bit15 MUST be 0. Calibrating the HF
         * path for an LF freq leaves the RX LF front-end uncalibrated (Error
         * bit9 RXFREQ_NO_FRONT_END_CALIB) and RX can't demodulate. */
        uint32_t freq_4mhz = (g_lr2021.frequency_hz + 4000000 - 1) / 4000000;
        uint8_t hf = (g_lr2021.frequency_hz >= 1000000000U) ? 0x80 : 0x00;
        uint16_t freq1 = (uint16_t)freq_4mhz;
        uint8_t cal_args[6] = {
            (uint8_t)(hf | ((freq1 >> 8) & 0x7F)), freq1 & 0xFF,
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

    /* Mark initialized BEFORE the config calls: set_frequency/set_modulation/
     * set_bitrate all guard on `initialized` and would otherwise return -ENODEV,
     * leaving the chip on default modulation (never applying bitrate/fdev). */
    g_lr2021.initialized = true;

    lr2021_set_frequency(g_lr2021.frequency_hz);

    lr2021_set_modulation(RADIO_MOD_FSK);
    lr2021_set_bitrate(g_lr2021.bitrate_bps);

    /* 32-bit syncword (MSB-first). SetFskSyncWord: 8 sync bytes + packed byte
     * = bit_order(bit7)|nb_bits. nb_bits<64 uses the LOW bytes [4..7]. */
    {
        uint8_t sync_args[9];
        sync_args[0] = 0x00; sync_args[1] = 0x00;
        sync_args[2] = 0x00; sync_args[3] = 0x00;
        sync_args[4] = 0x93; sync_args[5] = 0x0B;
        sync_args[6] = 0x51; sync_args[7] = 0xDE;
        sync_args[8] = (1 << 7) | 32;  /* bit_order=1 (MSB) | nb_bits=32 */
        lr2021_write_command(LR2021_CMD_SET_FSK_SYNCWORD, sync_args, 9);
    }

    /* --- Default FSK packet params (variable length, 8-bit header, 2-byte CRC) ---
     * Byte 8 = Crc(3:0) | dc_free(3:0): Crc=0x2 (2-byte IBM) upper nibble,
     * dc_free=0x1 (whitening) lower nibble → 0x21. Whitening prevents long runs
     * of identical bits that corrupt clock recovery and cause burst CRC errors. */
    {
        uint8_t pkt_args[7] = { 0x00, 0x40, 0x10, 0x01, 0x00, 0xFF, 0x21 };
        lr2021_write_command(LR2021_CMD_SET_FSK_PKT_PARAMS, pkt_args, 7);
    }
    /* --- IBM CRC params: polynomial 0x8005, init value 0xFFFF --- */
    {
        uint8_t crc_args[8] = {
            0x00, 0x00, 0x80, 0x05,  /* polynom = 0x00008005 */
            0x00, 0x00, 0xFF, 0xFF,  /* init    = 0x0000FFFF */
        };
        lr2021_write_command(LR2021_CMD_SET_FSK_CRC_PARAMS, crc_args, 8);
    }

    LOG_INF("LR2021 ready: %u Hz, %u bps",
            g_lr2021.frequency_hz, g_lr2021.bitrate_bps);
    return 0;
}

static int lr2021_deinit(void)
{
    if (!g_lr2021.initialized) {
        return 0;
    }

    if (g_lr2021.use_irq) {
        gpio_remove_callback(g_lr2021.irq.port, &g_lr2021.irq_cb);
        g_lr2021.use_irq = false;
    }
    g_lr2021.rx_armed = false;

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
        /* rx_path 0=LF (150-960MHz), 1=HF (1.5-2.5GHz). 868MHz => LF. */
        uint8_t rx_path = (g_lr2021.frequency_hz >= 1000000000U) ? 1 : 0;
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
    uint32_t fdev = bps;  /* deviation = bitrate (modulation index h=2) */

    /* rx_bw must be an enumerated code (Table 11-2); 0xFF is NOT valid and the
     * chip rejects it (CMD_ERROR). Use the Carson bandwidth 2*(fdev+bitrate/2).
     * Do NOT over-widen: if rx_bw >> 2*fdev the FSK discriminator cannot resolve
     * the deviation and the demod/preamble detection fails. h=2 keeps the
     * deviation large enough relative to this filter and tolerates xtal offset. */
    uint32_t carson_bw = 2 * (fdev + bps / 2);
    uint8_t rx_bw_code;
    if      (carson_bw <=   9600) rx_bw_code =  38;
    else if (carson_bw <=  12000) rx_bw_code =  30;
    else if (carson_bw <=  19200) rx_bw_code =  37;
    else if (carson_bw <=  24000) rx_bw_code =  29;
    else if (carson_bw <=  38500) rx_bw_code =  36;
    else if (carson_bw <=  48100) rx_bw_code =  28;
    else if (carson_bw <=  55600) rx_bw_code = 227;
    else if (carson_bw <=  64100) rx_bw_code =  20;
    else if (carson_bw <=  96200) rx_bw_code =  27;
    else if (carson_bw <= 128200) rx_bw_code =  19;
    else if (carson_bw <= 192300) rx_bw_code =  26;
    else if (carson_bw <= 384600) rx_bw_code =  25;
    else                          rx_bw_code =  24;

    uint8_t args[10];
    args[0] = (bps >> 24) & 0xFF;
    args[1] = (bps >> 16) & 0xFF;
    args[2] = (bps >> 8) & 0xFF;
    args[3] = bps & 0xFF;
    args[4] = 0x00;  /* pulse_shape: no shaping */
    args[5] = rx_bw_code;
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

    /* Go to standby before config commands. Semtech chips require Standby (not
     * RX/TX) for SetFskPacketParams and FIFO ops to take effect. If the daemon
     * left the chip in continuous RX, calling SetFskPacketParams from RX mode
     * is silently rejected, resulting in a stale pld_len on air. */
    {
        uint8_t mode = LR2021_STANDBY_XOSC;
        lr2021_write_command(LR2021_CMD_SET_STANDBY, &mode, 1);
    }

    lr2021_write_command(LR2021_CMD_CLEAR_TX_FIFO, NULL, 0);

    /* Update FSK packet params: max length = this payload. */
    {
        uint8_t pkt_args[8];
        memset(pkt_args, 0, sizeof(pkt_args));
        pkt_args[0] = 0x00;
        pkt_args[1] = 0x40;  /* pbl_len_tx: 64 bits (clock recovery) */
        pkt_args[2] = 0x10;  /* pbl_detect: 16 bits */
        pkt_args[3] = 0x01;  /* header_mode=1: 8-bit variable length header */
        pkt_args[4] = (uint8_t)(len >> 8);  /* pld_len = payload only (no manual byte) */
        pkt_args[5] = (uint8_t)len;
        pkt_args[6] = 0x21;  /* Crc2Byte (0x2) | whitening (0x1) */

        lr2021_write_command(LR2021_CMD_SET_FSK_PKT_PARAMS, pkt_args, 7);
    }

    /* --- Write payload only to TX FIFO (official driver scheme).
     * In variable-length mode the hardware adds the 8-bit length header on air;
     * do NOT add a manual length byte (it corrupts byte alignment). */
    {
        int ret = lr2021_wait_busy();
        if (ret < 0) {
            return ret;
        }

        uint8_t op_buf[2] = { (LR2021_CMD_WRITE_TX_FIFO >> 8) & 0xFF,
                               LR2021_CMD_WRITE_TX_FIFO & 0xFF };

        lr2021_cs_low();

        struct spi_buf tx[2] = {
            { .buf = op_buf, .len = 2 },
            { .buf = (void *)data, .len = len },
        };
        struct spi_buf_set tx_set = { .buffers = tx, .count = 2 };
        ret = spi_write_dt(&g_lr2021.spi, &tx_set);

        lr2021_cs_high();

        if (ret < 0) {
            LOG_ERR("TX FIFO write failed: %d", ret);
            return ret;
        }
    }

    {
        uint8_t timeout[3] = { (LR2021_TX_TIMEOUT_3S >> 16) & 0xFF,
                               (LR2021_TX_TIMEOUT_3S >> 8) & 0xFF,
                                LR2021_TX_TIMEOUT_3S & 0xFF };
        int ret = lr2021_write_command(LR2021_CMD_SET_TX, timeout, 3);
        if (ret < 0) {
            return ret;
        }
    }

    /* Wait for TX_DONE before releasing the lock.  SET_TX only means the chip
     * accepted the command and started transmitting — BUSY goes low well before
     * the last bit is on air.  Without this wait the RX thread (or any caller
     * that immediately re-takes the radio lock) issues SET_STANDBY and aborts
     * the in-flight frame, causing a truncated packet → CRC error at the peer. */
    {
        int64_t tx_deadline = k_uptime_get() + 3000;
        while (k_uptime_get() < tx_deadline) {
            uint8_t irq_buf[4] = { 0 };
            if (lr2021_read_command(LR2021_CMD_GET_AND_CLEAR_IRQ,
                                    NULL, 0, irq_buf, 4) < 0) {
                break;
            }
            uint32_t irq = ((uint32_t)irq_buf[0] << 24) |
                           ((uint32_t)irq_buf[1] << 16) |
                           ((uint32_t)irq_buf[2] <<  8) |
                            (uint32_t)irq_buf[3];
            if (irq & (LR2021_IRQ_TX_DONE | LR2021_IRQ_TIMEOUT)) {
                if (irq & LR2021_IRQ_TIMEOUT) {
                    LOG_ERR("LR2021 TX timeout waiting for TX_DONE");
                }
                break;
            }
            k_usleep(2000);
        }
    }

    g_lr2021.current_mode = RADIO_MODE_STANDBY;  /* Auto fallback to standby */
    g_lr2021.rx_armed = false;  /* left RX for TX — recv() re-arms continuous RX */
    LOG_DBG("LR2021 TX: %zu bytes at %u Hz, %d dBm",
            len, g_lr2021.frequency_hz, g_lr2021.tx_power_dbm);
    return 0;
}

/* Read one payload from the FIFO (polling path only — interrupt path reads
 * FIFO level before GET_AND_CLEAR_IRQ and bypasses this function). */
static int lr2021_read_packet(uint8_t *buffer, size_t max_len)
{
    uint8_t len_buf[2] = { 0 };
    int ret = lr2021_read_command(LR2021_CMD_GET_RX_FIFO_LEVEL, NULL, 0, len_buf, 2);
    if (ret < 0) {
        LOG_ERR("GetRxFifoLevel failed: %d", ret);
        return ret;
    }
    size_t pkt_len = ((size_t)len_buf[0] << 8) | len_buf[1];
    if (pkt_len == 0 || pkt_len > max_len) {
        LOG_WRN("LR2021 bad fifo_level=%zu (max=%zu)", pkt_len, max_len);
        return 0;
    }
    ret = lr2021_read_fifo(buffer, pkt_len);
    if (ret < 0) {
        LOG_ERR("RX FIFO read failed: %d", ret);
        return ret;
    }
    if (g_lr2021.event_cb) {
        radio_event_t ev = {
            .type = RADIO_EVENT_RX_DONE, .data = buffer, .len = pkt_len,
            .rssi = 0, .user_data = g_lr2021.event_user_data,
        };
        g_lr2021.event_cb(&ev, g_lr2021.event_user_data);
    }
    return (int)pkt_len;
}

/* Arm the chip in continuous RX (interrupt-driven mode). Stays in RX across
 * packets — FIFO reads advance the read pointer without leaving RX. */
static int lr2021_rx_arm_continuous(void)
{
    uint8_t mode = LR2021_STANDBY_XOSC;
    lr2021_write_command(LR2021_CMD_SET_STANDBY, &mode, 1);
    lr2021_write_command(LR2021_CMD_CLEAR_RX_FIFO, NULL, 0);

    uint8_t rx_path = (g_lr2021.frequency_hz >= 1000000000U) ? 1 : 0;
    uint8_t path_args[2] = { rx_path, (uint8_t)(rx_path ? 4 : 0) };
    int ret = lr2021_write_command(LR2021_CMD_SET_RX_PATH, path_args, 2);
    if (ret < 0) {
        LOG_ERR("SetRxPath failed: %d", ret);
        return ret;
    }

    /* Reset pld_len to max so RX accepts any payload size.
     * TX updates SetFskPacketParams with the TX payload length (pld_len = TX size),
     * which also acts as the max-receive-length. Without resetting here, the chip
     * silently discards packets larger than the last transmitted payload. */
    {
        uint8_t pkt_args[7] = { 0x00, 0x40, 0x10, 0x01, 0x00, 0xFF, 0x21 };
        lr2021_write_command(LR2021_CMD_SET_FSK_PKT_PARAMS, pkt_args, 7);
    }

    lr2021_write_command(LR2021_CMD_CLEAR_IRQ,
                         (uint8_t[]){ 0xFF, 0xFF, 0xFF, 0xFF }, 4);
    k_sem_reset(&g_lr2021.irq_sem);

    uint8_t rx_args[3] = { 0xFF, 0xFF, 0xFF };  /* timeout=continuous */
    ret = lr2021_write_command(LR2021_CMD_SET_RX, rx_args, 3);
    if (ret < 0) {
        LOG_ERR("SetRx (continuous) failed: %d", ret);
        return ret;
    }
    g_lr2021.current_mode = RADIO_MODE_RX;
    g_lr2021.rx_armed = true;
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

    /* --- Interrupt-driven continuous RX: arm once, then NON-blocking reads.
     * The blocking wait is done lock-free by the caller via lr2021_rx_wait().
     *
     * NOTE: The DIO GPIO is treated as an unreliable wake hint (line can fire
     * from noise). GET_AND_CLEAR_IRQ is always the authoritative RX_DONE check.
     * This path is polled every 1 s via the rx_wait timeout; the ISR just
     * accelerates the wake. */
    if (g_lr2021.use_irq) {
        if (!g_lr2021.rx_armed) {
            int ret = lr2021_rx_arm_continuous();
            if (ret < 0) {
                return ret;
            }
        }
        /* Read FIFO level BEFORE GET_AND_CLEAR_IRQ: the chip resets
         * GetRxFifoLevel to 0 when it re-arms continuous RX as a side-effect
         * of the IRQ-clear SPI command. Reading first gives the true byte count. */
        uint8_t lvl_buf[4] = { 0 };
        lr2021_read_command(LR2021_CMD_GET_RX_FIFO_LEVEL, NULL, 0, lvl_buf, 4);
        size_t pre_level = ((size_t)lvl_buf[0] << 8) | lvl_buf[1];

        uint8_t irq_buf[4] = { 0 };
        if (lr2021_read_command(LR2021_CMD_GET_AND_CLEAR_IRQ, NULL, 0, irq_buf, 4) < 0) {
            return -EIO;
        }
        uint32_t irq = ((uint32_t)irq_buf[0] << 24) | ((uint32_t)irq_buf[1] << 16) |
                       ((uint32_t)irq_buf[2] << 8)  |  (uint32_t)irq_buf[3];
        if (irq) {
            LOG_DBG("IRQ=0x%08X pre_level=%zu", irq, pre_level);
        }
        if (!(irq & LR2021_IRQ_RX_DONE)) {
            return 0;
        }
        if (irq & LR2021_IRQ_CRC_ERROR) {
            LOG_WRN("LR2021 CRC error — discarding packet");
            if (pre_level > 0 && pre_level <= max_len) {
                lr2021_read_fifo(buffer, pre_level);
            }
            return 0;
        }
        if (pre_level == 0 || pre_level > max_len) {
            LOG_WRN("LR2021 bad pre_level=%zu (max=%zu)", pre_level, max_len);
            return 0;
        }
        int n = lr2021_read_fifo(buffer, pre_level);
        if (n < 0) {
            return n;
        }
        if (g_lr2021.event_cb) {
            radio_event_t ev = {
                .type = RADIO_EVENT_RX_DONE, .data = buffer, .len = pre_level,
                .rssi = 0, .user_data = g_lr2021.event_user_data,
            };
            g_lr2021.event_cb(&ev, g_lr2021.event_user_data);
        }
        return (int)pre_level;
    }

    /* --- Polling fallback: arm RX for one packet, poll, read, standby. --- */
    {
        uint8_t mode = LR2021_STANDBY_XOSC;
        lr2021_write_command(LR2021_CMD_SET_STANDBY, &mode, 1);
    }
    lr2021_write_command(LR2021_CMD_CLEAR_RX_FIFO, NULL, 0);
    {
        uint8_t rx_path = (g_lr2021.frequency_hz >= 1000000000U) ? 1 : 0;
        uint8_t path_args[2] = { rx_path, (uint8_t)(rx_path ? 4 : 0) };
        if (lr2021_write_command(LR2021_CMD_SET_RX_PATH, path_args, 2) < 0) {
            return -EIO;
        }
    }
    {
        uint32_t timeout_units = timeout_ms * 1000 / 32;  /* ms → 32 µs units */
        if (timeout_units == 0) timeout_units = 0xFFFFFF;
        uint8_t rx_args[3] = { (timeout_units >> 16) & 0xFF,
                               (timeout_units >> 8) & 0xFF, timeout_units & 0xFF };
        if (lr2021_write_command(LR2021_CMD_SET_RX, rx_args, 3) < 0) {
            return -EIO;
        }
    }
    g_lr2021.current_mode = RADIO_MODE_RX;

    int64_t deadline = k_uptime_get() + (timeout_ms ? timeout_ms + 500 : 2500);
    uint32_t irq = 0;
    bool rx_done = false;
    while (k_uptime_get() < deadline) {
        uint8_t irq_buf[4] = { 0 };
        if (lr2021_read_command(LR2021_CMD_GET_AND_CLEAR_IRQ, NULL, 0, irq_buf, 4) < 0) {
            break;
        }
        irq = ((uint32_t)irq_buf[0] << 24) | ((uint32_t)irq_buf[1] << 16) |
              ((uint32_t)irq_buf[2] << 8)  |  (uint32_t)irq_buf[3];
        if (irq & LR2021_IRQ_RX_DONE) {
            rx_done = true;
            break;
        }
        if (irq & LR2021_IRQ_TIMEOUT) {
            break;
        }
        k_msleep(20);
    }

    int n = rx_done ? lr2021_read_packet(buffer, max_len) : 0;
    lr2021_write_command(LR2021_CMD_SET_STANDBY,
                         (uint8_t[]){ LR2021_STANDBY_RC }, 1);
    g_lr2021.current_mode = RADIO_MODE_STANDBY;
    return n;
}

/* Lock-free: block until the IRQ semaphore fires (RX_DONE/TIMEOUT) or timeout.
 * Caller must NOT hold the chip lock — this does no SPI. */
static int lr2021_rx_wait(uint32_t timeout_ms)
{
    if (!g_lr2021.use_irq) {
        return -ENOTSUP;
    }
    int r = k_sem_take(&g_lr2021.irq_sem,
                       timeout_ms ? K_MSEC(timeout_ms) : K_FOREVER);
    return (r == 0) ? 0 : -EAGAIN;
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
static int lr2021_ops_rx_wait(radio_handle_t *h, uint32_t t) { ARG_UNUSED(h); return g_lr2021.use_irq ? lr2021_rx_wait(t) : -ENOTSUP; }
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
    .rx_wait            = lr2021_ops_rx_wait,
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
