/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT akira_lsm6ds3

#define LOG_MODULE_NAME akira_lsm6ds3
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_lsm6ds3, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_lsm6ds3.c
 * @brief Out-of-tree driver for the ST LSM6DS3 6-axis IMU.
 *
 * The Zephyr upstream lsm6dsl driver only accepts WHO_AM_I = 0x6A
 * (LSM6DSL / LSM6DS3TR-C). The original LSM6DS3 reports 0x69 and is
 * therefore rejected by that driver. This minimal driver accepts 0x69,
 * configures accel + gyro for ±2g / ±245 dps, and exposes the standard
 * Zephyr sensor_driver_api so it integrates with the generic
 * akira_sensor_api without any chip-specific code there.
 *
 * Battery-life: the part boots powered DOWN (ODR=0, ~6 µA) rather than
 * free-running at 104 Hz (~0.9 mA for both blocks).  It is brought to
 * 104 Hz on demand — the first sample_fetch() from a consumer (the generic
 * akira_sensor_api read path is the only one) powers it up, and an inactivity
 * timer powers it back down LSM6DS3_IDLE_POWERDOWN_S seconds after the last
 * read.  On a battery handheld the IMU is idle the vast majority of the time,
 * so this is the state it should default to.
 *
 * Compatible: "akira,lsm6ds3"
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/pm/device.h>

/* ── Register map ─────────────────────────────────────────────────────────── */
#define LSM6DS3_REG_WHO_AM_I    0x0F
#define LSM6DS3_REG_CTRL1_XL    0x10   /* accel ODR + FS */
#define LSM6DS3_REG_CTRL2_G     0x11   /* gyro  ODR + FS */
#define LSM6DS3_REG_CTRL3_C     0x12   /* BDU, IF_INC … */
#define LSM6DS3_REG_OUTX_L_G    0x22   /* gyro  X LSB (6 bytes: G_XYZ) */
#define LSM6DS3_REG_OUTX_L_XL   0x28   /* accel X LSB (6 bytes: XL_XYZ) */

/* WHO_AM_I value for original LSM6DS3 (≠ 0x6A used by LSM6DSL/LSM6DS3TR-C) */
#define LSM6DS3_WHO_AM_I        0x69

/*
 * CTRL3_C: BDU=1 (block-data update), IF_INC=1 (auto-increment addresses).
 * IF_INC is required so a single multi-byte i2c_write_read reads consecutive
 * output registers in one transaction.
 */
#define CTRL3_C_INIT            0x44

/*
 * CTRL1_XL: ODR_XL=104 Hz (bits[7:4]=0x4), FS_XL=±2g (bits[3:2]=0x0)
 * → 0x40
 */
#define CTRL1_XL_104HZ_2G       0x40

/*
 * CTRL2_G: ODR_G=104 Hz (bits[7:4]=0x4), FS_G=±245 dps (bits[3:1]=0x0)
 * → 0x40
 */
#define CTRL2_G_104HZ_245DPS    0x40

/*
 * Sensitivity constants.
 *
 * Accel at ±2g:  0.061 mg/LSB = 0.061e-3 * 9.80665 m/s²/LSB
 *                             ≈ 598 µm/s²/LSB  (used as integer scale)
 *
 * Gyro at ±245 dps: 8.75 mdps/LSB = 8.75e-3 * π/180 rad/s/LSB
 *                                  ≈ 152 716 nrad/s/LSB
 */
#define ACCEL_SENS_UM_PER_LSB   598LL       /* µm/s² per LSB  → val2 in µ */
#define GYRO_SENS_NRAD_PER_LSB  152716LL    /* nrad/s per LSB             */

/*
 * Seconds of no reads after which the idle_work handler powers the sensors
 * back down.  Kept small so the ~0.9 mA active draw is reclaimed almost as
 * soon as an app stops polling, yet long enough to span the per-frame read
 * cadence of a tilt-controlled app (which reads on every UI refresh).  A plain
 * constant rather than a Kconfig — it is an internal power heuristic with no
 * reason for a per-board knob.
 */
#define LSM6DS3_IDLE_POWERDOWN_S 5

/* ── Driver structs ───────────────────────────────────────────────────────── */

struct lsm6ds3_config {
	struct i2c_dt_spec i2c;
};

