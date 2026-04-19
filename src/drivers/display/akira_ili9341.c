/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT akira_ili9341

#define LOG_MODULE_NAME akira_ili9341
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_ili9341, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_ili9341.c
 * @brief AkiraOS ILI9341 display driver via Zephyr display_driver_api.
 *
 * Self-contained driver using the proven AkiraOS init sequence (ported from
 * the working direct-SPI driver on main branch). All hardware pins come from
 * Device Tree — no hard-coded GPIO numbers.
 *
 * Compatible: "akira,ili9341"
 * Chosen:     zephyr,display = &<node>
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/display.h>
#include <string.h>

/* ── Register map ─────────────────────────────────────────────────────────── */
#define ILI9341_CASET       0x2A    /* Column address set  */
#define ILI9341_PASET       0x2B    /* Page   address set  */
#define ILI9341_RAMWR       0x2C    /* Memory write        */
#define ILI9341_MADCTL      0x36    /* Memory access ctrl  */
#define ILI9341_COLMOD      0x3A    /* Pixel format        */
#define ILI9341_PWCTR1      0xC0    /* Power control 1     */
#define ILI9341_PWCTR2      0xC1    /* Power control 2     */
#define ILI9341_VMCTR1      0xC5    /* VCOM control 1      */
#define ILI9341_VMCTR2      0xC7    /* VCOM control 2      */
#define ILI9341_GMCTRP1     0xE0    /* Positive gamma      */
#define ILI9341_GMCTRN1     0xE1    /* Negative gamma      */
#define ILI9341_DISPON      0x29    /* Display on          */
#define ILI9341_DISPOFF     0x28    /* Display off         */

/*
 * MADCTL = 0x28: MV (row/col exchange) + BGR → landscape 320×240.
 * COLMOD = 0x55: 16 bits/pixel RGB565.
 */
#define ILI9341_MADCTL_LANDSCAPE    0x28
#define ILI9341_COLMOD_RGB565       0x55

/* ── Driver structs ───────────────────────────────────────────────────────── */

struct akira_ili9341_config {
	struct spi_dt_spec spi;
	struct gpio_dt_spec dc_gpio;
	struct gpio_dt_spec reset_gpio;
	uint16_t width;
	uint16_t height;
};

struct akira_ili9341_data {
	bool blanking;
};

/* ── SPI helpers ──────────────────────────────────────────────────────────── */

static int send_cmd(const struct device *dev, uint8_t cmd)
{
	const struct akira_ili9341_config *cfg = dev->config;
	uint8_t buf[1] = { cmd };
	struct spi_buf tx = { .buf = buf, .len = 1 };
	struct spi_buf_set tx_set = { .buffers = &tx, .count = 1 };

	gpio_pin_set_dt(&cfg->dc_gpio, 0);
	return spi_write_dt(&cfg->spi, &tx_set);
}

static int send_data(const struct device *dev, const uint8_t *data, size_t len)
{
	const struct akira_ili9341_config *cfg = dev->config;
	/* Cast away const — spi_buf.buf is void*, data is not mutated */
	struct spi_buf tx = { .buf = (void *)data, .len = len };
	struct spi_buf_set tx_set = { .buffers = &tx, .count = 1 };

	gpio_pin_set_dt(&cfg->dc_gpio, 1);
	return spi_write_dt(&cfg->spi, &tx_set);
}

static int send_byte(const struct device *dev, uint8_t byte)
{
	return send_data(dev, &byte, 1);
}

/* Set the drawing window (column + page address registers) */
static int set_window(const struct device *dev,
		      uint16_t x, uint16_t y, uint16_t x_end, uint16_t y_end)
{
	int ret;
	uint8_t addr[4];

	ret = send_cmd(dev, ILI9341_CASET);
	if (ret) { return ret; }
	addr[0] = x >> 8;     addr[1] = x & 0xFF;
	addr[2] = x_end >> 8; addr[3] = x_end & 0xFF;
	ret = send_data(dev, addr, 4);
	if (ret) { return ret; }

	ret = send_cmd(dev, ILI9341_PASET);
	if (ret) { return ret; }
	addr[0] = y >> 8;     addr[1] = y & 0xFF;
	addr[2] = y_end >> 8; addr[3] = y_end & 0xFF;
	return send_data(dev, addr, 4);
}

/* ── display_driver_api ───────────────────────────────────────────────────── */

static int akira_ili9341_blanking_on(const struct device *dev)
{
	struct akira_ili9341_data *data = dev->data;
	int ret = send_cmd(dev, ILI9341_DISPOFF);

	if (ret == 0) {
		data->blanking = true;
	}
	return ret;
}

static int akira_ili9341_blanking_off(const struct device *dev)
{
	struct akira_ili9341_data *data = dev->data;
	int ret = send_cmd(dev, ILI9341_DISPON);

	if (ret == 0) {
		data->blanking = false;
	}
	return ret;
}

