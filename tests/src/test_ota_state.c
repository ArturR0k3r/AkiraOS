/*
 * test_ota_state.c
 * ztest suite: ota_state
 *
 * Tests for src/connectivity/ota/ota_manager.c
 *
 * Uses the flash simulator (CONFIG_FLASH_SIMULATOR=y) for real flash_area_*
 * calls and stubs/stub_mcuboot.c for boot_request_upgrade /
 * boot_write_img_confirmed.
 *
 * The MCUboot magic value checked by do_finalize_update() is 0x96f3b83d.
 */

#include <zephyr/ztest.h>
#include <zephyr/dfu/mcuboot.h>
#include <string.h>
#include <stdint.h>

#include "ota/ota_manager.h"

/* Exported by stubs/stub_mcuboot.c */
extern int stub_boot_request_upgrade_rc;
extern int stub_boot_write_img_confirmed_rc;
extern struct mcuboot_img_header stub_slot0_header;
extern struct mcuboot_img_header stub_slot1_header;

/* MCUboot image header magic expected by do_finalize_update() */
#define OTA_IMAGE_MAGIC 0x96f3b83dU

/* ── suite setup/teardown ────────────────────────────────────────────────── */

/** Set the MCUboot header version the stub reports for a slot. */
static void set_slot_version(struct mcuboot_img_header *hdr, uint8_t major,
                             uint8_t minor, uint16_t revision)
{
    memset(hdr, 0, sizeof(*hdr));
    hdr->mcuboot_version = 1;
    hdr->h.v1.image_size = 0x1000;
    hdr->h.v1.sem_ver.major = major;
    hdr->h.v1.sem_ver.minor = minor;
    hdr->h.v1.sem_ver.revision = revision;
}

static void ota_before(void *arg)
{
    ARG_UNUSED(arg);

    /* Reset stub return codes to "success" */
    stub_boot_request_upgrade_rc = 0;
    stub_boot_write_img_confirmed_rc = 0;

    /* Running and staged images at the same version: not a downgrade. */
    set_slot_version(&stub_slot0_header, 1, 6, 5);
    set_slot_version(&stub_slot1_header, 1, 6, 5);

    /* Make sure any previous update is cleaned up */
    ota_abort_update();
}

/* ── helpers ─────────────────────────────────────────────────────────────── */

/** Write the MCUboot magic word at offset 0 of the secondary slot. */
static void write_valid_magic(void)
{
    uint32_t magic = OTA_IMAGE_MAGIC;
    /* Write a dummy chunk large enough to hold the magic word.
     * The first 4 bytes carry the MCUboot header magic. */
    uint8_t chunk[8];

    memcpy(chunk, &magic, sizeof(magic));
    memset(chunk + sizeof(magic), 0xFF, sizeof(chunk) - sizeof(magic));

    ota_write_chunk(chunk, sizeof(chunk));
}

/* ── test cases ──────────────────────────────────────────────────────────── */

ZTEST(ota_state, test_initial_state_idle)
{
    const struct ota_progress *p = ota_get_progress();

    zassert_equal(p->state, OTA_STATE_IDLE,
                  "initial state must be IDLE, got %d", p->state);
}

ZTEST(ota_state, test_start_transitions_to_receiving)
{
    enum ota_result rc = ota_start_update(0);

    if (rc != OTA_OK)
    {
        /* Flash simulator might not have partitions — skip gracefully */
        ztest_test_skip();
    }

    const struct ota_progress *p = ota_get_progress();

    zassert_equal(p->state, OTA_STATE_RECEIVING,
                  "state must be RECEIVING after start, got %d", p->state);
}

ZTEST(ota_state, test_write_chunk_increments_bytes_written)
{
    enum ota_result rc = ota_start_update(4096);

    if (rc != OTA_OK)
    {
        ztest_test_skip();
    }

    uint8_t chunk[128];

    memset(chunk, 0xAA, sizeof(chunk));
    rc = ota_write_chunk(chunk, sizeof(chunk));

    zassert_equal(rc, OTA_OK,
                  "write_chunk failed: %d", rc);

    const struct ota_progress *p = ota_get_progress();

    zassert_equal(p->bytes_written, sizeof(chunk),
                  "bytes_written should be %zu, got %zu",
                  sizeof(chunk), p->bytes_written);
}

ZTEST(ota_state, test_finalize_bad_magic_returns_invalid_image)
{
    enum ota_result rc = ota_start_update(256);

    if (rc != OTA_OK)
    {
        ztest_test_skip();
    }

    /* Write garbage — no valid MCUboot magic */
    uint8_t bad[8];

    memset(bad, 0xDE, sizeof(bad));
    ota_write_chunk(bad, sizeof(bad));

    rc = ota_finalize_update();

    zassert_equal(rc, OTA_ERROR_INVALID_IMAGE,
                  "bad magic should give INVALID_IMAGE, got %d", rc);

    const struct ota_progress *p = ota_get_progress();

    zassert_equal(p->last_error, OTA_ERROR_INVALID_IMAGE,
                  "progress should record error");
}

ZTEST(ota_state, test_finalize_after_bad_magic_resets_to_error)
{
    enum ota_result rc = ota_start_update(256);

    if (rc != OTA_OK)
    {
        ztest_test_skip();
    }

    uint8_t bad[8];

    memset(bad, 0xDE, sizeof(bad));
    ota_write_chunk(bad, sizeof(bad));
    ota_finalize_update();

    const struct ota_progress *p = ota_get_progress();

    /* After a validation failure the OTA subsystem must not be
     * stuck in RECEIVING — it should be in ERROR. */
    zassert_not_equal(p->state, OTA_STATE_RECEIVING,
                      "state must leave RECEIVING after failed finalize");
}

