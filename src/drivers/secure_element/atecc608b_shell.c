/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file atecc608b_shell.c
 * @brief `atecc` shell commands — secure element bring-up.
 */

#include <drivers/secure_element/atecc608b.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <stdlib.h>

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	uint32_t revision;
	int rc = atecc608b_get_revision(&revision);

	if (rc != 0) {
		shell_error(sh, "no response from the secure element (%d)", rc);
		return rc;
	}

	shell_print(sh, "ATECC608B");
	shell_print(sh, "  revision : 0x%08x", revision);

	uint8_t serial[ATECC608B_SERIAL_LEN];

	if (atecc608b_get_serial(serial) == 0) {
		shell_fprintf(sh, SHELL_NORMAL, "  serial   : ");
		for (int i = 0; i < ATECC608B_SERIAL_LEN; i++) {
			shell_fprintf(sh, SHELL_NORMAL, "%02x", serial[i]);
		}
		shell_print(sh, "");
	}

	bool config_locked = false;
	bool data_locked = false;

	if (atecc608b_get_lock_state(&config_locked, &data_locked) == 0) {
		shell_print(sh, "  config   : %s",
			    config_locked ? "locked" : "UNLOCKED");
		shell_print(sh, "  data/OTP : %s",
			    data_locked ? "locked" : "UNLOCKED");

		if (!config_locked || !data_locked) {
			shell_warn(sh, "part is blank — no provisioned keys");
		}
	}

	return 0;
}

static int cmd_serial(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	uint8_t serial[ATECC608B_SERIAL_LEN];
	int rc = atecc608b_get_serial(serial);

	if (rc != 0) {
		shell_error(sh, "serial unavailable (%d)", rc);
		return rc;
	}

	for (int i = 0; i < ATECC608B_SERIAL_LEN; i++) {
		shell_fprintf(sh, SHELL_NORMAL, "%02x", serial[i]);
	}
	shell_print(sh, "");
	return 0;
}

static int cmd_random(const struct shell *sh, size_t argc, char **argv)
{
	int len = (argc > 1) ? atoi(argv[1]) : 32;

	if (len < 1 || len > ATECC608B_RANDOM_LEN) {
		shell_error(sh, "length must be 1..%d", ATECC608B_RANDOM_LEN);
		return -EINVAL;
	}

	uint8_t buf[ATECC608B_RANDOM_LEN];
	int rc = atecc608b_random(buf, (size_t)len);

	if (rc < 0) {
		shell_error(sh, "RNG failed (%d)", rc);
		return rc;
	}

	for (int i = 0; i < rc; i++) {
		shell_fprintf(sh, SHELL_NORMAL, "%02x", buf[i]);
	}
	shell_print(sh, "");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	atecc_cmds,
	SHELL_CMD(info, NULL, "Revision, serial number and zone lock state", cmd_info),
	SHELL_CMD(serial, NULL, "Print the factory serial number", cmd_serial),
	SHELL_CMD_ARG(random, NULL, "Draw N random bytes from the TRNG (default 32)",
		      cmd_random, 1, 1),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(atecc, &atecc_cmds, "ATECC608B secure element", NULL);
