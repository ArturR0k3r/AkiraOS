/**
 * @file main.c
 * @brief Hello World - Simple AkiraOS WASM App Example
 * 
 * This is a minimal example demonstrating how to create a WASM app
 * that runs on AkiraOS. It shows:
 * - Entry point definition
 * - Using the sleep API
 * - Basic structure of an AkiraOS app
 * 
 * Build with:
 *   make
 * Or manually:
 *   $WASI_SDK/bin/clang -O3 -nostdlib -Wl,--no-entry \
 *       -Wl,--export=_start -Wl,--allow-undefined \
 *       -I../include -o hello_world.wasm main.c
 */

#include "akira_api.h"

/* Global counter to track iterations */
static int counter = 0;

/**
 * @brief Application entry point
 * 
 * The entry point must be named `_start` and exported.
 * Use the AKIRA_APP_MAIN() macro for convenience.
 */
AKIRA_APP_MAIN()
{
    /* Get system information */
    ocre_utsname_t info;
    
    if (ocre_uname(&info) == 0) {
        /* System info retrieved successfully */
        /* In a real app, you might log this or display it */
    }
    
    /* Main application loop */
    while (1) {
        /* Increment our counter */
        counter++;
        
        /* Sleep for 1 second (1000 milliseconds) */
        ocre_sleep(1000);
        
        /* The app keeps running until stopped by the system
         * or until the user uninstalls it */
    }
}
