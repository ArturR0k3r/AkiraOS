/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_hardening.c
 * ztest suite: hardening
 *
 * Regression tests for the 2026-11 STRIDE review fixes that are pure logic:
 * the app name rule, 32-bit section-size wrap in the WASM/AOT scanners, and
 * the AkiraMesh DATA replay window.
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include <akira_apps.h>
#include "manifest_parser.h"
#include "app_signing.h"
#include "mesh/mesh_session.h"

/* ── App names (they become file names) ─────────────────────────────────── */

ZTEST(hardening, test_app_name_valid)
{
	zassert_true(akira_app_name_valid("hello_world"));
	zassert_true(akira_app_name_valid("gb-game.v2"));
	zassert_true(akira_app_name_valid("2048"));
	zassert_true(akira_app_name_valid("app_0badf00d"));
}

ZTEST(hardening, test_app_name_invalid)
{
	static const char *const bad[] = {
		"", "x/../002_victim", "a/b", "..", "a..b", "notes.", ".hidden",
		"Notes", "WALLET~1", "a b", "a\\b", "a:b", "-lead", "_lead",
		"/abs", "acme/../../../akp/policy",
	};
	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		zassert_false(akira_app_name_valid(bad[i]), "accepted '%s'", bad[i]);
	}
	zassert_false(akira_app_name_valid(NULL));

	char longname[AKIRA_APPS_NAME_LEN + 1];
	memset(longname, 'a', sizeof(longname) - 1);
	longname[sizeof(longname) - 1] = '\0';
	zassert_false(akira_app_name_valid(longname), "overlong name accepted");
	longname[AKIRA_APPS_NAME_LEN - 1] = '\0';
	zassert_true(akira_app_name_valid(longname), "max-length name refused");
}

/* ── Section sizes that wrap a 32-bit size_t ─────────────────────────────── */

/* WASM: magic, version, custom section (id 0) claiming 0xFFFFFFF4 bytes. The
 * body starts at offset 14, so on a 32-bit target "pos + size > total" wraps
 * to 2 and passes; the scanner then jumps back into the header. */
static const uint8_t wasm_wrap[] = {
	0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
	0x00, 0xF4, 0xFF, 0xFF, 0xFF, 0x0F,
	'x', 'x', 'x', 'x',
};

ZTEST(hardening, test_wasm_section_size_wrap)
{
	akira_manifest_t m;

	zassert_true(manifest_parse_wasm_section(wasm_wrap, sizeof(wasm_wrap), &m) < 0);
	zassert_equal(app_verify_wasm_integrity(wasm_wrap, sizeof(wasm_wrap), NULL), -EINVAL);
}

/* Custom section whose name length runs past the section. */
static const uint8_t wasm_name_wrap[] = {
	0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x06, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 'x',
};

ZTEST(hardening, test_wasm_custom_name_wrap)
{
	akira_manifest_t m;

	zassert_true(manifest_parse_wasm_section(wasm_name_wrap, sizeof(wasm_name_wrap), &m) < 0);
}

/* AOT: magic, version, section type 100 (custom) with size 0xFFFFFFF8: the
 * body starts at 16, "pos + size" wraps to 8 and the old check passed, so the
 * name compare read past the buffer and the scan restarted from offset 8. */
static const uint8_t aot_wrap[] = {
	0x00, 'a', 'o', 't', 0x03, 0x00, 0x00, 0x00,
	100, 0x00, 0x00, 0x00, 0xF8, 0xFF, 0xFF, 0xFF,
	0x00, 0x00, 0x00, 0x00, 0x10, 0x00, '.', 'a',
};

ZTEST(hardening, test_aot_section_size_wrap)
{
	akira_manifest_t m;

	zassert_true(manifest_parse_wasm_section(aot_wrap, sizeof(aot_wrap), &m) < 0);
}

/* ── AkiraMesh replay window ─────────────────────────────────────────────── */

ZTEST(hardening, test_mesh_replay_window)
{
	struct session_entry e;

	memset(&e, 0, sizeof(e));
	zassert_true(mesh_session_rx_accept(&e, 100), "first frame");
	zassert_false(mesh_session_rx_accept(&e, 100), "exact replay");
	zassert_true(mesh_session_rx_accept(&e, 105), "newer");
	zassert_true(mesh_session_rx_accept(&e, 103), "late but unseen");
	zassert_false(mesh_session_rx_accept(&e, 103), "late replay");
	zassert_false(mesh_session_rx_accept(&e, 100), "old replay");
	zassert_false(mesh_session_rx_accept(&e, 105 - 40), "older than the window");
	zassert_true(mesh_session_rx_accept(&e, 105 + 200), "big jump");
	zassert_false(mesh_session_rx_accept(&e, 105), "now outside the window");
	/* 16-bit wrap-around counts as newer. */
	memset(&e, 0, sizeof(e));
	zassert_true(mesh_session_rx_accept(&e, 0xFFFE));
	zassert_true(mesh_session_rx_accept(&e, 0x0001));
	zassert_false(mesh_session_rx_accept(&e, 0xFFFE));
}

ZTEST_SUITE(hardening, NULL, NULL, NULL, NULL, NULL);
