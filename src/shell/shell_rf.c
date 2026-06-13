/**
 * @file shell_rf.c
 * @brief RF Module Shell Commands
 */

#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <api/akira_rf_api.h>
#include "connectivity/radio_interface.h"
#include <stdlib.h>
#include <string.h>

LOG_MODULE_REGISTER(shell_rf, LOG_LEVEL_INF);

/* Shell command: rf init <chip> */
static int cmd_rf_init(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "Usage: rf init <chip>");
        shell_print(sh, "  chip: 0=None 1=NRF24L01 2=CC1101 3=LR1121 4=CC1121 5=LR2021");
        return -EINVAL;
    }

    int chip = atoi(argv[1]);
    if (chip < 0 || chip >= AKIRA_RF_CHIP_MAX) {
        shell_error(sh, "Invalid chip (0=None 1=NRF24L01 2=CC1101 3=LR1121 4=CC1121 5=LR2021)");
        return -EINVAL;
    }

    shell_print(sh, "Initializing RF chip %d...", chip);
    int ret = akira_rf_init((akira_rf_chip_t)chip);
    if (ret < 0) {
        shell_error(sh, "RF init failed: %d", ret);
        return ret;
    }

    shell_print(sh, "RF chip initialized successfully");
    return 0;
}

/* Shell command: rf freq <freq_hz> */
static int cmd_rf_freq(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "Usage: rf freq <freq_hz>");
        shell_print(sh, "  Example: rf freq 868000000 (868 MHz)");
        return -EINVAL;
    }

    uint32_t freq = (uint32_t)atol(argv[1]);
    shell_print(sh, "Setting frequency to %u Hz...", freq);

    int ret = akira_rf_set_frequency(freq);
    if (ret < 0) {
        shell_error(sh, "Failed to set frequency: %d", ret);
        return ret;
    }

    shell_print(sh, "Frequency set successfully");
    return 0;
}

/* Shell command: rf power <dbm> */
static int cmd_rf_power(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "Usage: rf power <dbm>");
        shell_print(sh, "  Example: rf power 14");
        return -EINVAL;
    }

    int8_t power = (int8_t)atoi(argv[1]);
    shell_print(sh, "Setting TX power to %d dBm...", power);

    int ret = akira_rf_set_power(power);
    if (ret < 0) {
        shell_error(sh, "Failed to set power: %d", ret);
        return ret;
    }

    shell_print(sh, "TX power set successfully");
    return 0;
}

/* Shell command: rf send <data> */
static int cmd_rf_send(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "Usage: rf send <data>");
        shell_print(sh, "  Example: rf send \"Hello World\"");
        return -EINVAL;
    }

    const char *data = argv[1];
    size_t len = strlen(data);

    shell_print(sh, "Sending %zu bytes...", len);

    int ret = akira_rf_send((const uint8_t *)data, len);
    if (ret < 0) {
        shell_error(sh, "Send failed: %d", ret);
        return ret;
    }

    shell_print(sh, "Data sent successfully");
    return 0;
}

/* Shell command: rf recv <timeout_ms> */
static int cmd_rf_recv(const struct shell *sh, size_t argc, char **argv)
{
    uint32_t timeout = 5000;  /* Default 5 seconds */
    if (argc >= 2) {
        timeout = (uint32_t)atol(argv[1]);
    }

    uint8_t buffer[256];
    shell_print(sh, "Receiving (timeout=%u ms)...", timeout);

    int ret = akira_rf_receive(buffer, sizeof(buffer), timeout);
    if (ret < 0) {
        shell_error(sh, "Receive failed: %d", ret);
        return ret;
    }

    if (ret == 0) {
        shell_print(sh, "No data received (timeout)");
        return 0;
    }

    shell_print(sh, "Received %d bytes:", ret);
    shell_hexdump(sh, buffer, ret);
    return 0;
}

/* Shell command: rf rssi */
static int cmd_rf_rssi(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    int16_t rssi;
    int ret = akira_rf_get_rssi(&rssi);
    if (ret < 0) {
        shell_error(sh, "Failed to read RSSI: %d", ret);
        return ret;
    }

    shell_print(sh, "RSSI: %d dBm", rssi);
    return 0;
}

/* Shell command: rf status */
static int cmd_rf_status(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    /* Active chip */
    radio_handle_t *active = akira_rf_get_active_handle();
    if (active) {
        int16_t rssi = -999;
        akira_rf_get_rssi(&rssi);
        shell_print(sh, "active:   %s  caps=0x%08x  rssi=%d dBm",
                    active->name, active->capabilities, rssi);
    } else {
        shell_print(sh, "active:   none");
    }

    /* All registered radios */
    radio_handle_t *handles[8];
    int n = radio_manager_get_all(RADIO_TYPE_NONE, handles, ARRAY_SIZE(handles));
    shell_print(sh, "registered: %d", n);
    for (int i = 0; i < n; i++) {
        shell_print(sh, "  [%d] %-12s  caps=0x%08x  ops=%s%s",
                    i,
                    handles[i]->name,
                    handles[i]->capabilities,
                    handles[i]->ops ? "yes" : "NULL(stub)",
                    handles[i] == active ? "  <-- active" : "");
    }

    return 0;
}

/* Shell command: rf deinit */
static int cmd_rf_deinit(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_print(sh, "Deinitializing RF...");
    int ret = akira_rf_deinit();
    if (ret < 0) {
        shell_error(sh, "RF deinit failed: %d", ret);
        return ret;
    }

    shell_print(sh, "RF deinitialized");
    return 0;
}

