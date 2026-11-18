/**
 * @file akira_aiinfer_api.cpp
 * @brief AkiraClaw TFLite Micro inference backend
 *
 * Each handle owns one MicroInterpreter backed by a static tensor arena.
 * Arena size: CONFIG_AKIRA_AIINFER_ARENA_KB KiB (default 128 KiB).
 * Model bytes are copied into PSRAM so the WASM input buffer can be freed
 * by the caller after aiinfer_load returns.
 *
 * Containment (akira_app_containment.h): a handle belongs to the app slot
 * that loaded it; no other app can run or unload it, and aiinfer_release()
 * frees it however the app ends.  The table mutex is only held for
 * bookkeeping, never while a model is verified, built or invoked, so an app
 * stopped (or crashing) in the middle of TFLM work holds no lock: the slot
 * is reclaimed by the release hook.  Inference time cannot be bounded from
 * here (TFLM has no cancel), but a stop still ends a long Invoke() by
 * aborting the app thread.
 */

#include "akira_aiinfer_api.h"

#ifdef CONFIG_AKIRA_AIINFER

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

#include <akira_app_containment.h>
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

/* Largest model an app may load.  The copy is PSRAM-only on PSRAM builds:
 * an app-sized allocation must never fall back to internal DRAM, which the
 * BT/Wi-Fi controllers and WASM thread stacks depend on. */
#ifdef CONFIG_AKIRA_PSRAM
#define AKIRA_AIINFER_MAX_MODEL_BYTES (1024u * 1024u)
#else
#define AKIRA_AIINFER_MAX_MODEL_BYTES (256u * 1024u)
#endif

LOG_MODULE_REGISTER(akira_aiinfer, CONFIG_AKIRA_LOG_LEVEL);

/* -----------------------------------------------------------------------
 * Slot management
 * ---------------------------------------------------------------------- */

/* Use static storage + placement new to avoid heap operator new/delete.
 * The objects are constructed in-place and destroyed via explicit dtor. */
struct InferSlot {
    bool used;                           /* loaded and ready              */
    bool loading;                        /* claimed by aiinfer_load       */
    uint8_t owner1;                      /* owning app slot + 1, 0 = none */
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

/* Free a slot.  Caller holds g_aiinfer_mutex, or owns the slot exclusively
 * (a claimed-but-loading slot).  @p run_dtors is false for a slot whose
 * load was cut short: the interpreter may be half-built, and its destructor
 * would walk node state that was never initialised.  Nothing in it needs
 * freeing (it lives in the slot and the arena). */
static void slot_free(InferSlot *s, bool run_dtors)
{
    if (s->interpreter) {
        if (run_dtors) {
            s->interpreter->~MicroInterpreter();
        }
        s->interpreter = nullptr;
    }
    if (s->resolver) {
        if (run_dtors) {
            s->resolver->~MicroMutableOpResolver();
        }
        s->resolver = nullptr;
    }
    if (s->model_buf) {
        akira_free_buffer(s->model_buf);
        s->model_buf = nullptr;
    }
    s->model_len = 0;
    s->owner1 = 0;
    s->loading = false;
    s->used = false;
}

static int owner_slot(wasm_exec_env_t exec_env)
{
    int slot = akira_runtime_slot_of_exec_env(exec_env);

    return (slot >= 0 && slot < UINT8_MAX) ? slot : -1;
}

/* Loaded slot @p handle if the calling app owns it.  Under the mutex. */
static InferSlot *slot_owned(int handle, int owner)
{
    InferSlot *s = slot_get(handle);

    if (!s || !s->used || !s->interpreter || owner < 0 ||
        s->owner1 != (uint8_t)(owner + 1)) {
        return nullptr;
    }
    return s;
}

/* Build the interpreter for the model already copied into @p s.  Runs
 * without the mutex: only the claiming app touches a loading slot. */
static int slot_build(InferSlot *s)
{
    /* The model is untrusted app data: run the flatbuffer verifier over the
     * whole buffer before the interpreter follows a single offset in it. */
    flatbuffers::Verifier verifier(s->model_buf, s->model_len);
    if (!tflite::VerifyModelBuffer(verifier)) {
        LOG_WRN_RATELIMIT("aiinfer_load: model failed flatbuffer verification");
        return AIINFER_ERR_INVALID;
    }

    const tflite::Model *model = tflite::GetModel(s->model_buf);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        LOG_WRN_RATELIMIT("aiinfer_load: schema version mismatch (%u != %u)",
                          (unsigned)model->version(), TFLITE_SCHEMA_VERSION);
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
        LOG_WRN_RATELIMIT("aiinfer_load: AllocateTensors failed (arena too small?)");
        return AIINFER_ERR_NOMEM;
    }
    return AIINFER_OK;
}

