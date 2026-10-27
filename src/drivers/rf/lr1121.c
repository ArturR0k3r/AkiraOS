/**
 * @file lr1121.c
 * @brief LR1121 LoRa/GFSK Transceiver Driver Implementation
 */

#include "lr1121.h"
#include "connectivity/radio_interface.h"
#include <zephyr/logging/log.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(akira_lr1121, LOG_LEVEL_INF);

/* LR1121 Commands (16-bit big-endian opcodes) */
#define LR1121_CMD_GET_VERSION      0x0101
#define LR1121_CMD_GET_STATUS       0x0100
#define LR1121_CMD_SET_SLEEP        0x0184
#define LR1121_CMD_SET_STANDBY      0x0180
#define LR1121_CMD_SET_FS           0x0181
#define LR1121_CMD_SET_TX           0x0183
#define LR1121_CMD_SET_RX           0x0182
#define LR1121_CMD_SET_RF_FREQUENCY 0x0208
#define LR1121_CMD_SET_TX_PARAMS    0x020E
#define LR1121_CMD_SET_PACKET_TYPE  0x020A
#define LR1121_CMD_SET_MODULATION_PARAMS 0x020B
#define LR1121_CMD_SET_PACKET_PARAMS 0x020C
#define LR1121_CMD_WRITE_BUFFER     0x020D
#define LR1121_CMD_READ_BUFFER      0x0211
#define LR1121_CMD_GET_RSSI_INST    0x0215

/* Standby modes */
#define LR1121_STANDBY_RC           0x00
#define LR1121_STANDBY_XOSC         0x01

/* BUSY timeout */
#define LR1121_BUSY_TIMEOUT_MS      1000

/* Device tree node */
#define LR1121_NODE DT_NODELABEL(lr1121_config)

static struct {
    bool initialized;
    struct spi_dt_spec spi;
    struct gpio_dt_spec cs;
    struct gpio_dt_spec reset;
    struct gpio_dt_spec busy;
    radio_mode_t current_mode;
    uint32_t frequency;
    int8_t tx_power;
    radio_event_cb_t event_cb;
    void *event_user_data;
} g_lr1121 = {
    .initialized = false,
};

/* Forward declarations */
static int lr1121_wait_busy(void);
static int lr1121_write_command(uint16_t cmd, const uint8_t *data, size_t len);
static int lr1121_read_command(uint16_t cmd, uint8_t *data, size_t len);
static int lr1121_reset_chip(void);
static int lr1121_init_from_dt(void);

/**
 * @brief Wait for BUSY pin to go low
 */
static int lr1121_wait_busy(void)
{
    if (!gpio_is_ready_dt(&g_lr1121.busy)) {
        return -ENODEV;
    }

    int64_t start = k_uptime_get();
    while (gpio_pin_get_dt(&g_lr1121.busy)) {
        if (k_uptime_get() - start > LR1121_BUSY_TIMEOUT_MS) {
            LOG_ERR("BUSY timeout");
            return -ETIMEDOUT;
        }
        k_usleep(100);
    }
    return 0;
}

/**
 * @brief Write command to LR1121
 */
static int lr1121_write_command(uint16_t cmd, const uint8_t *data, size_t len)
{
    int ret;
    uint8_t cmd_buf[2];
    
    /* Wait for BUSY to go low */
    ret = lr1121_wait_busy();
    if (ret < 0) {
        return ret;
    }

    /* Prepare command (big-endian) */
    cmd_buf[0] = (cmd >> 8) & 0xFF;
    cmd_buf[1] = cmd & 0xFF;

    /* Manual CS control */
    gpio_pin_set_dt(&g_lr1121.cs, 0);
    k_usleep(1);

    /* Send command */
    struct spi_buf tx_bufs[] = {
        { .buf = cmd_buf, .len = 2 },
        { .buf = (void *)data, .len = len },
    };
    struct spi_buf_set tx = { .buffers = tx_bufs, .count = data ? 2 : 1 };

    ret = spi_write_dt(&g_lr1121.spi, &tx);

    /* Release CS */
    k_usleep(1);
    gpio_pin_set_dt(&g_lr1121.cs, 1);

    if (ret < 0) {
        LOG_ERR("SPI write failed: %d", ret);
        return ret;
    }

    /* Wait for BUSY to go low after command */
    return lr1121_wait_busy();
}

