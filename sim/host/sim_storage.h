/*
 * AkiraConsole Simulator — Storage host (SD card simulation)
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Maps storage_open/read/write/close/delete/list to regular POSIX file ops
 * inside a configurable root directory (default: ./sdcard/).
 */

#ifndef SIM_STORAGE_H
#define SIM_STORAGE_H

#include <stdint.h>

/** Set the root directory used as the simulated SD card. Default: "./sdcard" */
void sim_storage_set_root(const char *path);

/* Mirror of the firmware storage_* API, minus the wasm_exec_env_t wrapper */
int  sim_storage_open(const char *path, int flags);
int  sim_storage_read(int fd, uint8_t *buf, int len);
int  sim_storage_write(int fd, const uint8_t *buf, int len);
void sim_storage_close(int fd);
int  sim_storage_delete(const char *path);
int  sim_storage_list(const char *dir, char *out, int out_len);

#endif /* SIM_STORAGE_H */
