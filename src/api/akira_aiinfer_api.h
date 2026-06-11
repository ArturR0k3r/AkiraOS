/**
 * @file akira_aiinfer_api.h
 * @brief AkiraClaw on-device ML inference native API
 *
 * Exposes aiinfer_load / aiinfer_run / aiinfer_unload to WASM apps.
 * Gated behind AKIRA_CAP_AIINFER ("ai.infer" in manifest).
 * @stability experimental
 * @since 2.0
 */

#ifndef AKIRA_AIINFER_API_H
#define AKIRA_AIINFER_API_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#else
typedef void *wasm_exec_env_t;
#endif

#define AIINFER_OK            0
#define AIINFER_ERR_NOMEM    -1
#define AIINFER_ERR_INVALID  -2
#define AIINFER_ERR_SHAPE    -3
#define AIINFER_ERR_NOSLOT   -4

/**
 * Load a TFLite Micro model into an inference slot.
 *
 * @param exec_env  WAMR execution environment
 * @param model_ptr WASM pointer to model bytes
 * @param model_len Model size in bytes
 * @return Slot handle (0 .. MAX_HANDLES-1) on success, negative AIINFER_ERR_* on failure
 */
int akira_native_aiinfer_load(wasm_exec_env_t exec_env,
                              const uint8_t *model_buf, uint32_t model_len);

/**
 * Run inference: copy input → tensor, invoke, copy output → WASM buffer.
 *
 * @param exec_env   WAMR execution environment
 * @param handle     Slot handle returned by aiinfer_load
 * @param in_buf     Native pointer to input data (converted by WAMR *~ signature)
 * @param in_len     Input data size in bytes (must match model input tensor)
 * @param out_buf    Native pointer to output buffer (converted by WAMR *~ signature)
 * @param out_len    Output buffer size (must be >= model output tensor size)
 * @return 0 on success, negative AIINFER_ERR_* on failure
 */
int akira_native_aiinfer_run(wasm_exec_env_t exec_env,
                             int handle,
                             const uint8_t *in_buf,  uint32_t in_len,
                             uint8_t       *out_buf, uint32_t out_len);

/**
 * Release an inference slot and free its arena.
 *
 * @param exec_env WAMR execution environment
 * @param handle   Slot handle to release
 */
void akira_native_aiinfer_unload(wasm_exec_env_t exec_env, int handle);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_AIINFER_API_H */
