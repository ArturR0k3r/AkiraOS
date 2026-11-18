#include "akira_python_api.h"
#include "akira_common_api.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <wasm_export.h>

LOG_MODULE_REGISTER(akira_python, CONFIG_AKIRA_LOG_LEVEL);

/* ── setjmp / longjmp ────────────────────────────────────────────────────────
 * MicroPython is compiled with Emscripten SUPPORT_LONGJMP=none, which leaves
 * setjmp/longjmp as env-module imports.  setjmp always returns 0 (first-call
 * semantics); longjmp is a no-op.  Python exceptions that unwind through C
 * frames will trap rather than recover, but exception-free scripts run fine.
 */
static int py_setjmp(wasm_exec_env_t exec_env, void *jmp_buf)
{
    (void)exec_env;
    (void)jmp_buf;
    return 0;
}

static void py_longjmp(wasm_exec_env_t exec_env, void *jmp_buf, int val)
{
    (void)exec_env;
    (void)jmp_buf;
    (void)val;
}

/* ── WASI fd_* ───────────────────────────────────────────────────────────────
 * Emscripten's libc routes Python print() through fd_write on fd 1/2.
 * We forward those writes to the rate-limited app console; all other fds are
 * discarded.  ciovec layout: [buf_app_offset: u32][buf_len: u32] × iovs_len
 *
 * These are reachable by every WASI app with no capability, and a bare "*" in
 * a native signature makes WAMR validate only the first byte, so the iovec
 * array, each buffer it points at and the result slot are checked here for
 * their full length, and every call is bounded.
 */
typedef struct { uint32_t buf; uint32_t buf_len; } wasi_ciovec_t;

/* WASI errno values (wasi_snapshot_preview1) */
#define WASI_ESUCCESS 0
#define WASI_EFAULT   21
#define WASI_EINVAL   28

#define PY_MAX_IOVS   16   /* iovecs consumed per call */
#define PY_MAX_WRITE  4096 /* bytes consumed per call; libc writes the rest next */

static bool py_out_valid(wasm_module_inst_t inst, void *p, uint32_t size)
{
    return p == NULL || wasm_runtime_validate_native_addr(inst, p, size);
}

static int py_fd_write(wasm_exec_env_t exec_env, int fd,
                       const wasi_ciovec_t *iovs, int iovs_len,
                       uint32_t *nwritten)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    uint32_t total = 0;

    if (!inst || iovs_len < 0) {
        return WASI_EINVAL;
    }
    /* A short write: libc writes whatever is left in its next call. */
    int n = MIN(iovs_len, PY_MAX_IOVS);

    if (!py_out_valid(inst, nwritten, sizeof(*nwritten)) ||
        (n > 0 &&
         (!iovs || !wasm_runtime_validate_native_addr(inst, (void *)iovs,
                                                      (uint32_t)n * sizeof(wasi_ciovec_t))))) {
        return WASI_EFAULT;
    }

    for (int i = 0; i < n && total < PY_MAX_WRITE; i++) {
        wasi_ciovec_t iov;

        /* One read of each entry (app memory, possibly unaligned). */
        memcpy(&iov, &iovs[i], sizeof(iov));
        uint32_t len = MIN(iov.buf_len, PY_MAX_WRITE - total);
        if (len == 0) {
            continue;
        }
        if (!wasm_runtime_validate_app_addr(inst, iov.buf, len)) {
            return WASI_EFAULT;
        }
        if (fd == 1 || fd == 2) {
            const char *buf = wasm_runtime_addr_app_to_native(inst, iov.buf);
            if (buf) {
                akira_app_console_write(exec_env, buf, len);
            }
        }
        total += len;
    }

    /* Dropped (rate-limited or non-console) output still counts as written,
     * so the app makes progress instead of retrying forever. */
    if (nwritten) {
        memcpy(nwritten, &total, sizeof(total));
    }
    return WASI_ESUCCESS;
}

static int py_fd_close(wasm_exec_env_t exec_env, int fd)
{
    (void)exec_env; (void)fd;
    return 0;
}

static int py_fd_sync(wasm_exec_env_t exec_env, int fd)
{
    (void)exec_env; (void)fd;
    return 0;
}

static int py_fd_seek(wasm_exec_env_t exec_env, int fd,
                      int64_t offset, int whence, int64_t *newoffset)
{
    (void)fd; (void)offset; (void)whence;
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    const int64_t zero = 0;

    if (!inst || !py_out_valid(inst, newoffset, sizeof(*newoffset))) {
        return WASI_EFAULT;
    }
    if (newoffset) {
        memcpy(newoffset, &zero, sizeof(zero)); /* may be unaligned */
    }
    return WASI_ESUCCESS;
}

static int py_fd_read(wasm_exec_env_t exec_env, int fd,
                      const wasi_ciovec_t *iovs, int iovs_len, uint32_t *nread)
{
    (void)fd; (void)iovs; /* nothing is ever read: the iovecs are not touched */
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    const uint32_t zero = 0;

    if (!inst || iovs_len < 0) {
        return WASI_EINVAL;
    }
    if (!py_out_valid(inst, nread, sizeof(*nread))) {
        return WASI_EFAULT;
    }
    if (nread) {
        memcpy(nread, &zero, sizeof(zero)); /* may be unaligned */
    }
    return WASI_ESUCCESS;
}

/* ── Registration ────────────────────────────────────────────────────────── */

#ifdef CONFIG_AKIRA_WASM_API
#include <akira_native_registry.h>

static const NativeSymbol python_env_natives[] = {
    {"setjmp",  (void *)py_setjmp,  "(*)i", NULL},
    {"longjmp", (void *)py_longjmp, "(*i)",  NULL},
};
AKIRA_NATIVE_API_DEFINE(akira_python_env_api, "env", python_env_natives);

static const NativeSymbol python_wasi_natives[] = {
    {"fd_write", (void *)py_fd_write, "(i*i*)i", NULL},
    {"fd_close", (void *)py_fd_close, "(i)i",    NULL},
    {"fd_sync",  (void *)py_fd_sync,  "(i)i",    NULL},
    {"fd_seek",  (void *)py_fd_seek,  "(iIi*)i", NULL},
    {"fd_read",  (void *)py_fd_read,  "(i*i*)i", NULL},
};
AKIRA_NATIVE_API_DEFINE(akira_python_wasi_api, "wasi_snapshot_preview1", python_wasi_natives);
#endif

/* Deprecated since 1.6: the runtime registers these tables through the native
 * API registry. Kept so existing callers keep linking. */
bool akira_python_register_natives(void)
{
    return true;
}
