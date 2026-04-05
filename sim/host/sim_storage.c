/*
 * AkiraConsole Simulator — Storage host (SD card simulation)
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "sim_storage.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

#define MAX_FDS     16
#define MAX_PATH   256
#define ROOT_DEFAULT "./sdcard"

static char g_root[MAX_PATH] = ROOT_DEFAULT;

/* File descriptor table: index → POSIX fd (-1 = free) */
static int g_fds[MAX_FDS];

static void fds_init(void) __attribute__((constructor));
static void fds_init(void)
{
    for (int i = 0; i < MAX_FDS; i++) g_fds[i] = -1;
}

void sim_storage_set_root(const char *path)
{
    strncpy(g_root, path, sizeof(g_root) - 1);
}

static void make_path(char *out, int out_sz, const char *rel)
{
    /* Strip leading slash from rel path to prevent absolute path escape */
    while (*rel == '/') rel++;
    snprintf(out, out_sz, "%s/%s", g_root, rel);
}

static int alloc_fd(int posix_fd)
{
    for (int i = 0; i < MAX_FDS; i++) {
        if (g_fds[i] == -1) { g_fds[i] = posix_fd; return i; }
    }
    return -1;
}

int sim_storage_open(const char *path, int flags)
{
    char full[MAX_PATH];
    make_path(full, sizeof(full), path);

    /* Ensure parent directories exist */
    char tmp[MAX_PATH];
    strncpy(tmp, full, sizeof(tmp));
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }

    int oflags = O_RDWR | O_CREAT;
    if (flags & 0x1) oflags |= O_TRUNC;  /* write mode — truncate */

    int pfd = open(full, oflags, 0644);
    if (pfd < 0) return -errno;

    int slot = alloc_fd(pfd);
    if (slot < 0) { close(pfd); return -EMFILE; }
    return slot;
}

int sim_storage_read(int fd, uint8_t *buf, int len)
{
    if (fd < 0 || fd >= MAX_FDS || g_fds[fd] < 0) return -EBADF;
    int n = (int)read(g_fds[fd], buf, (size_t)len);
    return n < 0 ? -errno : n;
}

int sim_storage_write(int fd, const uint8_t *buf, int len)
{
    if (fd < 0 || fd >= MAX_FDS || g_fds[fd] < 0) return -EBADF;
    int n = (int)write(g_fds[fd], buf, (size_t)len);
    return n < 0 ? -errno : n;
}

void sim_storage_close(int fd)
{
    if (fd < 0 || fd >= MAX_FDS || g_fds[fd] < 0) return;
    close(g_fds[fd]);
    g_fds[fd] = -1;
}

int sim_storage_delete(const char *path)
{
    char full[MAX_PATH];
    make_path(full, sizeof(full), path);
    return (unlink(full) == 0) ? 0 : -errno;
}

int sim_storage_list(const char *dir, char *out, int out_len)
{
    char full[MAX_PATH];
    make_path(full, sizeof(full), dir);

    /* Ensure directory exists */
    mkdir(full, 0755);

    DIR *d = opendir(full);
    if (!d) return -errno;

    int written = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        int n = snprintf(out + written, out_len - written,
                         "%s\n", ent->d_name);
        if (n <= 0 || written + n >= out_len) break;
        written += n;
    }
    closedir(d);
    return written;
}
