/**
 * @file akira_api.h
 * @brief AkiraOS WASM API Exports (Minimal)
 *
 * Only actively used APIs. Use OCRE's direct APIs for GPIO, ADC, etc.
 */

#ifndef AKIRA_API_H
#define AKIRA_API_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <wasm_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/* Display API - Low-level framebuffer access                               */
/*===========================================================================*/

void akira_display_clear(uint16_t color);
void akira_display_pixel(int x, int y, uint16_t color);
void akira_display_rect(int x, int y, int w, int h, uint16_t color);
void akira_display_get_size(int *width, int *height);

/* WASM wrappers (exported to OCRE) */
int akira_display_clear_wasm(wasm_exec_env_t exec_env, int color);
int akira_display_pixel_wasm(wasm_exec_env_t exec_env, int x, int y, int color);
int akira_display_rect_wasm(wasm_exec_env_t exec_env, int x, int y, int w, int h, int color);
int akira_display_get_size_wasm(wasm_exec_env_t exec_env, uint32_t width_ptr, uint32_t height_ptr);

/*===========================================================================*/
/* HID API - Keyboard/Mouse emulation                                        */
/*===========================================================================*/

#define AKIRA_HID_TRANSPORT_BLE 0
#define AKIRA_HID_TRANSPORT_USB 1

int akira_hid_set_transport(int transport);
int akira_hid_enable(void);
int akira_hid_disable(void);
int akira_hid_keyboard_type(const char *str);
int akira_hid_keyboard_press(int key);
int akira_hid_keyboard_release(int key);

/* WASM wrappers (exported to OCRE) */
int akira_hid_set_transport_wasm(wasm_exec_env_t exec_env, int transport);
int akira_hid_enable_wasm(wasm_exec_env_t exec_env);
int akira_hid_disable_wasm(wasm_exec_env_t exec_env);
int akira_hid_keyboard_type_wasm(wasm_exec_env_t exec_env, uint32_t str_ptr);
int akira_hid_keyboard_press_wasm(wasm_exec_env_t exec_env, int key);
int akira_hid_keyboard_release_wasm(wasm_exec_env_t exec_env, int key);

/*===========================================================================*/
/* Storage API - File I/O                                                    */
/*===========================================================================*/

int akira_storage_read(const char *path, void *buffer, size_t len);
int akira_storage_write(const char *path, const void *data, size_t len);
int akira_storage_delete(const char *path);
int akira_storage_size(const char *path);

/* WASM wrappers (exported to OCRE) */
int akira_storage_read_wasm(wasm_exec_env_t exec_env, uint32_t path_ptr, uint32_t buf_ptr, int len);
int akira_storage_write_wasm(wasm_exec_env_t exec_env, uint32_t path_ptr, uint32_t data_ptr, int len);
int akira_storage_delete_wasm(wasm_exec_env_t exec_env, uint32_t path_ptr);
int akira_storage_size_wasm(wasm_exec_env_t exec_env, uint32_t path_ptr);

/*===========================================================================*/
/* Network API - HTTP/MQTT                                                   */
/*===========================================================================*/

int akira_http_get(const char *url, uint8_t *buffer, size_t max_len);
int akira_http_post(const char *url, const uint8_t *data, size_t len);

typedef void (*akira_mqtt_callback_t)(const char *topic, const void *data, size_t len);

int akira_mqtt_publish(const char *topic, const void *data, size_t len);
int akira_mqtt_subscribe(const char *topic, akira_mqtt_callback_t callback);

/* WASM wrappers (exported to OCRE) */
int akira_http_get_wasm(wasm_exec_env_t exec_env, uint32_t url_ptr, uint32_t buf_ptr, int max_len);
int akira_http_post_wasm(wasm_exec_env_t exec_env, uint32_t url_ptr, uint32_t data_ptr, int len);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_API_H */
