/**
 * @file hid_manager.c
 * @brief HID Manager Implementation for AkiraOS
 */

#include "hid_manager.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(hid_manager, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* Configuration                                                             */
/*===========================================================================*/

#define MAX_HID_TRANSPORTS 4

/* HID Report Rate Limiting (USB HID spec: 125Hz max = 8ms interval) */
#define HID_REPORT_INTERVAL_MS  8  /**< Minimum interval between reports (125Hz) */
#define HID_REPORT_INTERVAL_US  (HID_REPORT_INTERVAL_MS * 1000)

/*===========================================================================*/
/* Internal State                                                            */
/*===========================================================================*/

static struct
{
    bool initialized;
    hid_config_t config;
    hid_state_t state;

    /* Registered transports */
    const hid_transport_ops_t *transports[MAX_HID_TRANSPORTS];
    int transport_count;
    const hid_transport_ops_t *active_transport;

    /* Callbacks */
    hid_event_callback_t event_cb;
    void *event_cb_data;
    hid_output_callback_t output_cb;
    void *output_cb_data;

    /* Rate limiting */
    int64_t last_keyboard_report_us;
    int64_t last_gamepad_report_us;

    /* Mutex for thread safety */
    struct k_mutex mutex;
} hid_mgr;

/*===========================================================================*/
/* Internal Functions                                                        */
/*===========================================================================*/

static const hid_transport_ops_t *find_transport(hid_transport_t type)
{
    for (int i = 0; i < hid_mgr.transport_count; i++)
    {
        const hid_transport_ops_t *t = hid_mgr.transports[i];
        if (t)
        {
            if ((type == HID_TRANSPORT_BLE && strcmp(t->name, "ble") == 0) ||
                (type == HID_TRANSPORT_USB && strcmp(t->name, "usb") == 0) ||
                (type == HID_TRANSPORT_SIMULATED && strcmp(t->name, "sim") == 0))
            {
                return t;
            }
        }
    }
    return NULL;
}

static int send_keyboard_report(void)
{
    if (!hid_mgr.active_transport || !hid_mgr.active_transport->send_keyboard)
    {
        return -ENODEV;
    }

    /* Rate limiting: enforce minimum interval between reports */
    int64_t now = k_uptime_get() * 1000; /* Convert to microseconds */
    int64_t elapsed = now - hid_mgr.last_keyboard_report_us;
    
    if (elapsed < HID_REPORT_INTERVAL_US)
    {
        /* Too soon, throttle */
        return -EAGAIN;
    }

    int ret = hid_mgr.active_transport->send_keyboard(&hid_mgr.state.keyboard);
    if (ret == 0)
    {
        hid_mgr.state.reports_sent++;
        hid_mgr.last_keyboard_report_us = now;
    }
    else
    {
        hid_mgr.state.errors++;
    }
    return ret;
}

static int send_gamepad_report(void)
{
    if (!hid_mgr.active_transport || !hid_mgr.active_transport->send_gamepad)
    {
        return -ENODEV;
    }

    /* Rate limiting: enforce minimum interval between reports */
    int64_t now = k_uptime_get() * 1000; /* Convert to microseconds */
    int64_t elapsed = now - hid_mgr.last_gamepad_report_us;
    
    if (elapsed < HID_REPORT_INTERVAL_US)
    {
        /* Too soon, throttle */
        return -EAGAIN;
    }

    int ret = hid_mgr.active_transport->send_gamepad(&hid_mgr.state.gamepad);
    if (ret == 0)
    {
        hid_mgr.state.reports_sent++;
        hid_mgr.last_gamepad_report_us = now;
    }
    else
    {
        hid_mgr.state.errors++;
    }
    return ret;
}

