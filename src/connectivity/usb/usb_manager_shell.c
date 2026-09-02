/**
 * @file usb_manager_shell.c
 * @brief USB Manager Shell Commands Implementation
 */

#include "usb_manager.h"
#include <zephyr/shell/shell.h>
#include <zephyr/kernel.h>
#include <stdlib.h>

#if defined(CONFIG_AKIRA_USB_MSC)
#include "../../storage/usb_msc.h"
#endif

/* Shell callback handle for event monitoring */
static int shell_callback_handle = -1;
static const struct shell *shell_instance = NULL;

/**
 * @brief Command: usb init
 */
static int cmd_usb_init(const struct shell *sh, size_t argc, char **argv)
{
    int ret;
    
    shell_print(sh, "Initializing USB manager...");
    
    ret = usb_manager_init();
    if (ret == 0) {
        shell_print(sh, "USB manager initialized successfully");
    } else if (ret == -EALREADY) {
        shell_error(sh, "USB manager already initialized");
    } else {
        shell_error(sh, "Failed to initialize USB manager: %d", ret);
    }
    
    return ret;
}

/**
 * @brief Command: usb finalize
 */
static int cmd_usb_finalize(const struct shell *sh, size_t argc, char **argv)
{
    int ret;
    
    shell_print(sh, "Finalizing USB device stack...");
    
    ret = usb_manager_finalize();
    if (ret == 0) {
        shell_print(sh, "USB device stack finalized successfully");
    } else {
        shell_error(sh, "Failed to finalize USB stack: %d", ret);
    }
    
    return ret;
}

/**
 * @brief Command: usb deinit
 */
static int cmd_usb_deinit(const struct shell *sh, size_t argc, char **argv)
{
    int ret;
    
    shell_print(sh, "Deinitializing USB manager...");
    
    /* Unregister shell callback if active */
    if (shell_callback_handle >= 0) {
        usb_manager_unregister_callback(shell_callback_handle);
        shell_callback_handle = -1;
        shell_instance = NULL;
    }
    
    ret = usb_manager_deinit();
    if (ret == 0) {
        shell_print(sh, "USB manager deinitialized successfully");
    } else {
        shell_error(sh, "Failed to deinitialize USB manager: %d", ret);
    }
    
    return ret;
}

/**
 * @brief Command: usb enable
 */
static int cmd_usb_enable(const struct shell *sh, size_t argc, char **argv)
{
    int ret;
    
    shell_print(sh, "Enabling USB device...");
    
    ret = usb_manager_enable();
    if (ret == 0) {
        shell_print(sh, "USB device enabled successfully");
    } else if (ret == -EALREADY) {
        shell_error(sh, "USB device already enabled");
    } else {
        shell_error(sh, "Failed to enable USB device: %d", ret);
    }
    
    return ret;
}

/**
 * @brief Command: usb disable
 */
static int cmd_usb_disable(const struct shell *sh, size_t argc, char **argv)
{
    int ret;
    
    shell_print(sh, "Disabling USB device...");
    
    ret = usb_manager_disable();
    if (ret == 0) {
        shell_print(sh, "USB device disabled successfully");
    } else {
        shell_error(sh, "Failed to disable USB device: %d", ret);
    }
    
    return ret;
}

/**
 * @brief Command: usb status
 */
static int cmd_usb_status(const struct shell *sh, size_t argc, char **argv)
{
    usb_manager_state_t state;
    
    state = usb_manager_get_state();
    
    shell_print(sh, "USB Manager Status:");
    shell_print(sh, "  State: %s", usb_manager_state_to_string(state));
    shell_print(sh, "  Configured: %s", usb_manager_is_configured() ? "Yes" : "No");
    shell_print(sh, "  Enabled: %s", usb_manager_is_enabled() ? "Yes" : "No");
    shell_print(sh, "  Monitor: %s", (shell_callback_handle >= 0) ? "Active" : "Inactive");
    
    return 0;
}

/**
 * @brief Command: usb stats
 */
static int cmd_usb_stats(const struct shell *sh, size_t argc, char **argv)
{
    usb_manager_stats_t stats;
    int ret;
    
    ret = usb_manager_get_stats(&stats);
    if (ret != 0) {
        shell_error(sh, "Failed to get statistics: %d", ret);
        return ret;
    }
    
    shell_print(sh, "USB Manager Statistics:");
    shell_print(sh, "  Configured count: %u", stats.configured_count);
    shell_print(sh, "  Suspended count:  %u", stats.suspended_count);
    shell_print(sh, "  Resumed count:    %u", stats.resumed_count);
    shell_print(sh, "  Reset count:      %u", stats.reset_count);
    shell_print(sh, "  Error count:      %u", stats.error_count);
    
    return 0;
}

/**
 * @brief Command: usb stats reset
 */
static int cmd_usb_stats_reset(const struct shell *sh, size_t argc, char **argv)
{
    int ret;
    
    ret = usb_manager_reset_stats();
    if (ret == 0) {
        shell_print(sh, "USB statistics reset successfully");
    } else {
        shell_error(sh, "Failed to reset statistics: %d", ret);
    }
    
    return ret;
}