struct lsm6ds3_data {
	int16_t accel[3];	/* raw accel X, Y, Z */
	int16_t gyro[3];	/* raw gyro  X, Y, Z */
	bool    initialized;	/* CTRL3_C/WHO_AM_I done on first fetch */
	bool    powered;	/* accel+gyro currently at active ODR */
	bool    consumer_on;	/* a consumer wants sampling (survives a blank) */
	const struct device *dev;	/* back-pointer for the idle work handler */
	struct k_work_delayable idle_work; /* powers down after inactivity */
};

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static inline int16_t le16_to_s16(const uint8_t *p)
{
	return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int read_regs(const struct device *dev, uint8_t reg,
		     uint8_t *buf, size_t len)
{
	const struct lsm6ds3_config *cfg = dev->config;

	return i2c_write_read_dt(&cfg->i2c, &reg, 1, buf, len);
}

static int write_reg(const struct device *dev, uint8_t reg, uint8_t val)
{
	const struct lsm6ds3_config *cfg = dev->config;
	uint8_t buf[2] = { reg, val };

	return i2c_write_dt(&cfg->i2c, buf, sizeof(buf));
}

/* ── Power up / down (ODR fields only; CTRL3_C/FS survive) ─────────────────── */

static int lsm6ds3_power_up(const struct device *dev)
{
	struct lsm6ds3_data *data = dev->data;
	int ret;

	ret = write_reg(dev, LSM6DS3_REG_CTRL1_XL, CTRL1_XL_104HZ_2G);
	if (ret < 0) {
		return ret;
	}
	ret = write_reg(dev, LSM6DS3_REG_CTRL2_G, CTRL2_G_104HZ_245DPS);
	if (ret < 0) {
		return ret;
	}
	data->powered = true;
	return 0;
}

static int lsm6ds3_power_down(const struct device *dev)
{
	struct lsm6ds3_data *data = dev->data;
	int ret;

	/* ODR=0 is the part's power-down state — accel and gyro both ~off. */
	ret = write_reg(dev, LSM6DS3_REG_CTRL1_XL, 0x00);
	if (ret < 0) {
		return ret;
	}
	ret = write_reg(dev, LSM6DS3_REG_CTRL2_G, 0x00);
	if (ret < 0) {
		return ret;
	}
	data->powered = false;
	return 0;
}

/* Inactivity timer: no reads for LSM6DS3_IDLE_POWERDOWN_S → power down and drop
 * the consumer flag.  A later read powers back up and re-arms this. */
static void lsm6ds3_idle_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct lsm6ds3_data *data =
		CONTAINER_OF(dwork, struct lsm6ds3_data, idle_work);

	if (data->powered) {
		(void)lsm6ds3_power_down(data->dev);
	}
	data->consumer_on = false;
	LOG_DBG("%s: idle power-down", data->dev->name);
}

/* ── sensor_driver_api ────────────────────────────────────────────────────── */

/* ── Lazy hardware init (called on first sample_fetch) ───────────────────── */

static int lsm6ds3_hw_init(const struct device *dev)
{
	const struct lsm6ds3_config *cfg = dev->config;
	uint8_t chip_id = 0;
	int ret;

	ret = read_regs(dev, LSM6DS3_REG_WHO_AM_I, &chip_id, 1);
	if (ret < 0) {
		/* I2C may be transiently busy right after boot — caller retries */
		LOG_DBG("%s: WHO_AM_I read failed: %d (will retry)", dev->name, ret);
		return ret;
	}

	LOG_INF("%s: WHO_AM_I = 0x%02x", dev->name, chip_id);

	if (chip_id != LSM6DS3_WHO_AM_I && chip_id != 0x6A) {
		LOG_ERR("%s: unexpected WHO_AM_I 0x%02x (need 0x69 or 0x6A)",
			dev->name, chip_id);
		return -ENODEV;
	}

	ret = write_reg(dev, LSM6DS3_REG_CTRL3_C, CTRL3_C_INIT);
	if (ret < 0) { LOG_ERR("%s: CTRL3_C write failed: %d", dev->name, ret); return ret; }

	/* Leave accel + gyro powered DOWN (ODR=0); the first sample_fetch() brings
	 * them to 104 Hz via lsm6ds3_power_up().  CTRL3_C (BDU/IF_INC) and the FS
	 * fields survive power-down, so a power-up only re-writes the ODR fields. */
	ret = write_reg(dev, LSM6DS3_REG_CTRL1_XL, 0x00);
	if (ret < 0) { LOG_ERR("%s: CTRL1_XL write failed: %d", dev->name, ret); return ret; }

	ret = write_reg(dev, LSM6DS3_REG_CTRL2_G, 0x00);
	if (ret < 0) { LOG_ERR("%s: CTRL2_G write failed: %d", dev->name, ret); return ret; }

	ARG_UNUSED(cfg);
	LOG_INF("%s: ready (WHO_AM_I=0x%02x, powered down until first read)",
		dev->name, chip_id);
	return 0;
}

