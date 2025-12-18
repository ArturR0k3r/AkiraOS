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
/* GATT Characteristics Implementation                                       */
/*===========================================================================*/

#if BT_AVAILABLE

/* OTA Control commands */
#define OTA_CMD_START  0x01
#define OTA_CMD_ABORT  0x02
#define OTA_CMD_STATUS 0x03

/* OTA Status responses */
#define OTA_STATUS_IDLE       0x00
#define OTA_STATUS_READY      0x01
#define OTA_STATUS_RECEIVING  0x02
#define OTA_STATUS_COMPLETE   0x03
#define OTA_STATUS_ERROR      0x04

/* Control characteristic write handler */
static ssize_t ota_control_write(struct bt_conn *conn,
                                  const struct bt_gatt_attr *attr,
                                  const void *buf, uint16_t len,
                                  uint16_t offset, uint8_t flags)
{
    if (offset > 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    
    if (len < 1) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    
    const uint8_t *data = buf;
    uint8_t cmd = data[0];
    
    LOG_INF("BLE OTA Control: cmd=0x%02x", cmd);
    
    switch (cmd) {
        case OTA_CMD_START:
            if (len >= 5) {
                /* Extract firmware size (4 bytes, little-endian) */
                uint32_t size = data[1] | (data[2] << 8) | 
                               (data[3] << 16) | (data[4] << 24);
                
                LOG_INF("Starting BLE OTA: size=%u bytes", size);
                
                /* Start OTA update */
                enum ota_result ret = ota_start_update(size);
                if (ret != OTA_OK) {
                    LOG_ERR("Failed to start OTA: %d", ret);
                    ble_ota.state = OTA_TRANSPORT_ERROR;
                    return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
                }
                
                ble_ota.state = OTA_TRANSPORT_RECEIVING;
                ble_ota.total_size = size;
                ble_ota.bytes_received = 0;
            }
            break;
            
        case OTA_CMD_ABORT:
            LOG_INF("Aborting BLE OTA");
            ota_abort_update();
            ble_ota.state = OTA_TRANSPORT_READY;
            ble_ota.bytes_received = 0;
            break;
            
        case OTA_CMD_STATUS:
            /* Status request - respond via notification */
            break;
            
        default:
            LOG_WRN("Unknown OTA control command: 0x%02x", cmd);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    
    return len;
}

/* Control characteristic read handler (return current status) */
static ssize_t ota_control_read(struct bt_conn *conn,
                                 const struct bt_gatt_attr *attr,
                                 void *buf, uint16_t len, uint16_t offset)
{
    uint8_t status;
    
    switch (ble_ota.state) {
        case OTA_TRANSPORT_IDLE:
            status = OTA_STATUS_IDLE;
            break;
        case OTA_TRANSPORT_READY:
            status = OTA_STATUS_READY;
            break;
        case OTA_TRANSPORT_RECEIVING:
            status = OTA_STATUS_RECEIVING;
            break;
        case OTA_TRANSPORT_ERROR:
            status = OTA_STATUS_ERROR;
            break;
        default:
            status = OTA_STATUS_IDLE;
            break;
    }
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &status, sizeof(status));
}

/* Data characteristic write handler - receives firmware chunks */
static ssize_t ota_data_write(struct bt_conn *conn,
                               const struct bt_gatt_attr *attr,
                               const void *buf, uint16_t len,
                               uint16_t offset, uint8_t flags)
{
    if (ble_ota.state != OTA_TRANSPORT_RECEIVING) {
        LOG_ERR("Not in receiving state");
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    
    /* Write data chunk */
    enum ota_result ret = ota_write_chunk(buf, len);
    if (ret != OTA_OK) {
        LOG_ERR("Failed to write OTA data: %d", ret);
        ble_ota.state = OTA_TRANSPORT_ERROR;
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    
    ble_ota.bytes_received += len;
    
    /* Log progress every 10KB */
    if ((ble_ota.bytes_received % 10240) == 0) {
        uint32_t progress = (ble_ota.bytes_received * 100) / ble_ota.total_size;
        LOG_INF("BLE OTA progress: %u%%", progress);
    }
    
    /* Check if complete */
    if (ble_ota.bytes_received >= ble_ota.total_size) {
        LOG_INF("BLE OTA complete, finalizing...");
        ret = ota_finalize_update();
        if (ret != OTA_OK) {
            LOG_ERR("Failed to finalize OTA: %d", ret);
            ble_ota.state = OTA_TRANSPORT_ERROR;
        } else {
            ble_ota.state = OTA_TRANSPORT_READY;
            LOG_INF("BLE OTA successful!");
        }
    }
    
    return len;
}

/* GATT Service Definition */
BT_GATT_SERVICE_DEFINE(ota_svc,
    BT_GATT_PRIMARY_SERVICE(OTA_SERVICE_UUID),
    
    /* Control Characteristic */
    BT_GATT_CHARACTERISTIC(OTA_CONTROL_UUID,
                          BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                          BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                          ota_control_read, ota_control_write, NULL),
    BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    /* Data Characteristic */
    BT_GATT_CHARACTERISTIC(OTA_DATA_UUID,
                          BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                          BT_GATT_PERM_WRITE,
                          NULL, ota_data_write, NULL),
);

#endif /* BT_AVAILABLE */

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
    /* GATT service is automatically registered by BT_GATT_SERVICE_DEFINE */
    LOG_INF("BLE OTA GATT service registered");
#else
    LOG_WRN("Bluetooth not available - BLE OTA disabled");
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
