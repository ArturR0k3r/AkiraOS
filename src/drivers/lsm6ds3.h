/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LSM6DS3 6-axis IMU (Accelerometer + Gyroscope) Driver for AkiraOS
 */

#ifndef LSM6DS3_H
#define LSM6DS3_H

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* I2C Address */
#define LSM6DS3_I2C_ADDR_PRIMARY 0x6A
#define LSM6DS3_I2C_ADDR_SECONDARY 0x6B

/* Register Map */
#define LSM6DS3_WHO_AM_I 0x0F
#define LSM6DS3_CTRL1_XL 0x10
#define LSM6DS3_CTRL2_G 0x11
#define LSM6DS3_CTRL3_C 0x12
#define LSM6DS3_STATUS_REG 0x1E
#define LSM6DS3_OUT_TEMP_L 0x20
#define LSM6DS3_OUT_TEMP_H 0x21
#define LSM6DS3_OUTX_L_G 0x22
#define LSM6DS3_OUTX_H_G 0x23
#define LSM6DS3_OUTY_L_G 0x24
#define LSM6DS3_OUTY_H_G 0x25
#define LSM6DS3_OUTZ_L_G 0x26
#define LSM6DS3_OUTZ_H_G 0x27
#define LSM6DS3_OUTX_L_XL 0x28
#define LSM6DS3_OUTX_H_XL 0x29
#define LSM6DS3_OUTY_L_XL 0x2A
#define LSM6DS3_OUTY_H_XL 0x2B
#define LSM6DS3_OUTZ_L_XL 0x2C
#define LSM6DS3_OUTZ_H_XL 0x2D

/* WHO_AM_I value */
#define LSM6DS3_WHO_AM_I_VALUE 0x69

    /* Accelerometer ODR */
    enum lsm6ds3_accel_odr
    {
        LSM6DS3_ACCEL_ODR_POWER_DOWN = 0,
        LSM6DS3_ACCEL_ODR_13Hz = 1,
        LSM6DS3_ACCEL_ODR_26Hz = 2,
        LSM6DS3_ACCEL_ODR_52Hz = 3,
        LSM6DS3_ACCEL_ODR_104Hz = 4,
        LSM6DS3_ACCEL_ODR_208Hz = 5,
        LSM6DS3_ACCEL_ODR_416Hz = 6,
        LSM6DS3_ACCEL_ODR_833Hz = 7,
        LSM6DS3_ACCEL_ODR_1660Hz = 8,
        LSM6DS3_ACCEL_ODR_3330Hz = 9,
        LSM6DS3_ACCEL_ODR_6660Hz = 10
    };

    /* Accelerometer range */
    enum lsm6ds3_accel_range
    {
        LSM6DS3_ACCEL_RANGE_2G = 0,
        LSM6DS3_ACCEL_RANGE_4G = 2,
        LSM6DS3_ACCEL_RANGE_8G = 3,
        LSM6DS3_ACCEL_RANGE_16G = 1
    };

    /* Gyroscope ODR */
    enum lsm6ds3_gyro_odr
    {
        LSM6DS3_GYRO_ODR_POWER_DOWN = 0,
        LSM6DS3_GYRO_ODR_13Hz = 1,
        LSM6DS3_GYRO_ODR_26Hz = 2,
        LSM6DS3_GYRO_ODR_52Hz = 3,
        LSM6DS3_GYRO_ODR_104Hz = 4,
        LSM6DS3_GYRO_ODR_208Hz = 5,
        LSM6DS3_GYRO_ODR_416Hz = 6,
        LSM6DS3_GYRO_ODR_833Hz = 7,
        LSM6DS3_GYRO_ODR_1660Hz = 8
    };

    /* Gyroscope range */
    enum lsm6ds3_gyro_range
    {
        LSM6DS3_GYRO_RANGE_250DPS = 0,
        LSM6DS3_GYRO_RANGE_500DPS = 1,
        LSM6DS3_GYRO_RANGE_1000DPS = 2,
        LSM6DS3_GYRO_RANGE_2000DPS = 3
    };

    /* Configuration structure */
    struct lsm6ds3_config
    {
        const struct device *i2c_dev;
        uint8_t i2c_addr;
        enum lsm6ds3_accel_odr accel_odr;
        enum lsm6ds3_accel_range accel_range;
        enum lsm6ds3_gyro_odr gyro_odr;
        enum lsm6ds3_gyro_range gyro_range;
    };

    /* Data structures */
    struct lsm6ds3_accel_data
    {
        float x; /* m/s^2 */
        float y; /* m/s^2 */
        float z; /* m/s^2 */
    };

    struct lsm6ds3_gyro_data
    {
        float x; /* deg/s */
        float y; /* deg/s */
        float z; /* deg/s */
    };

    /**
     * @brief Initialize LSM6DS3 sensor
     *
     * @param config Configuration structure
     * @return 0 on success, negative errno on failure
     */
    int lsm6ds3_init(struct lsm6ds3_config *config);

    /**
     * @brief Read accelerometer data
     *
     * @param config Configuration structure
     * @param data Output data structure
     * @return 0 on success, negative errno on failure
     */
    int lsm6ds3_read_accel(struct lsm6ds3_config *config, struct lsm6ds3_accel_data *data);

    /**
     * @brief Read gyroscope data
     *
     * @param config Configuration structure
     * @param data Output data structure
     * @return 0 on success, negative errno on failure
     */
    int lsm6ds3_read_gyro(struct lsm6ds3_config *config, struct lsm6ds3_gyro_data *data);

    /**
     * @brief Read temperature
     *
     * @param config Configuration structure
     * @param temp Temperature in Celsius
     * @return 0 on success, negative errno on failure
     */
    int lsm6ds3_read_temperature(struct lsm6ds3_config *config, float *temp);

    /**
     * @brief Check if data is ready
     *
     * @param config Configuration structure
     * @param accel_ready Accelerometer data ready flag (output)
     * @param gyro_ready Gyroscope data ready flag (output)
     * @return 0 on success, negative errno on failure
     */
    int lsm6ds3_data_ready(struct lsm6ds3_config *config, bool *accel_ready, bool *gyro_ready);

    /**
     * @brief Reset the sensor
     *
     * @param config Configuration structure
     * @return 0 on success, negative errno on failure
     */
    int lsm6ds3_reset(struct lsm6ds3_config *config);

#ifdef __cplusplus
}
#endif

#endif /* LSM6DS3_H */