/* Map ASCII to HID key code */
static hid_key_code_t ascii_to_keycode(char c, uint8_t *modifier)
{
    *modifier = 0;

    if (c >= 'a' && c <= 'z')
    {
        return HID_KEY_A + (c - 'a');
    }
    if (c >= 'A' && c <= 'Z')
    {
        *modifier = HID_MOD_LEFT_SHIFT;
        return HID_KEY_A + (c - 'A');
    }
    if (c >= '1' && c <= '9')
    {
        return HID_KEY_1 + (c - '1');
    }
    if (c == '0')
        return HID_KEY_0;
    if (c == ' ')
        return HID_KEY_SPACE;
    if (c == '\n')
        return HID_KEY_ENTER;
    if (c == '\t')
        return HID_KEY_TAB;

    /* Shifted symbols */
    switch (c)
    {
    case '!':
        *modifier = HID_MOD_LEFT_SHIFT;
        return HID_KEY_1;
    case '@':
        *modifier = HID_MOD_LEFT_SHIFT;
        return HID_KEY_2;
    case '#':
        *modifier = HID_MOD_LEFT_SHIFT;
        return HID_KEY_3;
    case '$':
        *modifier = HID_MOD_LEFT_SHIFT;
        return HID_KEY_4;
    case '%':
        *modifier = HID_MOD_LEFT_SHIFT;
        return HID_KEY_5;
    case '^':
        *modifier = HID_MOD_LEFT_SHIFT;
        return HID_KEY_6;
    case '&':
        *modifier = HID_MOD_LEFT_SHIFT;
        return HID_KEY_7;
    case '*':
        *modifier = HID_MOD_LEFT_SHIFT;
        return HID_KEY_8;
    case '(':
        *modifier = HID_MOD_LEFT_SHIFT;
        return HID_KEY_9;
    case ')':
        *modifier = HID_MOD_LEFT_SHIFT;
        return HID_KEY_0;
    case '-':
        return HID_KEY_MINUS;
    case '_':
        *modifier = HID_MOD_LEFT_SHIFT;
        return HID_KEY_MINUS;
    case '=':
        return HID_KEY_EQUAL;
    case '+':
        *modifier = HID_MOD_LEFT_SHIFT;
        return HID_KEY_EQUAL;
    case '.':
        return HID_KEY_DOT;
    case ',':
        return HID_KEY_COMMA;
    case '/':
        return HID_KEY_SLASH;
    case '?':
        *modifier = HID_MOD_LEFT_SHIFT;
        return HID_KEY_SLASH;
    default:
        return HID_KEY_NONE;
    }
}

/*===========================================================================*/
/* HID Manager API Implementation                                            */
/*===========================================================================*/

int hid_manager_init(const hid_config_t *config)
{
    if (hid_mgr.initialized)
    {
        return 0;
    }

    LOG_INF("Initializing HID manager");

    k_mutex_init(&hid_mgr.mutex);

    memset(&hid_mgr.state, 0, sizeof(hid_mgr.state));

    if (config)
    {
        memcpy(&hid_mgr.config, config, sizeof(hid_config_t));
        hid_mgr.state.device_type = config->device_types;
        hid_mgr.state.transport = config->preferred_transport;
    }
    else
    {
        /* Default config from Kconfig */
        hid_mgr.config.device_types = HID_DEVICE_COMBO;
        hid_mgr.config.preferred_transport = HID_TRANSPORT_BLE;
#ifdef CONFIG_AKIRA_HID_PRODUCT
        hid_mgr.config.device_name = CONFIG_AKIRA_HID_PRODUCT;
#else
        hid_mgr.config.device_name = "AkiraOS HID";
#endif
#ifdef CONFIG_AKIRA_HID_VID
        hid_mgr.config.vendor_id = CONFIG_AKIRA_HID_VID;
#else
        hid_mgr.config.vendor_id = 0x1209;  /* pid.codes */
#endif
#ifdef CONFIG_AKIRA_HID_PID
        hid_mgr.config.product_id = CONFIG_AKIRA_HID_PID;
#else
        hid_mgr.config.product_id = 0x0001;
#endif
        hid_mgr.state.device_type = HID_DEVICE_COMBO;
    }

    hid_mgr.initialized = true;

    LOG_INF("HID manager initialized (types=0x%02x)", hid_mgr.config.device_types);

    return 0;
}

int hid_manager_deinit(void)
{
    if (!hid_mgr.initialized)
    {
        return 0;
    }

    hid_manager_disable();

    k_mutex_lock(&hid_mgr.mutex, K_FOREVER);
    hid_mgr.initialized = false;
    hid_mgr.active_transport = NULL;
    k_mutex_unlock(&hid_mgr.mutex);

    LOG_INF("HID manager deinitialized");
    return 0;
}

