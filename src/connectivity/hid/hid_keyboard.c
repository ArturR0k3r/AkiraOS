/**
 * @file hid_keyboard.c
 * @brief HID Keyboard Profile Implementation
 */

#include "hid_keyboard.h"
#include "hid_manager.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <ctype.h>

LOG_MODULE_REGISTER(hid_keyboard, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* Internal State                                                            */
/*===========================================================================*/

static struct {
    bool initialized;
    hid_keyboard_report_t report;
    struct k_mutex mutex;
} kbd_state;

/*===========================================================================*/
/* Helper Functions                                                          */
/*===========================================================================*/

/**
 * @brief Find key in report
 * @return Index (0-5) if found, -1 if not found
 */
static int find_key_in_report(uint8_t keycode)
{
    for (int i = 0; i < 6; i++) {
        if (kbd_state.report.keys[i] == keycode) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief Find empty slot in report
 * @return Index (0-5) if found, -1 if full
 */
static int find_empty_slot(void)
{
    for (int i = 0; i < 6; i++) {
        if (kbd_state.report.keys[i] == 0) {
            return i;
        }
    }
    return -1;
}

/*===========================================================================*/
/* API Implementation                                                        */
/*===========================================================================*/

int hid_keyboard_init(void)
{
    if (kbd_state.initialized) {
        return 0;
    }

    LOG_INF("Initializing HID keyboard");

    k_mutex_init(&kbd_state.mutex);

    memset(&kbd_state.report, 0, sizeof(hid_keyboard_report_t));

    kbd_state.initialized = true;
    return 0;
}

int hid_keyboard_deinit(void)
{
    if (!kbd_state.initialized) {
        return -EALREADY;
    }

    k_mutex_lock(&kbd_state.mutex, K_FOREVER);
    memset(&kbd_state.report, 0, sizeof(hid_keyboard_report_t));
    kbd_state.initialized = false;
    k_mutex_unlock(&kbd_state.mutex);

    return 0;
}

int hid_keyboard_press_key(uint8_t keycode)
{
    if (!kbd_state.initialized) {
        return -ENODEV;
    }

    if (keycode == 0 || keycode == HID_KEY_NONE) {
        return -EINVAL;
    }

    k_mutex_lock(&kbd_state.mutex, K_FOREVER);

    // Check if already pressed
    if (find_key_in_report(keycode) >= 0) {
        k_mutex_unlock(&kbd_state.mutex);
        return 0;  // Already pressed
    }

    // Find empty slot
    int slot = find_empty_slot();
    if (slot < 0) {
        k_mutex_unlock(&kbd_state.mutex);
        LOG_WRN("Keyboard report full (6 key rollover limit)");
        return -ENOMEM;
    }

    kbd_state.report.keys[slot] = keycode;
    k_mutex_unlock(&kbd_state.mutex);

    LOG_DBG("Key pressed: 0x%02x", keycode);
    return 0;
}

int hid_keyboard_release_key(uint8_t keycode)
{
    if (!kbd_state.initialized) {
        return -ENODEV;
    }

    k_mutex_lock(&kbd_state.mutex, K_FOREVER);

    int slot = find_key_in_report(keycode);
    if (slot < 0) {
        k_mutex_unlock(&kbd_state.mutex);
        return -ENOENT;  // Not pressed
    }

    // Shift remaining keys down
    for (int i = slot; i < 5; i++) {
        kbd_state.report.keys[i] = kbd_state.report.keys[i + 1];
    }
    kbd_state.report.keys[5] = 0;

    k_mutex_unlock(&kbd_state.mutex);

    LOG_DBG("Key released: 0x%02x", keycode);
    return 0;
}

int hid_keyboard_clear(void)
{
    if (!kbd_state.initialized) {
        return -ENODEV;
    }

    k_mutex_lock(&kbd_state.mutex, K_FOREVER);
    memset(kbd_state.report.keys, 0, sizeof(kbd_state.report.keys));
    kbd_state.report.modifiers = 0;
    k_mutex_unlock(&kbd_state.mutex);

    LOG_DBG("All keys released");
    return 0;
}

int hid_keyboard_set_modifier(uint8_t modifiers)
{
    if (!kbd_state.initialized) {
        return -ENODEV;
    }

    k_mutex_lock(&kbd_state.mutex, K_FOREVER);
    kbd_state.report.modifiers = modifiers;
    k_mutex_unlock(&kbd_state.mutex);

    return 0;
}

int hid_keyboard_press_modifier(uint8_t modifier)
{
    if (!kbd_state.initialized) {
        return -ENODEV;
    }

    k_mutex_lock(&kbd_state.mutex, K_FOREVER);
    kbd_state.report.modifiers |= modifier;
    k_mutex_unlock(&kbd_state.mutex);

    return 0;
}

int hid_keyboard_release_modifier(uint8_t modifier)
{
    if (!kbd_state.initialized) {
        return -ENODEV;
    }

    k_mutex_lock(&kbd_state.mutex, K_FOREVER);
    kbd_state.report.modifiers &= ~modifier;
    k_mutex_unlock(&kbd_state.mutex);

    return 0;
}

uint8_t hid_keyboard_get_modifiers(void)
{
    if (!kbd_state.initialized) {
        return 0;
    }

    k_mutex_lock(&kbd_state.mutex, K_FOREVER);
    uint8_t mods = kbd_state.report.modifiers;
    k_mutex_unlock(&kbd_state.mutex);

    return mods;
}

int hid_keyboard_get_report(hid_keyboard_report_t *report)
{
    if (!kbd_state.initialized) {
        return -ENODEV;
    }

    if (!report) {
        return -EINVAL;
    }

    k_mutex_lock(&kbd_state.mutex, K_FOREVER);
    memcpy(report, &kbd_state.report, sizeof(hid_keyboard_report_t));
    k_mutex_unlock(&kbd_state.mutex);

    return 0;
}

bool hid_keyboard_is_key_pressed(uint8_t keycode)
{
    if (!kbd_state.initialized) {
        return false;
    }

    k_mutex_lock(&kbd_state.mutex, K_FOREVER);
    bool pressed = (find_key_in_report(keycode) >= 0);
    k_mutex_unlock(&kbd_state.mutex);

    return pressed;
}

uint8_t hid_keyboard_get_pressed_count(void)
{
    if (!kbd_state.initialized) {
        return 0;
    }

    k_mutex_lock(&kbd_state.mutex, K_FOREVER);
    uint8_t count = 0;
    for (int i = 0; i < 6; i++) {
        if (kbd_state.report.keys[i] != 0) {
            count++;
        }
    }
    k_mutex_unlock(&kbd_state.mutex);

    return count;
}

uint8_t hid_keyboard_ascii_to_keycode(char ch, uint8_t *modifier)
{
    if (!modifier) {
        return HID_KEY_NONE;
    }

    *modifier = 0;

    // Lowercase letters
    if (ch >= 'a' && ch <= 'z') {
        return HID_KEY_A + (ch - 'a');
    }

    // Uppercase letters
    if (ch >= 'A' && ch <= 'Z') {
        *modifier = HID_MOD_LEFT_SHIFT;
        return HID_KEY_A + (ch - 'A');
    }

    // Numbers
    if (ch >= '1' && ch <= '9') {
        return HID_KEY_1 + (ch - '1');
    }
    if (ch == '0') {
        return HID_KEY_0;
    }

    // Special characters
    switch (ch) {
    case ' ': return HID_KEY_SPACE;
    case '\n': return HID_KEY_ENTER;
    case '\t': return HID_KEY_TAB;
    case '\b': return HID_KEY_BACKSPACE;
    case 0x1B: return HID_KEY_ESC;

    // Symbols
    case '-': return HID_KEY_MINUS;
    case '_': *modifier = HID_MOD_LEFT_SHIFT; return HID_KEY_MINUS;
    case '=': return HID_KEY_EQUAL;
    case '+': *modifier = HID_MOD_LEFT_SHIFT; return HID_KEY_EQUAL;
    case '[': return HID_KEY_LEFT_BRACE;
    case '{': *modifier = HID_MOD_LEFT_SHIFT; return HID_KEY_LEFT_BRACE;
    case ']': return HID_KEY_RIGHT_BRACE;
    case '}': *modifier = HID_MOD_LEFT_SHIFT; return HID_KEY_RIGHT_BRACE;
    case '\\': return HID_KEY_BACKSLASH;
    case '|': *modifier = HID_MOD_LEFT_SHIFT; return HID_KEY_BACKSLASH;
    case ';': return HID_KEY_SEMICOLON;
    case ':': *modifier = HID_MOD_LEFT_SHIFT; return HID_KEY_SEMICOLON;
    case '\'': return HID_KEY_QUOTE;
    case '"': *modifier = HID_MOD_LEFT_SHIFT; return HID_KEY_QUOTE;
    case '`': return HID_KEY_GRAVE;
    case '~': *modifier = HID_MOD_LEFT_SHIFT; return HID_KEY_GRAVE;
    case ',': return HID_KEY_COMMA;
    case '<': *modifier = HID_MOD_LEFT_SHIFT; return HID_KEY_COMMA;
    case '.': return HID_KEY_DOT;
    case '>': *modifier = HID_MOD_LEFT_SHIFT; return HID_KEY_DOT;
    case '/': return HID_KEY_SLASH;
    case '?': *modifier = HID_MOD_LEFT_SHIFT; return HID_KEY_SLASH;

    // Shifted numbers
    case '!': *modifier = HID_MOD_LEFT_SHIFT; return HID_KEY_1;
    case '@': *modifier = HID_MOD_LEFT_SHIFT; return HID_KEY_2;
    case '#': *modifier = HID_MOD_LEFT_SHIFT; return HID_KEY_3;
    case '$': *modifier = HID_MOD_LEFT_SHIFT; return HID_KEY_4;
    case '%': *modifier = HID_MOD_LEFT_SHIFT; return HID_KEY_5;
    case '^': *modifier = HID_MOD_LEFT_SHIFT; return HID_KEY_6;
    case '&': *modifier = HID_MOD_LEFT_SHIFT; return HID_KEY_7;
    case '*': *modifier = HID_MOD_LEFT_SHIFT; return HID_KEY_8;
    case '(': *modifier = HID_MOD_LEFT_SHIFT; return HID_KEY_9;
    case ')': *modifier = HID_MOD_LEFT_SHIFT; return HID_KEY_0;

    default:
        return HID_KEY_NONE;
    }
}

int hid_keyboard_type_string(const char *str, int (*send_callback)(const hid_keyboard_report_t *))
{
    if (!kbd_state.initialized) {
        return -ENODEV;
    }

    if (!str) {
        return -EINVAL;
    }

    LOG_DBG("Typing string: %s", str);

    for (const char *p = str; *p != '\0'; p++) {
        uint8_t modifier = 0;
        uint8_t keycode = hid_keyboard_ascii_to_keycode(*p, &modifier);

        if (keycode == HID_KEY_NONE) {
            LOG_WRN("Cannot type character: 0x%02x", *p);
            continue;
        }

        // Press key with modifier
        if (modifier) {
            hid_keyboard_press_modifier(modifier);
        }
        hid_keyboard_press_key(keycode);

        // Send report
        if (send_callback) {
            hid_keyboard_report_t report;
            hid_keyboard_get_report(&report);
            send_callback(&report);
        }

        // Small delay between keypresses
        k_msleep(10);

        // Release key
        hid_keyboard_release_key(keycode);
        if (modifier) {
            hid_keyboard_release_modifier(modifier);
        }

        // Send release report
        if (send_callback) {
            hid_keyboard_report_t report;
            hid_keyboard_get_report(&report);
            send_callback(&report);
        }

        k_msleep(10);
    }

    return 0;
}
