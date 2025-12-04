/*
 * @copyright Copyright © contributors to Project Ocre,
 * which has been established as Project Ocre a Series of LF Projects, LLC
 * SPDX-License-Identifier: Apache-2.0
 * 
 * Modified for AkiraOS - Hello World WASM App Example
 * 
 * This is a minimal example demonstrating how to create a WASM app
 * that runs on AkiraOS using the Ocre Runtime.
 */

#include <stdio.h>

// Application entry point
int main()
{
    printf("\n\
     _    _    _           ___  ____  \n\
    / \\  | | _(_)_ __ __ _/ _ \\/ ___| \n\
   / _ \\ | |/ / | '__/ _` | | | \\___ \\ \n\
  / ___ \\|   <| | | | (_| | |_| |___) |\n\
 /_/   \\_\\_|\\_\\_|_|  \\__,_|\\___/|____/ \n\
                                       \n\
      Hello from WASM App!             \n\
      Running on AkiraOS v1.2.1        \n\
                      powered by Ocre  \n");
    
    return 0;
}