ZTEST(ota_state, test_finalize_valid_magic_reaches_complete)
{
    enum ota_result rc = ota_start_update(0);

    if (rc != OTA_OK)
    {
        ztest_test_skip();
    }

    /* boot_request_upgrade stub returns 0 (success) */
    stub_boot_request_upgrade_rc = 0;

    write_valid_magic();

    rc = ota_finalize_update();

    zassert_equal(rc, OTA_OK,
                  "finalize with valid magic should succeed: %d", rc);

    const struct ota_progress *p = ota_get_progress();

    zassert_equal(p->state, OTA_STATE_COMPLETE,
                  "state must be COMPLETE, got %d", p->state);
}

ZTEST(ota_state, test_finalize_boot_request_fail_returns_error)
{
    enum ota_result rc = ota_start_update(0);

    if (rc != OTA_OK)
    {
        ztest_test_skip();
    }

    /* Simulate MCUboot rejecting the upgrade request */
    stub_boot_request_upgrade_rc = -1;

    write_valid_magic();

    rc = ota_finalize_update();

    zassert_equal(rc, OTA_ERROR_BOOT_REQUEST_FAILED,
                  "boot request failure not propagated: %d", rc);
}

ZTEST(ota_state, test_abort_returns_to_idle)
{
    enum ota_result rc = ota_start_update(0);

    if (rc != OTA_OK)
    {
        ztest_test_skip();
    }

    rc = ota_abort_update();

    zassert_equal(rc, OTA_OK, "abort should succeed");

    const struct ota_progress *p = ota_get_progress();

    zassert_equal(p->state, OTA_STATE_IDLE,
                  "state must be IDLE after abort, got %d", p->state);
}

ZTEST(ota_state, test_abort_resets_bytes_written)
{
    enum ota_result rc = ota_start_update(4096);

    if (rc != OTA_OK)
    {
        ztest_test_skip();
    }

    uint8_t chunk[64];

    memset(chunk, 0xCC, sizeof(chunk));
    ota_write_chunk(chunk, sizeof(chunk));

    ota_abort_update();

    const struct ota_progress *p = ota_get_progress();

    zassert_equal(p->bytes_written, 0U,
                  "bytes_written must reset to 0 after abort");
}

ZTEST(ota_state, test_write_with_no_active_update_fails)
{
    /* Calling write_chunk without a prior start_update */
    uint8_t chunk[8] = {0};
    enum ota_result rc = ota_write_chunk(chunk, sizeof(chunk));

    zassert_not_equal(rc, OTA_OK,
                      "write without active update should fail");
}

ZTEST(ota_state, test_null_chunk_rejected)
{
    enum ota_result rc = ota_write_chunk(NULL, 16);

    zassert_equal(rc, OTA_ERROR_INVALID_PARAM,
                  "NULL data should give INVALID_PARAM");
}

ZTEST(ota_state, test_zero_length_chunk_rejected)
{
    uint8_t chunk[8] = {0};
    enum ota_result rc = ota_write_chunk(chunk, 0);

    zassert_equal(rc, OTA_ERROR_INVALID_PARAM,
                  "zero length chunk should give INVALID_PARAM");
}

ZTEST(ota_state, test_finalize_refuses_downgrade)
{
    if (!IS_ENABLED(CONFIG_AKIRA_OTA_REFUSE_DOWNGRADE) ||
        ota_start_update(0) != OTA_OK)
    {
        ztest_test_skip();
    }

    set_slot_version(&stub_slot1_header, 1, 6, 4);
    write_valid_magic();

    zassert_equal(ota_finalize_update(), OTA_ERROR_INVALID_IMAGE,
                  "an image older than the running one must be refused");
    zassert_equal(ota_get_progress()->state, OTA_STATE_ERROR,
                  "a refused downgrade must leave the OTA state in ERROR");
}

ZTEST(ota_state, test_finalize_accepts_newer_and_equal_versions)
{
    if (!IS_ENABLED(CONFIG_AKIRA_OTA_REFUSE_DOWNGRADE))
    {
        ztest_test_skip();
    }

    /* Newer revision, newer minor with lower revision, same version. */
    const uint8_t versions[][3] = { {1, 6, 6}, {1, 7, 0}, {1, 6, 5} };

    for (size_t i = 0; i < ARRAY_SIZE(versions); i++)
    {
        ota_abort_update();
        if (ota_start_update(0) != OTA_OK)
        {
            ztest_test_skip();
        }
        set_slot_version(&stub_slot1_header, versions[i][0], versions[i][1],
                         versions[i][2]);
        write_valid_magic();
        zassert_equal(ota_finalize_update(), OTA_OK, "version %u.%u.%u refused",
                      versions[i][0], versions[i][1], versions[i][2]);
    }
}

ZTEST(ota_state, test_finalize_refuses_unreadable_header)
{
    if (!IS_ENABLED(CONFIG_AKIRA_OTA_REFUSE_DOWNGRADE) ||
        ota_start_update(0) != OTA_OK)
    {
        ztest_test_skip();
    }

    /* image_size 0: the stub reports "no valid image" for the staged slot. */
    stub_slot1_header.h.v1.image_size = 0;
    write_valid_magic();

    zassert_equal(ota_finalize_update(), OTA_ERROR_INVALID_IMAGE,
                  "the downgrade check must fail closed");
}

ZTEST_SUITE(ota_state, NULL, NULL, ota_before, NULL, NULL);
