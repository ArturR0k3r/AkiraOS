/**
 * @file akira_api.h
 * @brief AkiraOS WASM API Exports
 *
 * All functions exported to WASM applications.
 * Each API requires specific capabilities granted in app manifest.
 */

#ifndef AKIRA_API_H
#define AKIRA_API_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/* Display API - Requires: display.write                                     */
/*===========================================================================*/

/**
 * @brief Clear display with solid color
 * @param color RGB565 color value
 */
void akira_display_clear(uint16_t color);

/**
 * @brief Draw single pixel
 * @param x X coordinate
 * @param y Y coordinate
 * @param color RGB565 color value
 */
void akira_display_pixel(int x, int y, uint16_t color);

/**
 * @brief Draw filled rectangle
 * @param x X coordinate
 * @param y Y coordinate
 * @param w Width
 * @param h Height
 * @param color RGB565 color value
 */
void akira_display_rect(int x, int y, int w, int h, uint16_t color);

/**
 * @brief Draw text string
 * @param x X coordinate
 * @param y Y coordinate
 * @param text Null-terminated string
 * @param color RGB565 color value
 */
void akira_display_text(int x, int y, const char *text, uint16_t color);

/**
 * @brief Flush framebuffer to display
 */
void akira_display_flush(void);

/**
 * @brief Get display dimensions
 * @param width Output for display width
 * @param height Output for display height
 */
void akira_display_get_size(int *width, int *height);

/*===========================================================================*/
/* Input API - Requires: input.read                                          */
/*===========================================================================*/

/**
 * @brief Button bit masks
 */
#define AKIRA_BTN_POWER    (1 << 0)
#define AKIRA_BTN_SETTINGS (1 << 1)
#define AKIRA_BTN_UP       (1 << 2)
#define AKIRA_BTN_DOWN     (1 << 3)
#define AKIRA_BTN_LEFT     (1 << 4)
#define AKIRA_BTN_RIGHT    (1 << 5)
#define AKIRA_BTN_A        (1 << 6)
#define AKIRA_BTN_B        (1 << 7)
#define AKIRA_BTN_X        (1 << 8)
#define AKIRA_BTN_Y        (1 << 9)

/**
 * @brief Read current button state
 * @return Bitmask of pressed buttons
 */
uint32_t akira_input_read_buttons(void);

/**
 * @brief Check if specific button is pressed
 * @param button Button mask (AKIRA_BTN_*)
 * @return true if pressed
 */
bool akira_input_button_pressed(uint32_t button);

/**
 * @brief Input callback type
 */
typedef void (*akira_input_callback_t)(uint32_t buttons);

/**
 * @brief Set input callback for button events
 * @param callback Function to call on button change
 */
void akira_input_set_callback(akira_input_callback_t callback);

/*===========================================================================*/
/* RF API - Requires: rf.transceive                                          */
/*===========================================================================*/

/* Use types from rf_framework.h */
typedef int akira_rf_chip_t;  /* Maps to rf_chip_t values */
typedef int akira_rf_mode_t;  /* Maps to rf_mode_t values */

/* RF chip values - same as rf_framework.h */
#define AKIRA_RF_CHIP_NONE     0
#define AKIRA_RF_CHIP_NRF24L01 1
#define AKIRA_RF_CHIP_LR1121   2
#define AKIRA_RF_CHIP_CC1101   3
#define AKIRA_RF_CHIP_SX1276   4
#define AKIRA_RF_CHIP_RFM69    5

/* RF mode values */
#define AKIRA_RF_MODE_IDLE     0
#define AKIRA_RF_MODE_RX       1
#define AKIRA_RF_MODE_TX       2
#define AKIRA_RF_MODE_SLEEP    3

/**
 * @brief Initialize RF chip
 * @param chip RF chip type
 * @return 0 on success, negative on error
 */
int akira_rf_init(akira_rf_chip_t chip);

/**
 * @brief Deinitialize RF chip
 * @return 0 on success
 */
int akira_rf_deinit(void);

/**
 * @brief Send data packet
 * @param data Data buffer
 * @param len Data length
 * @return 0 on success, negative on error
 */
int akira_rf_send(const uint8_t *data, size_t len);

/**
 * @brief Receive data packet
 * @param buffer Receive buffer
 * @param max_len Maximum buffer length
 * @param timeout_ms Receive timeout in milliseconds
 * @return Number of bytes received, negative on error
 */
int akira_rf_receive(uint8_t *buffer, size_t max_len, uint32_t timeout_ms);

/**
 * @brief Set RF frequency
 * @param freq_hz Frequency in Hz
 * @return 0 on success
 */
int akira_rf_set_frequency(uint32_t freq_hz);

/**
 * @brief Set TX power
 * @param dbm Power in dBm
 * @return 0 on success
 */
int akira_rf_set_power(int8_t dbm);

/**
 * @brief Get RSSI of last received packet
 * @param rssi Output for RSSI value
 * @return 0 on success
 */
int akira_rf_get_rssi(int16_t *rssi);

