/* Simple PSRAM stub interface for AkiraOS Minimalist Architecture
 * If CONFIG_AKIRA_PSRAM is enabled, these will allocate via malloc as a placeholder.
 */

#ifndef AKIRA_PSRAM_H
#define AKIRA_PSRAM_H

#include <stddef.h>
#include <stdbool.h>

bool akira_psram_available(void);
void *akira_psram_alloc(size_t size);
void akira_psram_free(void *ptr);

#endif /* AKIRA_PSRAM_H */
