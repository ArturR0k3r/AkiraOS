/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Read-only registries of the akira-os module. Registered with
 * zephyr_linker_sources(SECTIONS ...): every ESP32 linker template includes
 * snippets-sections.ld but none includes snippets-rom-sections.ld, so the
 * ROM_SECTIONS location would silently drop these tables there.
 */

#include <zephyr/linker/iterable_sections.h>

ITERABLE_SECTION_ROM(akira_capability, Z_LINK_ITERABLE_SUBALIGN)
ITERABLE_SECTION_ROM(akira_native_api, Z_LINK_ITERABLE_SUBALIGN)
ITERABLE_SECTION_ROM(akira_hook, Z_LINK_ITERABLE_SUBALIGN)
ITERABLE_SECTION_ROM(akira_app_admission, Z_LINK_ITERABLE_SUBALIGN)
ITERABLE_SECTION_ROM(akira_net_policy, Z_LINK_ITERABLE_SUBALIGN)
ITERABLE_SECTION_ROM(akira_settings_key_provider, Z_LINK_ITERABLE_SUBALIGN)
ITERABLE_SECTION_ROM(akira_audit_sink, Z_LINK_ITERABLE_SUBALIGN)
ITERABLE_SECTION_ROM(akira_identity_provider, Z_LINK_ITERABLE_SUBALIGN)
