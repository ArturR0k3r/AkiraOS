/**
 * @file main.c
 * @brief Sensor Demo - AkiraOS Sensor Reading Example
 * 
 * This example demonstrates how to:
 * - Initialize the sensor subsystem
 * - Discover available sensors
 * - Read sensor values periodically
 * - Use events for sensor updates
 * 
 * Build with:
 *   make
 */

#include "akira_api.h"

/* Maximum sensors we'll handle */
#define MAX_SENSORS 8

/* Sensor handles */
static int sensor_handles[MAX_SENSORS];
static int sensor_count = 0;

/**
 * @brief Initialize sensors
 * @return Number of sensors found, or negative on error
 */
static int init_sensors(void)
{
    int ret;
    
    /* Initialize sensor subsystem */
    ret = ocre_sensors_init();
    if (ret < 0) {
        return ret;
    }
    
    /* Discover available sensors */
    int discovered = ocre_sensors_discover();
    if (discovered <= 0) {
        return discovered;
    }
    
    /* Open each sensor and store handle */
    sensor_count = 0;
    for (int i = 0; i < discovered && sensor_count < MAX_SENSORS; i++) {
        ret = ocre_sensors_open(i);
        if (ret == 0) {
            sensor_handles[sensor_count] = ocre_sensors_get_handle(i);
            if (sensor_handles[sensor_count] >= 0) {
                sensor_count++;
            }
        }
    }
    
    return sensor_count;
}

/**
 * @brief Read all sensors once
 */
static void read_all_sensors(void)
{
    for (int i = 0; i < sensor_count; i++) {
        int handle = sensor_handles[i];
        int channels = ocre_sensors_get_channel_count(handle);
        
        for (int ch = 0; ch < channels; ch++) {
            /* Read sensor value */
            float value = ocre_sensors_read(handle, ch);
            
            /* In a real app, you would:
             * - Store the value
             * - Update display
             * - Send to cloud
             * - Trigger alerts based on thresholds
             */
            (void)value;  /* Suppress unused warning */
        }
    }
}

/**
 * @brief Application entry point
 */
AKIRA_APP_MAIN()
{
    /* Initialize sensors */
    int count = init_sensors();
    if (count <= 0) {
        /* No sensors found - just sleep forever */
        while (1) {
            ocre_sleep(10000);
        }
    }
    
    /* Main sensor reading loop */
    while (1) {
        /* Read all sensors */
        read_all_sensors();
        
        /* Wait 1 second before next reading */
        ocre_sleep(1000);
    }
}