/*===========================================================================*/
/* Sensor API - Requires: sensor.<type>.read                                 */
/*===========================================================================*/

/**
 * @brief Sensor types
 */
typedef enum {
	SENSOR_TYPE_NONE = 0,
	SENSOR_TYPE_ACCEL,
	SENSOR_TYPE_GYRO,
	SENSOR_TYPE_TEMP,
	SENSOR_TYPE_HUMIDITY,
	SENSOR_TYPE_PRESSURE,
	SENSOR_TYPE_LIGHT,
	SENSOR_TYPE_VOLTAGE,
	SENSOR_TYPE_CURRENT,
	SENSOR_TYPE_POWER
} akira_sensor_type_t;

/**
 * @brief IMU data structure
 */
typedef struct {
	float accel_x, accel_y, accel_z;
	float gyro_x, gyro_y, gyro_z;
} akira_imu_data_t;

/**
 * @brief Environmental data structure
 */
typedef struct {
	float temperature;
	float humidity;
	float pressure;
} akira_env_data_t;

/**
 * @brief Power data structure
 */
typedef struct {
	float voltage;
	float current;
	float power;
} akira_power_data_t;

/**
 * @brief Read single sensor value
 * @param type Sensor type
 * @param value Output for sensor value
 * @return 0 on success
 */
int akira_sensor_read(akira_sensor_type_t type, float *value);

/**
 * @brief Read IMU data (accelerometer + gyroscope)
 * @param data Output for IMU data
 * @return 0 on success
 */
int akira_sensor_read_imu(akira_imu_data_t *data);

/**
 * @brief Read environmental data (temp, humidity, pressure)
 * @param data Output for environmental data
 * @return 0 on success
 */
int akira_sensor_read_env(akira_env_data_t *data);

/**
 * @brief Read power data (voltage, current, power)
 * @param data Output for power data
 * @return 0 on success
 */
int akira_sensor_read_power(akira_power_data_t *data);

/*===========================================================================*/
/* Storage API - Requires: storage.read / storage.write                      */
/*===========================================================================*/

/**
 * @brief Read file from app storage
 * @param path File path (relative to app storage)
 * @param buffer Output buffer
 * @param len Maximum bytes to read
 * @return Bytes read, negative on error
 */
int akira_storage_read(const char *path, void *buffer, size_t len);

/**
 * @brief Write file to app storage
 * @param path File path (relative to app storage)
 * @param data Data to write
 * @param len Data length
 * @return Bytes written, negative on error
 */
int akira_storage_write(const char *path, const void *data, size_t len);

/**
 * @brief Delete file from app storage
 * @param path File path
 * @return 0 on success
 */
int akira_storage_delete(const char *path);

/**
 * @brief List files in directory
 * @param path Directory path
 * @param files Output array of file names
 * @param max_count Maximum number of files
 * @return Number of files, negative on error
 */
int akira_storage_list(const char *path, char **files, int max_count);

/**
 * @brief Get file size
 * @param path File path
 * @return File size in bytes, negative on error
 */
int akira_storage_size(const char *path);

/*===========================================================================*/
/* Network API - Requires: network.http / network.mqtt                       */
/*===========================================================================*/

/**
 * @brief HTTP GET request
 * @param url URL to fetch
 * @param buffer Response buffer
 * @param max_len Maximum response length
 * @return Response length, negative on error
 */
int akira_http_get(const char *url, uint8_t *buffer, size_t max_len);

/**
 * @brief HTTP POST request
 * @param url URL to post to
 * @param data Request body
 * @param len Request body length
 * @return Response code, negative on error
 */
int akira_http_post(const char *url, const uint8_t *data, size_t len);

/**
 * @brief MQTT message callback
 */
typedef void (*akira_mqtt_callback_t)(const char *topic, const void *data, size_t len);

/**
 * @brief Publish MQTT message
 * @param topic Topic name
 * @param data Message data
 * @param len Data length
 * @return 0 on success
 */
int akira_mqtt_publish(const char *topic, const void *data, size_t len);

/**
 * @brief Subscribe to MQTT topic
 * @param topic Topic pattern (supports wildcards)
 * @param callback Function to call on message
 * @return 0 on success
 */
int akira_mqtt_subscribe(const char *topic, akira_mqtt_callback_t callback);

/*===========================================================================*/
/* System API - Requires: system.info                                        */
/*===========================================================================*/

/**
 * @brief Get system uptime
 * @return Uptime in milliseconds
 */
uint64_t akira_system_uptime_ms(void);

/**
 * @brief Get free heap memory
 * @return Free bytes
 */
size_t akira_system_free_memory(void);

/**
 * @brief Get platform name
 * @return Platform string (e.g., "ESP32-S3")
 */
const char *akira_system_platform(void);

/**
 * @brief Sleep for specified time
 * @param ms Milliseconds to sleep
 */
void akira_system_sleep(uint32_t ms);

/**
 * @brief Log message (for debugging)
 * @param level Log level (0=error, 1=warn, 2=info, 3=debug)
 * @param message Log message
 */
void akira_log(int level, const char *message);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_API_H */
