/**
 * @file ota_ble.c
 * @brief Bluetooth OTA Transport for AkiraOS
 *
 * Receives firmware updates via BLE (from mobile app).
 */

#include "../ota_transport.h"
#include "../ota_manager.h"
#include "../../connectivity/bluetooth/bt_manager.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#if defined(CONFIG_BT)
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#define BT_AVAILABLE 1
#else
#define BT_AVAILABLE 0
#endif

LOG_MODULE_REGISTER(ota_ble, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* OTA Service UUIDs                                                         */
/*===========================================================================*/

/* Custom OTA Service UUID: 0x1825 is DFU, but we use custom for flexibility */
#define OTA_SERVICE_UUID BT_UUID_DECLARE_16(0xFE59)
#define OTA_CONTROL_UUID BT_UUID_DECLARE_16(0xFE5A)
#define OTA_DATA_UUID BT_UUID_DECLARE_16(0xFE5B)

/*===========================================================================*/
/* Internal State                                                            */
/*===========================================================================*/

static struct
{
    bool initialized;
    bool enabled;
    ota_transport_state_t state;
    size_t bytes_received;
    size_t total_size;
    uint32_t crc;
} ble_ota;

/*===========================================================================*/
/* GATT Characteristics (Stub - requires full BLE implementation)            */
/*===========================================================================*/

#if BT_AVAILABLE
/* TODO: Implement GATT service for OTA */
/* - Control characteristic: Start/abort/status commands */
/* - Data characteristic: Firmware data chunks */
/* - Notification: Progress updates */
#endif

/*===========================================================================*/
/* Transport Implementation                                                  */
/*===========================================================================*/

static int ble_init(void)
{
    if (ble_ota.initialized)
    {
        return 0;
    }

    LOG_INF("Initializing BLE OTA transport");

    memset(&ble_ota, 0, sizeof(ble_ota));
    ble_ota.state = OTA_TRANSPORT_IDLE;

#if BT_AVAILABLE
    /* Register GATT service */
    /* TODO: Register OTA GATT service */
#endif

    ble_ota.initialized = true;
    return 0;
}

static int ble_deinit(void)
{
    ble_ota.initialized = false;
    ble_ota.enabled = false;
    return 0;
}

static int ble_enable(void)
{
    if (!ble_ota.initialized)
    {
        return -EINVAL;
    }

    ble_ota.enabled = true;
    ble_ota.state = OTA_TRANSPORT_READY;

    LOG_INF("BLE OTA transport enabled");
    return 0;
}

static int ble_disable(void)
{
    ble_ota.enabled = false;
    ble_ota.state = OTA_TRANSPORT_IDLE;

    LOG_INF("BLE OTA transport disabled");
    return 0;
}

static bool ble_is_available(void)
{
#if BT_AVAILABLE
    return bt_manager_is_connected();
#else
    return false;
#endif
}

static bool ble_is_active(void)
{
    return ble_ota.state == OTA_TRANSPORT_RECEIVING;
}

static int ble_abort(void)
{
    if (ble_ota.state == OTA_TRANSPORT_RECEIVING)
    {
        ota_abort_update();
        ble_ota.state = OTA_TRANSPORT_READY;
    }
    return 0;
}

static ota_transport_state_t ble_get_state(void)
{
    return ble_ota.state;
}

/*===========================================================================*/
/* Transport Registration                                                    */
/*===========================================================================*/

static const ota_transport_ops_t ble_transport = {
    .name = "ble",
    .source = OTA_SOURCE_BLE,
    .init = ble_init,
    .deinit = ble_deinit,
    .enable = ble_enable,
    .disable = ble_disable,
    .is_available = ble_is_available,
    .is_active = ble_is_active,
    .abort = ble_abort,
    .get_state = ble_get_state};

int ota_ble_init(void)
{
    return ota_transport_register(&ble_transport);
}
