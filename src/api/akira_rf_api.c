/**
 * @file akira_rf_api.c
 * @brief RF API implementation for WASM exports
 */

#include "akira_api.h"
#include <runtime/security.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(akira_rf_api, LOG_LEVEL_INF);

static akira_rf_chip_t g_active_chip = AKIRA_RF_CHIP_NONE;

int akira_rf_init(akira_rf_chip_t chip)
{
    if (!akira_security_check("rf.transceive"))
        return -EPERM;

    LOG_INF("RF init: chip=%d", chip);

#ifdef CONFIG_AKIRA_RF_FRAMEWORK
    /* TODO: Initialize actual driver for each chip */
    g_active_chip = chip;
    return 0;
#else
    (void)chip;
    return -ENOSYS;
#endif
}

int akira_rf_deinit(void)
{
    if (!akira_security_check("rf.transceive"))
        return -EPERM;

    LOG_INF("RF deinit");
    g_active_chip = AKIRA_RF_CHIP_NONE;
    return 0;
}

int akira_rf_send(const uint8_t *data, size_t len)
{
    if (!akira_security_check("rf.transceive"))
        return -EPERM;

    if (g_active_chip == AKIRA_RF_CHIP_NONE)
    {
        LOG_ERR("RF not initialized");
        return -ENODEV;
    }

    if (!data || len == 0)
    {
        return -EINVAL;
    }

    LOG_DBG("RF send: %zu bytes", len);

#ifdef CONFIG_AKIRA_RF_FRAMEWORK
    // TODO: Dispatch to active driver
    (void)data; (void)len;
    return -ENOSYS;
#else
    (void)data; (void)len;
    return -ENOSYS;
#endif
}

int akira_rf_receive(uint8_t *buffer, size_t max_len, uint32_t timeout_ms)
{
    if (!akira_security_check("rf.transceive"))
        return -EPERM;

    if (g_active_chip == AKIRA_RF_CHIP_NONE)
    {
        LOG_ERR("RF not initialized");
        return -ENODEV;
    }

    if (!buffer || max_len == 0)
    {
        return -EINVAL;
    }

    LOG_DBG("RF receive: max=%zu, timeout=%u", max_len, timeout_ms);
    (void)buffer; (void)max_len; (void)timeout_ms;
    return -ENOSYS;
}

int akira_rf_set_frequency(uint32_t freq_hz)
{
    if (!akira_security_check("rf.transceive"))
        return -EPERM;

    LOG_INF("RF set frequency: %u Hz", freq_hz);
    (void)freq_hz;
    return -ENOSYS;
}

int akira_rf_set_power(int8_t dbm)
{
    if (!akira_security_check("rf.transceive"))
        return -EPERM;

    LOG_INF("RF set power: %d dBm", dbm);
    (void)dbm;
    return -ENOSYS;
}

int akira_rf_get_rssi(int16_t *rssi)
{
    if (!akira_security_check("rf.transceive"))
        return -EPERM;

    if (!rssi)
        return -EINVAL;

    *rssi = -100;
    return -ENOSYS;
}