/**
 * @brief Read response from LR1121
 */
static int lr1121_read_command(uint16_t cmd, uint8_t *data, size_t len)
{
    int ret;
    uint8_t cmd_buf[2];
    uint8_t dummy = 0xFF;

    if (!data || len == 0) {
        return -EINVAL;
    }

    /* Wait for BUSY to go low */
    ret = lr1121_wait_busy();
    if (ret < 0) {
        return ret;
    }

    /* Prepare command (big-endian) */
    cmd_buf[0] = (cmd >> 8) & 0xFF;
    cmd_buf[1] = cmd & 0xFF;

    /* Manual CS control */
    gpio_pin_set_dt(&g_lr1121.cs, 0);
    k_usleep(1);

    /* Phase 1: Send command */
    struct spi_buf tx_cmd = { .buf = cmd_buf, .len = 2 };
    struct spi_buf_set tx_cmd_set = { .buffers = &tx_cmd, .count = 1 };
    ret = spi_write_dt(&g_lr1121.spi, &tx_cmd_set);
    
    /* Release CS */
    k_usleep(1);
    gpio_pin_set_dt(&g_lr1121.cs, 1);

    if (ret < 0) {
        LOG_ERR("SPI write cmd failed: %d", ret);
        return ret;
    }

    /* Wait for BUSY to go low */
    ret = lr1121_wait_busy();
    if (ret < 0) {
        return ret;
    }

    /* Phase 2: Read response */
    gpio_pin_set_dt(&g_lr1121.cs, 0);
    k_usleep(1);

    /* Send dummy byte then read data */
    struct spi_buf tx_bufs[] = {
        { .buf = &dummy, .len = 1 },
    };
    struct spi_buf rx_bufs[] = {
        { .buf = &dummy, .len = 1 },  /* Dummy read */
        { .buf = data, .len = len },
    };
    struct spi_buf_set tx = { .buffers = tx_bufs, .count = 1 };
    struct spi_buf_set rx = { .buffers = rx_bufs, .count = 2 };

    ret = spi_transceive_dt(&g_lr1121.spi, &tx, &rx);

    /* Release CS */
    k_usleep(1);
    gpio_pin_set_dt(&g_lr1121.cs, 1);

    if (ret < 0) {
        LOG_ERR("SPI read failed: %d", ret);
        return ret;
    }

    return 0;
}

/**
 * @brief Reset LR1121 chip
 */
static int lr1121_reset_chip(void)
{
    if (!gpio_is_ready_dt(&g_lr1121.reset)) {
        LOG_ERR("RESET GPIO not ready");
        return -ENODEV;
    }

    LOG_INF("Resetting LR1121...");

    /* Assert reset (low) */
    gpio_pin_set_dt(&g_lr1121.reset, 1);  /* Active low */
    k_msleep(100);

    /* Release reset */
    gpio_pin_set_dt(&g_lr1121.reset, 0);
    k_msleep(10);

    /* Wait for BUSY to go low */
    return lr1121_wait_busy();
}

/**
 * @brief Initialize from device tree
 */
