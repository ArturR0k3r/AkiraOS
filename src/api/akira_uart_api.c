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
 */

#include "akira_uart_api.h"
#include <runtime/security.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <errno.h>
#include <string.h>

#ifdef CONFIG_AKIRA_WASM_RUNTIME

#ifndef CONFIG_AKIRA_WASM_UART_RX_BUF_SIZE
#define CONFIG_AKIRA_WASM_UART_RX_BUF_SIZE 256
#endif

/* Maximum number of simultaneous UART handles (one per secondary UART) */
#define AKIRA_UART_MAX_HANDLES 2

/* -------------------------------------------------------------------------- */
/* Handle table                                                                */
/* -------------------------------------------------------------------------- */

struct akira_uart_handle {
    bool               open;
    const struct device *dev;
    const void         *owner;  /* wasm_module_inst_t */
    struct ring_buf     rx_rb;
    uint8_t             rx_buf[CONFIG_AKIRA_WASM_UART_RX_BUF_SIZE];
};

static struct akira_uart_handle s_handles[AKIRA_UART_MAX_HANDLES];

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

/* -------------------------------------------------------------------------- */
/* IRQ callback                                                                */
/* -------------------------------------------------------------------------- */

static void uart_irq_cb(const struct device *dev, void *user_data)
{
    struct akira_uart_handle *h = (struct akira_uart_handle *)user_data;

    if (!uart_irq_update(dev)) {
        return;
    }
    while (uart_irq_rx_ready(dev)) {
        uint8_t byte;
        int n = uart_fifo_read(dev, &byte, 1);
        if (n == 1) {
            /* Drop byte silently if ring buffer is full */
            ring_buf_put(&h->rx_rb, &byte, 1);
        }
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
/* WASM native exports                                                         */
/* -------------------------------------------------------------------------- */

int akira_native_uart_open(wasm_exec_env_t exec_env,
                            int32_t port_id, int32_t baud_rate)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_UART, -EPERM);

    if (port_id < 0 || port_id >= AKIRA_UART_MAX_HANDLES) {
        LOG_ERR("uart_open: invalid port_id=%d", port_id);
        return -EINVAL;
    }
    if (baud_rate <= 0) {
        LOG_ERR("uart_open: invalid baud_rate=%d", baud_rate);
        return -EINVAL;
    }

    struct akira_uart_handle *h = &s_handles[port_id];
    if (h->open) {
        LOG_WRN("uart_open: port %d already open", port_id);
        return -EBUSY;
    }

    const struct device *dev = device_get_binding(s_uart_names[port_id]);
    if (!dev || !device_is_ready(dev)) {
        LOG_ERR("uart_open: device %s not ready", s_uart_names[port_id]);
        return -ENODEV;
    }

    /* Configure baud rate */
    struct uart_config cfg = {
        .baudrate  = (uint32_t)baud_rate,
        .parity    = UART_CFG_PARITY_NONE,
        .stop_bits = UART_CFG_STOP_BITS_1,
        .data_bits = UART_CFG_DATA_BITS_8,
        .flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
    };
    int ret = uart_configure(dev, &cfg);
    if (ret < 0) {
        LOG_ERR("uart_open: configure failed: %d", ret);
        return ret;
    }

    ring_buf_init(&h->rx_rb, sizeof(h->rx_buf), h->rx_buf);
    h->dev   = dev;
    h->owner = (const void *)wasm_runtime_get_module_inst(exec_env);
    h->open  = true;

    uart_irq_callback_user_data_set(dev, uart_irq_cb, h);
    uart_irq_rx_enable(dev);

    LOG_INF("uart_open: port_id=%d baud=%d handle=%d", port_id, baud_rate, port_id);
    return port_id; /* handle == port_id */
}

int akira_native_uart_write(wasm_exec_env_t exec_env, int32_t handle,
                             const uint8_t *buf, uint32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_UART, -EPERM);

    if (handle < 0 || handle >= AKIRA_UART_MAX_HANDLES) {
        return -EINVAL;
    }
    if (!buf || len == 0) {
        return -EINVAL;
    }

    struct akira_uart_handle *h = &s_handles[handle];
    if (!h->open) {
        return -EBADF;
    }
    if (h->owner != (const void *)wasm_runtime_get_module_inst(exec_env)) {
        return -EPERM;
    }

    for (uint32_t i = 0; i < len; i++) {
        uart_poll_out(h->dev, buf[i]);
    }
    return (int)len;
}

int akira_native_uart_read(wasm_exec_env_t exec_env, int32_t handle,
                            uint8_t *buf, uint32_t max_len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_UART, -EPERM);

    if (handle < 0 || handle >= AKIRA_UART_MAX_HANDLES) {
        return -EINVAL;
    }
    if (!buf || max_len == 0) {
        return -EINVAL;
    }

    struct akira_uart_handle *h = &s_handles[handle];
    if (!h->open) {
        return -EBADF;
    }
    if (h->owner != (const void *)wasm_runtime_get_module_inst(exec_env)) {
        return -EPERM;
    }

    uint32_t got = ring_buf_get(&h->rx_rb, buf, max_len);
    return (int)got; /* 0 = no data available; caller polls with delay() */
}

int akira_native_uart_close(wasm_exec_env_t exec_env, int32_t handle)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_UART, -EPERM);

    if (handle < 0 || handle >= AKIRA_UART_MAX_HANDLES) {
        return -EINVAL;
    }

    struct akira_uart_handle *h = &s_handles[handle];
    if (!h->open) {
        return -EBADF;
    }
    if (h->owner != (const void *)wasm_runtime_get_module_inst(exec_env)) {
        return -EPERM;
    }

    uart_irq_rx_disable(h->dev);
    uart_irq_callback_user_data_set(h->dev, NULL, NULL);
    ring_buf_reset(&h->rx_rb);
    memset(h, 0, sizeof(*h));

    LOG_INF("uart_close: handle=%d", handle);
    return 0;
}

#else /* !CONFIG_AKIRA_WASM_RUNTIME */

int akira_uart_api_init(void) { return 0; }

#endif /* CONFIG_AKIRA_WASM_RUNTIME */
