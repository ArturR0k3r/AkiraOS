#pragma once

#include <stdbool.h>

/**
 * Register Emscripten/WASI native symbols required by MicroPython WASM apps.
 *
 * Registers to two WAMR modules:
 *   "env"                   — setjmp, longjmp (Emscripten SUPPORT_LONGJMP=none stubs)
 *   "wasi_snapshot_preview1"— fd_write (→ rate-limited app console), fd_close, fd_sync, fd_seek, fd_read
 *
 * Called from akira_register_native_apis() when CONFIG_AKIRA_WASM_PYTHON=y.
 */
bool akira_python_register_natives(void);
