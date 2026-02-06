/**
 * @file akira_bt_shell_api.c
 * @brief BT Shell API implementation for WASM exports
 */

#include "akira_api.h"
#include "akira_bt_shell_api.h"
#include <runtime/security.h>
#include <zephyr/logging/log.h>
#include "runtime/security.h"


#if defined(CONFIG_AKIRA_BT_SHELL)
#include "bt_shell.h"
#endif

LOG_MODULE_REGISTER(akira_bt_shell_api, LOG_LEVEL_INF);

/* Core BT Shell API functions (no security checks) */

int akira_bt_shell_send(const char *message)
{
    if (!message)
    {
        return -EINVAL;
    }

#if defined(CONFIG_AKIRA_BT_SHELL)
    return bt_shell_send_command(message);
#else
    (void)message;
    return -ENOSYS;
#endif
}

int akira_bt_shell_send_data(const uint8_t *data, size_t len)
{
    if (!data || len == 0)
    {
        return -EINVAL;
    }

#if defined(CONFIG_AKIRA_BT_SHELL)
    return bt_shell_send_data(data, len);
#else
    (void)data;
    (void)len;
    return -ENOSYS;
#endif
}

int akira_bt_shell_is_ready(void)
{
#if defined(CONFIG_AKIRA_BT_SHELL)
    return bt_shell_notifications_enabled() ? 1 : 0;
#else
    return 0;
#endif
}

/* WASM Native export API */

int akira_native_bt_shell_send(wasm_exec_env_t exec_env, const char *message)
{
    /* Use inline capability check for <60ns overhead */
    uint32_t cap_mask = akira_security_get_cap_mask(exec_env);
    AKIRA_CHECK_CAP_OR_RETURN(cap_mask, AKIRA_CAP_BT_SHELL, -EPERM);

    return akira_bt_shell_send(message);
}

int akira_native_bt_shell_send_data(wasm_exec_env_t exec_env, uint32_t data_ptr, uint32_t len)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst)
        return -1;

    /* Use inline capability check for <60ns overhead */
    uint32_t cap_mask = akira_security_get_cap_mask(exec_env);
    AKIRA_CHECK_CAP_OR_RETURN(cap_mask, AKIRA_CAP_BT_SHELL, -EPERM);

    if (len == 0)
        return -EINVAL;

    uint8_t *ptr = (uint8_t *)wasm_runtime_addr_app_to_native(module_inst, data_ptr);
    if (!ptr)
        return -EFAULT;

    return akira_bt_shell_send_data(ptr, len);
}

int akira_native_bt_shell_is_ready(wasm_exec_env_t exec_env)
{
    /* Use inline capability check for <60ns overhead */
    uint32_t cap_mask = akira_security_get_cap_mask(exec_env);
    AKIRA_CHECK_CAP_OR_RETURN(cap_mask, AKIRA_CAP_BT_SHELL, -EPERM);

    return akira_bt_shell_is_ready();
}