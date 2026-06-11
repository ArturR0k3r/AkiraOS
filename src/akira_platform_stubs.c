/**
 * @file akira_platform_stubs.c
 * @brief Weak no-op stubs for AkiraPlatform extension hooks.
 *
 * AkiraPlatform replaces these with strong
 * implementations at link time. When building without a platform overlay
 * these no-ops are used so the rest of AkiraOS compiles and links cleanly.
 *
 * Declarations live in akira.h.
 *
 * @copyright Copyright (c) 2026 PenEngineering S.R.L
 */

#include <zephyr/kernel.h>
#include "akira_platform_stubs.h"

__weak void akira_on_app_installed(const char *name, int id, const char *version)
{
	ARG_UNUSED(name);
	ARG_UNUSED(id);
	ARG_UNUSED(version);
}

__weak void akira_on_app_uninstalled(const char *name)
{
	ARG_UNUSED(name);
}

__weak void akira_on_app_started(const char *name, int id)
{
	ARG_UNUSED(name);
	ARG_UNUSED(id);
}

__weak void akira_on_app_crashed(const char *name, int exit_code)
{
	ARG_UNUSED(name);
	ARG_UNUSED(exit_code);
}

/* WAMR calls __stdout_hook_install() from bh_platform_init() to install a
 * custom printf hook into newlib.  When building native_sim with
 * CONFIG_EXTERNAL_LIBC=y (host glibc) the symbol does not exist.
 * Provide a no-op weak stub so the linker is satisfied. */
#ifdef CONFIG_EXTERNAL_LIBC
__attribute__((weak)) void __stdout_hook_install(int (*hook)(int))
{
	(void)hook;
}
#endif
