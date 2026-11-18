/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_uart
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_uart, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_uart_api.c
 * @brief Full-duplex UART API with IRQ-driven RX ring buffer
 *
 * UART0 (zephyr_console) is reserved for shell/logging.
 * port_id 0 maps to the first secondary UART (uart1 in DT).
 *
 * Containment: a port is owned by the app slot that opened it; only that
 * app can use or close it, and it is closed (RX IRQ off, callback cleared,
 * line settings restored) when the app ends, however it ends.  A UART the
 * system uses — the console, shell, Bluetooth HCI or mcumgr UART, or one
 * with a device node on it — is never handed to an app.
 */

#include "akira_uart_api.h"
#include <runtime/security.h>
#include <lib/mem_helper.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <string.h>

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <akira_app_containment.h>

#ifndef CONFIG_AKIRA_WASM_UART_RX_BUF_SIZE
#define CONFIG_AKIRA_WASM_UART_RX_BUF_SIZE 256
#endif

/* Maximum number of simultaneous UART handles (one per secondary UART) */
#define AKIRA_UART_MAX_HANDLES 2

/* Hard limits for app-chosen parameters.  The baud range keeps the driver's
 * divider arithmetic sane (0 or a huge rate divides by zero in the ESP32
 * HAL).  uart_write() polls bytes out on the calling thread, so one call is
 * capped both in bytes and in wire time (~UART_WRITE_MAX_MS at the port's
 * baud) to keep it bounded; callers loop on the returned count. */
#define UART_BAUD_MIN        1200
#define UART_BAUD_MAX        3000000
#define UART_WRITE_MAX_BYTES 4096u
#define UART_WRITE_MAX_MS    250u
#define UART_STOP_CHECK_MASK 63u  /* look for a stop request every 64 bytes */

/* -------------------------------------------------------------------------- */
/* Handle table                                                                */
/* -------------------------------------------------------------------------- */

struct akira_uart_handle {
    bool               open;
    bool               cfg_saved;   /* saved_cfg holds the pre-open settings */
    const struct device *dev;
    int                owner_slot;  /* app slot (akira_runtime_slot_of_exec_env) */
    uint32_t           baud;
    struct uart_config saved_cfg;
    struct ring_buf     rx_rb;
    uint8_t             rx_buf[CONFIG_AKIRA_WASM_UART_RX_BUF_SIZE];
};

/* PSRAM on boards that have it: the RX rings are the bulk of this. */
static struct akira_uart_handle AKIRA_BULK_BSS s_handles[AKIRA_UART_MAX_HANDLES];
/* Serialises open/close/release on the handle table (flags only — never held
 * across a driver call that can block). */
static struct k_spinlock s_uart_lock;

/*
 * Secondary UARTs available to WASM — extend array as needed.
 * uart0 is intentionally excluded (console/shell).
 */
static const char *const s_uart_names[] = {
    "uart1",
    "uart2",
};
BUILD_ASSERT(ARRAY_SIZE(s_uart_names) >= AKIRA_UART_MAX_HANDLES,
             "s_uart_names must cover AKIRA_UART_MAX_HANDLES entries");

/* A UART the system uses, whatever its label: chosen for the console, shell,
 * Bluetooth HCI, mcumgr, uart-pipe or OpenThread, or carrying a device node
 * (modem, GNSS, HCI controller ...).  Compile-time, from the devicetree. */
#define UART_CHOSEN_IS(node, prop) \
    COND_CODE_1(DT_HAS_CHOSEN(prop), (DT_SAME_NODE(node, DT_CHOSEN(prop))), (0))
#define UART_NODE_IS_SYSTEM(node)                                                         \
    (UART_CHOSEN_IS(node, zephyr_console) || UART_CHOSEN_IS(node, zephyr_shell_uart) ||   \
     UART_CHOSEN_IS(node, zephyr_bt_uart) || UART_CHOSEN_IS(node, zephyr_bt_c2h_uart) ||  \
     UART_CHOSEN_IS(node, zephyr_uart_mcumgr) || UART_CHOSEN_IS(node, zephyr_uart_pipe) || \
     UART_CHOSEN_IS(node, zephyr_ot_uart) || (DT_CHILD_NUM_STATUS_OKAY(node) > 0))
#define UART_LABEL_IS_SYSTEM(label)                                                       \
    COND_CODE_1(DT_NODE_EXISTS(DT_NODELABEL(label)),                                      \
                (UART_NODE_IS_SYSTEM(DT_NODELABEL(label))), (0))

static const bool s_uart_system[] = {
    UART_LABEL_IS_SYSTEM(uart1),
    UART_LABEL_IS_SYSTEM(uart2),
};
BUILD_ASSERT(ARRAY_SIZE(s_uart_system) == ARRAY_SIZE(s_uart_names),
             "s_uart_system must match s_uart_names");

/* -------------------------------------------------------------------------- */
/* IRQ callback                                                                */
/* -------------------------------------------------------------------------- */

