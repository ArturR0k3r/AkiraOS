/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 *
 * Integration test for the Matter accessory (device-as-endpoint) path,
 * driven against the in-firmware mock co-processor
 * (CONFIG_AKIRA_MATTER_COPROC_MOCK). Exercises the full IPC round-trip:
 * endpoint registration, onboarding-code retrieval, attribute reporting,
 * pairing-window open, and inbound-command delivery.
 */

#include <zephyr/ztest.h>
#include <string.h>

#include "runtime/akira_matter_ipc.h"
#include "matter_coproc_mock.h"

/* Matter cluster / device-type IDs used by the test. */
#define DEVTYPE_COLOR_LIGHT 0x0102
#define CLUSTER_ONOFF       0x0006
#define CLUSTER_LEVEL       0x0008
#define CLUSTER_COLOR       0x0300
#define CMD_ON              0x0001

ZTEST(matter_accessory, test_endpoint_add_returns_id)
{
    zassert_ok(akira_matter_ipc_init(), "ipc init failed");

    uint32_t clusters[] = { CLUSTER_ONOFF, CLUSTER_LEVEL, CLUSTER_COLOR };
    uint8_t ep = 0xFF;
    int rc = akira_matter_ipc_endpoint_add(DEVTYPE_COLOR_LIGHT, clusters, 3, &ep);
    zassert_ok(rc, "endpoint_add rc=%d", rc);
    zassert_equal(ep, 1, "first endpoint should be 1, got %u", ep);
}

ZTEST(matter_accessory, test_get_qr_is_not_hardcoded_stub)
{
    zassert_ok(akira_matter_ipc_init(), "ipc init failed");

    char qr[AKIRA_MATTER_IPC_QR_LEN];
    char manual[AKIRA_MATTER_IPC_MANUAL_LEN];
    int rc = akira_matter_ipc_get_qr(qr, sizeof(qr), manual, sizeof(manual));
    zassert_ok(rc, "get_qr rc=%d", rc);

    zassert_true(strncmp(qr, "MT:", 3) == 0, "QR should start with MT: (got '%s')", qr);
    /* The old stub returned a fixed string; the mock returns its own payload. */
    zassert_true(strcmp(qr, "MT:Y.K9042C00KA0648G00") != 0, "QR is the dead stub value");
    zassert_true(strlen(manual) > 0, "manual code empty");
}

ZTEST(matter_accessory, test_inbound_command_delivered)
{
    zassert_ok(akira_matter_ipc_init(), "ipc init failed");

    /* Drain any commands auto-injected by earlier endpoint_add calls so this
     * test is order-independent. */
    struct akira_matter_event evt;
    while (akira_matter_ipc_poll(&evt, K_NO_WAIT) == 0) {
        /* discard */
    }

    /* Simulate a controller (Home Assistant / Google Home) issuing On to EP 7. */
    matter_coproc_mock_inject_command(7, CLUSTER_ONOFF, CMD_ON, NULL, 0);

    int rc = akira_matter_ipc_poll(&evt, K_MSEC(1000));
    zassert_ok(rc, "poll rc=%d (no command delivered)", rc);
    zassert_equal(evt.kind, AKIRA_MATTER_EVT_ACC_CMD, "wrong event kind %u", evt.kind);
    zassert_equal(evt.endpoint_id, 7, "wrong endpoint %u", evt.endpoint_id);
    zassert_equal(evt.cluster_id, CLUSTER_ONOFF, "wrong cluster 0x%x", evt.cluster_id);
    zassert_equal(evt.cmd_id, CMD_ON, "wrong cmd 0x%x", evt.cmd_id);
}

ZTEST(matter_accessory, test_report_and_pairing_succeed)
{
    zassert_ok(akira_matter_ipc_init(), "ipc init failed");

    uint8_t on = 1;
    int rc = akira_matter_ipc_report(1, CLUSTER_ONOFF, 0x0000, &on, 1);
    zassert_ok(rc, "report rc=%d", rc);

    rc = akira_matter_ipc_open_pairing(300);
    zassert_ok(rc, "open_pairing rc=%d", rc);
}

ZTEST(matter_accessory, test_report_rejects_bad_args)
{
    zassert_ok(akira_matter_ipc_init(), "ipc init failed");

    uint8_t v = 0;
    zassert_equal(akira_matter_ipc_report(1, CLUSTER_ONOFF, 0, NULL, 1), -EINVAL);
    zassert_equal(akira_matter_ipc_report(1, CLUSTER_ONOFF, 0, &v, 0), -EINVAL);
}

ZTEST_SUITE(matter_accessory, NULL, NULL, NULL, NULL, NULL);
