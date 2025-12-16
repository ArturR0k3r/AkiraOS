/**
 * @file error_codes.c
 * @brief AkiraOS Error Code Implementation
 */

#include "error_codes.h"
#include <stddef.h>

const char *akira_strerror(int error)
{
    /* Check range */
    if (error < AKIRA_ERR_BASE) {
        return "Not an AkiraOS error";
    }

    /* App Manager Errors */
    if (error == AKIRA_ERR_APP_NOT_FOUND) return "App not found";
    if (error == AKIRA_ERR_APP_ALREADY_EXISTS) return "App already exists";
    if (error == AKIRA_ERR_APP_RUNNING) return "App is running";
    if (error == AKIRA_ERR_APP_NOT_RUNNING) return "App is not running";
    if (error == AKIRA_ERR_APP_FAILED) return "App failed";
    if (error == AKIRA_ERR_APP_MAX_REACHED) return "Maximum apps installed";
    if (error == AKIRA_ERR_APP_MAX_RUNNING) return "Maximum running apps reached";

    /* WASM Errors */
    if (error == AKIRA_ERR_WASM_INVALID) return "Invalid WASM binary";
    if (error == AKIRA_ERR_WASM_TOO_LARGE) return "WASM binary too large";
    if (error == AKIRA_ERR_WASM_LOAD_FAILED) return "WASM load failed";
    if (error == AKIRA_ERR_WASM_EXEC_FAILED) return "WASM execution failed";
    if (error == AKIRA_ERR_WASM_OUT_OF_MEMORY) return "WASM out of memory";
    if (error == AKIRA_ERR_WASM_INSTANTIATE) return "WASM instantiation failed";

    /* Storage Errors */
    if (error == AKIRA_ERR_STORAGE_FULL) return "Storage full";
    if (error == AKIRA_ERR_STORAGE_QUOTA) return "Storage quota exceeded";
    if (error == AKIRA_ERR_STORAGE_CORRUPTED) return "Storage corrupted";
    if (error == AKIRA_ERR_PATH_INVALID) return "Invalid path";
    if (error == AKIRA_ERR_PATH_TRAVERSAL) return "Path traversal attempt";

    /* Network Errors */
    if (error == AKIRA_ERR_NET_NOT_CONNECTED) return "Network not connected";
    if (error == AKIRA_ERR_NET_TIMEOUT) return "Network timeout";
    if (error == AKIRA_ERR_NET_DNS_FAILED) return "DNS resolution failed";
    if (error == AKIRA_ERR_NET_TLS_FAILED) return "TLS handshake failed";
    if (error == AKIRA_ERR_HTTP_BAD_REQUEST) return "HTTP 400 Bad Request";
    if (error == AKIRA_ERR_HTTP_UNAUTHORIZED) return "HTTP 401 Unauthorized";
    if (error == AKIRA_ERR_HTTP_NOT_FOUND) return "HTTP 404 Not Found";
    if (error == AKIRA_ERR_HTTP_SERVER_ERROR) return "HTTP 500 Server Error";

    /* Cloud Errors */
    if (error == AKIRA_ERR_CLOUD_NOT_CONNECTED) return "Cloud not connected";
    if (error == AKIRA_ERR_CLOUD_AUTH_FAILED) return "Cloud authentication failed";
    if (error == AKIRA_ERR_CLOUD_PROTOCOL) return "Cloud protocol error";
    if (error == AKIRA_ERR_CLOUD_RATE_LIMIT) return "Cloud rate limit exceeded";

    /* OTA Errors */
    if (error == AKIRA_ERR_OTA_IN_PROGRESS) return "OTA update in progress";
    if (error == AKIRA_ERR_OTA_INVALID_IMAGE) return "Invalid OTA image";
    if (error == AKIRA_ERR_OTA_VERIFY_FAILED) return "OTA verification failed";
    if (error == AKIRA_ERR_OTA_NO_SPACE) return "Not enough space for OTA";

    /* Security Errors */
    if (error == AKIRA_ERR_PERMISSION_DENIED) return "Permission denied";
    if (error == AKIRA_ERR_CAPABILITY_MISSING) return "Required capability missing";
    if (error == AKIRA_ERR_SIGNATURE_INVALID) return "Invalid signature";
    if (error == AKIRA_ERR_CERTIFICATE_INVALID) return "Invalid certificate";

    /* Bluetooth Errors */
    if (error == AKIRA_ERR_BLE_NOT_ENABLED) return "Bluetooth not enabled";
    if (error == AKIRA_ERR_BLE_SCAN_FAILED) return "BLE scan failed";
    if (error == AKIRA_ERR_BLE_CONNECT_FAILED) return "BLE connection failed";
    if (error == AKIRA_ERR_BLE_DISCONNECTED) return "BLE disconnected";
    if (error == AKIRA_ERR_BLE_GATT_FAILED) return "BLE GATT operation failed";

    /* Sensor Errors */
    if (error == AKIRA_ERR_SENSOR_NOT_FOUND) return "Sensor not found";
    if (error == AKIRA_ERR_SENSOR_NOT_READY) return "Sensor not ready";
    if (error == AKIRA_ERR_SENSOR_READ_FAILED) return "Sensor read failed";
    if (error == AKIRA_ERR_SENSOR_CALIBRATION) return "Sensor calibration error";

    /* Display Errors */
    if (error == AKIRA_ERR_DISPLAY_NOT_READY) return "Display not ready";
    if (error == AKIRA_ERR_DISPLAY_BUSY) return "Display busy";
    if (error == AKIRA_ERR_DISPLAY_BAD_PARAMS) return "Display bad parameters";

    /* Generic Errors */
    if (error == AKIRA_ERR_NOT_INITIALIZED) return "Not initialized";
    if (error == AKIRA_ERR_ALREADY_INITIALIZED) return "Already initialized";
    if (error == AKIRA_ERR_NOT_SUPPORTED) return "Not supported";
    if (error == AKIRA_ERR_INTERNAL) return "Internal error";
    if (error == AKIRA_ERR_TIMEOUT) return "Operation timed out";
    if (error == AKIRA_ERR_WOULD_BLOCK) return "Operation would block";

    return "Unknown AkiraOS error";
}