int hid_manager_enable(void)
{
    if (!hid_mgr.initialized)
    {
        return -EINVAL;
    }

    k_mutex_lock(&hid_mgr.mutex, K_FOREVER);

    /* Find preferred transport */
    hid_mgr.active_transport = find_transport(hid_mgr.config.preferred_transport);
    if (!hid_mgr.active_transport && hid_mgr.transport_count > 0)
    {
        hid_mgr.active_transport = hid_mgr.transports[0];
    }

    if (!hid_mgr.active_transport)
    {
        k_mutex_unlock(&hid_mgr.mutex);
        LOG_WRN("No HID transport available");
        return -ENODEV;
    }

    /* Initialize and enable transport */
    if (hid_mgr.active_transport->init)
    {
        int ret = hid_mgr.active_transport->init(hid_mgr.config.device_types);
        if (ret != 0)
        {
            k_mutex_unlock(&hid_mgr.mutex);
            return ret;
        }
    }

    if (hid_mgr.active_transport->enable)
    {
        int ret = hid_mgr.active_transport->enable();
        if (ret != 0)
        {
            k_mutex_unlock(&hid_mgr.mutex);
            return ret;
        }
    }

    hid_mgr.state.enabled = true;

    k_mutex_unlock(&hid_mgr.mutex);

    LOG_INF("HID enabled via %s", hid_mgr.active_transport->name);
    return 0;
}

int hid_manager_disable(void)
{
    if (!hid_mgr.initialized)
    {
        return 0;
    }

    k_mutex_lock(&hid_mgr.mutex, K_FOREVER);

    if (hid_mgr.active_transport && hid_mgr.active_transport->disable)
    {
        hid_mgr.active_transport->disable();
    }

    hid_mgr.state.enabled = false;
    hid_mgr.state.connected = false;

    k_mutex_unlock(&hid_mgr.mutex);

    LOG_INF("HID disabled");
    return 0;
}

int hid_manager_set_transport(hid_transport_t transport)
{
    if (!hid_mgr.initialized)
    {
        return -EINVAL;
    }

    k_mutex_lock(&hid_mgr.mutex, K_FOREVER);

    const hid_transport_ops_t *new_transport = find_transport(transport);
    if (!new_transport)
    {
        k_mutex_unlock(&hid_mgr.mutex);
        return -ENODEV;
    }

    /* Disable current transport */
    if (hid_mgr.active_transport && hid_mgr.active_transport->disable)
    {
        hid_mgr.active_transport->disable();
    }

    /* Switch to new transport */
    hid_mgr.active_transport = new_transport;
    hid_mgr.state.transport = transport;

    /* Enable new transport if HID was enabled */
    if (hid_mgr.state.enabled && new_transport->enable)
    {
        new_transport->enable();
    }

    k_mutex_unlock(&hid_mgr.mutex);

    LOG_INF("Switched to HID transport: %s", new_transport->name);
    return 0;
}

hid_transport_t hid_manager_get_transport(void)
{
    return hid_mgr.state.transport;
}

bool hid_manager_is_connected(void)
{
    if (!hid_mgr.active_transport || !hid_mgr.active_transport->is_connected)
    {
        return false;
    }
    return hid_mgr.active_transport->is_connected();
}

const hid_state_t *hid_manager_get_state(void)
{
    hid_mgr.state.connected = hid_manager_is_connected();
    return &hid_mgr.state;
}

/*===========================================================================*/
/* Keyboard API Implementation                                               */
/*===========================================================================*/

int hid_keyboard_press(hid_key_code_t key)
{
    if (!hid_mgr.initialized || !(hid_mgr.config.device_types & HID_DEVICE_KEYBOARD))
    {
        return -EINVAL;
    }

    k_mutex_lock(&hid_mgr.mutex, K_FOREVER);

    /* Add key to report if not already present */
    for (int i = 0; i < HID_MAX_KEYS; i++)
    {
        if (hid_mgr.state.keyboard.keys[i] == key)
        {
            k_mutex_unlock(&hid_mgr.mutex);
            return 0; /* Already pressed */
        }
        if (hid_mgr.state.keyboard.keys[i] == HID_KEY_NONE)
        {
            hid_mgr.state.keyboard.keys[i] = key;
            break;
        }
    }

    int ret = send_keyboard_report();
    k_mutex_unlock(&hid_mgr.mutex);
    return ret;
}

