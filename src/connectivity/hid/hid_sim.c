/**
 * @file hid_sim.c
 * @brief HID Simulation Transport for native_sim Testing
 */

#include "hid_sim.h"
#include "hid_manager.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(hid_sim, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* Internal State                                                            */
/*===========================================================================*/

static struct
{
    bool initialized;
    bool connected;
    bool enabled;
    hid_device_type_t device_types;

    hid_keyboard_report_t last_keyboard;
    hid_gamepad_report_t last_gamepad;

    hid_event_callback_t event_cb;
    void *event_cb_data;
    hid_output_callback_t output_cb;
    void *output_cb_data;
} sim_state;

/*===========================================================================*/
/* Debug Output                                                              */
/*===========================================================================*/

static void print_keyboard_report(const hid_keyboard_report_t *report)
{
    LOG_DBG("KB Report: mod=0x%02x keys=[%02x %02x %02x %02x %02x %02x]",
            report->modifiers,
            report->keys[0], report->keys[1], report->keys[2],
            report->keys[3], report->keys[4], report->keys[5]);
}

static void print_gamepad_report(const hid_gamepad_report_t *report)
{
    LOG_DBG("GP Report: btns=0x%04x hat=%d axes=[%d %d %d %d %d %d]",
            report->buttons, report->hat,
            report->axes[0], report->axes[1], report->axes[2],
            report->axes[3], report->axes[4], report->axes[5]);
}

/*===========================================================================*/
/* Transport Implementation                                                  */
/*===========================================================================*/

static int sim_init(hid_device_type_t types)
{
    LOG_INF("HID Simulation initializing (types=0x%02x)", types);

    memset(&sim_state, 0, sizeof(sim_state));
    sim_state.device_types = types;
    sim_state.initialized = true;

    return 0;
}

static int sim_deinit(void)
{
    sim_state.initialized = false;
    sim_state.connected = false;
    sim_state.enabled = false;

    LOG_INF("HID Simulation deinitialized");
    return 0;
}

static int sim_enable(void)
{
    if (!sim_state.initialized)
    {
        return -EINVAL;
    }

    sim_state.enabled = true;
    LOG_INF("HID Simulation enabled - ready for virtual connection");

    /* Auto-connect in simulation for easier testing */
    hid_sim_connect();

    return 0;
}

static int sim_disable(void)
{
    sim_state.enabled = false;
    sim_state.connected = false;

    LOG_INF("HID Simulation disabled");
    return 0;
}

static int sim_send_keyboard(const hid_keyboard_report_t *report)
{
    if (!sim_state.connected)
    {
        LOG_WRN("SIM: Cannot send keyboard - not connected");
        return -ENOTCONN;
    }

    memcpy(&sim_state.last_keyboard, report, sizeof(hid_keyboard_report_t));
    print_keyboard_report(report);

    return 0;
}

static int sim_send_gamepad(const hid_gamepad_report_t *report)
{
    if (!sim_state.connected)
    {
        LOG_WRN("SIM: Cannot send gamepad - not connected");
        return -ENOTCONN;
    }

    memcpy(&sim_state.last_gamepad, report, sizeof(hid_gamepad_report_t));
    print_gamepad_report(report);

    return 0;
}

static int sim_register_event_cb(hid_event_callback_t cb, void *user_data)
{
    sim_state.event_cb = cb;
    sim_state.event_cb_data = user_data;
    return 0;
}

static int sim_register_output_cb(hid_output_callback_t cb, void *user_data)
{
    sim_state.output_cb = cb;
    sim_state.output_cb_data = user_data;
    return 0;
}

static bool sim_is_connected(void)
{
    return sim_state.connected;
}

/*===========================================================================*/
/* Transport Operations Structure                                            */
/*===========================================================================*/

static const hid_transport_ops_t sim_transport = {
    .name = "sim",
    .init = sim_init,
    .deinit = sim_deinit,
    .enable = sim_enable,
    .disable = sim_disable,
    .send_keyboard = sim_send_keyboard,
    .send_gamepad = sim_send_gamepad,
    .register_event_cb = sim_register_event_cb,
    .register_output_cb = sim_register_output_cb,
    .is_connected = sim_is_connected};

/*===========================================================================*/
/* Public API                                                                */
/*===========================================================================*/

int hid_sim_init(void)
{
    LOG_INF("Registering HID simulation transport");
    return hid_manager_register_transport(&sim_transport);
}

const hid_transport_ops_t *hid_sim_get_transport(void)
{
    return &sim_transport;
}

void hid_sim_connect(void)
{
    if (!sim_state.enabled)
    {
        LOG_WRN("Cannot connect - HID sim not enabled");
        return;
    }

    sim_state.connected = true;
    LOG_INF("HID Simulation: Host connected");

    if (sim_state.event_cb)
    {
        sim_state.event_cb(HID_EVENT_CONNECTED, NULL, sim_state.event_cb_data);
    }
}

void hid_sim_disconnect(void)
{
    sim_state.connected = false;
    LOG_INF("HID Simulation: Host disconnected");

    if (sim_state.event_cb)
    {
        sim_state.event_cb(HID_EVENT_DISCONNECTED, NULL, sim_state.event_cb_data);
    }
}

const hid_keyboard_report_t *hid_sim_get_last_keyboard_report(void)
{
    return &sim_state.last_keyboard;
}

const hid_gamepad_report_t *hid_sim_get_last_gamepad_report(void)
{
    return &sim_state.last_gamepad;
}

void hid_sim_send_output_report(const uint8_t *data, size_t len)
{
    if (sim_state.output_cb)
    {
        sim_state.output_cb(data, len, sim_state.output_cb_data);
    }
}
