/**
 * @file path_utils.h
 * @brief Shared Path Utilities - Security & Validation
 *
 * Centralized path handling to ensure consistent security checks
 * across storage, filesystem, and app management modules.
 */

#ifndef AKIRA_PATH_UTILS_H
#define AKIRA_PATH_UTILS_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Sanitize a path for security
 *
 * Checks for:
 * - NULL or empty paths
 * - Absolute paths (reject paths starting with '/')
 * - Directory traversal attempts (reject "..")
 * - Path length limits
 *
 * @param path Path to validate
 * @return 0 if safe, negative error code if unsafe:
 *         -EINVAL: NULL or empty path
 *         -EPERM: Absolute path not allowed
 *         -EACCES: Directory traversal attempt
 *         -ENAMETOOLONG: Path too long
 */
int path_sanitize(const char *path);

/**
 * @brief Check if a path is safe to use
 *
 * Similar to path_sanitize() but returns boolean.
 *
 * @param path Path to check
 * @return true if safe, false otherwise
 */
bool path_is_safe(const char *path);

/**
 * @brief Build a safe path from components
 *
 * Constructs: base/app/file
 * Example: path_build(out, 64, "/data", "myapp", "config.json")
 *          -> "/data/myapp/config.json"
 *
 * @param out Output buffer
 * @param out_len Output buffer size
 * @param base Base directory (can be NULL)
 * @param app App name (can be NULL)
 * @param file File name (can be NULL)
 * @return 0 on success, negative on error:
 *         -EINVAL: Invalid parameters
 *         -ENAMETOOLONG: Result too long
 */
int path_build(char *out, size_t out_len, const char *base,
               const char *app, const char *file);

/**
 * @brief Normalize a path (remove redundant separators, etc.)
 *
 * Converts: "//data//app///file" -> "/data/app/file"
 *
 * @param path Path to normalize (modified in-place)
 * @return 0 on success, negative on error
 */
int path_normalize(char *path);

/**
 * @brief Get the filename from a path
 *
 * "/data/apps/hello.wasm" -> "hello.wasm"
 *
 * @param path Full path
 * @return Pointer to filename within path, or NULL if no filename
 */
const char *path_get_filename(const char *path);

/**
 * @brief Get the directory from a path
 *
 * Copies directory portion into output buffer.
 * "/data/apps/hello.wasm" -> "/data/apps"
 *
 * @param path Full path
 * @param out Output buffer for directory
 * @param out_len Output buffer size
 * @return 0 on success, negative on error
 */
int path_get_directory(const char *path, char *out, size_t out_len);

/**
 * @brief Get file extension
 *
 * "hello.wasm" -> "wasm"
 * "config.json" -> "json"
 *
 * @param path File path
 * @return Pointer to extension (without dot), or NULL if no extension
 */
const char *path_get_extension(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_PATH_UTILS_H */