int hid_keyboard_release(hid_key_code_t key)
{
    if (!hid_mgr.initialized)
    {
        return -EINVAL;
    }

    k_mutex_lock(&hid_mgr.mutex, K_FOREVER);

    /* Remove key from report */
    for (int i = 0; i < HID_MAX_KEYS; i++)
    {
        if (hid_mgr.state.keyboard.keys[i] == key)
        {
            /* Shift remaining keys */
            for (int j = i; j < HID_MAX_KEYS - 1; j++)
            {
                hid_mgr.state.keyboard.keys[j] = hid_mgr.state.keyboard.keys[j + 1];
            }
            hid_mgr.state.keyboard.keys[HID_MAX_KEYS - 1] = HID_KEY_NONE;
            break;
        }
    }

    int ret = send_keyboard_report();
    k_mutex_unlock(&hid_mgr.mutex);
    return ret;
}

int hid_keyboard_release_all(void)
{
    if (!hid_mgr.initialized)
    {
        return -EINVAL;
    }

    k_mutex_lock(&hid_mgr.mutex, K_FOREVER);

    memset(hid_mgr.state.keyboard.keys, 0, sizeof(hid_mgr.state.keyboard.keys));
    hid_mgr.state.keyboard.modifiers = 0;

    int ret = send_keyboard_report();
    k_mutex_unlock(&hid_mgr.mutex);
    return ret;
}

int hid_keyboard_set_modifiers(uint8_t modifiers)
{
    if (!hid_mgr.initialized)
    {
        return -EINVAL;
    }

    k_mutex_lock(&hid_mgr.mutex, K_FOREVER);
    hid_mgr.state.keyboard.modifiers = modifiers;
    int ret = send_keyboard_report();
    k_mutex_unlock(&hid_mgr.mutex);
    return ret;
}

int hid_keyboard_type_string(const char *str)
{
    if (!hid_mgr.initialized || !str)
    {
        return -EINVAL;
    }

    for (const char *p = str; *p; p++)
    {
        uint8_t modifier;
        hid_key_code_t key = ascii_to_keycode(*p, &modifier);

        if (key != HID_KEY_NONE)
        {
            if (modifier)
            {
                hid_keyboard_set_modifiers(modifier);
            }
            hid_keyboard_press(key);
            k_sleep(K_MSEC(10));
            hid_keyboard_release(key);
            if (modifier)
            {
                hid_keyboard_set_modifiers(0);
            }
            k_sleep(K_MSEC(10));
        }
    }

    return 0;
}

int hid_keyboard_send_report(const hid_keyboard_report_t *report)
{
    if (!hid_mgr.initialized || !report)
    {
        return -EINVAL;
    }

    k_mutex_lock(&hid_mgr.mutex, K_FOREVER);
    memcpy(&hid_mgr.state.keyboard, report, sizeof(hid_keyboard_report_t));
    int ret = send_keyboard_report();
    k_mutex_unlock(&hid_mgr.mutex);
    return ret;
}

/*===========================================================================*/
/* Gamepad API Implementation                                                */
/*===========================================================================*/

int hid_gamepad_press(hid_gamepad_btn_t button)
{
    if (!hid_mgr.initialized || !(hid_mgr.config.device_types & HID_DEVICE_GAMEPAD))
    {
        return -EINVAL;
    }

    k_mutex_lock(&hid_mgr.mutex, K_FOREVER);
    hid_mgr.state.gamepad.buttons |= button;
    int ret = send_gamepad_report();
    k_mutex_unlock(&hid_mgr.mutex);
    return ret;
}

int hid_gamepad_release(hid_gamepad_btn_t button)
{
    if (!hid_mgr.initialized)
    {
        return -EINVAL;
    }

    k_mutex_lock(&hid_mgr.mutex, K_FOREVER);
    hid_mgr.state.gamepad.buttons &= ~button;
    int ret = send_gamepad_report();
    k_mutex_unlock(&hid_mgr.mutex);
    return ret;
}

