/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * WASM app ABI and minimum-firmware-version gate (akira_abi.h).
 */

#include "akira_abi_check.h"

#include <akira_abi.h>
#include <akira.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(akira_abi, CONFIG_AKIRA_LOG_LEVEL);

/* The firmware must know its own version to enforce min_akiraos_version. tests/
 * do not link the akira_os interface target, so AKIRA_VERSION_* is 0 there and
 * the check is skipped in unit tests. */
BUILD_ASSERT(AKIRA_VERSION_MAJOR > 0 || IS_ENABLED(CONFIG_ZTEST),
             "AKIRA_VERSION_MAJOR is 0: firmware built without a VERSION file");

int akira_abi_check(const akira_manifest_t *manifest, uint8_t fw_abi_major,
                    uint8_t fw_abi_minor, const uint16_t fw_version[3])
{
    if (!manifest || !manifest->valid) {
        return 0; /* No manifest to check; capability clamp still applies. */
    }

    if (manifest->has_abi) {
        if (manifest->abi_major != fw_abi_major) {
            LOG_ERR("App targets WASM ABI %u.%u; this firmware provides %u.%u — refusing",
                    manifest->abi_major, manifest->abi_minor, fw_abi_major, fw_abi_minor);
            return -ENOTSUP;
        }
        if (manifest->abi_minor > fw_abi_minor) {
            LOG_WRN("App targets WASM ABI %u.%u, newer than this firmware's %u.%u; "
                    "imports added after %u.%u will trap if called",
                    manifest->abi_major, manifest->abi_minor, fw_abi_major, fw_abi_minor,
                    fw_abi_major, fw_abi_minor);
        }
    } else {
        LOG_WRN("App declares no WASM ABI version; assuming legacy 1.x");
    }

    if (manifest->has_min_os && fw_version && fw_version[0] > 0) {
        for (int i = 0; i < 3; i++) {
            if (manifest->min_os[i] != fw_version[i]) {
                if (manifest->min_os[i] > fw_version[i]) {
                    LOG_ERR("App needs AkiraOS >= %u.%u.%u; this firmware is %u.%u.%u — refusing",
                            manifest->min_os[0], manifest->min_os[1], manifest->min_os[2],
                            fw_version[0], fw_version[1], fw_version[2]);
                    return -ENOTSUP;
                }
                break; /* firmware is newer at this component: satisfied */
            }
        }
    }

    return 0;
}

int akira_abi_check_default(const akira_manifest_t *manifest)
{
    const uint16_t fw_version[3] = {
        AKIRA_VERSION_MAJOR, AKIRA_VERSION_MINOR, AKIRA_VERSION_PATCH,
    };

    return akira_abi_check(manifest, AKIRA_WASM_ABI_VERSION_MAJOR,
                           AKIRA_WASM_ABI_VERSION_MINOR, fw_version);
}
