/**
 * @file bme280.h
 * @brief BME280 Environmental Sensor Driver
 */

#ifndef AKIRA_BME280_H
#define AKIRA_BME280_H

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief BME280 oversampling settings
     */
    typedef enum
    {
        BME280_OVERSAMPLING_SKIP = 0,
        BME280_OVERSAMPLING_1X = 1,
        BME280_OVERSAMPLING_2X = 2,
        BME280_OVERSAMPLING_4X = 3,
        BME280_OVERSAMPLING_8X = 4,
        BME280_OVERSAMPLING_16X = 5
    } bme280_oversampling_t;

    /**
     * @brief BME280 configuration
     */
    struct bme280_config
    {
        const struct device *i2c_dev;
        uint16_t i2c_addr; // 0x76 or 0x77
        bme280_oversampling_t temp_os;
        bme280_oversampling_t hum_os;
        bme280_oversampling_t press_os;
    };

    /**
     * @brief BME280 readings
     */
    struct bme280_data
    {
        float temperature; // °C
        float humidity;    // %RH
        float pressure;    // hPa
    };

    /**
     * @brief Initialize BME280 sensor
     * @param config Hardware configuration
     * @return 0 on success
     */
    int bme280_init(const struct bme280_config *config);

    /**
     * @brief Read all sensor values
     * @param data Output for sensor data
     * @return 0 on success
     */
    int bme280_read(struct bme280_data *data);

    /**
     * @brief Read temperature only
     * @param temperature Output for temperature (°C)
     * @return 0 on success
     */
    int bme280_read_temperature(float *temperature);

    /**
     * @brief Read humidity only
     * @param humidity Output for humidity (%RH)
     * @return 0 on success
     */
    int bme280_read_humidity(float *humidity);

    /**
     * @brief Read pressure only
     * @param pressure Output for pressure (hPa)
     * @return 0 on success
     */
    int bme280_read_pressure(float *pressure);

    /**
     * @brief Put sensor in sleep mode
     * @return 0 on success
     */
    int bme280_sleep(void);

    /**
     * @brief Calculate altitude from pressure
     * @param pressure Current pressure (hPa)
     * @param sea_level_pressure Reference pressure (hPa)
     * @return Altitude in meters
     */
    float bme280_calculate_altitude(float pressure, float sea_level_pressure);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_BME280_H */
