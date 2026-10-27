/**
 * @file akira_aiinfer_api.cpp
 * @brief AkiraClaw TFLite Micro inference backend
 *
 * Each handle owns one MicroInterpreter backed by a static tensor arena.
 * Arena size: CONFIG_AKIRA_AIINFER_ARENA_KB KiB (default 128 KiB).
 * Model bytes are copied into PSRAM so the WASM input buffer can be freed
 * by the caller after aiinfer_load returns.
 */

#include "akira_aiinfer_api.h"

#ifdef CONFIG_AKIRA_AIINFER

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

#include <runtime/security.h>
#include "lib/mem_helper.h"

/* TFLite Micro headers */
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#ifndef AKIRA_AIINFER_ARENA_KB
#define AKIRA_AIINFER_ARENA_KB 128
#endif
#ifndef AKIRA_AIINFER_MAX_HANDLES
#define AKIRA_AIINFER_MAX_HANDLES 4
#endif

LOG_MODULE_REGISTER(akira_aiinfer, CONFIG_AKIRA_LOG_LEVEL);

/* -----------------------------------------------------------------------
 * Slot management
 * ---------------------------------------------------------------------- */

/* Use static storage + placement new to avoid heap operator new/delete.
 * The objects are constructed in-place and destroyed via explicit dtor. */
struct InferSlot {
    bool used;
    uint8_t *model_buf;                  /* PSRAM copy of model bytes     */
    size_t   model_len;
    uint8_t arena[AKIRA_AIINFER_ARENA_KB * 1024] __aligned(16);
    alignas(tflite::MicroMutableOpResolver<16>)
        uint8_t resolver_buf[sizeof(tflite::MicroMutableOpResolver<16>)];
    alignas(tflite::MicroInterpreter)
        uint8_t interpreter_buf[sizeof(tflite::MicroInterpreter)];
    tflite::MicroMutableOpResolver<16> *resolver;
    tflite::MicroInterpreter           *interpreter;
};

static InferSlot AKIRA_BULK_BSS g_slots[AKIRA_AIINFER_MAX_HANDLES];
static K_MUTEX_DEFINE(g_aiinfer_mutex);

/* -----------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static InferSlot *slot_get(int handle)
{
    if (handle < 0 || handle >= AKIRA_AIINFER_MAX_HANDLES)
        return nullptr;
    return &g_slots[handle];
}

static void slot_free(int handle)
{
    InferSlot *s = slot_get(handle);
    if (!s || !s->used)
        return;

    if (s->interpreter) {
        s->interpreter->~MicroInterpreter();
        s->interpreter = nullptr;
    }
    if (s->resolver) {
        s->resolver->~MicroMutableOpResolver();
        s->resolver = nullptr;
    }
    if (s->model_buf) {
        akira_free_buffer(s->model_buf);
        s->model_buf = nullptr;
    }
    s->model_len = 0;
    s->used = false;
}

/* -----------------------------------------------------------------------
 * WASM native implementations
 * ---------------------------------------------------------------------- */

