/*
 * SPDX-License-Identifier: Apache-2.0
 * 
 * LSM6DS3 6-axis IMU Driver Implementation
 */

#include "lsm6ds3.h"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(lsm6ds3, CONFIG_LOG_DEFAULT_LEVEL);

/* Sensitivity factors */
static const float accel_sensitivity[] = {
	0.061f,   /* ±2g */
	0.122f,   /* ±4g (also for 16g at index 1) */
	0.244f,   /* ±8g */
	0.488f    /* ±16g */
};

static const float gyro_sensitivity[] = {
	8.75f,    /* ±250 dps */
	17.50f,   /* ±500 dps */
	35.0f,    /* ±1000 dps */
	70.0f     /* ±2000 dps */
};

static int lsm6ds3_write_reg(struct lsm6ds3_config *config, uint8_t reg, uint8_t value)
{
	uint8_t buf[2] = {reg, value};
	return i2c_write(config->i2c_dev, buf, 2, config->i2c_addr);
}

static int lsm6ds3_read_reg(struct lsm6ds3_config *config, uint8_t reg, uint8_t *value)
{
	return i2c_write_read(config->i2c_dev, config->i2c_addr, &reg, 1, value, 1);
}

static int lsm6ds3_read_regs(struct lsm6ds3_config *config, uint8_t reg, 
                              uint8_t *data, size_t len)
{
	return i2c_write_read(config->i2c_dev, config->i2c_addr, &reg, 1, data, len);
}

int lsm6ds3_init(struct lsm6ds3_config *config)
{
	int ret;
	uint8_t who_am_i;
	
	if (!config || !config->i2c_dev) {
		return -EINVAL;
	}
	
	/* Check WHO_AM_I */
	ret = lsm6ds3_read_reg(config, LSM6DS3_WHO_AM_I, &who_am_i);
	if (ret < 0) {
		LOG_ERR("Failed to read WHO_AM_I: %d", ret);
		return ret;
	}
	
	if (who_am_i != LSM6DS3_WHO_AM_I_VALUE) {
		LOG_ERR("Invalid WHO_AM_I: 0x%02x (expected 0x%02x)", 
		        who_am_i, LSM6DS3_WHO_AM_I_VALUE);
		return -ENODEV;
	}
	
	/* Reset device */
	ret = lsm6ds3_reset(config);
	if (ret < 0) {
		return ret;
	}
	
	k_msleep(10);
	
	/* Configure accelerometer */
	uint8_t accel_cfg = (config->accel_odr << 4) | (config->accel_range << 2);
	ret = lsm6ds3_write_reg(config, LSM6DS3_CTRL1_XL, accel_cfg);
	if (ret < 0) {
		LOG_ERR("Failed to configure accelerometer: %d", ret);
		return ret;
	}
	
	/* Configure gyroscope */
	uint8_t gyro_cfg = (config->gyro_odr << 4) | (config->gyro_range << 2);
	ret = lsm6ds3_write_reg(config, LSM6DS3_CTRL2_G, gyro_cfg);
	if (ret < 0) {
		LOG_ERR("Failed to configure gyroscope: %d", ret);
		return ret;
	}
	
	/* Enable block data update */
	ret = lsm6ds3_write_reg(config, LSM6DS3_CTRL3_C, 0x44);
	if (ret < 0) {
		return ret;
	}
	
	LOG_INF("LSM6DS3 initialized (accel: ODR=%d range=%d, gyro: ODR=%d range=%d)",
	        config->accel_odr, config->accel_range,
	        config->gyro_odr, config->gyro_range);
	
	return 0;
}

int lsm6ds3_read_accel(struct lsm6ds3_config *config, struct lsm6ds3_accel_data *data)
{
	uint8_t raw_data[6];
	int ret;
	
	ret = lsm6ds3_read_regs(config, LSM6DS3_OUTX_L_XL, raw_data, 6);
	if (ret < 0) {
		return ret;
	}
	
	int16_t raw_x = (int16_t)((raw_data[1] << 8) | raw_data[0]);
	int16_t raw_y = (int16_t)((raw_data[3] << 8) | raw_data[2]);
	int16_t raw_z = (int16_t)((raw_data[5] << 8) | raw_data[4]);
	
	/* Get sensitivity factor */
	uint8_t range_idx = config->accel_range;
	if (range_idx == 1) {  /* 16g uses index 3 */
		range_idx = 3;
	}
	float sensitivity = accel_sensitivity[range_idx];
	
	/* Convert to m/s^2 (multiply by 9.81/1000) */
	data->x = (raw_x * sensitivity * 9.81f) / 1000.0f;
	data->y = (raw_y * sensitivity * 9.81f) / 1000.0f;
	data->z = (raw_z * sensitivity * 9.81f) / 1000.0f;
	
	return 0;
}

int lsm6ds3_read_gyro(struct lsm6ds3_config *config, struct lsm6ds3_gyro_data *data)
{
	uint8_t raw_data[6];
	int ret;
	
	ret = lsm6ds3_read_regs(config, LSM6DS3_OUTX_L_G, raw_data, 6);
	if (ret < 0) {
		return ret;
	}
	
	int16_t raw_x = (int16_t)((raw_data[1] << 8) | raw_data[0]);
	int16_t raw_y = (int16_t)((raw_data[3] << 8) | raw_data[2]);
	int16_t raw_z = (int16_t)((raw_data[5] << 8) | raw_data[4]);
	
	/* Get sensitivity factor */
	float sensitivity = gyro_sensitivity[config->gyro_range];
	
	/* Convert to deg/s (divide by 1000) */
	data->x = (raw_x * sensitivity) / 1000.0f;
	data->y = (raw_y * sensitivity) / 1000.0f;
	data->z = (raw_z * sensitivity) / 1000.0f;
	
	return 0;
}

int lsm6ds3_read_temperature(struct lsm6ds3_config *config, float *temp)
{
	uint8_t raw_data[2];
	int ret;
	
	ret = lsm6ds3_read_regs(config, LSM6DS3_OUT_TEMP_L, raw_data, 2);
	if (ret < 0) {
		return ret;
	}
	
	int16_t raw_temp = (int16_t)((raw_data[1] << 8) | raw_data[0]);
	
	/* Temperature in Celsius = (raw / 256) + 25 */
	*temp = (raw_temp / 256.0f) + 25.0f;
	
	return 0;
}

int lsm6ds3_data_ready(struct lsm6ds3_config *config, bool *accel_ready, bool *gyro_ready)
{
	uint8_t status;
	int ret;
	
	ret = lsm6ds3_read_reg(config, LSM6DS3_STATUS_REG, &status);
	if (ret < 0) {
		return ret;
	}
	
	*accel_ready = (status & 0x01) != 0;
	*gyro_ready = (status & 0x02) != 0;
	
	return 0;
}

int lsm6ds3_reset(struct lsm6ds3_config *config)
{
	int ret;
	
	/* Software reset */
	ret = lsm6ds3_write_reg(config, LSM6DS3_CTRL3_C, 0x01);
	if (ret < 0) {
		return ret;
	}
	
	/* Wait for reset to complete */
	k_msleep(10);
	
	uint8_t ctrl3;
	int timeout = 100;
	while (timeout--) {
		ret = lsm6ds3_read_reg(config, LSM6DS3_CTRL3_C, &ctrl3);
		if (ret < 0) {
			return ret;
		}
		if ((ctrl3 & 0x01) == 0) {
			break;
		}
		k_msleep(1);
	}
	
	if (timeout <= 0) {
		LOG_ERR("Reset timeout");
		return -ETIMEDOUT;
	}
	
	return 0;
}
