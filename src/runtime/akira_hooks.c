/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * System event dispatch (akira_hooks.h).
 */

#include <akira_hooks.h>
#include <stddef.h>

void akira_hooks_emit(const struct akira_hook_event *event)
{
    if (!event) {
        return;
    }

    STRUCT_SECTION_FOREACH(akira_hook, hook) {
        if (hook->handler && (hook->event_mask & BIT64(event->type))) {
            hook->handler(event, hook->user_data);
        }
    }
}

void akira_hooks_emit_app(enum akira_hook_event_type type, const char *name,
                          int registry_id, int container_id, const char *version,
                          int exit_code)
{
    struct akira_hook_event event = {
        .type = type,
        .app = {
            .name = name,
            .registry_id = registry_id,
            .container_id = container_id,
            .version = version ? version : "",
            .exit_code = exit_code,
        },
    };

    akira_hooks_emit(&event);
}