extern "C"
int akira_native_aiinfer_load(wasm_exec_env_t exec_env,
                              const uint8_t *model_buf, uint32_t model_len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_AIINFER, AIINFER_ERR_INVALID);

    if (model_len == 0 || !model_buf) {
        LOG_ERR("aiinfer_load: invalid model buffer");
        return AIINFER_ERR_INVALID;
    }

    k_mutex_lock(&g_aiinfer_mutex, K_FOREVER);

    /* Find a free slot */
    int handle = -1;
    for (int i = 0; i < AKIRA_AIINFER_MAX_HANDLES; i++) {
        if (!g_slots[i].used) {
            handle = i;
            break;
        }
    }
    if (handle < 0) {
        k_mutex_unlock(&g_aiinfer_mutex);
        LOG_ERR("aiinfer_load: no free inference slots");
        return AIINFER_ERR_NOSLOT;
    }

    InferSlot *s = &g_slots[handle];

    /* Copy model to PSRAM via shared multi-heap (falls back to SRAM) */
    s->model_buf = (uint8_t *)akira_malloc_buffer(model_len);
    if (!s->model_buf) {
        k_mutex_unlock(&g_aiinfer_mutex);
        LOG_ERR("aiinfer_load: OOM allocating %u B for model", model_len);
        return AIINFER_ERR_NOMEM;
    }
    memcpy(s->model_buf, model_buf, model_len);
    s->model_len = model_len;

    /* Validate flatbuffer model */
    const tflite::Model *model = tflite::GetModel(s->model_buf);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        LOG_ERR("aiinfer_load: schema version mismatch (%u != %u)",
                (unsigned)model->version(), TFLITE_SCHEMA_VERSION);
        akira_free_buffer(s->model_buf);
        s->model_buf = nullptr;
        k_mutex_unlock(&g_aiinfer_mutex);
        return AIINFER_ERR_INVALID;
    }

    /* Build op resolver via placement new into static slot storage */
    s->resolver = new (s->resolver_buf) tflite::MicroMutableOpResolver<16>();
    s->resolver->AddDepthwiseConv2D();
    s->resolver->AddConv2D();
    s->resolver->AddFullyConnected();
    s->resolver->AddSoftmax();
    s->resolver->AddReshape();
    s->resolver->AddAveragePool2D();
    s->resolver->AddMaxPool2D();
    s->resolver->AddQuantize();
    s->resolver->AddDequantize();

    /* Create interpreter via placement new into static slot storage */
    s->interpreter = new (s->interpreter_buf) tflite::MicroInterpreter(
        model, *s->resolver, s->arena, sizeof(s->arena));

    if (s->interpreter->AllocateTensors() != kTfLiteOk) {
        LOG_ERR("aiinfer_load: AllocateTensors failed (arena too small?)");
        s->interpreter->~MicroInterpreter();  s->interpreter = nullptr;
        s->resolver->~MicroMutableOpResolver(); s->resolver   = nullptr;
        akira_free_buffer(s->model_buf);         s->model_buf  = nullptr;
        k_mutex_unlock(&g_aiinfer_mutex);
        return AIINFER_ERR_NOMEM;
    }

    s->used = true;
    k_mutex_unlock(&g_aiinfer_mutex);

    LOG_INF("aiinfer_load: slot %d loaded, model=%u B, arena=%u B",
            handle, (unsigned)model_len, (unsigned)sizeof(s->arena));
    return handle;
}

extern "C"
int akira_native_aiinfer_run(wasm_exec_env_t exec_env,
                             int handle,
                             const uint8_t *in_buf,  uint32_t in_len,
                             uint8_t       *out_buf, uint32_t out_len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_AIINFER, AIINFER_ERR_INVALID);

    InferSlot *s = slot_get(handle);
    if (!s || !s->used || !s->interpreter)
        return AIINFER_ERR_INVALID;

    if (!in_buf || !out_buf) {
        LOG_ERR("aiinfer_run: invalid buffer pointer");
        return AIINFER_ERR_INVALID;
    }

    TfLiteTensor *input = s->interpreter->input(0);
    if (!input || (uint32_t)input->bytes != in_len) {
        LOG_ERR("aiinfer_run: input size mismatch (expected %u got %u)",
                (unsigned)(input ? input->bytes : 0), (unsigned)in_len);
        return AIINFER_ERR_SHAPE;
    }

    TfLiteTensor *output = s->interpreter->output(0);
    if (!output || (uint32_t)output->bytes > out_len) {
        LOG_ERR("aiinfer_run: output buffer too small (need %u got %u)",
                (unsigned)(output ? output->bytes : 0), (unsigned)out_len);
        return AIINFER_ERR_SHAPE;
    }

    memcpy(input->data.raw, in_buf, in_len);

    if (s->interpreter->Invoke() != kTfLiteOk) {
        LOG_ERR("aiinfer_run: Invoke() failed");
        return AIINFER_ERR_INVALID;
    }

    memcpy(out_buf, output->data.raw, output->bytes);
    return AIINFER_OK;
}

extern "C"
void akira_native_aiinfer_unload(wasm_exec_env_t exec_env, int handle)
{
    AKIRA_CHECK_CAP_OR_RETURN_VOID(exec_env, AKIRA_CAP_AIINFER);

    k_mutex_lock(&g_aiinfer_mutex, K_FOREVER);
    slot_free(handle);
    k_mutex_unlock(&g_aiinfer_mutex);
    LOG_INF("aiinfer_unload: slot %d released", handle);
}

#endif /* CONFIG_AKIRA_AIINFER */