static int lr1121_init_from_dt(void)
{
    int ret;

    /* Get SPI device from device tree (LR1121 is child of SPI bus) */
    const struct device *spi_dev = DEVICE_DT_GET(DT_BUS(LR1121_NODE));
    if (!device_is_ready(spi_dev)) {
        LOG_ERR("SPI device not ready");
        return -ENODEV;
    }

    /* Initialize SPI spec */
    g_lr1121.spi.bus = spi_dev;
    g_lr1121.spi.config.frequency = DT_PROP(LR1121_NODE, spi_max_frequency);
    g_lr1121.spi.config.operation = SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8);

    /* Get GPIO specs from device tree */
    g_lr1121.cs = (struct gpio_dt_spec)GPIO_DT_SPEC_GET(LR1121_NODE, cs_gpios);
    g_lr1121.reset = (struct gpio_dt_spec)GPIO_DT_SPEC_GET(LR1121_NODE, reset_gpios);
    g_lr1121.busy = (struct gpio_dt_spec)GPIO_DT_SPEC_GET(LR1121_NODE, busy_gpios);

    /* Check GPIO readiness */
    if (!gpio_is_ready_dt(&g_lr1121.cs)) {
        LOG_ERR("CS GPIO not ready");
        return -ENODEV;
    }
    if (!gpio_is_ready_dt(&g_lr1121.reset)) {
        LOG_ERR("RESET GPIO not ready");
        return -ENODEV;
    }
    if (!gpio_is_ready_dt(&g_lr1121.busy)) {
        LOG_ERR("BUSY GPIO not ready");
        return -ENODEV;
    }

    /* Configure CS pin (output, initially high) */
    ret = gpio_pin_configure_dt(&g_lr1121.cs, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        LOG_ERR("Failed to configure CS: %d", ret);
        return ret;
    }
    gpio_pin_set_dt(&g_lr1121.cs, 1);

    /* Configure RESET pin (output, initially inactive) */
    ret = gpio_pin_configure_dt(&g_lr1121.reset, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        LOG_ERR("Failed to configure RESET: %d", ret);
        return ret;
    }

    /* Configure BUSY pin (input with pull-down) */
    ret = gpio_pin_configure_dt(&g_lr1121.busy, GPIO_INPUT);
    if (ret < 0) {
        LOG_ERR("Failed to configure BUSY: %d", ret);
        return ret;
    }

    LOG_INF("LR1121 DT init complete");
    return 0;
}

/**
 * @brief Initialize LR1121 driver
 */
static int lr1121_init(void)
{
    int ret;
    uint8_t version[4];

    if (g_lr1121.initialized) {
        LOG_WRN("LR1121 already initialized");
        return 0;
    }

    /* Initialize from device tree */
    ret = lr1121_init_from_dt();
    if (ret < 0) {
        LOG_ERR("DT init failed: %d", ret);
        return ret;
    }

    /* Reset the chip */
    ret = lr1121_reset_chip();
    if (ret < 0) {
        LOG_ERR("Chip reset failed: %d", ret);
        return ret;
    }

    /* Read version */
    ret = lr1121_read_command(LR1121_CMD_GET_VERSION, version, 4);
    if (ret == 0) {
        LOG_INF("LR1121 version: HW=0x%02X, FW=%d.%d", 
                version[1], version[2], version[3]);
    } else {
        LOG_WRN("Failed to read version: %d", ret);
    }

    /* Set standby mode (XOSC) */
    uint8_t standby_mode = LR1121_STANDBY_XOSC;
    ret = lr1121_write_command(LR1121_CMD_SET_STANDBY, &standby_mode, 1);
    if (ret < 0) {
        LOG_ERR("Failed to set standby: %d", ret);
        return ret;
    }

    g_lr1121.current_mode = RADIO_MODE_STANDBY;
    g_lr1121.initialized = true;

    LOG_INF("LR1121 initialized successfully");
    return 0;
}

static int lr1121_deinit(void)
{
    if (!g_lr1121.initialized) {
        return 0;
    }

    /* Put chip in sleep mode */
    uint8_t sleep_cfg = 0x04;  /* Warm start */
    lr1121_write_command(LR1121_CMD_SET_SLEEP, &sleep_cfg, 1);

    g_lr1121.initialized = false;
    LOG_INF("LR1121 deinitialized");
    return 0;
}