static int lsm6ds3_sample_fetch(const struct device *dev,
				enum sensor_channel chan)
{
	struct lsm6ds3_data *data = dev->data;
	uint8_t buf[6];
	int ret;
	if (!data->initialized) {
		ret = lsm6ds3_hw_init(dev);
		if (ret < 0) {
			return ret;
		}
		data->initialized = true;
	}

	bool fetch_gyro  = (chan == SENSOR_CHAN_ALL ||
			    chan == SENSOR_CHAN_GYRO_XYZ ||
			    chan == SENSOR_CHAN_GYRO_X   ||
			    chan == SENSOR_CHAN_GYRO_Y   ||
			    chan == SENSOR_CHAN_GYRO_Z);
	bool fetch_accel = (chan == SENSOR_CHAN_ALL ||
			    chan == SENSOR_CHAN_ACCEL_XYZ ||
			    chan == SENSOR_CHAN_ACCEL_X   ||
			    chan == SENSOR_CHAN_ACCEL_Y   ||
			    chan == SENSOR_CHAN_ACCEL_Z);

	if (!fetch_gyro && !fetch_accel) {
		/* Not our channel — do NOT power the part up just because the
		 * generic sensor API probed us for an unrelated reading. */
		return -ENOTSUP;
	}

	/* First use (or first read after an idle power-down) brings the sensors
	 * to active ODR.  consumer_on is latched so a screen-blank suspend can be
	 * undone by RESUME only if a consumer was actually using the IMU. */
	if (!data->powered) {
		ret = lsm6ds3_power_up(dev);
		if (ret < 0) {
			LOG_ERR("%s: power-up failed: %d", dev->name, ret);
			return ret;
		}
	}
	data->consumer_on = true;
	k_work_reschedule(&data->idle_work, K_SECONDS(LSM6DS3_IDLE_POWERDOWN_S));

	if (fetch_gyro) {
		ret = read_regs(dev, LSM6DS3_REG_OUTX_L_G, buf, 6);
		if (ret < 0) {
			LOG_ERR("gyro read failed: %d", ret);
			return ret;
		}
		data->gyro[0] = le16_to_s16(&buf[0]);
		data->gyro[1] = le16_to_s16(&buf[2]);
		data->gyro[2] = le16_to_s16(&buf[4]);
	}

	if (fetch_accel) {
		ret = read_regs(dev, LSM6DS3_REG_OUTX_L_XL, buf, 6);
		if (ret < 0) {
			LOG_ERR("accel read failed: %d", ret);
			return ret;
		}
		data->accel[0] = le16_to_s16(&buf[0]);
		data->accel[1] = le16_to_s16(&buf[2]);
		data->accel[2] = le16_to_s16(&buf[4]);
	}

	return 0;
}

static int lsm6ds3_channel_get(const struct device *dev,
				enum sensor_channel chan,
				struct sensor_value *val)
{
	const struct lsm6ds3_data *data = dev->data;
	int64_t raw;
	int idx;

	switch (chan) {
	case SENSOR_CHAN_ACCEL_X:
	case SENSOR_CHAN_ACCEL_Y:
	case SENSOR_CHAN_ACCEL_Z:
		idx = (int)chan - (int)SENSOR_CHAN_ACCEL_X;
		raw = (int64_t)data->accel[idx] * ACCEL_SENS_UM_PER_LSB;
		/* raw is in µm/s² — val1 in m/s², val2 in µm/s² (millionths) */
		val->val1 = (int32_t)(raw / 1000000LL);
		val->val2 = (int32_t)(raw % 1000000LL);
		break;

	case SENSOR_CHAN_GYRO_X:
	case SENSOR_CHAN_GYRO_Y:
	case SENSOR_CHAN_GYRO_Z:
		idx = (int)chan - (int)SENSOR_CHAN_GYRO_X;
		raw = (int64_t)data->gyro[idx] * GYRO_SENS_NRAD_PER_LSB;
		/* raw is in nrad/s — val1 in rad/s, val2 in µrad/s (millionths) */
		val->val1 = (int32_t)(raw / 1000000000LL);
		val->val2 = (int32_t)((raw % 1000000000LL) / 1000LL);
		break;

	default:
		return -ENOTSUP;
	}

