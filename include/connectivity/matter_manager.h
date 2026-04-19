/**
 * @file matter_manager.h
 * @brief Matter Protocol Stack Management for AkiraOS
 *
 * Provides Matter (Connected Home over IP) protocol support with hardware-agnostic
 * transport binding through the Radio Abstraction Layer.
 *
 * @copyright Copyright (c) 2026 PenEngineering S.R.L
*/

#ifndef AKIRA_MATTER_MANAGER_H
#define AKIRA_MATTER_MANAGER_H

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Matter device types (subset of common types) */
typedef enum {
    MATTER_DEVICE_TYPE_LIGHT = 0x0100,              /* On/Off Light */
    MATTER_DEVICE_TYPE_LIGHT_DIMMABLE = 0x0101,     /* Dimmable Light */
    MATTER_DEVICE_TYPE_LIGHT_COLOR = 0x0102,        /* Color Light */
    MATTER_DEVICE_TYPE_SWITCH = 0x0103,             /* On/Off Switch */
    MATTER_DEVICE_TYPE_PLUG = 0x010A,               /* Smart Plug */
    MATTER_DEVICE_TYPE_SENSOR_TEMP = 0x0302,        /* Temperature Sensor */
    MATTER_DEVICE_TYPE_SENSOR_OCCUPANCY = 0x0107,   /* Occupancy Sensor */
    MATTER_DEVICE_TYPE_DOOR_LOCK = 0x000A,          /* Door Lock */
    MATTER_DEVICE_TYPE_THERMOSTAT = 0x0301,         /* Thermostat */
    MATTER_DEVICE_TYPE_BRIDGE = 0x000E,             /* Bridge (aggregator) */
} matter_device_type_t;

/* Matter transport types */
typedef enum {
    MATTER_TRANSPORT_WIFI,      /* Matter over WiFi (IPv6) */
    MATTER_TRANSPORT_THREAD,    /* Matter over Thread (IPv6) */
    MATTER_TRANSPORT_BLE,       /* Matter over BLE (commissioning only) */
} matter_transport_t;

/* Matter commissioning state */
typedef enum {
    MATTER_COMM_STATE_NONE = 0,        /* Not commissioned */
    MATTER_COMM_STATE_BLE_ADVERTISING, /* BLE commissioning active */
    MATTER_COMM_STATE_PASE,            /* PASE (Password Authenticated Session) */
    MATTER_COMM_STATE_CASE,            /* CASE (Certificate Authenticated Session) */
    MATTER_COMM_STATE_COMMISSIONED,    /* Fully commissioned */
    MATTER_COMM_STATE_ERROR,           /* Commissioning error */
} matter_comm_state_t;

/* Matter configuration */
typedef struct {
    uint16_t vendor_id;                /* Vendor ID (assigned by CSA) */
    uint16_t product_id;               /* Product ID */
    uint16_t discriminator;            /* 12-bit discriminator for commissioning */
    uint32_t setup_pin_code;           /* 8-digit setup PIN */
    char device_name[33];              /* Device name */
    char serial_number[33];            /* Serial number */
    matter_device_type_t device_type;  /* Primary device type */
    matter_transport_t transport;      /* Preferred transport */
    bool enable_ota;                   /* Enable Matter OTA */
} matter_config_t;

/* Matter statistics */
typedef struct {
    uint32_t messages_sent;
    uint32_t messages_received;
    uint32_t commissioning_attempts;
    uint32_t commissioning_success;
    uint32_t binding_updates;
    matter_comm_state_t state;
    uint64_t uptime_sec;
} matter_stats_t;

/* Matter event types */
typedef enum {
    MATTER_EVENT_COMMISSIONED,         /* Device successfully commissioned */
    MATTER_EVENT_DECOMMISSIONED,       /* Factory reset / decommission */
    MATTER_EVENT_BINDING_UPDATED,      /* Binding table updated */
    MATTER_EVENT_OTA_AVAILABLE,        /* OTA update available */
    MATTER_EVENT_ATTRIBUTE_CHANGED,    /* Cluster attribute changed */
} matter_event_type_t;

