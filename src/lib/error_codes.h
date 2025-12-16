/**
 * @file error_codes.h
 * @brief AkiraOS Error Code Standardization
 *
 * Provides consistent error handling across the codebase.
 *
 * Convention:
 * - System errors: Use standard negative errno values (-EINVAL, -ENOMEM, etc.)
 * - Domain-specific errors: Use positive AKIRA_ERR_* codes
 * - Success: Always 0
 * - NEVER use -1 (ambiguous)
 *
 * Usage Example:
 * ```c
 * int ret = app_manager_install(...);
 * if (ret < 0) {
 *     // System error (errno)
 *     LOG_ERR("System error: %d (%s)", ret, strerror(-ret));
 * } else if (ret > 0) {
 *     // Domain error
 *     LOG_ERR("App error: %s", akira_strerror(ret));
 * } else {
 *     // Success
 * }
 * ```
 */

#ifndef AKIRA_ERROR_CODES_H
#define AKIRA_ERROR_CODES_H

#include <errno.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== Domain Error Base ===== */
#define AKIRA_ERR_BASE 1000

/* ===== App Manager Errors (1000-1099) ===== */
#define AKIRA_ERR_APP_NOT_FOUND       (AKIRA_ERR_BASE + 1)   /* 1001 */
#define AKIRA_ERR_APP_ALREADY_EXISTS  (AKIRA_ERR_BASE + 2)   /* 1002 */
#define AKIRA_ERR_APP_RUNNING         (AKIRA_ERR_BASE + 3)   /* 1003 */
#define AKIRA_ERR_APP_NOT_RUNNING     (AKIRA_ERR_BASE + 4)   /* 1004 */
#define AKIRA_ERR_APP_FAILED          (AKIRA_ERR_BASE + 5)   /* 1005 */
#define AKIRA_ERR_APP_MAX_REACHED     (AKIRA_ERR_BASE + 6)   /* 1006 */
#define AKIRA_ERR_APP_MAX_RUNNING     (AKIRA_ERR_BASE + 7)   /* 1007 */

/* ===== WASM Errors (1100-1199) ===== */
#define AKIRA_ERR_WASM_INVALID        (AKIRA_ERR_BASE + 100) /* 1100 */
#define AKIRA_ERR_WASM_TOO_LARGE      (AKIRA_ERR_BASE + 101) /* 1101 */
#define AKIRA_ERR_WASM_LOAD_FAILED    (AKIRA_ERR_BASE + 102) /* 1102 */
#define AKIRA_ERR_WASM_EXEC_FAILED    (AKIRA_ERR_BASE + 103) /* 1103 */
#define AKIRA_ERR_WASM_OUT_OF_MEMORY  (AKIRA_ERR_BASE + 104) /* 1104 */
#define AKIRA_ERR_WASM_INSTANTIATE    (AKIRA_ERR_BASE + 105) /* 1105 */

/* ===== Storage Errors (1200-1299) ===== */
#define AKIRA_ERR_STORAGE_FULL        (AKIRA_ERR_BASE + 200) /* 1200 */
#define AKIRA_ERR_STORAGE_QUOTA       (AKIRA_ERR_BASE + 201) /* 1201 */
#define AKIRA_ERR_STORAGE_CORRUPTED   (AKIRA_ERR_BASE + 202) /* 1202 */
#define AKIRA_ERR_PATH_INVALID        (AKIRA_ERR_BASE + 203) /* 1203 */
#define AKIRA_ERR_PATH_TRAVERSAL      (AKIRA_ERR_BASE + 204) /* 1204 */

/* ===== Network Errors (1300-1399) ===== */
#define AKIRA_ERR_NET_NOT_CONNECTED   (AKIRA_ERR_BASE + 300) /* 1300 */
#define AKIRA_ERR_NET_TIMEOUT         (AKIRA_ERR_BASE + 301) /* 1301 */
#define AKIRA_ERR_NET_DNS_FAILED      (AKIRA_ERR_BASE + 302) /* 1302 */
#define AKIRA_ERR_NET_TLS_FAILED      (AKIRA_ERR_BASE + 303) /* 1303 */
#define AKIRA_ERR_HTTP_BAD_REQUEST    (AKIRA_ERR_BASE + 310) /* 1310 */
#define AKIRA_ERR_HTTP_UNAUTHORIZED   (AKIRA_ERR_BASE + 311) /* 1311 */
#define AKIRA_ERR_HTTP_NOT_FOUND      (AKIRA_ERR_BASE + 312) /* 1312 */
#define AKIRA_ERR_HTTP_SERVER_ERROR   (AKIRA_ERR_BASE + 313) /* 1313 */

