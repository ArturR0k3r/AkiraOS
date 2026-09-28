/*
 * test_boot_guard.c
 * ztest suite: boot_guard
 *
 * Tests for src/runtime/akira_boot_guard.c: trial-boot detection, confirmation
 * (manual and after BOOT_READY), and rollback detection on the next boot.
 *
 * MCUboot is simulated by stubs/stub_mcuboot.c (image confirmed flag and slot
 * headers), settings by stubs/stub_settings.c. The deadline reboot and
 * akira_boot_guard_reject() are not exercised: sys_reboot() ends native_sim.
 */

#include <zephyr/ztest.h>
#include <zephyr/dfu/mcuboot.h>
#include <errno.h>
#include <string.h>

#include <akira_boot_guard.h>
#include <akira_hooks.h>
#include "settings/settings.h"
#include "ota/ota_manager.h"

/* Exported by stubs/stub_mcuboot.c */
extern int stub_boot_request_upgrade_rc;
extern int stub_boot_write_img_confirmed_rc;
extern int stub_boot_write_img_confirmed_calls;
extern bool stub_boot_img_confirmed;
extern struct mcuboot_img_header stub_slot0_header;
extern struct mcuboot_img_header stub_slot1_header;

#define TRIAL_KEY "ota/trial"

static int confirmed_events;
static int rollback_events;
static int rollback_error;

static void on_ota_event(const struct akira_hook_event *event, void *user_data)
{
    ARG_UNUSED(user_data);

    if (event->type == AKIRA_HOOK_OTA_CONFIRMED) {
        confirmed_events++;
    } else if (event->type == AKIRA_HOOK_OTA_ERROR) {
        rollback_events++;
        rollback_error = event->ota.error;
    }
}

AKIRA_HOOK_DEFINE(test_boot_guard_hook,
                  AKIRA_HOOK_MASK(AKIRA_HOOK_OTA_CONFIRMED) |
                      AKIRA_HOOK_MASK(AKIRA_HOOK_OTA_ERROR),
                  on_ota_event, NULL);

static void set_slot0(uint8_t major, uint8_t minor, uint32_t size)
{
    memset(&stub_slot0_header, 0, sizeof(stub_slot0_header));
    stub_slot0_header.mcuboot_version = 1;
    stub_slot0_header.h.v1.image_size = size;
    stub_slot0_header.h.v1.sem_ver.major = major;
    stub_slot0_header.h.v1.sem_ver.minor = minor;
}

static void emit_boot_ready(void)
{
    struct akira_hook_event evt = { .type = AKIRA_HOOK_BOOT_READY };

    akira_hooks_emit(&evt);
}

static void guard_before(void *arg)
{
    ARG_UNUSED(arg);

    stub_boot_request_upgrade_rc = 0;
    stub_boot_write_img_confirmed_rc = 0;
    stub_boot_write_img_confirmed_calls = 0;
    stub_boot_img_confirmed = true;
    set_slot0(1, 6, 1000);
    memset(&stub_slot1_header, 0, sizeof(stub_slot1_header));
    (void)akira_settings_delete(TRIAL_KEY);
    confirmed_events = 0;
    rollback_events = 0;
    rollback_error = 0;
}

static void guard_after(void *arg)
{
    ARG_UNUSED(arg);

    /* Never leave a trial (and its deadline timer) running between tests. */
    stub_boot_write_img_confirmed_rc = 0;
    (void)akira_boot_guard_confirm();
}

ZTEST(boot_guard, test_confirmed_image_is_not_a_trial)
{
    zassert_ok(akira_boot_guard_init());
    zassert_false(akira_boot_guard_is_trial());
    zassert_false(akira_boot_guard_rolled_back());
    zassert_ok(akira_boot_guard_confirm(), "confirming a confirmed image is a no-op");
    zassert_equal(stub_boot_write_img_confirmed_calls, 0);
}

ZTEST(boot_guard, test_unconfirmed_image_starts_trial_and_records_it)
{
    char rec[64];

    stub_boot_img_confirmed = false;
    zassert_ok(akira_boot_guard_init());
    zassert_true(akira_boot_guard_is_trial());
    zassert_ok(akira_settings_get(TRIAL_KEY, rec, sizeof(rec)));
    zassert_str_equal(rec, "1.6.0+0/1000");
}

