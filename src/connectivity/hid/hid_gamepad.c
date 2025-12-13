/**
 * @file hid_gamepad.c
 * @brief HID Gamepad Profile Implementation
 */

#include "hid_gamepad.h"
#include "hid_manager.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(hid_gamepad, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* Internal State                                                            */
/*===========================================================================*/

static struct {
    bool initialized;
    hid_gamepad_report_t report;
    struct k_mutex mutex;
} gp_state;

/*===========================================================================*/
/* API Implementation                                                        */
/*===========================================================================*/

int hid_gamepad_init(void)
{
    if (gp_state.initialized) {
        return 0;
    }

    LOG_INF("Initializing HID gamepad");

    k_mutex_init(&gp_state.mutex);

    memset(&gp_state.report, 0, sizeof(hid_gamepad_report_t));
    gp_state.report.report_id = 0x02;  // Gamepad report ID
    gp_state.report.hat = -1;          // Center position

    gp_state.initialized = true;
    return 0;
}

int hid_gamepad_deinit(void)
{
    if (!gp_state.initialized) {
        return -EALREADY;
    }

    k_mutex_lock(&gp_state.mutex, K_FOREVER);
    memset(&gp_state.report, 0, sizeof(hid_gamepad_report_t));
    gp_state.initialized = false;
    k_mutex_unlock(&gp_state.mutex);

    return 0;
}

int hid_gamepad_press_button(uint8_t button)
{
    if (!gp_state.initialized) {
        return -ENODEV;
    }

    if (button > 15) {
        return -EINVAL;
    }

    k_mutex_lock(&gp_state.mutex, K_FOREVER);
    gp_state.report.buttons |= (1 << button);
    k_mutex_unlock(&gp_state.mutex);

    LOG_DBG("Button pressed: %u", button);
    return 0;
}

int hid_gamepad_release_button(uint8_t button)
{
    if (!gp_state.initialized) {
        return -ENODEV;
    }

    if (button > 15) {
        return -EINVAL;
    }

    k_mutex_lock(&gp_state.mutex, K_FOREVER);
    gp_state.report.buttons &= ~(1 << button);
    k_mutex_unlock(&gp_state.mutex);

    LOG_DBG("Button released: %u", button);
    return 0;
}

int hid_gamepad_release_all_buttons(void)
{
    if (!gp_state.initialized) {
        return -ENODEV;
    }

    k_mutex_lock(&gp_state.mutex, K_FOREVER);
    gp_state.report.buttons = 0;
    k_mutex_unlock(&gp_state.mutex);

    LOG_DBG("All buttons released");
    return 0;
}

int hid_gamepad_set_axis(hid_gamepad_axis_t axis, int16_t value)
{
    if (!gp_state.initialized) {
        return -ENODEV;
    }

    if (axis >= HID_GAMEPAD_AXIS_COUNT) {
        return -EINVAL;
    }

    k_mutex_lock(&gp_state.mutex, K_FOREVER);
    gp_state.report.axes[axis] = value;
    k_mutex_unlock(&gp_state.mutex);

    LOG_DBG("Axis %d set to %d", axis, value);
    return 0;
}

int hid_gamepad_set_trigger(uint8_t trigger, int16_t value)
{
    if (!gp_state.initialized) {
        return -ENODEV;
    }

    if (trigger > 1) {
        return -EINVAL;
    }

    // Clamp to 0-32767
    if (value < 0) {
        value = 0;
    }

    k_mutex_lock(&gp_state.mutex, K_FOREVER);
    gp_state.report.triggers[trigger] = value;
    k_mutex_unlock(&gp_state.mutex);

    LOG_DBG("Trigger %u set to %d", trigger, value);
    return 0;
}

int hid_gamepad_set_hat(int8_t direction)
{
    if (!gp_state.initialized) {
        return -ENODEV;
    }

    if (direction < -1 || direction > 7) {
        return -EINVAL;
    }

    k_mutex_lock(&gp_state.mutex, K_FOREVER);
    gp_state.report.hat = direction;
    k_mutex_unlock(&gp_state.mutex);

    LOG_DBG("D-pad set to %d", direction);
    return 0;
}

int hid_gamepad_reset(void)
{
    if (!gp_state.initialized) {
        return -ENODEV;
    }

    k_mutex_lock(&gp_state.mutex, K_FOREVER);
    
    // Reset buttons
    gp_state.report.buttons = 0;
    
    // Center all axes
    for (int i = 0; i < HID_GAMEPAD_AXIS_COUNT; i++) {
        gp_state.report.axes[i] = 0;
    }
    
    // Release triggers
    gp_state.report.triggers[0] = 0;
    gp_state.report.triggers[1] = 0;
    
    // Center D-pad
    gp_state.report.hat = -1;
    
    k_mutex_unlock(&gp_state.mutex);

    LOG_DBG("Gamepad reset to neutral");
    return 0;
}

int hid_gamepad_get_report(hid_gamepad_report_t *report)
{
    if (!gp_state.initialized) {
        return -ENODEV;
    }

    if (!report) {
        return -EINVAL;
    }

    k_mutex_lock(&gp_state.mutex, K_FOREVER);
    memcpy(report, &gp_state.report, sizeof(hid_gamepad_report_t));
    k_mutex_unlock(&gp_state.mutex);

    return 0;
}

bool hid_gamepad_is_button_pressed(uint8_t button)
{
    if (!gp_state.initialized || button > 15) {
        return false;
    }

    k_mutex_lock(&gp_state.mutex, K_FOREVER);
    bool pressed = (gp_state.report.buttons & (1 << button)) != 0;
    k_mutex_unlock(&gp_state.mutex);

    return pressed;
}

uint16_t hid_gamepad_get_button_state(void)
{
    if (!gp_state.initialized) {
        return 0;
    }

    k_mutex_lock(&gp_state.mutex, K_FOREVER);
    uint16_t state = gp_state.report.buttons;
    k_mutex_unlock(&gp_state.mutex);

    return state;
}

int16_t hid_gamepad_get_axis(hid_gamepad_axis_t axis)
{
    if (!gp_state.initialized || axis >= HID_GAMEPAD_AXIS_COUNT) {
        return 0;
    }

    k_mutex_lock(&gp_state.mutex, K_FOREVER);
    int16_t value = gp_state.report.axes[axis];
    k_mutex_unlock(&gp_state.mutex);

    return value;
}
