/**
 * @file akira_native_registry.h
 * @brief Registry of native functions exported to WASM apps
 *
 * Each table of WAMR NativeSymbol entries is registered with
 * AKIRA_NATIVE_API_DEFINE() next to the functions it exports. At start-up the
 * runtime registers every table with WAMR. Product firmware adds natives the
 * same way, from its own sources:
 *
 * @code
 * static const NativeSymbol acme_natives[] = {
 *     {"acme_valve_open", (void *)acme_native_valve_open, "(i)i", NULL},
 * };
 * AKIRA_NATIVE_API_DEFINE(acme_api, "env", acme_natives);
 * @endcode
 *
 * Rules:
 * - Every native checks the capability it needs with AKIRA_CHECK_CAP_OR_RETURN()
 *   (runtime/security.h); the table carries no permissions.
 * - A (module, name) pair defined twice stops the runtime from starting. WAMR
 *   itself would silently let one definition shadow the other.
 * - Tables may be const: the runtime copies them into writable memory, because
 *   WAMR sorts each registered array in place and keeps the pointer.
 * - Place definitions in sources built into the application or a
 *   zephyr_library(); those are linked whole-archive, so the entry is kept.
 * - WASM apps import natives by module name (usually "env"), function name and
 *   signature. Renaming or re-signing a native breaks apps: see the WASM ABI
 *   policy in docs/api-stability-policy.md.
 *
 * @stability experimental
 * @since 1.6
 */

#ifndef AKIRA_NATIVE_REGISTRY_H
#define AKIRA_NATIVE_REGISTRY_H

#include <stdint.h>
#include <wasm_export.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Registration failure is logged as a warning and does not stop the runtime. */
#define AKIRA_NATIVE_API_OPTIONAL BIT(0)

/** One registered table. Create entries with AKIRA_NATIVE_API_DEFINE(). */
struct akira_native_api {
    /** Descriptor id, used in diagnostics. */
    const char *name;
    /** WASM import module name, e.g. "env". */
    const char *module;
    const NativeSymbol *symbols;
    uint32_t count;
    uint32_t flags;
};

/**
 * @brief Register a table of natives with flags.
 *
 * @param id            C identifier for the descriptor, unique across the image.
 * @param module_name   WASM import module name (string literal).
 * @param symbols_array Array of NativeSymbol (may be const).
 * @param api_flags     0 or AKIRA_NATIVE_API_OPTIONAL.
 */
#define AKIRA_NATIVE_API_DEFINE_FLAGS(id, module_name, symbols_array, api_flags)   \
    const STRUCT_SECTION_ITERABLE(akira_native_api, id) = {                        \
        STRINGIFY(id), module_name, symbols_array,                                 \
        (uint32_t)ARRAY_SIZE(symbols_array), (uint32_t)(api_flags)                 \
    }

/**
 * @brief Register a table of natives.
 *
 * @param id            C identifier for the descriptor, unique across the image.
 * @param module_name   WASM import module name (string literal), usually "env".
 * @param symbols_array Array of NativeSymbol (may be const).
 */
#define AKIRA_NATIVE_API_DEFINE(id, module_name, symbols_array)                    \
    AKIRA_NATIVE_API_DEFINE_FLAGS(id, module_name, symbols_array, 0)

/**
 * @brief Register every table in the registry with WAMR.
 *
 * Called by akira_runtime_init() after wasm_runtime_full_init(). Runs once;
 * later calls return the first result.
 *
 * @return 0 on success, -EEXIST if a (module, name) pair is defined twice,
 *         -ENOMEM if the writable copy cannot be allocated, -EIO if WAMR
 *         rejects a required table.
 */
int akira_native_registry_register_all(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_NATIVE_REGISTRY_H */
