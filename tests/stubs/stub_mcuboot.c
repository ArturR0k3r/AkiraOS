/*
 * stub_mcuboot.c — MCUboot stubs for AkiraOS OTA tests.
 *
 * Provides weak-linked replacements for the <zephyr/dfu/mcuboot.h> calls made
 * by ota_manager.c and akira_boot_guard.c so they compile and link on
 * native_sim without CONFIG_MCUBOOT_IMG_MANAGER.
 *
 * Tests can override the return codes and image state via the exported
 * variables below.
 */

#include <zephyr/kernel.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>
#include <errno.h>

/* Test-controllable state --------------------------------------------------- */
int stub_boot_request_upgrade_rc;     /* 0 = succeed, non-zero = fail */
int stub_boot_write_img_confirmed_rc; /* 0 = succeed, non-zero = fail */
int stub_boot_write_img_confirmed_calls;
bool stub_boot_img_confirmed = true;  /* state of the running image */

/* Image headers reported for slot0 / slot1; a zero image_size means "no valid
 * image" (boot_read_bank_header() fails). */
struct mcuboot_img_header stub_slot0_header;
struct mcuboot_img_header stub_slot1_header;

/* Weak stubs ---------------------------------------------------------------- */
int __attribute__((weak)) boot_request_upgrade(int permanent)
{
    (void)permanent;
    return stub_boot_request_upgrade_rc;
}

int __attribute__((weak)) boot_write_img_confirmed(void)
{
    stub_boot_write_img_confirmed_calls++;
    if (stub_boot_write_img_confirmed_rc == 0) {
        stub_boot_img_confirmed = true;
    }
    return stub_boot_write_img_confirmed_rc;
}

bool __attribute__((weak)) boot_is_img_confirmed(void)
{
    return stub_boot_img_confirmed;
}

int __attribute__((weak)) boot_read_bank_header(uint8_t area_id,
                                                struct mcuboot_img_header *header,
                                                size_t header_size)
{
    const struct mcuboot_img_header *src;

    if (area_id == FIXED_PARTITION_ID(slot0_partition)) {
        src = &stub_slot0_header;
    } else if (area_id == FIXED_PARTITION_ID(slot1_partition)) {
        src = &stub_slot1_header;
    } else {
        return -EINVAL;
    }
    if (src->h.v1.image_size == 0 || header_size < sizeof(*src)) {
        return -EIO;
    }
    *header = *src;
    return 0;
}
