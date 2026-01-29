/**
 * @file akira_display_module.h
 * @brief Modular WASM Display API for AkiraOS (WAMR)
 *
 * Declares the registration function for the display module.
 */

#ifndef AKIRA_DISPLAY_MODULE_H
#define AKIRA_DISPLAY_MODULE_H

#ifdef __cplusplus
extern "C" {
#endif

int akira_register_display_module(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_DISPLAY_MODULE_H */
