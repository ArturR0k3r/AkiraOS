/**
 * @file akira_sensor_api.c
 * @brief Sensor API implementation for WASM exports
 */

#include "akira_api.h"
#include "akira_sensor_api.h"
#include <runtime/security.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(akira_sensor_api, LOG_LEVEL_INF);

int akira_sensor_read(akira_sensor_type_t type, float *value)
{
    if (!value)
        return -EINVAL;

#ifdef CONFIG_AKIRA_BME280
    switch (type)
    {
    case SENSOR_TYPE_TEMP:
        // TODO: call bme280
        *value = 25.0f;
        break;
    case SENSOR_TYPE_HUMIDITY:
        *value = 50.0f;
        break;
    case SENSOR_TYPE_PRESSURE:
        *value = 1013.25f;
        break;
    default:
        return -ENOTSUP;
    }
    return 0;
#else
    (void)type; (void)value;
    return -ENOSYS;
#endif
}

int akira_sensor_read_imu(akira_imu_data_t *data)
{
    if (!data)
        return -EINVAL;

    data->accel_x = 0.0f;
    data->accel_y = 0.0f;
    data->accel_z = 9.81f;
    data->gyro_x = 0.0f;
    data->gyro_y = 0.0f;
    data->gyro_z = 0.0f;
    return 0;
}

int akira_sensor_read_env(akira_env_data_t *data)
{
    if (!data)
        return -EINVAL;

    data->temperature = 25.0f;
    data->humidity = 50.0f;
    data->pressure = 1013.25f;
    return 0;
}

int akira_sensor_read_power(akira_power_data_t *data)
{
    if (!data)
        return -EINVAL;

    data->voltage = 3.7f;
    data->current = 0.15f;
    data->power = 0.555f;
    return 0;
}

#ifdef CONFIG_AKIRA_WASM_RUNTIME
/* WASM Native export api */

int akira_native_sensor_read(wasm_exec_env_t exec_env, int32_t type)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_SENSOR_READ, -EPERM);
#ifdef CONFIG_AKIRA_BME280
    {
        float v = 0.0f;
        if (akira_sensor_read((akira_sensor_type_t)type, &v) == 0)
        {
            return (int)(v * 1000.0f);
        }
    }
#endif
    (void)type;
    return -ENOSYS;
}

#endif /* CONFIG_AKIRA_WASM_RUNTIME */