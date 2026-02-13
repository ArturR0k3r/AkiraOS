/**
 * @file akira_sensor_api.h
 * @brief Sensor API declarations for WASM exports
 */

#ifndef AKIRA_SENSOR_API_H
#define AKIRA_SENSOR_API_H

#include <stdint.h>
#include <wasm_export.h>

/* Sensor types */
typedef enum {
    SENSOR_TYPE_TEMP = 0,
    SENSOR_TYPE_HUMIDITY,
    SENSOR_TYPE_PRESSURE,
    SENSOR_TYPE_ACCEL_X,
    SENSOR_TYPE_ACCEL_Y,
    SENSOR_TYPE_ACCEL_Z,
    SENSOR_TYPE_GYRO_X,
    SENSOR_TYPE_GYRO_Y,
    SENSOR_TYPE_GYRO_Z
} akira_sensor_type_t;

/* IMU data structure */
typedef struct {
    float accel_x;
    float accel_y;
    float accel_z;
    float gyro_x;
    float gyro_y;
    float gyro_z;
} akira_imu_data_t;

/* Environmental data structure */
typedef struct {
    float temperature;
    float humidity;
    float pressure;
} akira_env_data_t;

/* Power data structure */
typedef struct {
    float voltage;
    float current;
    float power;
} akira_power_data_t;

/* Core sensor API functions (no security checks) */
int akira_sensor_read(akira_sensor_type_t type, float *value);
int akira_sensor_read_imu(akira_imu_data_t *data);
int akira_sensor_read_env(akira_env_data_t *data);
int akira_sensor_read_power(akira_power_data_t *data);

#ifdef CONFIG_AKIRA_WASM_RUNTIME
/* WASM native export functions (with capability checks) */
int akira_native_sensor_read(wasm_exec_env_t exec_env, int32_t type);
#endif /* CONFIG_AKIRA_WASM_RUNTIME */

#endif /* AKIRA_SENSOR_API_H */