/* Writes only into the handle's own ring (never app memory). */
static void uart_irq_cb(const struct device *dev, void *user_data)
{
    struct akira_uart_handle *h = (struct akira_uart_handle *)user_data;

    if (!uart_irq_update(dev)) {
        return;
    }
    while (uart_irq_rx_ready(dev)) {
        uint8_t byte;
        int n = uart_fifo_read(dev, &byte, 1);
        if (n != 1) {
            break;
        }
        /* Drop byte silently if ring buffer is full */
        ring_buf_put(&h->rx_rb, &byte, 1);
    }
}

/* -------------------------------------------------------------------------- */
/* Init                                                                        */
/* -------------------------------------------------------------------------- */

int akira_uart_api_init(void)
{
    memset(s_handles, 0, sizeof(s_handles));
    return 0;
}

SYS_INIT(akira_uart_api_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* -------------------------------------------------------------------------- */
/* Ownership helpers                                                           */
/* -------------------------------------------------------------------------- */

/* Detach the IRQ path, put the line settings back, then free the handle.
 * Called by the owner's own close or by the release hook, which never runs
 * concurrently with the owner's natives.  The handle stays marked open
 * until the end, so another app cannot open the port mid-teardown and have
 * its settings overwritten by the restore. */
static void uart_handle_teardown(struct akira_uart_handle *h)
{
    const struct device *dev = h->dev;
    k_spinlock_key_t key = k_spin_lock(&s_uart_lock);

    if (dev) {
        /* Register writes only: safe under the spinlock. */
        uart_irq_rx_disable(dev);
        uart_irq_callback_user_data_set(dev, NULL, NULL);
    }
    k_spin_unlock(&s_uart_lock, key);

    if (dev && h->cfg_saved) {
        (void)uart_configure(dev, &h->saved_cfg);
    }

    key = k_spin_lock(&s_uart_lock);
    ring_buf_reset(&h->rx_rb);
    memset(h, 0, sizeof(*h));
    k_spin_unlock(&s_uart_lock, key);
}

/* Handle owned by the calling app, or NULL (with *err set). */
static struct akira_uart_handle *uart_handle_of(wasm_exec_env_t exec_env, int32_t handle,
                                                int *err)
{
    if (handle < 0 || handle >= AKIRA_UART_MAX_HANDLES) {
        *err = -EINVAL;
        return NULL;
    }
    struct akira_uart_handle *h = &s_handles[handle];
    if (!h->open) {
        *err = -EBADF;
        return NULL;
    }
    if (h->owner_slot != akira_runtime_slot_of_exec_env(exec_env)) {
        *err = -EPERM;
        return NULL;
    }
    return h;
}

/* Release hook: close every port the app left open. Non-blocking. */
static void uart_release(int slot, const char *app_name)
{
    ARG_UNUSED(app_name);

    if (slot < 0) {
        return;
    }
    for (int i = 0; i < AKIRA_UART_MAX_HANDLES; i++) {
        struct akira_uart_handle *h = &s_handles[i];

        if (h->open && h->owner_slot == slot) {
            uart_handle_teardown(h);
            LOG_DBG("uart: closed handle %d left open by app slot %d", i, slot);
        }
    }
}

AKIRA_APP_RESOURCE_DEFINE(uart_ports, 50, NULL, uart_release);

/* -------------------------------------------------------------------------- */
/* WASM native exports                                                         */
/* -------------------------------------------------------------------------- */

int akira_native_uart_open(wasm_exec_env_t exec_env,
                            int32_t port_id, int32_t baud_rate)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_UART, -EPERM);

    if (port_id < 0 || port_id >= AKIRA_UART_MAX_HANDLES) {
        LOG_DBG("uart_open: invalid port_id=%d", port_id);
        return -EINVAL;
    }
    if (baud_rate < UART_BAUD_MIN || baud_rate > UART_BAUD_MAX) {
        LOG_DBG("uart_open: invalid baud_rate=%d", baud_rate);
        return -EINVAL;
    }
    if (s_uart_system[port_id]) {
        LOG_DBG("uart_open: %s is used by the system, refused", s_uart_names[port_id]);
        return -EACCES;
    }
    int slot = akira_runtime_slot_of_exec_env(exec_env);
    if (slot < 0) {
        return -EPERM;
    }

    const struct device *dev = device_get_by_dt_nodelabel(s_uart_names[port_id]);
    if (!dev || !device_is_ready(dev)) {
        LOG_DBG("uart_open: device %s not ready", s_uart_names[port_id]);
        return -ENODEV;
    }

    /* Claim the slot before touching the device so two opens can't race. */
    struct akira_uart_handle *h = &s_handles[port_id];
    k_spinlock_key_t key = k_spin_lock(&s_uart_lock);
    if (h->open) {
        k_spin_unlock(&s_uart_lock, key);
        LOG_DBG("uart_open: port %d already open", port_id);
        return -EBUSY;
    }
    memset(h, 0, sizeof(*h));
    h->open = true;
    h->owner_slot = slot;
    h->dev = dev;   /* set now: a teardown after an abort mid-open still detaches */
    k_spin_unlock(&s_uart_lock, key);

    /* Keep the board's settings to restore on close. */
    h->cfg_saved = (uart_config_get(dev, &h->saved_cfg) == 0);

    struct uart_config cfg = {
        .baudrate  = (uint32_t)baud_rate,
        .parity    = UART_CFG_PARITY_NONE,
        .stop_bits = UART_CFG_STOP_BITS_1,
        .data_bits = UART_CFG_DATA_BITS_8,
        .flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
    };
    int ret = uart_configure(dev, &cfg);
    if (ret < 0) {
        LOG_DBG("uart_open: configure failed: %d", ret);
        if (h->cfg_saved) {
            (void)uart_configure(dev, &h->saved_cfg);
        }
        key = k_spin_lock(&s_uart_lock);
        memset(h, 0, sizeof(*h));
        k_spin_unlock(&s_uart_lock, key);
        return ret;
    }

    ring_buf_init(&h->rx_rb, sizeof(h->rx_buf), h->rx_buf);
    h->baud = (uint32_t)baud_rate;

    uart_irq_callback_user_data_set(dev, uart_irq_cb, h);
    uart_irq_rx_enable(dev);

    LOG_DBG("uart_open: port_id=%d baud=%d handle=%d", port_id, baud_rate, port_id);
    return port_id; /* handle == port_id */
}