static int lr1121_set_mode(radio_mode_t mode)
{
    int ret;
    uint8_t mode_cfg;

    if (!g_lr1121.initialized) {
        return -ENODEV;
    }

    switch (mode) {
    case RADIO_MODE_SLEEP:
        mode_cfg = 0x04;  /* Warm start */
        ret = lr1121_write_command(LR1121_CMD_SET_SLEEP, &mode_cfg, 1);
        break;

    case RADIO_MODE_STANDBY:
        mode_cfg = LR1121_STANDBY_XOSC;
        ret = lr1121_write_command(LR1121_CMD_SET_STANDBY, &mode_cfg, 1);
        break;

    case RADIO_MODE_RX:
        /* Set RX continuous mode */
        ret = lr1121_write_command(LR1121_CMD_SET_RX, NULL, 0);
        break;

    case RADIO_MODE_TX:
        /* TX mode is set during transmit */
        ret = 0;
        break;

    default:
        LOG_ERR("Invalid mode: %d", mode);
        return -EINVAL;
    }

    if (ret == 0) {
        g_lr1121.current_mode = mode;
        LOG_DBG("LR1121 mode set to %d", mode);
    } else {
        LOG_ERR("Failed to set mode %d: %d", mode, ret);
    }

    return ret;
}

static int lr1121_set_frequency(uint32_t freq_hz)
{
    if (!g_lr1121.initialized) {
        return -ENODEV;
    }

    /* Validate frequency range (150-960 MHz) */
    if (freq_hz < 150000000 || freq_hz > 960000000) {
        LOG_ERR("Frequency out of range: %u Hz", freq_hz);
        return -EINVAL;
    }

    /* LR1121 uses 32-bit frequency value */
    uint8_t freq_buf[4];
    freq_buf[0] = (freq_hz >> 24) & 0xFF;
    freq_buf[1] = (freq_hz >> 16) & 0xFF;
    freq_buf[2] = (freq_hz >> 8) & 0xFF;
    freq_buf[3] = freq_hz & 0xFF;

    int ret = lr1121_write_command(LR1121_CMD_SET_RF_FREQUENCY, freq_buf, 4);
    if (ret == 0) {
        g_lr1121.frequency = freq_hz;
        LOG_INF("LR1121 frequency set to %u Hz", freq_hz);
    } else {
        LOG_ERR("Failed to set frequency: %d", ret);
    }

    return ret;
}

static int lr1121_set_power(int8_t dbm)
{
    if (!g_lr1121.initialized) {
        return -ENODEV;
    }

    /* Clamp to chip limits (-17 to +22 dBm) */
    if (dbm < -17) dbm = -17;
    if (dbm > 22) dbm = 22;

    /* TX params: power (int8_t), ramp time */
    uint8_t tx_params[2];
    tx_params[0] = (uint8_t)dbm;
    tx_params[1] = 0x02;  /* Ramp time: 40us */

    int ret = lr1121_write_command(LR1121_CMD_SET_TX_PARAMS, tx_params, 2);
    if (ret == 0) {
        g_lr1121.tx_power = dbm;
        LOG_INF("LR1121 TX power set to %d dBm", dbm);
    } else {
        LOG_ERR("Failed to set power: %d", ret);
    }

    return ret;
}

static int lr1121_set_modulation(radio_modulation_t mod)
{
    if (!g_lr1121.initialized) {
        return -ENODEV;
    }

    /* For now, return not supported for non-LoRa modes */
    if (mod != RADIO_MOD_LORA) {
        LOG_WRN("Modulation %d not yet implemented", mod);
        return -ENOSYS;
    }

    LOG_DBG("LR1121 modulation set to %d", mod);
    return 0;
}

static int lr1121_set_bitrate(uint32_t bps)
{
    if (!g_lr1121.initialized) {
        return -ENODEV;
    }

    /* GFSK bitrate configuration not yet implemented */
    LOG_WRN("Bitrate configuration not yet implemented");
    (void)bps;
    return -ENOSYS;
}

