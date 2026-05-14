/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file akira_delta.h
 * @brief Streaming binary delta (bspatch) for OTA updates.
 *
 * Applies a bsdiff-format patch on-the-fly while data arrives from the
 * HTTP transport.  The old image is read from the primary MCUboot slot
 * and the patched output is written directly to the secondary (OTA) slot.
 *
 * Memory: ~4 KB heap for the bzip2 decompressor state.
 * Stack: ≤ 512 B additional on the OTA thread.
 *
 * Gate: CONFIG_AKIRA_OTA_DELTA=y
 * @stability experimental
 * @since 1.6
 */

#ifndef AKIRA_DELTA_H
#define AKIRA_DELTA_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque delta patching context — allocated on heap by akira_delta_open(). */
typedef struct akira_delta_ctx akira_delta_ctx_t;

/**
 * @brief Open a delta patch session.
 *
 * Reads the MCUboot primary slot size to determine old-image boundaries.
 * Allocates internal bzip2 decompressor state on the system heap.
 *
 * @param[out] ctx_out  Filled with the allocated context pointer.
 * @return 0 on success, negative errno on failure.
 */
int akira_delta_open(akira_delta_ctx_t **ctx_out);

/**
 * @brief Feed a chunk of compressed patch data.
 *
 * May be called multiple times as HTTP chunks arrive.  Internally buffers
 * until a full bspatch block is available, then applies it.
 *
 * @param ctx    Context from akira_delta_open().
 * @param chunk  Pointer to patch chunk.
 * @param len    Chunk length (should match CONFIG_AKIRA_OTA_DELTA_CHUNK_SIZE).
 * @return 0 on success, negative errno on error.
 */
int akira_delta_feed(akira_delta_ctx_t *ctx, const void *chunk, size_t len);

/**
 * @brief Finalise the patch session.
 *
 * Flushes remaining decompressor output, writes the final bytes to the
 * secondary slot, and calls ota_finalize_update().
 *
 * @param ctx  Context from akira_delta_open() (freed on return).
 * @return 0 on success, negative errno on failure.
 */
int akira_delta_close(akira_delta_ctx_t *ctx);

/**
 * @brief Abort and free the delta context without finalising.
 * @param ctx  Context to free.
 */
void akira_delta_abort(akira_delta_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_DELTA_H */