/* Signature "(i*~)i": WAMR validated buf..buf+len.  Returns bytes written,
 * which may be fewer than len: one call is capped at UART_WRITE_MAX_BYTES and
 * ~UART_WRITE_MAX_MS of wire time at the port's baud, and ends early once the
 * app is being stopped — callers loop.  uart_poll_out() takes no lock, so no
 * critical section is needed. */
int akira_native_uart_write(wasm_exec_env_t exec_env, int32_t handle,
                             const uint8_t *buf, uint32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_UART, -EPERM);

    if (!buf || len == 0) {
        return -EINVAL;
    }
    int err = 0;
    struct akira_uart_handle *h = uart_handle_of(exec_env, handle, &err);
    if (!h) {
        return err;
    }

    /* 10 bits per byte on the wire (8N1). */
    uint32_t max_bytes = MAX(16u, (h->baud / 10u) * UART_WRITE_MAX_MS / 1000u);
    uint32_t n = MIN(len, MIN(max_bytes, UART_WRITE_MAX_BYTES));
    const struct device *dev = h->dev;

    for (uint32_t i = 0; i < n; i++) {
        if ((i & UART_STOP_CHECK_MASK) == UART_STOP_CHECK_MASK &&
            akira_runtime_stop_requested(exec_env)) {
            return (int)i;
        }
        uart_poll_out(dev, buf[i]);
    }
    return (int)n;
}

/* Signature "(i*~)i": WAMR validated buf..buf+max_len.  Never blocks: copies
 * what the RX ring holds (0 = nothing yet; the app polls). */
int akira_native_uart_read(wasm_exec_env_t exec_env, int32_t handle,
                            uint8_t *buf, uint32_t max_len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_UART, -EPERM);

    if (!buf || max_len == 0) {
        return -EINVAL;
    }
    int err = 0;
    struct akira_uart_handle *h = uart_handle_of(exec_env, handle, &err);
    if (!h) {
        return err;
    }

    uint32_t got = ring_buf_get(&h->rx_rb, buf, max_len);
    return (int)got; /* 0 = no data available; caller polls with delay() */
}

int akira_native_uart_close(wasm_exec_env_t exec_env, int32_t handle)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_UART, -EPERM);

    int err = 0;
    struct akira_uart_handle *h = uart_handle_of(exec_env, handle, &err);
    if (!h) {
        return err;
    }

    uart_handle_teardown(h);

    LOG_DBG("uart_close: handle=%d", handle);
    return 0;
}

#else /* !CONFIG_AKIRA_WASM_RUNTIME */

int akira_uart_api_init(void) { return 0; }

#endif /* CONFIG_AKIRA_WASM_RUNTIME */

/* ===== WASM exports =====
 * Registered with WAMR by the native API registry (akira_native_registry.h).
 * Import names and signatures are WASM ABI: see docs/api-stability-policy.md. */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && (defined(CONFIG_AKIRA_WASM_UART))
#include <akira_native_registry.h>

static const NativeSymbol akira_uart_natives[] = {
    {"uart_open", (void *)akira_native_uart_open, "(ii)i", NULL},
    {"uart_write", (void *)akira_native_uart_write, "(i*~)i", NULL},
    {"uart_read", (void *)akira_native_uart_read, "(i*~)i", NULL},
    {"uart_close", (void *)akira_native_uart_close, "(i)i", NULL},
};

AKIRA_NATIVE_API_DEFINE(akira_uart_api, "env", akira_uart_natives);
#endif