static int lr1121_tx(const uint8_t *data, size_t len)
{
    if (!g_lr1121.initialized) {
        return -ENODEV;
    }

    if (!data || len == 0) {
        return -EINVAL;
    }

    /* For now, basic packet mode not fully implemented */
    LOG_WRN("TX not yet fully implemented");
    LOG_DBG("LR1121 TX: %zu bytes", len);
    return -ENOSYS;
}

static int lr1121_rx(uint8_t *buffer, size_t max_len, uint32_t timeout_ms)
{
    if (!g_lr1121.initialized) {
        return -ENODEV;
    }

    if (!buffer || max_len == 0) {
        return -EINVAL;
    }

    /* For now, basic packet mode not fully implemented */
    LOG_WRN("RX not yet fully implemented");
    LOG_DBG("LR1121 RX: max=%zu, timeout=%u ms", max_len, timeout_ms);
    (void)timeout_ms;
    return -ENOSYS;
}

static int lr1121_get_rssi(int16_t *rssi)
{
    if (!g_lr1121.initialized) {
        return -ENODEV;
    }

    if (!rssi) {
        return -EINVAL;
    }

    /* Read instantaneous RSSI */
    uint8_t rssi_buf[2];
    int ret = lr1121_read_command(LR1121_CMD_GET_RSSI_INST, rssi_buf, 2);
    if (ret < 0) {
        LOG_ERR("Failed to read RSSI: %d", ret);
        return ret;
    }

    /* Combine MSB and LSB, convert to dBm */
    int16_t rssi_raw = (rssi_buf[0] << 8) | rssi_buf[1];
    *rssi = rssi_raw / 2;  /* LR1121 RSSI is in 0.5 dBm units */

    LOG_DBG("LR1121 RSSI: %d dBm", *rssi);
    return 0;
}

static int lr1121_set_event_callback(radio_handle_t *handle, radio_event_cb_t cb, void *user_data)
{
    ARG_UNUSED(handle);
    g_lr1121.event_cb = cb;
    g_lr1121.event_user_data = user_data;
    return 0;
}

static int lr1121_set_spreading_factor(uint8_t sf)
{
    if (!g_lr1121.initialized) {
        return -ENODEV;
    }

    /* Validate SF range (5-12) */
    if (sf < 5 || sf > 12) {
        LOG_ERR("Invalid spreading factor: %d", sf);
        return -EINVAL;
    }

    /* LoRa modulation params not yet fully implemented */
    LOG_WRN("LoRa SF configuration not yet implemented");
    LOG_DBG("LR1121 set SF: %d", sf);
    return -ENOSYS;
}

static int lr1121_set_bandwidth(uint32_t bw_hz)
{
    if (!g_lr1121.initialized) {
        return -ENODEV;
    }

    /* LoRa bandwidth configuration not yet fully implemented */
    LOG_WRN("LoRa BW configuration not yet implemented");
    LOG_DBG("LR1121 set BW: %u Hz", bw_hz);
    (void)bw_hz;
    return -ENOSYS;
}

static int lr1121_set_coding_rate(uint8_t cr)
{
    if (!g_lr1121.initialized) {
        return -ENODEV;
    }

    /* Validate CR range (4/5 to 4/8) */
    if (cr < 5 || cr > 8) {
        LOG_ERR("Invalid coding rate: 4/%d", cr);
        return -EINVAL;
    }

    /* LoRa coding rate configuration not yet fully implemented */
    LOG_WRN("LoRa CR configuration not yet implemented");
    LOG_DBG("LR1121 set CR: 4/%d", cr);
    return -ENOSYS;
}

/* =========================================================================
 * radio_ops_t vtable shims
 * ========================================================================= */

