/**
 * @file hid_shell.c
 * @brief Shell commands for testing HID functionality
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>
#include "../connectivity/hid/hid_manager.h"

LOG_MODULE_REGISTER(hid_shell, CONFIG_AKIRA_LOG_LEVEL);

static int cmd_hid_info(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "HID Manager Status:");
    shell_print(sh, "  Keyboard support: %s", IS_ENABLED(CONFIG_AKIRA_HID_KEYBOARD) ? "enabled" : "disabled");
    shell_print(sh, "  Gamepad support: %s", IS_ENABLED(CONFIG_AKIRA_HID_GAMEPAD) ? "enabled" : "disabled");
    shell_print(sh, "  USB HID (Kconfig): %s", IS_ENABLED(CONFIG_AKIRA_USB_HID) ? "enabled" : "disabled");
    shell_print(sh, "  BT HID (Kconfig): %s", IS_ENABLED(CONFIG_AKIRA_BT_HID) ? "enabled" : "disabled");
    
    /* Get runtime status */
    const hid_state_t *state = hid_manager_get_state();
    if (state) {
        shell_print(sh, "\nRuntime Status:");
        shell_print(sh, "  Manager enabled: %s", state->enabled ? "yes" : "no");
        shell_print(sh, "  Active transport: %d", state->transport);
        shell_print(sh, "  Connected: %s", state->connected ? "yes" : "no");
        shell_print(sh, "  Reports sent: %u", state->reports_sent);
        shell_print(sh, "  Errors: %u", state->errors);
    } else {
        shell_print(sh, "\nRuntime Status: Not initialized");
    }
    
    return 0;
}

static int cmd_hid_kbd_test(const struct shell *sh, size_t argc, char **argv)
{
    int ret;
    
    if (argc < 2) {
        shell_error(sh, "Usage: hid kbd test <string>");
        return -EINVAL;
    }
    
    shell_print(sh, "Typing: '%s'", argv[1]);
    
    ret = hid_keyboard_type_string(argv[1], NULL);
    if (ret < 0) {
        shell_error(sh, "Failed to type string: %d", ret);
        return ret;
    }
    
    shell_print(sh, "String typed successfully!");
    return 0;
}

static int cmd_hid_kbd_press(const struct shell *sh, size_t argc, char **argv)
{
    int ret;
    uint8_t keycode;
    
    if (argc < 2) {
        shell_error(sh, "Usage: hid kbd press <keycode>");
        return -EINVAL;
    }
    
    keycode = (uint8_t)strtol(argv[1], NULL, 0);
    
    ret = hid_keyboard_press(keycode);
    if (ret < 0) {
        shell_error(sh, "Failed to press key: %d", ret);
        return ret;
    }
    
    shell_print(sh, "Key 0x%02x pressed", keycode);
    return 0;
}

static int cmd_hid_kbd_release(const struct shell *sh, size_t argc, char **argv)
{
    int ret;
    uint8_t keycode;
    
    if (argc < 2) {
        shell_error(sh, "Usage: hid kbd release <keycode>");
        return -EINVAL;
    }
    
    keycode = (uint8_t)strtol(argv[1], NULL, 0);
    
    ret = hid_keyboard_release(keycode);
    if (ret < 0) {
        shell_error(sh, "Failed to release key: %d", ret);
        return ret;
    }
    
    shell_print(sh, "Key 0x%02x released", keycode);
    return 0;
}

static int cmd_hid_kbd_clear(const struct shell *sh, size_t argc, char **argv)
{
    int ret;
    
    ret = hid_keyboard_release_all();
    if (ret < 0) {
        shell_error(sh, "Failed to clear keyboard: %d", ret);
        return ret;
    }
    
    shell_print(sh, "All keys released");
    return 0;
}