/* -----------------------------------------------------------------------
 * WASM native implementations
 * ---------------------------------------------------------------------- */

extern "C"
int akira_native_aiinfer_load(wasm_exec_env_t exec_env,
                              const uint8_t *model_buf, uint32_t model_len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_AIINFER, AIINFER_ERR_INVALID);

    /* "*~" validated [model_buf, model_buf + model_len). */
    if (model_len == 0 || !model_buf) {
        return AIINFER_ERR_INVALID;
    }
    if (model_len > AKIRA_AIINFER_MAX_MODEL_BYTES) {
        LOG_WRN_RATELIMIT("aiinfer_load: model too large (%u > %u B)",
                          (unsigned)model_len, AKIRA_AIINFER_MAX_MODEL_BYTES);
        return AIINFER_ERR_NOMEM;
    }
    int owner = owner_slot(exec_env);
    if (owner < 0) {
        return AIINFER_ERR_INVALID;
    }

    /* Claim a free slot for this app. */
    int handle = -1;
    akira_native_critical_enter(exec_env);
    k_mutex_lock(&g_aiinfer_mutex, K_FOREVER);
    for (int i = 0; i < AKIRA_AIINFER_MAX_HANDLES; i++) {
        if (!g_slots[i].used && !g_slots[i].loading) {
            handle = i;
            g_slots[i].loading = true;
            g_slots[i].owner1 = (uint8_t)(owner + 1);
            break;
        }
    }
    k_mutex_unlock(&g_aiinfer_mutex);
    akira_native_critical_exit(exec_env);
    if (handle < 0) {
        LOG_WRN_RATELIMIT("aiinfer_load: no free inference slots");
        return AIINFER_ERR_NOSLOT;
    }

    InferSlot *s = &g_slots[handle];
    int ret = AIINFER_OK;

    /* Copy the model out of the app's memory (PSRAM only on PSRAM builds). */
    mem_source_t src = MEM_SOURCE_UNKNOWN;
    s->model_buf = (uint8_t *)akira_malloc_buffer_ex(model_len, &src);
#ifdef CONFIG_AKIRA_PSRAM
    if (s->model_buf && src != MEM_SOURCE_PSRAM) {
        akira_free_buffer(s->model_buf);
        s->model_buf = nullptr;
    }
#else
    ARG_UNUSED(src);
