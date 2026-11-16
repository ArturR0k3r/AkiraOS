/**
 * @file akira_os.h
 * @brief Entry point for product firmware that uses AkiraOS as a module
 *
 * @code
 * #include <akira_os.h>
 *
 * int main(void)
 * {
 *     return akira_start();
 * }
 * @endcode
 *
 * @stability stable
 * @since 1.6 (header since 1.7; previously declared in src/akira.h)
 */

#ifndef AKIRA_OS_H
#define AKIRA_OS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run the standard AkiraOS boot sequence and idle loop.
 *
 * Initialises the HAL, connectivity, the WASM runtime and the app manager,
 * raises AKIRA_HOOK_BOOT_PRE_RUNTIME and AKIRA_HOOK_BOOT_READY, then idles.
 * Does not return under normal operation.
 *
 * @return negative errno if initialisation failed.
 */
int akira_start(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_OS_H */
