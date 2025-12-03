#ifndef AKIRA_WASM_APP_MANAGER_H
#define AKIRA_WASM_APP_MANAGER_H

#include <stddef.h>
#include <stdint.h>
#include <ocre/ocre_container_runtime/ocre_container_runtime.h>

typedef ocre_container_t akira_ocre_container_t;

int wasm_app_upload(const char *name, const void *binary, size_t size, uint32_t version);
int wasm_app_update(const char *name, const void *binary, size_t size, uint32_t version);
int wasm_app_list(akira_ocre_container_t *out_list, int max_count);
int wasm_app_start(const char *name);
int wasm_app_stop(const char *name);
int wasm_app_delete(const char *name);

#endif
