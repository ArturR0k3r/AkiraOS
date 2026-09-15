/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * WASM app ABI and minimum-firmware-version gate.
 */

#ifndef AKIRA_ABI_CHECK_H
#define AKIRA_ABI_CHECK_H

#include <stdint.h>
#include "manifest_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Decide whether an app may load, from its manifest.
 *
 * Pure function: the firmware ABI and version are arguments so it is unit
 * testable. Rejects a major ABI mismatch and a too-high min_akiraos_version;
 * warns on a newer minor ABI or a missing ABI stamp.
 *
 * @return 0 to allow, -ENOTSUP to reject.
 */
int akira_abi_check(const akira_manifest_t *manifest, uint8_t fw_abi_major,
                    uint8_t fw_abi_minor, const uint16_t fw_version[3]);

/** As akira_abi_check(), using this firmware's ABI and version. */
int akira_abi_check_default(const akira_manifest_t *manifest);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_ABI_CHECK_H */
