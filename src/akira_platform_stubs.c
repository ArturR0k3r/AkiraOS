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
#include <akira_hooks.h>

/* Compatibility shim: forward app-lifecycle hooks to the legacy weak
 * akira_on_app_* symbols that AkiraPlatform (>= 1.5.4) overrides. app_manager
 * emits events instead of calling these directly, so this one handler keeps the
 * old contract: same thread as the emitter, container id on start, crash only
 * on a non-zero exit. Deprecated: new code should AKIRA_HOOK_DEFINE() its own. */
static void akira_platform_app_hook(const struct akira_hook_event *e, void *user)
{
	ARG_UNUSED(user);
	switch (e->type) {
	case AKIRA_HOOK_APP_INSTALLED:
		akira_on_app_installed(e->app.name, e->app.registry_id, e->app.version);
		break;
	case AKIRA_HOOK_APP_UNINSTALLED:
		akira_on_app_uninstalled(e->app.name);
		break;
	case AKIRA_HOOK_APP_STARTED:
		akira_on_app_started(e->app.name, e->app.container_id);
		break;
	case AKIRA_HOOK_APP_CRASHED:
		akira_on_app_crashed(e->app.name, e->app.exit_code);
		break;
	default:
		break;
	}
}

AKIRA_HOOK_DEFINE(akira_platform_compat_hook,
		  AKIRA_HOOK_MASK(AKIRA_HOOK_APP_INSTALLED) |
			  AKIRA_HOOK_MASK(AKIRA_HOOK_APP_UNINSTALLED) |
			  AKIRA_HOOK_MASK(AKIRA_HOOK_APP_STARTED) |
			  AKIRA_HOOK_MASK(AKIRA_HOOK_APP_CRASHED),
		  akira_platform_app_hook, NULL);

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