static int lr1121_ops_init(radio_handle_t *h)        { ARG_UNUSED(h); return lr1121_init(); }
static int lr1121_ops_deinit(radio_handle_t *h)      { ARG_UNUSED(h); return lr1121_deinit(); }
static int lr1121_ops_send(radio_handle_t *h, const uint8_t *d, size_t l) { ARG_UNUSED(h); return lr1121_tx(d, l); }
static int lr1121_ops_recv(radio_handle_t *h, uint8_t *b, size_t l, uint32_t t) { ARG_UNUSED(h); return lr1121_rx(b, l, t); }
static int lr1121_ops_set_frequency(radio_handle_t *h, uint32_t hz) { ARG_UNUSED(h); return lr1121_set_frequency(hz); }
static int lr1121_ops_set_power(radio_handle_t *h, int8_t dbm)      { ARG_UNUSED(h); return lr1121_set_power(dbm); }
static int lr1121_ops_get_rssi(radio_handle_t *h, int16_t *r)       { ARG_UNUSED(h); return lr1121_get_rssi(r); }
static int lr1121_ops_set_mode(radio_handle_t *h, radio_mode_t m)   { ARG_UNUSED(h); return lr1121_set_mode(m); }
static int lr1121_ops_set_modulation(radio_handle_t *h, radio_modulation_t m) { ARG_UNUSED(h); return lr1121_set_modulation(m); }
static int lr1121_ops_set_bitrate(radio_handle_t *h, uint32_t bps)  { ARG_UNUSED(h); return lr1121_set_bitrate(bps); }
static int lr1121_ops_set_sf(radio_handle_t *h, uint8_t sf)         { ARG_UNUSED(h); return lr1121_set_spreading_factor(sf); }
static int lr1121_ops_set_bw(radio_handle_t *h, uint32_t bw)        { ARG_UNUSED(h); return lr1121_set_bandwidth(bw); }
static int lr1121_ops_set_cr(radio_handle_t *h, uint8_t cr)         { ARG_UNUSED(h); return lr1121_set_coding_rate(cr); }

static const radio_ops_t lr1121_ops = {
    .init                = lr1121_ops_init,
    .deinit              = lr1121_ops_deinit,
    .send                = lr1121_ops_send,
    .recv                = lr1121_ops_recv,
    .set_event_callback  = lr1121_set_event_callback,
    .set_frequency       = lr1121_ops_set_frequency,
    .set_power           = lr1121_ops_set_power,
    .get_rssi            = lr1121_ops_get_rssi,
    .set_mode            = lr1121_ops_set_mode,
    .set_modulation      = lr1121_ops_set_modulation,
    .set_bitrate         = lr1121_ops_set_bitrate,
    .set_spreading_factor = lr1121_ops_set_sf,
    .set_bandwidth       = lr1121_ops_set_bw,
    .set_coding_rate     = lr1121_ops_set_cr,
};

static radio_handle_t lr1121_handle = {
    .type         = RADIO_TYPE_LORA,
    .name         = "LR1121",
    .capabilities = RADIO_CAP_TX | RADIO_CAP_RX | RADIO_CAP_CCA | RADIO_CAP_RAW_MODE |
                    RADIO_CAP_LOW_POWER | RADIO_CAP_BAND_SUBGHZ | RADIO_CAP_BAND_2GHZ4 |
                    RADIO_CAP_MOD_FSK | RADIO_CAP_MOD_LORA,
    .ops          = &lr1121_ops,
};

radio_handle_t *lr1121_get_handle(void)
{
    return &lr1121_handle;
}

#ifdef CONFIG_AKIRA_LR1121
static int lr1121_auto_register(void)
{
    int ret = radio_manager_register(&lr1121_handle);
    if (ret < 0 && ret != -EALREADY) {
        LOG_ERR("Failed to register LR1121: %d", ret);
        return ret;
    }
    LOG_INF("LR1121 registered with radio_manager");
    return 0;
}

SYS_INIT(lr1121_auto_register, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
#endif /* CONFIG_AKIRA_LR1121 */