/* ===== Cloud Errors (1400-1499) ===== */
#define AKIRA_ERR_CLOUD_NOT_CONNECTED (AKIRA_ERR_BASE + 400) /* 1400 */
#define AKIRA_ERR_CLOUD_AUTH_FAILED   (AKIRA_ERR_BASE + 401) /* 1401 */
#define AKIRA_ERR_CLOUD_PROTOCOL      (AKIRA_ERR_BASE + 402) /* 1402 */
#define AKIRA_ERR_CLOUD_RATE_LIMIT    (AKIRA_ERR_BASE + 403) /* 1403 */

/* ===== OTA Errors (1500-1599) ===== */
#define AKIRA_ERR_OTA_IN_PROGRESS     (AKIRA_ERR_BASE + 500) /* 1500 */
#define AKIRA_ERR_OTA_INVALID_IMAGE   (AKIRA_ERR_BASE + 501) /* 1501 */
#define AKIRA_ERR_OTA_VERIFY_FAILED   (AKIRA_ERR_BASE + 502) /* 1502 */
#define AKIRA_ERR_OTA_NO_SPACE        (AKIRA_ERR_BASE + 503) /* 1503 */

/* ===== Security Errors (1600-1699) ===== */
#define AKIRA_ERR_PERMISSION_DENIED   (AKIRA_ERR_BASE + 600) /* 1600 */
#define AKIRA_ERR_CAPABILITY_MISSING  (AKIRA_ERR_BASE + 601) /* 1601 */
#define AKIRA_ERR_SIGNATURE_INVALID   (AKIRA_ERR_BASE + 602) /* 1602 */
#define AKIRA_ERR_CERTIFICATE_INVALID (AKIRA_ERR_BASE + 603) /* 1603 */

/* ===== Bluetooth Errors (1700-1799) ===== */
#define AKIRA_ERR_BLE_NOT_ENABLED     (AKIRA_ERR_BASE + 700) /* 1700 */
#define AKIRA_ERR_BLE_SCAN_FAILED     (AKIRA_ERR_BASE + 701) /* 1701 */
#define AKIRA_ERR_BLE_CONNECT_FAILED  (AKIRA_ERR_BASE + 702) /* 1702 */
#define AKIRA_ERR_BLE_DISCONNECTED    (AKIRA_ERR_BASE + 703) /* 1703 */
#define AKIRA_ERR_BLE_GATT_FAILED     (AKIRA_ERR_BASE + 704) /* 1704 */

/* ===== Sensor Errors (1800-1899) ===== */
#define AKIRA_ERR_SENSOR_NOT_FOUND    (AKIRA_ERR_BASE + 800) /* 1800 */
#define AKIRA_ERR_SENSOR_NOT_READY    (AKIRA_ERR_BASE + 801) /* 1801 */
#define AKIRA_ERR_SENSOR_READ_FAILED  (AKIRA_ERR_BASE + 802) /* 1802 */
#define AKIRA_ERR_SENSOR_CALIBRATION  (AKIRA_ERR_BASE + 803) /* 1803 */

/* ===== Display Errors (1900-1999) ===== */
#define AKIRA_ERR_DISPLAY_NOT_READY   (AKIRA_ERR_BASE + 900) /* 1900 */
#define AKIRA_ERR_DISPLAY_BUSY        (AKIRA_ERR_BASE + 901) /* 1901 */
#define AKIRA_ERR_DISPLAY_BAD_PARAMS  (AKIRA_ERR_BASE + 902) /* 1902 */

/* ===== Generic Errors (2000+) ===== */
#define AKIRA_ERR_NOT_INITIALIZED     (AKIRA_ERR_BASE + 1000) /* 2000 */
#define AKIRA_ERR_ALREADY_INITIALIZED (AKIRA_ERR_BASE + 1001) /* 2001 */
#define AKIRA_ERR_NOT_SUPPORTED       (AKIRA_ERR_BASE + 1002) /* 2002 */
#define AKIRA_ERR_INTERNAL            (AKIRA_ERR_BASE + 1003) /* 2003 */
#define AKIRA_ERR_TIMEOUT             (AKIRA_ERR_BASE + 1004) /* 2004 */
#define AKIRA_ERR_WOULD_BLOCK         (AKIRA_ERR_BASE + 1005) /* 2005 */

/* ===== Error to String Conversion ===== */

/**
 * @brief Convert AkiraOS error code to string
 *
 * @param error Positive AKIRA_ERR_* code
 * @return Human-readable error string
 */
const char *akira_strerror(int error);

/**
 * @brief Check if error code is a system error
 *
 * @param error Error code
 * @return true if system error (negative), false if domain error (positive) or success (0)
 */
static inline bool akira_is_system_error(int error)
{
    return error < 0;
}

/**
 * @brief Check if error code is a domain error
 *
 * @param error Error code
 * @return true if domain error (positive), false otherwise
 */
static inline bool akira_is_domain_error(int error)
{
    return error >= AKIRA_ERR_BASE;
}

/**
 * @brief Check if operation succeeded
 *
 * @param error Error code
 * @return true if success (0), false otherwise
 */
static inline bool akira_is_success(int error)
{
    return error == 0;
}

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_ERROR_CODES_H */