#endif
    if (!s->model_buf) {
        LOG_WRN_RATELIMIT("aiinfer_load: OOM allocating %u B for model",
                          (unsigned)model_len);
        ret = AIINFER_ERR_NOMEM;
    } else {
        memcpy(s->model_buf, model_buf, model_len);
        s->model_len = model_len;
        ret = slot_build(s);
    }

    akira_native_critical_enter(exec_env);
    k_mutex_lock(&g_aiinfer_mutex, K_FOREVER);
    if (ret == AIINFER_OK) {
        s->loading = false;
        s->used = true;
    } else {
        /* Built far enough to run its destructors? Only if AllocateTensors
         * returned; otherwise nothing was constructed past the verifier. */
        slot_free(s, true);
    }
    k_mutex_unlock(&g_aiinfer_mutex);
    akira_native_critical_exit(exec_env);

    if (ret != AIINFER_OK) {
        return ret;
    }
    LOG_INF_RATELIMIT("aiinfer_load: slot %d loaded, model=%u B, arena=%u B",
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

    /* "*~" validated both buffers for their lengths. */
    if (!in_buf || !out_buf) {
        return AIINFER_ERR_INVALID;
    }

    akira_native_critical_enter(exec_env);
    k_mutex_lock(&g_aiinfer_mutex, K_FOREVER);
    InferSlot *s = slot_owned(handle, owner_slot(exec_env));
    k_mutex_unlock(&g_aiinfer_mutex);
    akira_native_critical_exit(exec_env);
    if (!s) {
        return AIINFER_ERR_INVALID;
    }

    /* From here no lock is held: only this app can unload the slot (and it
     * is busy here), and the release hook runs only once it is gone. */
    TfLiteTensor *input = s->interpreter->input(0);
    if (!input || !input->data.raw || (uint32_t)input->bytes != in_len) {
        LOG_WRN_RATELIMIT("aiinfer_run: input size mismatch (expected %u got %u)",
                          (unsigned)(input ? input->bytes : 0), (unsigned)in_len);
        return AIINFER_ERR_SHAPE;
    }

    TfLiteTensor *output = s->interpreter->output(0);
    if (!output || !output->data.raw || (uint32_t)output->bytes > out_len) {
        LOG_WRN_RATELIMIT("aiinfer_run: output buffer too small (need %u got %u)",
                          (unsigned)(output ? output->bytes : 0), (unsigned)out_len);
        return AIINFER_ERR_SHAPE;
    }

    memcpy(input->data.raw, in_buf, in_len);

    if (s->interpreter->Invoke() != kTfLiteOk) {
        LOG_WRN_RATELIMIT("aiinfer_run: Invoke() failed");
        return AIINFER_ERR_INVALID;
    }

    memcpy(out_buf, output->data.raw, output->bytes);
    return AIINFER_OK;
}

extern "C"
void akira_native_aiinfer_unload(wasm_exec_env_t exec_env, int handle)
{
    AKIRA_CHECK_CAP_OR_RETURN_VOID(exec_env, AKIRA_CAP_AIINFER);

    bool freed = false;

    akira_native_critical_enter(exec_env);
    k_mutex_lock(&g_aiinfer_mutex, K_FOREVER);
    InferSlot *s = slot_owned(handle, owner_slot(exec_env));
    if (s) {   /* only the app's own models */
        slot_free(s, true);
        freed = true;
    }
    k_mutex_unlock(&g_aiinfer_mutex);
    akira_native_critical_exit(exec_env);
    if (freed) {
        LOG_DBG("aiinfer_unload: slot %d released", handle);
    }
}

/* -----------------------------------------------------------------------
 * App teardown
 * ---------------------------------------------------------------------- */

/* Free every model slot app @p slot loaded, including one it died while
 * loading.  The app thread is finished or dead, so its slots are idle. */
static void aiinfer_release(int slot, const char *app_name)
{
    ARG_UNUSED(app_name);

    if (slot < 0 || slot >= UINT8_MAX) {
        return;
    }
    /* Held only for bookkeeping, inside critical sections: a timeout here
     * means something is badly wrong; leak the slots rather than hang. */
    if (k_mutex_lock(&g_aiinfer_mutex, K_MSEC(1000)) != 0) {
        LOG_ERR("aiinfer: table busy, slots of app slot %d not freed", slot);
        return;
    }
    for (int i = 0; i < AKIRA_AIINFER_MAX_HANDLES; i++) {
        InferSlot *s = &g_slots[i];

        if ((s->used || s->loading) && s->owner1 == (uint8_t)(slot + 1)) {
            slot_free(s, s->used);
            LOG_INF("aiinfer: freed model slot %d left by app slot %d", i, slot);
        }
    }
    k_mutex_unlock(&g_aiinfer_mutex);
}

AKIRA_APP_RESOURCE_DEFINE(aiinfer, 50, NULL, aiinfer_release);

#endif /* CONFIG_AKIRA_AIINFER */
