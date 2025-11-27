/**
 * @file akira_sensor_api.c
 * @brief Sensor API implementation for WASM exports
 */

#include "akira_api.h"
#include "../drivers/lsm6ds3.h"
#include "../drivers/ina219.h"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(akira_sensor_api, LOG_LEVEL_INF);

// TODO: Add capability check before each API call
// TODO: Add sensor discovery/enumeration
// TODO: Add calibration storage
// TODO: Add filtering/averaging
// TODO: Add sensor fusion (complementary filter, etc.)

int akira_sensor_read(akira_sensor_type_t type, float *value)
{
	// TODO: Check CAP_SENSOR_<type>_READ capability
	
	if (!value) {
		return -1;
	}
	
	switch (type) {
	case SENSOR_TYPE_TEMP:
		// TODO: Read from BME280 or LSM6DS3 temperature
		*value = 25.0f; // Placeholder
		break;
		
	case SENSOR_TYPE_HUMIDITY:
		// TODO: Read from BME280
		*value = 50.0f; // Placeholder
		break;
		
	case SENSOR_TYPE_PRESSURE:
		// TODO: Read from BME280 or BMP280
		*value = 1013.25f; // Placeholder
		break;
		
	case SENSOR_TYPE_LIGHT:
		// TODO: Read from VEML7700 or similar
		*value = 100.0f; // Placeholder
		break;
		
	case SENSOR_TYPE_VOLTAGE:
		// TODO: Read from INA219
		*value = 3.3f; // Placeholder
		break;
		
	case SENSOR_TYPE_CURRENT:
		// TODO: Read from INA219
		*value = 0.1f; // Placeholder
		break;
		
	case SENSOR_TYPE_POWER:
		// TODO: Read from INA219
		*value = 0.33f; // Placeholder
		break;
		
	default:
		LOG_WRN("Unknown sensor type: %d", type);
		return -2;
	}
	
	return 0;
}

int akira_sensor_read_imu(akira_imu_data_t *data)
{
	// TODO: Check CAP_SENSOR_IMU_READ capability
	// TODO: Read from LSM6DS3 or MPU6050
	
	if (!data) {
		return -1;
	}
	
	// TODO: Call lsm6ds3_read_accel() and lsm6ds3_read_gyro()
	// TODO: Apply calibration offsets
	// TODO: Convert to SI units (m/s², rad/s)
	
	data->accel_x = 0.0f;
	data->accel_y = 0.0f;
	data->accel_z = 9.81f; // 1g downward
	data->gyro_x = 0.0f;
	data->gyro_y = 0.0f;
	data->gyro_z = 0.0f;
	
	LOG_DBG("IMU read: ax=%.2f ay=%.2f az=%.2f", 
	        data->accel_x, data->accel_y, data->accel_z);
	
	return 0;
}

int akira_sensor_read_env(akira_env_data_t *data)
{
	// TODO: Check CAP_SENSOR_ENV_READ capability
	// TODO: Read from BME280
	
	if (!data) {
		return -1;
	}
	
	// TODO: Call bme280_read()
	
	data->temperature = 25.0f;  // °C
	data->humidity = 50.0f;     // %RH
	data->pressure = 1013.25f;  // hPa
	
	LOG_DBG("ENV read: T=%.1f H=%.1f P=%.1f", 
	        data->temperature, data->humidity, data->pressure);
	
	return 0;
}

int akira_sensor_read_power(akira_power_data_t *data)
{
	// TODO: Check CAP_SENSOR_POWER_READ capability
	// TODO: Read from INA219
	
	if (!data) {
		return -1;
	}
	
	// TODO: Call ina219_read()
	
	data->voltage = 3.7f;   // V
	data->current = 0.15f;  // A
	data->power = 0.555f;   // W
	
	LOG_DBG("Power read: V=%.2f I=%.3f P=%.3f", 
	        data->voltage, data->current, data->power);
	
	return 0;
}