	return 0;
}

static const struct sensor_driver_api lsm6ds3_driver_api = {
	.sample_fetch = lsm6ds3_sample_fetch,
	.channel_get  = lsm6ds3_channel_get,
};

/* ── Initialisation ───────────────────────────────────────────────────────── */

static int lsm6ds3_init(const struct device *dev)
{
	const struct lsm6ds3_config *cfg = dev->config;

	/*
	 * Hardware access is deferred to the first sample_fetch() call.
	 *
	 * Reason: this driver runs at POST_KERNEL, but on ESP32 the I2C bus
	 * may not yet accept transfers this early even though
	 * device_is_ready(i2c_bus) returns true. Lazy init avoids the race
	 * and ensures all LOG* messages are visible (log backend is up by then).
	 */
	if (!device_is_ready(cfg->i2c.bus)) {
		LOG_ERR("%s: I2C bus %s not ready at POST_KERNEL",
			dev->name, cfg->i2c.bus->name);
		return -ENODEV;
	}

	/* Mark initialized=false so first sample_fetch does hw setup */
	struct lsm6ds3_data *data = dev->data;
	data->initialized = false;
	data->powered = false;
	data->consumer_on = false;
	data->dev = dev;
	k_work_init_delayable(&data->idle_work, lsm6ds3_idle_work_handler);
	return 0;
}

/* ── Power management ─────────────────────────────────────────────────────── */

#ifdef CONFIG_PM_DEVICE
/*
 * Screen-blank gating from power_manager (akira_pm_gate_peripherals()).
 *
 * The part is already demand-powered: it idles at ODR=0 unless a consumer is
 * actively reading (see sample_fetch + idle_work).  So SUSPEND only forces a
 * power-down if a consumer currently has it on, and — crucially — RESUME must
 * NOT blindly power the sensors back up: it does so ONLY when consumer_on shows
 * a reader had them running before the blank.  Otherwise an unused IMU would be
 * woken to 104 Hz on every screen-on, defeating the whole scheme.
 *
 * consumer_on is preserved across SUSPEND (the idle_work that would otherwise
 * clear it is cancelled) so RESUME can make the right decision.
 *
 * Lazy-init contract: nothing is touched until the first sample_fetch() has run
 * (I2C may still be unusable this early on ESP32), so both actions are no-ops
 * until data->initialized is set.
 */
static int lsm6ds3_pm_action(const struct device *dev,
			     enum pm_device_action action)
{
	struct lsm6ds3_data *data = dev->data;

	if (!data->initialized) {
		return 0;
	}

	switch (action) {
	case PM_DEVICE_ACTION_SUSPEND:
		/* Keep consumer_on intact for RESUME; stop the idle timer so it
		 * cannot clear it while blanked. */
		k_work_cancel_delayable(&data->idle_work);
		if (data->powered) {
			return lsm6ds3_power_down(dev);
		}
		return 0;

	case PM_DEVICE_ACTION_RESUME:
		if (!data->consumer_on) {
			return 0; /* nobody was using it — leave it asleep */
		}
		if (!data->powered) {
			int ret = lsm6ds3_power_up(dev);
			if (ret < 0) {
				return ret;
			}
		}
		k_work_reschedule(&data->idle_work,
				  K_SECONDS(LSM6DS3_IDLE_POWERDOWN_S));
		return 0;

	default:
		return -ENOTSUP;
	}
}
#endif /* CONFIG_PM_DEVICE */

/* ── Device instantiation ─────────────────────────────────────────────────── */

#define LSM6DS3_DEFINE(inst)						\
	static struct lsm6ds3_data lsm6ds3_data_##inst;			\
	static const struct lsm6ds3_config lsm6ds3_cfg_##inst = {	\
		.i2c = I2C_DT_SPEC_INST_GET(inst),			\
	};								\
	PM_DEVICE_DT_INST_DEFINE(inst, lsm6ds3_pm_action);		\
	SENSOR_DEVICE_DT_INST_DEFINE(inst,				\
				     lsm6ds3_init,			\
				     PM_DEVICE_DT_INST_GET(inst),	\
				     &lsm6ds3_data_##inst,		\
				     &lsm6ds3_cfg_##inst,		\
				     POST_KERNEL,			\
				     CONFIG_SENSOR_INIT_PRIORITY,	\
				     &lsm6ds3_driver_api);

DT_INST_FOREACH_STATUS_OKAY(LSM6DS3_DEFINE)
