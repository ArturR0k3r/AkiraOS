/**
 * @file akpkg.h
 * @brief .akpkg archive decompressor
 *
 * An .akpkg is a gzip-compressed tar archive containing:
 *   manifest.json  — application metadata
 *   app.wasm       — compiled WebAssembly binary
 *   sig.ed25519    — optional Ed25519 signature (64 bytes)
 *   sig.dilithium2 — optional Dilithium-2 signature (2420 bytes, AkiraPlatform PQC)
 *
 * Provides gzip magic detection, in-memory DEFLATE decompression (RFC 1951),
 * and tar entry extraction with no external compression library dependencies.
 *
 * @stability experimental
 * @since 1.6
 */

#ifndef AKIRA_AKPKG_H
#define AKIRA_AKPKG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Check whether a buffer starts with a gzip magic header.
 *
 * @param data  Buffer to inspect.
 * @param len   Buffer length.
 * @return true if the buffer is a gzip stream, false otherwise.
 */
bool akpkg_is_gzip(const uint8_t *data, size_t len);

/**
 * @brief Decompress a gzip stream into a caller-supplied buffer.
 *
 * Parses the gzip header (RFC 1952) and runs raw DEFLATE (RFC 1951) on
 * the payload.  The uncompressed size is read from the ISIZE field at the
 * end of the gzip stream and must fit within @p out_cap.
 *
 * @param gz        Gzip-compressed input data.
 * @param gz_len    Input length in bytes.
 * @param out       Output buffer (must be at least ISIZE bytes).
 * @param out_cap   Output buffer capacity.
 * @return Number of decompressed bytes on success, negative errno on error.
 */
int32_t akpkg_inflate(const uint8_t *gz, size_t gz_len,
                      uint8_t *out, size_t out_cap);

/**
 * @brief Locate app.wasm, manifest.json, and optional entries inside a tar archive.
 *
 * Walks a POSIX tar image (must be fully in memory) and sets output pointers
 * directly into @p tar — no copies are made.  app.wasm and manifest.json must
 * be present for the function to succeed.  model.tflite and the PQC signature
 * entries are optional; pass NULL for the pointer/size pairs to ignore them.
 *
 * @param tar              Decompressed tar data.
 * @param tar_len          Length of @p tar.
 * @param wasm_ptr         Out: pointer to app.wasm data inside @p tar.
 * @param wasm_size        Out: app.wasm size in bytes.
 * @param manifest_ptr     Out: pointer to manifest.json data inside @p tar.
 * @param manifest_size    Out: manifest.json size in bytes.
 * @param model_ptr        Out: pointer to model.tflite data (NULL if absent or not requested).
 * @param model_size       Out: model.tflite size in bytes (0 if absent or not requested).
 * @param dilithium2_ptr   Out: pointer to sig.dilithium2 data (NULL if absent or not requested).
 * @param dilithium2_size  Out: sig.dilithium2 size in bytes (0 if absent or not requested).
 * @return 0 on success, -ENOENT if app.wasm or manifest.json is missing.
 */
int akpkg_tar_extract(const uint8_t *tar, size_t tar_len,
                      const uint8_t **wasm_ptr,        size_t *wasm_size,
                      const char    **manifest_ptr,    size_t *manifest_size,
                      const uint8_t **model_ptr,       size_t *model_size,
                      const uint8_t **dilithium2_ptr,  size_t *dilithium2_size);

/**
 * @brief Decode a base64-encoded string into raw bytes (RFC 4648).
 *
 * Whitespace is skipped; decoding stops at '=' padding or any non-alphabet
 * character.  No allocation is performed — the caller supplies the output
 * buffer.  A safe upper bound for the output size is @p src_len * 3 / 4 + 4.
 *
 * @param src     Base64 input (need not be null-terminated).
 * @param src_len Input length in bytes.
 * @param out     Caller-supplied output buffer.
 * @param out_cap Output buffer capacity in bytes.
 * @return Number of decoded bytes written to @p out, or 0 on buffer overflow.
 */
size_t akpkg_base64_decode(const char *src, size_t src_len,
                           uint8_t *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_AKPKG_H */
