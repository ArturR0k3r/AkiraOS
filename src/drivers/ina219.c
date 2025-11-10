/*
 * SPDX-License-Identifier: Apache-2.0
 * 
 * INA219 Current/Power Monitor Driver Implementation
 */

#include "ina219.h"
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(ina219, CONFIG_LOG_DEFAULT_LEVEL);

static int ina219_write_reg(struct ina219_config *config, uint8_t reg, uint16_t value)
{
	uint8_t buf[3];
	buf[0] = reg;
	buf[1] = (value >> 8) & 0xFF;
	buf[2] = value & 0xFF;
	return i2c_write(config->i2c_dev, buf, 3, config->i2c_addr);
}

static int ina219_read_reg(struct ina219_config *config, uint8_t reg, uint16_t *value)
{
	uint8_t buf[2];
	int ret;
	
	ret = i2c_write_read(config->i2c_dev, config->i2c_addr, &reg, 1, buf, 2);
	if (ret < 0) {
		return ret;
	}
	
	*value = (buf[0] << 8) | buf[1];
	return 0;
}

static uint16_t ina219_calculate_calibration(struct ina219_config *config)
{
	/* Calibration = 0.04096 / (Current_LSB * Shunt_Resistor) */
	/* Current_LSB = Max_Expected_Current / 32767 */
	
	float current_lsb = config->max_expected_current_amps / 32767.0f;
	float calibration = 0.04096f / (current_lsb * config->shunt_resistor_ohms);
	
	/* Store current LSB for later use */
	/* This would typically be stored in a runtime structure */
	
	return (uint16_t)calibration;
}

int ina219_init(struct ina219_config *config)
{
	int ret;
	
	if (!config || !config->i2c_dev) {
		return -EINVAL;
	}
	
	/* Reset device */
	ret = ina219_reset(config);
	if (ret < 0) {
		LOG_ERR("Failed to reset INA219: %d", ret);
		return ret;
	}
	
	k_msleep(5);
	
	/* Build configuration register */
	uint16_t config_reg = 0;
	config_reg |= (config->bus_range & 0x01) << 13;
	config_reg |= (config->pga_gain & 0x03) << 11;
	config_reg |= (config->bus_adc & 0x0F) << 7;
	config_reg |= (config->shunt_adc & 0x0F) << 3;
	config_reg |= (config->mode & 0x07);
	
	ret = ina219_write_reg(config, INA219_REG_CONFIG, config_reg);
	if (ret < 0) {
		LOG_ERR("Failed to write config: %d", ret);
		return ret;
	}
	
	/* Calculate and write calibration */
	uint16_t calibration = ina219_calculate_calibration(config);
	ret = ina219_write_reg(config, INA219_REG_CALIBRATION, calibration);
	if (ret < 0) {
		LOG_ERR("Failed to write calibration: %d", ret);
		return ret;
	}
	
	LOG_INF("INA219 initialized (addr=0x%02x, shunt=%.3fΩ, max_current=%.3fA, cal=0x%04x)",
	        config->i2c_addr, config->shunt_resistor_ohms,
	        config->max_expected_current_amps, calibration);
	
	return 0;
}

int ina219_read_bus_voltage(struct ina219_config *config, float *voltage)
{
	uint16_t raw;
	int ret;
	
	ret = ina219_read_reg(config, INA219_REG_BUS_VOLTAGE, &raw);
	if (ret < 0) {
		return ret;
	}
	
	/* Check for valid data (bit 1 = 1 means conversion ready) */
	if ((raw & 0x02) == 0) {
		return -EAGAIN;
	}
	
	/* Shift right 3 bits, LSB = 4mV */
	*voltage = ((raw >> 3) * 4) / 1000.0f;
	
	return 0;
}

int ina219_read_shunt_voltage(struct ina219_config *config, float *voltage)
{
	uint16_t raw;
	int ret;
	
	ret = ina219_read_reg(config, INA219_REG_SHUNT_VOLTAGE, &raw);
	if (ret < 0) {
		return ret;
	}
	
	/* Convert to signed value */
	int16_t signed_val = (int16_t)raw;
	
	/* LSB = 10µV = 0.01mV */
	*voltage = signed_val * 0.01f;
	
	return 0;
}

int ina219_read_current(struct ina219_config *config, float *current)
{
	uint16_t raw;
	int ret;
	
	ret = ina219_read_reg(config, INA219_REG_CURRENT, &raw);
	if (ret < 0) {
		return ret;
	}
	
	/* Convert to signed value */
	int16_t signed_val = (int16_t)raw;
	
	/* Calculate current LSB */
	float current_lsb = config->max_expected_current_amps / 32767.0f;
	
	/* Current in amps, convert to mA */
	*current = (signed_val * current_lsb) * 1000.0f;
	
	return 0;
}

int ina219_read_power(struct ina219_config *config, float *power)
{
	uint16_t raw;
	int ret;
	
	ret = ina219_read_reg(config, INA219_REG_POWER, &raw);
	if (ret < 0) {
		return ret;
	}
	
	/* Calculate power LSB = 20 * Current_LSB */
	float current_lsb = config->max_expected_current_amps / 32767.0f;
	float power_lsb = 20.0f * current_lsb;
	
	/* Power in watts, convert to mW */
	*power = (raw * power_lsb) * 1000.0f;
	
	return 0;
}

int ina219_read_all(struct ina219_config *config, struct ina219_measurement *data)
{
	int ret;
	
	ret = ina219_read_bus_voltage(config, &data->bus_voltage_v);
	if (ret < 0) {
		return ret;
	}
	
	ret = ina219_read_shunt_voltage(config, &data->shunt_voltage_mv);
	if (ret < 0) {
		return ret;
	}
	
	ret = ina219_read_current(config, &data->current_ma);
	if (ret < 0) {
		return ret;
	}
	
	ret = ina219_read_power(config, &data->power_mw);
	if (ret < 0) {
		return ret;
	}
	
	return 0;
}

int ina219_set_mode(struct ina219_config *config, enum ina219_mode mode)
{
	uint16_t config_reg;
	int ret;
	
	ret = ina219_read_reg(config, INA219_REG_CONFIG, &config_reg);
	if (ret < 0) {
		return ret;
	}
	
	/* Clear mode bits and set new mode */
	config_reg = (config_reg & ~0x07) | (mode & 0x07);
	
	ret = ina219_write_reg(config, INA219_REG_CONFIG, config_reg);
	if (ret < 0) {
		return ret;
	}
	
	config->mode = mode;
	return 0;
}

int ina219_reset(struct ina219_config *config)
{
	return ina219_write_reg(config, INA219_REG_CONFIG, INA219_CONFIG_RESET);
}

int ina219_sleep(struct ina219_config *config)
{
	return ina219_set_mode(config, INA219_MODE_POWER_DOWN);
}

int ina219_wake(struct ina219_config *config)
{
	return ina219_set_mode(config, config->mode);
}
