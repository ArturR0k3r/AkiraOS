/**
 * @file wasm_app_manager.c
 * @brief AkiraOS WASM App Upload/Update Manager Implementation
 */

#include "wasm_app_manager.h"
#include <ocre/api/ocre_api.h>
#include <ocre/ocre_container_runtime/ocre_container_runtime.h>

int wasm_app_upload(const char *name, const void *binary, size_t size, uint32_t version)
{
    // Delegate upload to OCRE runtime
    return ocre_container_runtime_load(name, binary, size);
}

int wasm_app_update(const char *name, const void *binary, size_t size, uint32_t version)
{
    // Delegate update to OCRE runtime (could be same as load)
    return ocre_container_runtime_load(name, binary, size);
}

int wasm_app_list(akira_ocre_container_t *out_list, int max_count)
{
    // Delegate list to OCRE runtime
    return ocre_container_runtime_list(out_list, max_count);
}