static int cmd_hid_gamepad_button(const struct shell *sh, size_t argc, char **argv)
{
    int ret;
    uint8_t button;
    bool pressed;
    
    if (argc < 3) {
        shell_error(sh, "Usage: hid gamepad button <button> <0|1>");
        return -EINVAL;
    }
    
    button = (uint8_t)strtol(argv[1], NULL, 0);
    pressed = (bool)strtol(argv[2], NULL, 0);
    
    if (pressed) {
        ret = hid_gamepad_press(button);
    } else {
        ret = hid_gamepad_release(button);
    }
    
    if (ret < 0) {
        shell_error(sh, "Failed to set button: %d", ret);
        return ret;
    }
    
    shell_print(sh, "Button %d %s", button, pressed ? "pressed" : "released");
    return 0;
}

static int cmd_hid_gamepad_axis(const struct shell *sh, size_t argc, char **argv)
{
    int ret;
    uint8_t axis;
    int16_t value;
    
    if (argc < 3) {
        shell_error(sh, "Usage: hid gamepad axis <axis> <value>");
        shell_print(sh, "  axis: 0=X, 1=Y, 2=Z, 3=Rz");
        shell_print(sh, "  value: -32768 to 32767");
        return -EINVAL;
    }
    
    axis = (uint8_t)strtol(argv[1], NULL, 0);
    value = (int16_t)strtol(argv[2], NULL, 0);
    
    ret = hid_gamepad_set_axis(axis, value);
    if (ret < 0) {
        shell_error(sh, "Failed to set axis: %d", ret);
        return ret;
    }
    
    shell_print(sh, "Axis %d set to %d", axis, value);
    return 0;
}

static int cmd_hid_gamepad_dpad(const struct shell *sh, size_t argc, char **argv)
{
    int ret;
    uint8_t direction;
    
    if (argc < 2) {
        shell_error(sh, "Usage: hid gamepad dpad <direction>");
        shell_print(sh, "  0=Up, 1=UpRight, 2=Right, 3=DownRight");
        shell_print(sh, "  4=Down, 5=DownLeft, 6=Left, 7=UpLeft, 8=Center");
        return -EINVAL;
    }
    
    direction = (uint8_t)strtol(argv[1], NULL, 0);
    
    ret = hid_gamepad_set_dpad(direction);
    if (ret < 0) {
        shell_error(sh, "Failed to set dpad: %d", ret);
        return ret;
    }
    
    shell_print(sh, "D-pad set to direction %d", direction);
    return 0;
}

static int cmd_hid_gamepad_reset(const struct shell *sh, size_t argc, char **argv)
{
    int ret;
    
    ret = hid_gamepad_reset();
    if (ret < 0) {
        shell_error(sh, "Failed to reset gamepad: %d", ret);
        return ret;
    }
    
    shell_print(sh, "Gamepad reset");
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_hid_kbd,
    SHELL_CMD(test, NULL, "Type a string", cmd_hid_kbd_test),
    SHELL_CMD(press, NULL, "Press a key", cmd_hid_kbd_press),
    SHELL_CMD(release, NULL, "Release a key", cmd_hid_kbd_release),
    SHELL_CMD(clear, NULL, "Release all keys", cmd_hid_kbd_clear),
    SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_hid_gamepad,
    SHELL_CMD(button, NULL, "Press/release button", cmd_hid_gamepad_button),
    SHELL_CMD(axis, NULL, "Set axis value", cmd_hid_gamepad_axis),
    SHELL_CMD(dpad, NULL, "Set D-pad direction", cmd_hid_gamepad_dpad),
    SHELL_CMD(reset, NULL, "Reset gamepad state", cmd_hid_gamepad_reset),
    SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_hid,
    SHELL_CMD(info, NULL, "Show HID status", cmd_hid_info),
    SHELL_CMD(kbd, &sub_hid_kbd, "Keyboard commands", NULL),
    SHELL_CMD(gamepad, &sub_hid_gamepad, "Gamepad commands", NULL),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(hid, &sub_hid, "HID test commands", NULL);