/**
 * @brief Write pixel data to the display.
 *
 * The framebuffer holds uint16_t RGB565 values in host (little-endian) byte
 * order.  ILI9341 expects big-endian over SPI — byte[0]=MSB, byte[1]=LSB.
 * We swap pairs in a small stack buffer so no extra heap allocation is needed.
 */
static int akira_ili9341_write(const struct device *dev,
			       const uint16_t x, const uint16_t y,
			       const struct display_buffer_descriptor *desc,
			       const void *buf)
{
	int ret;

	ret = set_window(dev, x, y, x + desc->width - 1, y + desc->height - 1);
	if (ret) { return ret; }

	ret = send_cmd(dev, ILI9341_RAMWR);
	if (ret) { return ret; }

	/*
	 * Byte-swap RGB565 pairs: host little-endian [LSB, MSB] →
	 * wire big-endian [MSB, LSB] expected by ILI9341.
	 * Process in 256-pixel (512-byte) stack chunks to avoid heap allocation.
	 */
	const uint8_t *src = (const uint8_t *)buf;
	size_t remaining = desc->buf_size;
	uint8_t chunk[512];

	while (remaining > 0) {
		size_t n = (remaining < sizeof(chunk)) ? remaining : sizeof(chunk);

		for (size_t i = 0; i + 1 < n; i += 2) {
			chunk[i]     = src[i + 1];	/* MSB first */
			chunk[i + 1] = src[i];
		}
		/* Handle odd trailing byte (shouldn't happen for RGB565) */
		if (n & 1) {
			chunk[n - 1] = src[n - 1];
		}

		ret = send_data(dev, chunk, n);
		if (ret) { return ret; }

		src += n;
		remaining -= n;
	}

	return 0;
}

static void akira_ili9341_get_capabilities(const struct device *dev,
					   struct display_capabilities *caps)
{
	const struct akira_ili9341_config *cfg = dev->config;

	memset(caps, 0, sizeof(*caps));
	caps->x_resolution          = cfg->width;
	caps->y_resolution          = cfg->height;
	caps->supported_pixel_formats = PIXEL_FORMAT_RGB_565;
	caps->current_pixel_format    = PIXEL_FORMAT_RGB_565;
}

static int akira_ili9341_set_pixel_format(const struct device *dev,
					  const enum display_pixel_format fmt)
{
	ARG_UNUSED(dev);
	if (fmt != PIXEL_FORMAT_RGB_565) {
		return -ENOTSUP;
	}
	return 0;
}

static const struct display_driver_api akira_ili9341_api = {
	.blanking_on      = akira_ili9341_blanking_on,
	.blanking_off     = akira_ili9341_blanking_off,
	.write            = akira_ili9341_write,
	.get_capabilities = akira_ili9341_get_capabilities,
	.set_pixel_format = akira_ili9341_set_pixel_format,
};

/* ── Hardware init sequence (proven AkiraOS main-branch sequence) ─────────── */

