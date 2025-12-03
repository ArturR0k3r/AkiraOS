#ifndef AKIRA_OCRE_RUNTIME_H
#define AKIRA_OCRE_RUNTIME_H

#include <ocre/api/ocre_api.h>
#include <ocre/ocre_container_runtime/ocre_container_runtime.h>
#include <stddef.h>
#include <stdint.h>

typedef ocre_container_t akira_ocre_container_t;

int ocre_runtime_init(void);
int ocre_runtime_load_app(const char *name, const void *binary, size_t size);
int ocre_runtime_start_app(const char *name);
int ocre_runtime_stop_app(const char *name);
int ocre_runtime_list_apps(akira_ocre_container_t *out_list, int max_count);
int ocre_runtime_get_status(const char *name);
int ocre_runtime_destroy_app(const char *name);

#endif
