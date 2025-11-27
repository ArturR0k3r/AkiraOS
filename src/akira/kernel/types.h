/**
 * @file types.h
 * @brief AkiraOS Core Types
 * 
 * Common type definitions used throughout AkiraOS.
 */

#ifndef AKIRA_KERNEL_TYPES_H
#define AKIRA_KERNEL_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/* Result Types                                                              */
/*===========================================================================*/

/** Result/error codes */
typedef enum {
    AKIRA_RESULT_OK = 0,
    AKIRA_RESULT_ERROR = -1,
    AKIRA_RESULT_NOMEM = -2,
    AKIRA_RESULT_BUSY = -3,
    AKIRA_RESULT_TIMEOUT = -4,
    AKIRA_RESULT_INVALID = -5,
    AKIRA_RESULT_NOT_FOUND = -6,
    AKIRA_RESULT_EXISTS = -7,
    AKIRA_RESULT_PERMISSION = -8,
    AKIRA_RESULT_NOT_READY = -9,
    AKIRA_RESULT_FULL = -10,
    AKIRA_RESULT_EMPTY = -11
} akira_result_t;

/*===========================================================================*/
/* Version Types                                                             */
/*===========================================================================*/

/** Version structure */
typedef struct {
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
} akira_version_t;

/*===========================================================================*/
/* Priority Types                                                            */
/*===========================================================================*/

/** Priority level */
typedef int8_t akira_priority_t;

/** Flags type */
typedef uint32_t akira_flags_t;

/*===========================================================================*/
/* Handle Types                                                              */
/*===========================================================================*/

/** Generic handle type */
typedef int32_t akira_handle_t;

/** Invalid handle value */
#define AKIRA_INVALID_HANDLE    (-1)

/** Service handle */
typedef akira_handle_t akira_service_handle_t;

/** Process handle */
typedef akira_handle_t akira_process_handle_t;

/** App handle */
typedef akira_handle_t akira_app_handle_t;

/** Timer handle */
typedef akira_handle_t akira_timer_handle_t;

/** Event subscription handle */
typedef akira_handle_t akira_subscription_t;

/*===========================================================================*/
/* ID Types                                                                  */
/*===========================================================================*/

/** Process ID */
typedef uint32_t akira_pid_t;

/** Thread ID */
typedef uint32_t akira_tid_t;

/** User ID (for permissions) */
typedef uint16_t akira_uid_t;

/** Group ID */
typedef uint16_t akira_gid_t;

/*===========================================================================*/
/* Time Types                                                                */
/*===========================================================================*/

/** Timestamp in milliseconds */
typedef uint64_t akira_time_t;

/** Duration in milliseconds */
typedef uint32_t akira_duration_t;

/** Special timeout values */
#define AKIRA_NO_WAIT           0
#define AKIRA_WAIT_FOREVER      UINT32_MAX

/*===========================================================================*/
/* Size Types                                                                */
/*===========================================================================*/

/** Memory offset */
typedef uint32_t akira_offset_t;

/*===========================================================================*/
/* Callback Types                                                            */
/*===========================================================================*/

/** Generic callback with user data */
typedef void (*akira_callback_t)(void *user_data);

/** Callback with result */
typedef void (*akira_result_callback_t)(int result, void *user_data);

/*===========================================================================*/
/* Utility Macros                                                            */
/*===========================================================================*/

/** Check if handle is valid */
#define AKIRA_HANDLE_VALID(h)   ((h) >= 0)

/** Get minimum of two values */
#ifndef MIN
#define MIN(a, b)               (((a) < (b)) ? (a) : (b))
#endif

/** Get maximum of two values */
#ifndef MAX
#define MAX(a, b)               (((a) > (b)) ? (a) : (b))
#endif

/** Get array element count */
#define ARRAY_SIZE(arr)         (sizeof(arr) / sizeof((arr)[0]))

/** Align value up to alignment */
#define ALIGN_UP(val, align)    (((val) + (align) - 1) & ~((align) - 1))

/** Align value down to alignment */
#define ALIGN_DOWN(val, align)  ((val) & ~((align) - 1))

/** Check if value is power of 2 */
#define IS_POWER_OF_2(x)        (((x) != 0) && (((x) & ((x) - 1)) == 0))

/** Mark parameter as unused */
#define UNUSED(x)               ((void)(x))

/** Stringify macro */
#define STRINGIFY(x)            #x
#define STRINGIFY_EXPAND(x)     STRINGIFY(x)

/** Concatenate macros */
#define CONCAT(a, b)            a ## b
#define CONCAT_EXPAND(a, b)     CONCAT(a, b)

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_KERNEL_TYPES_H */
