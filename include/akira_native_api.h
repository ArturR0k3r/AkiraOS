/**
 * @file akira_native_api.h
 * @brief AkiraOS Native API Exports for WASM Applications
 *
 * Declares the interface for native functions exported to WASM,
 * available for WASM applications to call via the "akira" module.
 */

#ifndef AKIRA_NATIVE_API_H
#define AKIRA_NATIVE_API_H

#ifdef CONFIG_WAMR_ENABLE

#include "wasm_export.h"

/* ===== System APIs ===== */

/**
 * @brief Get system information
 *
 * Signature: "(*~)i"
 *
 * @param buffer Pointer to output buffer (WASM address space)
 * @param buf_len Buffer size
 * @return 0 on success, -1 on error
 *
 * Fills buffer with null-terminated system information string.
 * Example: "AkiraOS v1.3.0 (WAMR Runtime)"
 */
int sys_info(char *buffer, int buf_len);

/* ===== Logging APIs ===== */

/**
 * @brief Log debug message
 *
 * Signature: "($)v"
 *
 * Sends debug level log message from WASM app to system logs.
 *
 * Example in WASM:
 * @code
 * log_debug("Processing data...");
 * @endcode
 */
void log_debug(const char *message);

/**
 * @brief Log info message
 *
 * Signature: "($)v"
 */
void log_info(const char *message);

/**
 * @brief Log error message
 *
 * Signature: "($)v"
 */
void log_error(const char *message);

/* ===== Memory APIs ===== */

/**
 * @brief Allocate memory from WASM heap
 *
 * Signature: "(i)I"
 *
 * @param size Number of bytes to allocate
 * @return Address in WASM memory space, 0 on error
 *
 * Allocates memory that persists for the lifetime of the instance.
 * Caller must call malloc() to free.
 *
 * Example in WASM:
 * @code
 * void *ptr = malloc(256);  // Allocate 256 bytes
 * // ... use ptr ...
 * free(ptr);                // Free when done
 * @endcode
 *
 * Limits:
 * - Maximum 1 MB per allocation
 * - Limited by WAMR_INSTANCE_HEAP (64 KB default)
 */
void *malloc(int size);

/**
 * @brief Free allocated memory
 *
 * Signature: "(I)v"
 *
 * @param ptr Address returned by malloc()
 *
 * Frees memory previously allocated with malloc().
 */
void free(void *ptr);

/* ===== Time APIs ===== */

/**
 * @brief Get system uptime in milliseconds
 *
 * Signature: "()I"
 *
 * @return System uptime in milliseconds since boot
 *
 * Example in WASM:
 * @code
 * uint64_t now = get_time_ms();
 * @endcode
 */
uint64_t get_time_ms(void);

/**
 * @brief Sleep for specified duration
 *
 * Signature: "(i)v"
 *
 * @param ms Milliseconds to sleep
 *
 * Blocks current instance for specified time.
 * Maximum sleep: 1 hour (3600000 ms)
 *
 * Example in WASM:
 * @code
 * sleep_ms(500);  // Sleep for 500 ms
 * @endcode
 */
void sleep_ms(int ms);

/* ===== Display APIs ===== */

/**
 * @brief Write to display
 *
 * Signature: "(iii*~)i"
 *
 * @param x Starting X coordinate
 * @param y Starting Y coordinate
 * @param width Width in pixels
 * @param height Height in pixels
 * @param buffer Pointer to pixel buffer (WASM address space)
 * @param size Size of buffer in bytes
 * @return 0 on success, -1 on error
 *
 * Implementation depends on HAL display driver.
 * This is a stub in current build.
 *
 * Example in WASM:
 * @code
 * uint16_t pixels[240] = {...};  // 240 16-bit color values
 * display_write(0, 0, 240, 1, pixels, sizeof(pixels));
 * @endcode
 */
int display_write(int x, int y, int width, int height,
                  const void *buffer, int size);

/* ===== File I/O APIs ===== */

/**
 * @brief Read file
 *
 * Signature: "($*~)i"
 *
 * @param filename Path to file (string)
 * @param buffer Pointer to output buffer (WASM address space)
 * @param buf_len Buffer size
 * @return Bytes read, -1 on error
 *
 * This is a stub in current build.
 *
 * Example in WASM:
 * @code
 * char buffer[512];
 * int bytes = file_read("/lfs/data.txt", buffer, sizeof(buffer));
 * @endcode
 */
int file_read(const char *filename, void *buffer, int buf_len);

/**
 * @brief Write file
 *
 * Signature: "($*~)i"
 *
 * @param filename Path to file (string)
 * @param buffer Pointer to data to write (WASM address space)
 * @param buf_len Data size in bytes
 * @return Bytes written, -1 on error
 *
 * This is a stub in current build.
 *
 * Example in WASM:
 * @code
 * const char *data = "Hello from WASM!";
 * int bytes = file_write("/lfs/output.txt", data, strlen(data));
 * @endcode
 */
int file_write(const char *filename, const void *buffer, int buf_len);

/* ===== Registration ===== */

/**
 * @brief Register AkiraOS native module with WAMR
 *
 * Called during WAMR initialization to export AkiraOS native functions.
 * This must be called before loading WASM modules.
 *
 * @return 0 on success, -1 on error
 *
 * @internal Used by akira_runtime_init()
 */
int akira_register_native_apis(void);

/**
 * @brief Get native symbols array
 *
 * @param out_count Pointer to receive number of symbols
 * @return Pointer to NativeSymbol array, or NULL if not initialized
 *
 * @internal Used by WAMR initialization
 */
NativeSymbol *akira_get_native_symbols(int *out_count);

#else  /* CONFIG_WAMR_ENABLE not defined */

/* Stub functions when WAMR is disabled */
int akira_register_native_apis(void);
NativeSymbol *akira_get_native_symbols(int *out_count);

#endif /* CONFIG_WAMR_ENABLE */

#endif /* AKIRA_NATIVE_API_H */
