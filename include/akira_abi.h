/**
 * @file akira_abi.h
 * @brief WASM application ABI version of this firmware
 *
 * A WASM app is built against the AkiraOS import interface (the "env" native
 * functions and their signatures) published by an AkiraSDK release. That
 * interface is versioned independently of the firmware version, because the SDK
 * ships separately:
 *
 * - MAJOR changes when an import is removed, renamed or re-signed. Apps built
 *   for an older major do not run.
 * - MINOR changes when imports are added. An app built for a newer minor may
 *   reference natives this firmware lacks; those calls trap.
 *
 * An app declares the ABI it targets with the manifest key "abi": "MAJOR.MINOR".
 * The runtime rejects an app whose major differs from AKIRA_WASM_ABI_VERSION_MAJOR
 * and warns when its minor is newer. An app with no "abi" key is treated as the
 * legacy ABI (1.x) and accepted with a warning, so apps built before ABI stamps
 * existed keep loading.
 *
 * Keep these constants in sync with AkiraSDK/include/akira_abi.h; the CI check
 * scripts/check_wasm_abi.py enforces it.
 *
 * @stability experimental
 * @since 1.6
 */

#ifndef AKIRA_ABI_H
#define AKIRA_ABI_H

#define AKIRA_WASM_ABI_VERSION_MAJOR 1
#define AKIRA_WASM_ABI_VERSION_MINOR 0

#endif /* AKIRA_ABI_H */
