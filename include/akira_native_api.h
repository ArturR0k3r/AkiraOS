/**
 * @file akira_native_api.h
 * @brief AkiraOS Native API Exports for WASM Applications
 *
 * Declares the interface for native functions exported to WASM,
 * available for WASM applications to call via the "akira" module.
 * @stability stable
 * @since 1.4
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
 * Example: "AkiraOS v1.5.8 (WAMR Runtime)"
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

/* ===== Input APIs =====
 *
 * There is currently no button input native API. The runtime registers no
 * input_* symbol (see src/api/akira_export_api.c), so nothing is declared here.
 * AKIRA_CAP_INPUT_READ / AKIRA_CAP_INPUT_WRITE remain defined and reserved.
 *
 * Apps needing button state should use the LVGL input path
 * (CONFIG_AKIRA_LVGL_INPUT_ZEPHYR), which bridges Zephyr input events into the
 * display driver.
 */

/* ===== GPIO APIs ===== */

/**
 * @brief GPIO configuration flags
 *
 * These flags can be combined using bitwise OR to configure GPIO pins.
 */
#define AKIRA_GPIO_INPUT (1U << 0)            /* Configure as input */
#define AKIRA_GPIO_OUTPUT (1U << 1)           /* Configure as output */
#define AKIRA_GPIO_OUTPUT_INIT_LOW (1U << 2)  /* Initialize output to low */
#define AKIRA_GPIO_OUTPUT_INIT_HIGH (1U << 3) /* Initialize output to high */
#define AKIRA_GPIO_PULL_UP (1U << 4)          /* Enable internal pull-up resistor */
#define AKIRA_GPIO_PULL_DOWN (1U << 5)        /* Enable internal pull-down resistor */
#define AKIRA_GPIO_ACTIVE_LOW (1U << 6)       /* Active-low (inverted logic) */
#define AKIRA_GPIO_ACTIVE_HIGH (1U << 7)      /* Active-high (normal logic) */

/**
 * @brief Configure a GPIO pin
 *
 * Signature: "(ii)i"
 *
 * @param pin GPIO pin number (platform-defined range, e.g. 0–48 on ESP32-S3)
 * @param flags Configuration flags (AKIRA_GPIO_* constants)
 * @return 0 on success, negative error code on failure
 *
 * Requires: AKIRA_CAP_GPIO_READ for input, AKIRA_CAP_GPIO_WRITE for output
 *
 * Example in WASM:
 * @code
 * // Configure GPIO 2 as output, initially low
 * gpio_configure(2, AKIRA_GPIO_OUTPUT | AKIRA_GPIO_OUTPUT_INIT_LOW);
 *
 * // Configure GPIO 4 as input with pull-up
 * gpio_configure(4, AKIRA_GPIO_INPUT | AKIRA_GPIO_PULL_UP);
 * @endcode
 */
int gpio_configure(int pin, int flags);

/**
 * @brief Read a GPIO pin value
 *
 * Signature: "(i)i"
 *
 * @param pin GPIO pin number to read
 * @return 1 if high, 0 if low, negative error code on failure
 *
 * Requires: AKIRA_CAP_GPIO_READ capability
 *
 * Example in WASM:
 * @code
 * int value = gpio_read(4);
 * if (value == 1) {
 *     // Pin is high
 * }
 * @endcode
 */
int gpio_read(int pin);

/**
 * @brief Write a GPIO pin value
 *
 * Signature: "(ii)i"
 *
 * @param pin GPIO pin number to write
 * @param value Output value (0 for low, non-zero for high)
 * @return 0 on success, negative error code on failure
 *
 * Requires: AKIRA_CAP_GPIO_WRITE capability
 *
 * Example in WASM:
 * @code
 * gpio_write(2, 1);  // Set GPIO 2 to high
 * gpio_write(2, 0);  // Set GPIO 2 to low
 * @endcode
 */
int gpio_write(int pin, int value);

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

#else /* CONFIG_WAMR_ENABLE not defined */

/* Stub functions when WAMR is disabled */
int akira_register_native_apis(void);
NativeSymbol *akira_get_native_symbols(int *out_count);

#endif /* CONFIG_WAMR_ENABLE */

#endif /* AKIRA_NATIVE_API_H */