ZTEST(boot_guard, test_confirm_ends_trial)
{
    char rec[64];

    stub_boot_img_confirmed = false;
    zassert_ok(akira_boot_guard_init());
    zassert_ok(akira_boot_guard_confirm());

    zassert_false(akira_boot_guard_is_trial());
    zassert_equal(stub_boot_write_img_confirmed_calls, 1);
    zassert_equal(confirmed_events, 1);
    zassert_equal(akira_settings_get(TRIAL_KEY, rec, sizeof(rec)), -ENOENT,
                  "trial record must be cleared once confirmed");
}

ZTEST(boot_guard, test_failed_confirm_keeps_trial)
{
    stub_boot_img_confirmed = false;
    zassert_ok(akira_boot_guard_init());

    stub_boot_write_img_confirmed_rc = -EIO;
    zassert_equal(akira_boot_guard_confirm(), -EIO);
    zassert_true(akira_boot_guard_is_trial());
    zassert_equal(confirmed_events, 0);
}

ZTEST(boot_guard, test_ota_confirm_firmware_goes_through_guard)
{
    char rec[64];

    stub_boot_img_confirmed = false;
    zassert_ok(akira_boot_guard_init());

    /* Shell "ota confirm" and WASM ota_confirm() must end the trial too,
     * not just set the MCUboot flag behind the guard's back. */
    zassert_equal(ota_confirm_firmware(), OTA_OK);
    zassert_false(akira_boot_guard_is_trial());
    zassert_equal(confirmed_events, 1);
    zassert_equal(akira_settings_get(TRIAL_KEY, rec, sizeof(rec)), -ENOENT);
}

ZTEST(boot_guard, test_boot_ready_confirms_after_delay)
{
    stub_boot_img_confirmed = false;
    zassert_ok(akira_boot_guard_init());

    emit_boot_ready();
    zassert_true(akira_boot_guard_is_trial(), "must not confirm before the delay");

    k_sleep(K_SECONDS(CONFIG_AKIRA_BOOT_GUARD_CONFIRM_DELAY_S + 1));
    zassert_false(akira_boot_guard_is_trial());
    zassert_equal(stub_boot_write_img_confirmed_calls, 1);
}

ZTEST(boot_guard, test_rollback_detected_on_next_boot)
{
    char rec[64];

    /* Boot 1: 1.7 trial starts, then "crashes" before confirming. */
    set_slot0(1, 7, 2000);
    stub_boot_img_confirmed = false;
    zassert_ok(akira_boot_guard_init());

    /* Boot 2: MCUboot restored the confirmed 1.6 image. */
    set_slot0(1, 6, 1000);
    stub_boot_img_confirmed = true;
    zassert_ok(akira_boot_guard_init());

    zassert_false(akira_boot_guard_is_trial());
    zassert_true(akira_boot_guard_rolled_back());
    zassert_equal(akira_settings_get(TRIAL_KEY, rec, sizeof(rec)), -ENOENT);

    emit_boot_ready();
    zassert_equal(rollback_events, 1);
    zassert_equal(rollback_error, -ECANCELED);
}

ZTEST(boot_guard, test_trial_confirmed_elsewhere_is_not_a_rollback)
{
    /* The trial image got confirmed without the guard (e.g. mcumgr), so the
     * record survived but the same image is running. */
    stub_boot_img_confirmed = false;
    zassert_ok(akira_boot_guard_init());

    stub_boot_img_confirmed = true;
    zassert_ok(akira_boot_guard_init());

    zassert_false(akira_boot_guard_rolled_back());
    emit_boot_ready();
    zassert_equal(rollback_events, 0);
}

ZTEST(boot_guard, test_manual_rollback_refused_without_previous_image)
{
    /* Confirmed image and an empty secondary slot (e.g. the failed image was
     * discarded after an automatic rollback): nothing to go back to, and no
     * reboot may be scheduled. */
    zassert_ok(akira_boot_guard_init());
    zassert_equal(ota_request_rollback(), OTA_ERROR_INVALID_IMAGE);
}

ZTEST_SUITE(boot_guard, NULL, NULL, guard_before, guard_after, NULL);
