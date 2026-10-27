/**
 * @file akira_wasm_mem.h
 * @brief Shared WASM pointer validation for native API implementations.
 * @stability stable
 *
 * Native APIs that take a pointer + length from a WASM app must validate the
 * whole [ptr, ptr+len) range against the app's linear memory before touching
 * it — validating only the base byte lets an app pass a length that runs off
 * the end of the sandbox (OOB read/write). This helper centralises the
 * validate-then-translate idiom so every call site does it the same, correct
 * way (previously duplicated in akira_net_api.c and akira_ble_api.c).
 */
#ifndef AKIRA_WASM_MEM_H
#define AKIRA_WASM_MEM_H

#ifdef CONFIG_AKIRA_WASM_RUNTIME

#include <stddef.h>
#include <stdint.h>
#include <wasm_export.h>

/**
 * @brief Validate a WASM app pointer+length and return its native address.
 *
 * @param env  WAMR execution environment for the calling app.
 * @param ptr  App-relative (linear memory) offset.
 * @param len  Number of bytes the caller intends to access from @p ptr.
 * @return Native pointer on success; NULL if @p env is invalid or the range
 *         [ptr, ptr+len) is not entirely inside the app's linear memory.
 */
static inline void *akira_wasm_ptr_to_native(wasm_exec_env_t env,
					     uint32_t ptr, uint32_t len)
{
	wasm_module_inst_t mi = wasm_runtime_get_module_inst(env);

	if (!mi) {
		return NULL;
	}
	if (!wasm_runtime_validate_app_addr(mi, ptr, len)) {
		return NULL;
	}
	return wasm_runtime_addr_app_to_native(mi, ptr);
}

#endif /* CONFIG_AKIRA_WASM_RUNTIME */
#endif /* AKIRA_WASM_MEM_H */
