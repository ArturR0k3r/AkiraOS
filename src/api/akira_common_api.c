#include "akira_api.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <runtime/security.h>

LOG_MODULE_REGISTER(akira_common_api, CONFIG_AKIRA_LOG_LEVEL);

#ifdef CONFIG_AKIRA_WASM_RUNTIME
int akira_native_printf(wasm_exec_env_t exec_env, char *message)
{
    if (!message) {
        return -EINVAL;
    }

    LOG_INF("%s", message);
    return 0;
}

int akira_native_delay(wasm_exec_env_t exec_env, uint32_t microseconds)
{
    /* Note: delay() does not require capability checks - basic timing should always be allowed */
    k_usleep(microseconds);
    return 0;
}

#endif

/* ===== WASM exports =====
 * Registered with WAMR by the native API registry (akira_native_registry.h).
 * Import names and signatures are WASM ABI: see docs/api-stability-policy.md. */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && (defined(CONFIG_AKIRA_WASM_API))
#include <akira_native_registry.h>

static const NativeSymbol akira_common_natives[] = {
    {"printf_native", (void *)akira_native_printf, "($)i", NULL},
    {"delay", (void *)akira_native_delay, "(i)i", NULL},
};

AKIRA_NATIVE_API_DEFINE(akira_common_api, "env", akira_common_natives);
#endif