int hid_gamepad_set_axis(hid_gamepad_axis_t axis, int16_t value)
{
    if (!hid_mgr.initialized || axis >= HID_GAMEPAD_MAX_AXES)
    {
        return -EINVAL;
    }

    k_mutex_lock(&hid_mgr.mutex, K_FOREVER);
    hid_mgr.state.gamepad.axes[axis] = value;
    int ret = send_gamepad_report();
    k_mutex_unlock(&hid_mgr.mutex);
    return ret;
}

int hid_gamepad_set_dpad(uint8_t direction)
{
    if (!hid_mgr.initialized || direction > 8)
    {
        return -EINVAL;
    }

    k_mutex_lock(&hid_mgr.mutex, K_FOREVER);
    hid_mgr.state.gamepad.hat = direction;
    int ret = send_gamepad_report();
    k_mutex_unlock(&hid_mgr.mutex);
    return ret;
}

int hid_gamepad_send_report(const hid_gamepad_report_t *report)
{
    if (!hid_mgr.initialized || !report)
    {
        return -EINVAL;
    }

    k_mutex_lock(&hid_mgr.mutex, K_FOREVER);
    memcpy(&hid_mgr.state.gamepad, report, sizeof(hid_gamepad_report_t));
    int ret = send_gamepad_report();
    k_mutex_unlock(&hid_mgr.mutex);
    return ret;
}

int hid_gamepad_reset(void)
{
    if (!hid_mgr.initialized)
    {
        return -EINVAL;
    }

    k_mutex_lock(&hid_mgr.mutex, K_FOREVER);
    memset(&hid_mgr.state.gamepad, 0, sizeof(hid_gamepad_report_t));
    int ret = send_gamepad_report();
    k_mutex_unlock(&hid_mgr.mutex);
    return ret;
}

/*===========================================================================*/
/* Callback Registration                                                     */
/*===========================================================================*/

int hid_manager_register_event_callback(hid_event_callback_t callback, void *user_data)
{
    k_mutex_lock(&hid_mgr.mutex, K_FOREVER);
    hid_mgr.event_cb = callback;
    hid_mgr.event_cb_data = user_data;
    k_mutex_unlock(&hid_mgr.mutex);
    return 0;
}

int hid_manager_register_output_callback(hid_output_callback_t callback, void *user_data)
{
    k_mutex_lock(&hid_mgr.mutex, K_FOREVER);
    hid_mgr.output_cb = callback;
    hid_mgr.output_cb_data = user_data;
    k_mutex_unlock(&hid_mgr.mutex);
    return 0;
}

/*===========================================================================*/
/* Transport Registration                                                    */
/*===========================================================================*/

int hid_manager_register_transport(const hid_transport_ops_t *ops)
{
    if (!ops || !ops->name)
    {
        return -EINVAL;
    }

    k_mutex_lock(&hid_mgr.mutex, K_FOREVER);

    if (hid_mgr.transport_count >= MAX_HID_TRANSPORTS)
    {
        k_mutex_unlock(&hid_mgr.mutex);
        return -ENOMEM;
    }

    hid_mgr.transports[hid_mgr.transport_count++] = ops;

    k_mutex_unlock(&hid_mgr.mutex);

    LOG_INF("Registered HID transport: %s", ops->name);
    return 0;
}

int hid_manager_unregister_transport(const char *name)
{
    if (!name)
    {
        return -EINVAL;
    }

    k_mutex_lock(&hid_mgr.mutex, K_FOREVER);

    for (int i = 0; i < hid_mgr.transport_count; i++)
    {
        if (hid_mgr.transports[i] && strcmp(hid_mgr.transports[i]->name, name) == 0)
        {
            /* Shift remaining */
            for (int j = i; j < hid_mgr.transport_count - 1; j++)
            {
                hid_mgr.transports[j] = hid_mgr.transports[j + 1];
            }
            hid_mgr.transport_count--;

            if (hid_mgr.active_transport &&
                strcmp(hid_mgr.active_transport->name, name) == 0)
            {
                hid_mgr.active_transport = NULL;
            }

            k_mutex_unlock(&hid_mgr.mutex);
            LOG_INF("Unregistered HID transport: %s", name);
            return 0;
        }
    }

    k_mutex_unlock(&hid_mgr.mutex);
    return -ENOENT;
}