/**
 * @brief Command: usb wakeup
 */
static int cmd_usb_wakeup(const struct shell *sh, size_t argc, char **argv)
{
    int ret;
    
    shell_print(sh, "Triggering remote wakeup...");
    
    ret = usb_manager_remote_wakeup();
    if (ret == 0) {
        shell_print(sh, "Remote wakeup triggered successfully");
    } else if (ret == -EAGAIN) {
        shell_error(sh, "Device not in suspended state");
    } else {
        shell_error(sh, "Failed to trigger remote wakeup: %d", ret);
    }
    
    return ret;
}

/**
 * @brief Command: usb info
 */
static int cmd_usb_info(const struct shell *sh, size_t argc, char **argv)
{
    struct usbd_context *ctx;
    
    ctx = usb_manager_get_context();
    
    shell_print(sh, "USB Manager Information:");
    shell_print(sh, "  Version: 1.0.0");
    shell_print(sh, "  Zephyr RTOS: 4.3");
    shell_print(sh, "  Context: %p", (void *)ctx);
    shell_print(sh, "  Max Callbacks: %d", USB_MANAGER_MAX_CALLBACKS);
    
    return 0;
}



/* Subcommand array for 'usb stats' */
SHELL_STATIC_SUBCMD_SET_CREATE(sub_usb_stats,
    SHELL_CMD(reset, NULL, "Reset USB statistics", cmd_usb_stats_reset),
    SHELL_SUBCMD_SET_END
);

#if defined(CONFIG_AKIRA_USB_MSC)
/**
 * @brief Command: usb msc enter
 */
static int cmd_usb_msc_enter(const struct shell *sh, size_t argc, char **argv)
{
    int ret;

    shell_print(sh, "Entering USB MSC mode (waits for SD if busy)...");

    ret = akira_usb_msc_enter();
    if (ret == 0) {
        shell_print(sh, "USB MSC mode active — SD card handed to host");
    } else if (ret == -ETIMEDOUT) {
        shell_error(sh, "Timed out waiting for SD to free up");
    } else if (ret == -ECANCELED) {
        shell_error(sh, "Cancelled");
    } else {
        shell_error(sh, "Failed to enter USB MSC mode: %d", ret);
    }

    return ret;
}

/**
 * @brief Command: usb msc exit
 */
static int cmd_usb_msc_exit(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "Exiting USB MSC mode...");
    akira_usb_msc_exit();
    shell_print(sh, "USB MSC mode exited, SD card remounted");

    return 0;
}

/**
 * @brief Command: usb msc status
 */
static int cmd_usb_msc_status(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "USB MSC Status:");
    shell_print(sh, "  Mode: %d (0=IDLE, 1=MSC, 2=HID)", (int)usb_manager_get_mode());
    shell_print(sh, "  SD owned by MSC: %s", akira_usb_msc_owns_sd() ? "Yes" : "No");

    return 0;
}

/* Subcommand array for 'usb msc' */
SHELL_STATIC_SUBCMD_SET_CREATE(sub_usb_msc,
    SHELL_CMD(enter, NULL, "Enter USB MSC mode (hand SD card to host)", cmd_usb_msc_enter),
    SHELL_CMD(exit, NULL, "Exit USB MSC mode (reclaim SD card)", cmd_usb_msc_exit),
    SHELL_CMD(status, NULL, "Show USB MSC/mode status", cmd_usb_msc_status),
    SHELL_SUBCMD_SET_END
);
#endif /* CONFIG_AKIRA_USB_MSC */

/* Main 'usb' command subcommands */
SHELL_STATIC_SUBCMD_SET_CREATE(sub_usb,
    SHELL_CMD(init, NULL, "Initialize USB manager", cmd_usb_init),
    SHELL_CMD(finalize, NULL, "Finalize USB device stack", cmd_usb_finalize),
    SHELL_CMD(deinit, NULL, "Deinitialize USB manager", cmd_usb_deinit),
    SHELL_CMD(enable, NULL, "Enable USB device", cmd_usb_enable),
    SHELL_CMD(disable, NULL, "Disable USB device", cmd_usb_disable),
    SHELL_CMD(status, NULL, "Show USB status", cmd_usb_status),
    SHELL_CMD(stats, &sub_usb_stats, "Show USB statistics", cmd_usb_stats),
    SHELL_CMD(wakeup, NULL, "Trigger remote wakeup", cmd_usb_wakeup),
    SHELL_CMD(info, NULL, "Show USB manager information", cmd_usb_info),
#if defined(CONFIG_AKIRA_USB_MSC)
    SHELL_CMD(msc, &sub_usb_msc, "USB Mass Storage commands", NULL),
#endif
    SHELL_SUBCMD_SET_END
);

/* Register main 'usb' command */
SHELL_CMD_REGISTER(usb, &sub_usb, "USB Manager commands", NULL);