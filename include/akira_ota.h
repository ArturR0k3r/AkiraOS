/**
 * @file akira_ota.h
 * @brief Write a firmware update into the secondary slot
 *
 * For update clients (a fleet agent, a custom transport). The image must be an
 * MCUboot image; MCUboot verifies its signature at the next boot. After the
 * reboot the image runs on trial: akira_boot_guard.h decides whether it is
 * confirmed or rolled back.
 *
 * @code
 * akira_ota_begin(size, sha256);        // sha256 may be NULL
 * while (more) akira_ota_write(buf, n);
 * akira_ota_finish();                   // verify size/hash, mark for test boot
 * akira_ota_reboot(1000);
 * @endcode
 *
 * One update at a time; calls block on flash erase/write.
 *
 * @stability experimental
 * @since 1.7
 */

#ifndef AKIRA_OTA_H
#define AKIRA_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** True if this build can stage firmware updates (MCUboot + flash map + OTA). */
bool akira_ota_available(void);

/**
 * @brief Start an update.
 * @param image_size  Exact image size in bytes.
 * @param sha256      Expected SHA-256 of the whole image, or NULL. Required
 *                    when CONFIG_AKIRA_OTA_REQUIRE_HASH=y.
 * @return 0, -EBUSY if an update is in progress, -ENOSPC if it does not fit,
 *         -ENOTSUP without OTA support, or negative errno.
 */
int akira_ota_begin(size_t image_size, const uint8_t sha256[32]);

/** Append image data. @return 0 or negative errno (the update is aborted). */
int akira_ota_write(const uint8_t *data, size_t len);

/**
 * @brief Finish: check size and hash, then request a test boot of the new image.
 * @return 0, -EBADMSG on a hash or image mismatch, or negative errno.
 */
int akira_ota_finish(void);

/** Abandon the update in progress. */
int akira_ota_abort(void);

/** Raise AKIRA_HOOK_OTA_PRE_REBOOT, then reboot after @p delay_ms. */
void akira_ota_reboot(uint32_t delay_ms);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_OTA_H */