static int akira_ili9341_init(const struct device *dev)
{
	const struct akira_ili9341_config *cfg = dev->config;
	int ret;

	if (!spi_is_ready_dt(&cfg->spi)) {
		LOG_ERR("%s: SPI bus not ready", dev->name);
		return -ENODEV;
	}
	if (!gpio_is_ready_dt(&cfg->dc_gpio)) {
		LOG_ERR("%s: DC GPIO not ready", dev->name);
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&cfg->dc_gpio, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("%s: DC GPIO configure failed: %d", dev->name, ret);
		return ret;
	}

	/* Optional hardware reset */
	if (cfg->reset_gpio.port != NULL) {
		if (!gpio_is_ready_dt(&cfg->reset_gpio)) {
			LOG_ERR("%s: RST GPIO not ready", dev->name);
			return -ENODEV;
		}
		ret = gpio_pin_configure_dt(&cfg->reset_gpio, GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			LOG_ERR("%s: RST GPIO configure failed: %d", dev->name, ret);
			return ret;
		}
		gpio_pin_set_dt(&cfg->reset_gpio, 1);  /* assert reset (pin LOW) */
		k_msleep(10);
		gpio_pin_set_dt(&cfg->reset_gpio, 0);  /* release reset (pin HIGH) */
		k_msleep(120);                          /* wait for display to stabilize */
	}

	/* Software reset + sleep out */
	ret = send_cmd(dev, 0x01);   /* SWRESET */
	if (ret < 0) { return ret; }
	k_msleep(150);

	ret = send_cmd(dev, 0x11);   /* SLPOUT  */
	if (ret < 0) { return ret; }
	k_msleep(120);

	/* Power / timing control (vendor init sequence) */
	const uint8_t power_a[]  = { 0x39, 0x2C, 0x00, 0x34, 0x02 };
	send_cmd(dev, 0xCB); send_data(dev, power_a,  sizeof(power_a));

	const uint8_t power_b[]  = { 0x00, 0x83, 0x30 };
	send_cmd(dev, 0xCF); send_data(dev, power_b,  sizeof(power_b));

	const uint8_t timing_a[] = { 0x85, 0x01, 0x79 };
	send_cmd(dev, 0xE8); send_data(dev, timing_a, sizeof(timing_a));

	const uint8_t timing_b[] = { 0x00, 0x00 };
	send_cmd(dev, 0xEA); send_data(dev, timing_b, sizeof(timing_b));

	const uint8_t power_seq[] = { 0x64, 0x03, 0x12, 0x81 };
	send_cmd(dev, 0xED); send_data(dev, power_seq, sizeof(power_seq));

	send_cmd(dev, 0xF7); send_byte(dev, 0x20);     /* Pump ratio     */
	send_cmd(dev, ILI9341_PWCTR1); send_byte(dev, 0x26);   /* VRH=4.75 V    */
	send_cmd(dev, ILI9341_PWCTR2); send_byte(dev, 0x11);   /* SAP, BT       */

	const uint8_t vcom1[] = { 0x35, 0x3E };
	send_cmd(dev, ILI9341_VMCTR1); send_data(dev, vcom1, sizeof(vcom1));
	send_cmd(dev, ILI9341_VMCTR2); send_byte(dev, 0xBE);

	/* Orientation + pixel format */
	send_cmd(dev, ILI9341_MADCTL); send_byte(dev, ILI9341_MADCTL_LANDSCAPE);
	send_cmd(dev, ILI9341_COLMOD); send_byte(dev, ILI9341_COLMOD_RGB565);

	/* Frame rate: fosc / (1+31) ≈ 70 Hz */
	const uint8_t frmctr[] = { 0x00, 0x1B };
	send_cmd(dev, 0xB1); send_data(dev, frmctr, sizeof(frmctr));

	/* Display function control */
	const uint8_t dfunctr[] = { 0x0A, 0x82, 0x27, 0x00 };
	send_cmd(dev, 0xB6); send_data(dev, dfunctr, sizeof(dfunctr));

	/* 3Gamma disable, gamma curve 1 */
	send_cmd(dev, 0xF2); send_byte(dev, 0x08);
	send_cmd(dev, 0x26); send_byte(dev, 0x01);

	/* Positive gamma correction */
	const uint8_t gamma_p[] = {
		0x1F, 0x1A, 0x18, 0x0A, 0x0F, 0x06, 0x45, 0x87,
		0x32, 0x0A, 0x07, 0x02, 0x07, 0x05, 0x00
	};
	send_cmd(dev, ILI9341_GMCTRP1); send_data(dev, gamma_p, sizeof(gamma_p));

	/* Negative gamma correction */
	const uint8_t gamma_n[] = {
		0x00, 0x25, 0x27, 0x05, 0x10, 0x09, 0x3A, 0x78,
		0x4D, 0x05, 0x18, 0x0D, 0x38, 0x3A, 0x1F
	};
	send_cmd(dev, ILI9341_GMCTRN1); send_data(dev, gamma_n, sizeof(gamma_n));

	/* Full-screen address window */
	const uint8_t col_addr[]  = { 0x00, 0x00, 0x00, 0xEF };
	send_cmd(dev, ILI9341_CASET); send_data(dev, col_addr,  sizeof(col_addr));

	const uint8_t page_addr[] = { 0x00, 0x00, 0x01, 0x3F };
	send_cmd(dev, ILI9341_PASET); send_data(dev, page_addr, sizeof(page_addr));

	/* Entry mode */
	send_cmd(dev, 0xB7); send_byte(dev, 0x07);

	/* Display on */
	ret = send_cmd(dev, ILI9341_DISPON);
	if (ret < 0) { return ret; }
	k_msleep(120);

	LOG_INF("%s: ready (%dx%d RGB565)", dev->name, cfg->width, cfg->height);
	return 0;
}

/* ── Device instantiation ─────────────────────────────────────────────────── */

#define AKIRA_ILI9341_DEFINE(inst)						\
	static struct akira_ili9341_data akira_ili9341_data_##inst;		\
	static const struct akira_ili9341_config akira_ili9341_cfg_##inst = {	\
		.spi       = SPI_DT_SPEC_INST_GET(inst,				\
				SPI_OP_MODE_MASTER | SPI_WORD_SET(8)		\
				| SPI_TRANSFER_MSB, 0),				\
		.dc_gpio   = GPIO_DT_SPEC_INST_GET(inst, dc_gpios),		\
		.reset_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, reset_gpios, {0}),	\
		.width  = DT_INST_PROP(inst, width),				\
		.height = DT_INST_PROP(inst, height),				\
	};									\
	DEVICE_DT_INST_DEFINE(inst, akira_ili9341_init, NULL,			\
			      &akira_ili9341_data_##inst,			\
			      &akira_ili9341_cfg_##inst,			\
			      POST_KERNEL, CONFIG_DISPLAY_INIT_PRIORITY,	\
			      &akira_ili9341_api);

DT_INST_FOREACH_STATUS_OKAY(AKIRA_ILI9341_DEFINE)
