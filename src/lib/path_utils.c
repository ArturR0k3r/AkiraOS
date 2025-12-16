/**
 * @file path_utils.c
 * @brief Shared Path Utilities Implementation
 */

#include "path_utils.h"
#include <string.h>
#include <errno.h>
#include <ctype.h>

/* Configuration */
#ifndef PATH_MAX_LEN
#define PATH_MAX_LEN 256
#endif

/* ===== Path Sanitization ===== */

int path_sanitize(const char *path)
{
    /* Check for NULL or empty */
    if (!path || path[0] == '\0') {
        return -EINVAL;
    }

    /* Reject absolute paths (should use relative paths for isolation) */
    if (path[0] == '/') {
        return -EPERM;
    }

    /* Check for directory traversal */
    if (strstr(path, "..") != NULL) {
        return -EACCES;
    }

    /* Check length */
    size_t len = strlen(path);
    if (len >= PATH_MAX_LEN) {
        return -ENAMETOOLONG;
    }

    /* Additional checks for suspicious patterns */
    /* Reject paths with null bytes in the middle */
    for (size_t i = 0; i < len; i++) {
        if (path[i] == '\0') {
            return -EINVAL;
        }
    }

    return 0;
}

bool path_is_safe(const char *path)
{
    return path_sanitize(path) == 0;
}

/* ===== Path Building ===== */

int path_build(char *out, size_t out_len, const char *base,
               const char *app, const char *file)
{
    if (!out || out_len == 0) {
        return -EINVAL;
    }

    /* Start with empty string */
    out[0] = '\0';
    size_t pos = 0;

    /* Add base if provided */
    if (base && base[0]) {
        size_t len = strlen(base);
        if (pos + len >= out_len) {
            return -ENAMETOOLONG;
        }
        strcpy(out, base);
        pos += len;

        /* Ensure trailing slash */
        if (out[pos - 1] != '/') {
            if (pos + 1 >= out_len) {
                return -ENAMETOOLONG;
            }
            out[pos++] = '/';
            out[pos] = '\0';
        }
    }

    /* Add app if provided */
    if (app && app[0]) {
        size_t len = strlen(app);
        if (pos + len >= out_len) {
            return -ENAMETOOLONG;
        }
        strcat(out, app);
        pos += len;

        /* Add separator if file follows */
        if (file && file[0]) {
            if (pos + 1 >= out_len) {
                return -ENAMETOOLONG;
            }
            out[pos++] = '/';
            out[pos] = '\0';
        }
    }

    /* Add file if provided */
    if (file && file[0]) {
        size_t len = strlen(file);
        if (pos + len >= out_len) {
            return -ENAMETOOLONG;
        }
        strcat(out, file);
        pos += len;
    }

    return 0;
}

/* ===== Path Normalization ===== */

int path_normalize(char *path)
{
    if (!path) {
        return -EINVAL;
    }

    size_t len = strlen(path);
    if (len == 0) {
        return 0;
    }

    /* Remove redundant slashes */
    char *src = path;
    char *dst = path;
    bool last_was_slash = false;

    while (*src) {
        if (*src == '/') {
            if (!last_was_slash) {
                *dst++ = *src;
                last_was_slash = true;
            }
        } else {
            *dst++ = *src;
            last_was_slash = false;
        }
        src++;
    }
    *dst = '\0';

    /* Remove trailing slash (unless it's root "/") */
    len = strlen(path);
    if (len > 1 && path[len - 1] == '/') {
        path[len - 1] = '\0';
    }

    return 0;
}

/* ===== Path Parsing ===== */

const char *path_get_filename(const char *path)
{
    if (!path) {
        return NULL;
    }

    /* Find last slash */
    const char *last_slash = strrchr(path, '/');
    if (last_slash) {
        return last_slash + 1;
    }

    /* No slash - entire path is filename */
    return path;
}

int path_get_directory(const char *path, char *out, size_t out_len)
{
    if (!path || !out || out_len == 0) {
        return -EINVAL;
    }

    /* Find last slash */
    const char *last_slash = strrchr(path, '/');
    if (!last_slash) {
        /* No directory - return empty or "." */
        out[0] = '.';
        out[1] = '\0';
        return 0;
    }

    /* Copy directory part */
    size_t dir_len = last_slash - path;
    if (dir_len >= out_len) {
        return -ENAMETOOLONG;
    }

    strncpy(out, path, dir_len);
    out[dir_len] = '\0';

    return 0;
}

const char *path_get_extension(const char *path)
{
    if (!path) {
        return NULL;
    }

    /* Get filename first */
    const char *filename = path_get_filename(path);
    if (!filename) {
        return NULL;
    }

    /* Find last dot */
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename) {
        /* No extension or dot at start (hidden file) */
        return NULL;
    }

    /* Return extension (without dot) */
    return dot + 1;
}