/* Shell command: rf select <chip> */
static int cmd_rf_select(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "Usage: rf select <chip>");
        shell_print(sh, "  chip: 0=None, 1=NRF24L01, 2=CC1101, 3=LR1121, 4=CC1121, 5=LR2021");
        return -EINVAL;
    }

    int chip = atoi(argv[1]);
    int ret = akira_rf_select((akira_rf_chip_t)chip);
    if (ret < 0) {
        shell_error(sh, "select failed: %d", ret);
        return ret;
    }

    shell_print(sh, "active chip -> %d", chip);
    return 0;
}

/* Shell command: rf test registry — dump all registered handles */
static int cmd_rf_test_registry(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    radio_handle_t *handles[8];
    int n = radio_manager_get_all(RADIO_TYPE_NONE, handles, ARRAY_SIZE(handles));
    if (n <= 0) {
        shell_print(sh, "no radios registered");
        return 0;
    }

    shell_print(sh, "%-12s  caps", "name");
    shell_print(sh, "%-12s  ----", "----");
    for (int i = 0; i < n; i++) {
        shell_print(sh, "%-12s  0x%08x  ops=%s",
                    handles[i]->name,
                    handles[i]->capabilities,
                    handles[i]->ops ? "yes" : "NULL");
    }
    return 0;
}

/* Shell command: rf test caps <hex_mask> — lookup by capability */
static int cmd_rf_test_caps(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "Usage: rf test caps <hex_mask>");
        shell_print(sh, "  e.g. rf test caps 0x00010001  (TX|BAND_SUBGHZ)");
        return -EINVAL;
    }

    uint32_t mask = (uint32_t)strtoul(argv[1], NULL, 16);
    radio_handle_t *h = radio_manager_get_by_caps(mask);
    if (!h) {
        shell_print(sh, "no match for caps 0x%08x", mask);
        return 0;
    }

    shell_print(sh, "match: %s (caps=0x%08x)", h->name, h->capabilities);
    return 0;
}

/* Shell command: rf test loopback <data> — send then recv */
static int cmd_rf_test_loopback(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "Usage: rf test loopback <data>");
        return -EINVAL;
    }

    const char *data = argv[1];
    size_t len = strlen(data);

    int ret = akira_rf_send((const uint8_t *)data, len);
    if (ret < 0) {
        shell_error(sh, "send failed: %d", ret);
        return ret;
    }
    shell_print(sh, "sent %zu bytes", len);

    uint8_t buf[256];
    ret = akira_rf_receive(buf, sizeof(buf), 3000);
    if (ret < 0) {
        shell_error(sh, "recv failed: %d", ret);
        return ret;
    }
    if (ret == 0) {
        shell_print(sh, "recv timeout (no echo — expected without second node)");
        return 0;
    }

    shell_print(sh, "recv %d bytes:", ret);
    shell_hexdump(sh, buf, ret);
    return 0;
}

/* Shell command: rf test nohandle — send with no active chip, expect error */
static int cmd_rf_test_nohandle(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    akira_rf_deinit();
    int ret = akira_rf_send((const uint8_t *)"x", 1);
    if (ret < 0) {
        shell_print(sh, "PASS: send with no handle returned %d", ret);
    } else {
        shell_error(sh, "FAIL: expected error, got %d", ret);
    }
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_rf_test,
    SHELL_CMD_ARG(registry, NULL, "Dump registered radio handles", cmd_rf_test_registry, 1, 0),
    SHELL_CMD_ARG(caps,     NULL, "Lookup handle by cap mask (hex)", cmd_rf_test_caps, 2, 0),
    SHELL_CMD_ARG(loopback, NULL, "Send data and attempt recv", cmd_rf_test_loopback, 2, 0),
    SHELL_CMD_ARG(nohandle, NULL, "Send with no active chip (expect error)", cmd_rf_test_nohandle, 1, 0),
    SHELL_SUBCMD_SET_END
);

/* Register RF shell commands */
SHELL_STATIC_SUBCMD_SET_CREATE(sub_rf,
    SHELL_CMD_ARG(init,    NULL,         "Initialize RF chip",      cmd_rf_init,   2, 0),
    SHELL_CMD_ARG(deinit,  NULL,         "Deinitialize RF",         cmd_rf_deinit, 1, 0),
    SHELL_CMD_ARG(select,  NULL,         "Select active chip",      cmd_rf_select, 2, 0),
    SHELL_CMD_ARG(freq,    NULL,         "Set frequency (Hz)",      cmd_rf_freq,   2, 0),
    SHELL_CMD_ARG(power,   NULL,         "Set TX power (dBm)",      cmd_rf_power,  2, 0),
    SHELL_CMD_ARG(send,    NULL,         "Send data",               cmd_rf_send,   2, 0),
    SHELL_CMD_ARG(recv,    NULL,         "Receive data",            cmd_rf_recv,   1, 1),
    SHELL_CMD_ARG(rssi,    NULL,         "Read RSSI",               cmd_rf_rssi,   1, 0),
    SHELL_CMD_ARG(status,  NULL,         "Show RF status",          cmd_rf_status, 1, 0),
    SHELL_CMD(test, &sub_rf_test,        "Radio abstraction tests", NULL),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(rf, &sub_rf, "RF transceiver commands", NULL);
