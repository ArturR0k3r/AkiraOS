#ifndef AKIRA_RUNTIME_LOADER_H
#define AKIRA_RUNTIME_LOADER_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*app_loader_provider_cb_t)(const uint8_t *chunk, size_t len, bool final, void *ctx);

int app_loader_init(void);
int app_loader_install_memory(const char *name, const void *binary, size_t size);
int app_loader_install_with_manifest(const char *name, const void *binary, size_t size, const char *manifest_json, size_t manifest_size);
int app_loader_register_provider(app_loader_provider_cb_t cb, void *ctx);
int app_loader_receive_chunk(const uint8_t *chunk, size_t len, bool final);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_RUNTIME_LOADER_H */