/* Matter event structure */
typedef struct {
    matter_event_type_t type;
    void *data;
    size_t data_len;
} matter_event_t;

/* Matter event callback */
typedef void (*matter_event_cb_t)(const matter_event_t *event, void *user_data);

/**
 * @brief Initialize Matter protocol stack
 *
 * Must be called before any other Matter functions. Initializes the
 * Matter stack, registers device endpoints, and prepares for commissioning.
 *
 * @param config Matter configuration parameters
 * @return 0 on success, negative errno on failure
 */
int matter_manager_init(const matter_config_t *config);

/**
 * @brief Start Matter commissioning
 *
 * Opens BLE commissioning window or enables soft-AP for device onboarding.
 * Commissioning window remains open for specified timeout.
 *
 * @param timeout_sec Commissioning window timeout (0 = infinite)
 * @return 0 on success, negative errno on failure
 */
int matter_start_commissioning(uint32_t timeout_sec);

/**
 * @brief Stop Matter commissioning
 *
 * Closes commissioning window immediately.
 *
 * @return 0 on success, negative errno on failure
 */
int matter_stop_commissioning(void);

/**
 * @brief Perform Matter factory reset
 *
 * Erases all persistent Matter data including fabric information,
 * credentials, and configuration. Device will need to be re-commissioned.
 *
 * @return 0 on success, negative errno on failure
 */
int matter_factory_reset(void);

/**
 * @brief Get Matter commissioning state
 *
 * @return Current commissioning state
 */
matter_comm_state_t matter_get_commissioning_state(void);

/**
 * @brief Get Matter QR code for commissioning
 *
 * Generates QR code payload string that can be displayed or encoded
 * as QR code for easy commissioning with Matter controllers.
 *
 * @param buffer Buffer to store QR code string
 * @param buffer_len Buffer length
 * @return 0 on success, negative errno on failure
 */
int matter_get_qr_code(char *buffer, size_t buffer_len);

/**
 * @brief Get Matter manual pairing code
 *
 * Generates 11-digit manual pairing code for commissioning without QR code.
 *
 * @param buffer Buffer to store pairing code
 * @param buffer_len Buffer length (minimum 12 bytes for null terminator)
 * @return 0 on success, negative errno on failure
 */
int matter_get_manual_code(char *buffer, size_t buffer_len);

/**
 * @brief Get Matter statistics
 *
 * @param stats Pointer to statistics structure
 * @return 0 on success, negative errno on failure
 */
int matter_get_stats(matter_stats_t *stats);

/**
 * @brief Register Matter event callback
 *
 * @param callback Event callback function
 * @param user_data User data passed to callback
 * @return 0 on success, negative errno on failure
 */
int matter_register_event_callback(matter_event_cb_t callback, void *user_data);

/**
 * @brief Set Matter attribute value (simplified API)
 *
 * Helper function to update cluster attributes. Used by WASM apps
 * to update device state.
 *
 * @param endpoint Endpoint ID
 * @param cluster Cluster ID
 * @param attribute Attribute ID
 * @param value Pointer to attribute value
 * @param value_len Value length
 * @return 0 on success, negative errno on failure
 */
int matter_set_attribute(uint8_t endpoint, uint32_t cluster, 
                        uint32_t attribute, const void *value, size_t value_len);

/**
 * @brief Get Matter attribute value (simplified API)
 *
 * @param endpoint Endpoint ID
 * @param cluster Cluster ID
 * @param attribute Attribute ID
 * @param value Buffer to store attribute value
 * @param value_len Buffer length / returned value length
 * @return 0 on success, negative errno on failure
 */
int matter_get_attribute(uint8_t endpoint, uint32_t cluster,
                        uint32_t attribute, void *value, size_t *value_len);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_MATTER_MANAGER_H */
