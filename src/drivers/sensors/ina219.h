/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * INA219 Current/Power Monitor Driver for AkiraOS
 */

#ifndef INA219_H
#define INA219_H

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* I2C Addresses (A0, A1 pins) */
#define INA219_I2C_ADDR_DEFAULT 0x40
#define INA219_I2C_ADDR_A0 0x41
#define INA219_I2C_ADDR_A1 0x44
#define INA219_I2C_ADDR_A0_A1 0x45

/* Register Map */
#define INA219_REG_CONFIG 0x00
#define INA219_REG_SHUNT_VOLTAGE 0x01
#define INA219_REG_BUS_VOLTAGE 0x02
#define INA219_REG_POWER 0x03
#define INA219_REG_CURRENT 0x04
#define INA219_REG_CALIBRATION 0x05

/* Configuration bits */
#define INA219_CONFIG_RESET 0x8000

    /* Bus voltage range */
    enum ina219_bus_range
    {
        INA219_BUS_RANGE_16V = 0,
        INA219_BUS_RANGE_32V = 1
    };

    /* PGA gain */
    enum ina219_pga_gain
    {
        INA219_PGA_GAIN_1_40MV = 0,
        INA219_PGA_GAIN_2_80MV = 1,
        INA219_PGA_GAIN_4_160MV = 2,
        INA219_PGA_GAIN_8_320MV = 3
    };

    /* ADC resolution/averaging */
    enum ina219_adc_mode
    {
        INA219_ADC_9BIT_1SAMPLE = 0,
        INA219_ADC_10BIT_1SAMPLE = 1,
        INA219_ADC_11BIT_1SAMPLE = 2,
        INA219_ADC_12BIT_1SAMPLE = 3,
        INA219_ADC_12BIT_2SAMPLES = 9,
        INA219_ADC_12BIT_4SAMPLES = 10,
        INA219_ADC_12BIT_8SAMPLES = 11,
        INA219_ADC_12BIT_16SAMPLES = 12,
        INA219_ADC_12BIT_32SAMPLES = 13,
        INA219_ADC_12BIT_64SAMPLES = 14,
        INA219_ADC_12BIT_128SAMPLES = 15
    };

    /* Operating mode */
    enum ina219_mode
    {
        INA219_MODE_POWER_DOWN = 0,
        INA219_MODE_SHUNT_TRIGGERED = 1,
        INA219_MODE_BUS_TRIGGERED = 2,
        INA219_MODE_SHUNT_BUS_TRIGGERED = 3,
        INA219_MODE_ADC_OFF = 4,
        INA219_MODE_SHUNT_CONTINUOUS = 5,
        INA219_MODE_BUS_CONTINUOUS = 6,
        INA219_MODE_SHUNT_BUS_CONTINUOUS = 7
    };

    /* Configuration structure */
    struct ina219_config
    {
        const struct device *i2c_dev;
        uint8_t i2c_addr;
        enum ina219_bus_range bus_range;
        enum ina219_pga_gain pga_gain;
        enum ina219_adc_mode bus_adc;
        enum ina219_adc_mode shunt_adc;
        enum ina219_mode mode;
        float shunt_resistor_ohms;       /* Shunt resistor value in ohms */
        float max_expected_current_amps; /* Maximum expected current in amps */
    };

    /* Measurement data */
    struct ina219_measurement
    {
        float bus_voltage_v;    /* Bus voltage in volts */
        float shunt_voltage_mv; /* Shunt voltage in millivolts */
        float current_ma;       /* Current in milliamps */
        float power_mw;         /* Power in milliwatts */
    };

    /**
     * @brief Initialize INA219 sensor
     *
     * @param config Configuration structure
     * @return 0 on success, negative errno on failure
     */
    int ina219_init(struct ina219_config *config);

    /**
     * @brief Read all measurements
     *
     * @param config Configuration structure
     * @param data Output measurement structure
     * @return 0 on success, negative errno on failure
     */
    int ina219_read_all(struct ina219_config *config, struct ina219_measurement *data);

    /**
     * @brief Read bus voltage only
     *
     * @param config Configuration structure
     * @param voltage Bus voltage in volts (output)
     * @return 0 on success, negative errno on failure
     */
    int ina219_read_bus_voltage(struct ina219_config *config, float *voltage);

    /**
     * @brief Read shunt voltage only
     *
     * @param config Configuration structure
     * @param voltage Shunt voltage in millivolts (output)
     * @return 0 on success, negative errno on failure
     */
    int ina219_read_shunt_voltage(struct ina219_config *config, float *voltage);

    /**
     * @brief Read current only
     *
     * @param config Configuration structure
     * @param current Current in milliamps (output)
     * @return 0 on success, negative errno on failure
     */
    int ina219_read_current(struct ina219_config *config, float *current);

    /**
     * @brief Read power only
     *
     * @param config Configuration structure
     * @param power Power in milliwatts (output)
     * @return 0 on success, negative errno on failure
     */
    int ina219_read_power(struct ina219_config *config, float *power);

    /**
     * @brief Set operating mode
     *
     * @param config Configuration structure
     * @param mode Operating mode
     * @return 0 on success, negative errno on failure
     */
    int ina219_set_mode(struct ina219_config *config, enum ina219_mode mode);

    /**
     * @brief Reset the sensor
     *
     * @param config Configuration structure
     * @return 0 on success, negative errno on failure
     */
    int ina219_reset(struct ina219_config *config);

    /**
     * @brief Enter sleep mode (power down)
     *
     * @param config Configuration structure
     * @return 0 on success, negative errno on failure
     */
    int ina219_sleep(struct ina219_config *config);

    /**
     * @brief Wake from sleep mode
     *
     * @param config Configuration structure
     * @return 0 on success, negative errno on failure
     */
    int ina219_wake(struct ina219_config *config);

#ifdef __cplusplus
}
#endif

#endif /* INA219_H */
