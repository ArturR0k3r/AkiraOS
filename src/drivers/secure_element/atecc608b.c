/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file atecc608b.c
 * @brief Microchip ATECC608B secure element driver.
 *
 * ── Protocol notes ───────────────────────────────────────────────────────
 *
 * The part is an I2C slave with a one-byte "word address" prefix selecting
 * what a transaction means: 0x00 reset the address counter, 0x01 sleep,
 * 0x02 idle, 0x03 a command packet.
 *
 * A command packet is:
 *     [count][opcode][param1][param2 lo][param2 hi][data...][crc lo][crc hi]
 * where count covers everything from itself through the CRC, and the CRC is
 * the ATCA CRC-16 (polynomial 0x8005, bit-reversed feed) over the packet up to
 * but not including the CRC itself.
 *
 * A response is [count][data...][crc lo][crc hi]. A single-byte body is a
 * status/error code rather than data.
 *
 * ── Waking it ────────────────────────────────────────────────────────────
 *
 * There is no wake pin. The part wakes when SDA is held low for at least
 * tWLO (60 µs), so the idiomatic trick — used here — is to address I2C
 * device 0x00 and clock out a zero byte. Nothing acknowledges, and the -EIO
 * that comes back is the expected outcome, not a failure. At 100 kHz that
 * byte holds SDA low for roughly 80 µs; above about 133 kHz the pulse is too
 * short and the part never wakes. That is why the Latch I2C bus is pinned to
 * standard mode — see boards/latch/latch-pinctrl.dtsi.
 *
 * The driver returns the part to sleep after every exchange. On a device
 * budgeted for two years on one ER14505 cell, leaving a secure element idling
 * at ~800 µA would dominate the entire power budget. Operations that need
 * state across commands (TempKey for Sign, the SHA context for HMAC) run all
 * their commands inside one wake.
 *
 * Origin: latch-fw (upstreamed in AkiraOS 1.7). The key, signing, HMAC, write
 * and lock commands were added here from the ATECC608B datasheet and are not
 * yet validated on hardware.
 */

#include <drivers/secure_element/atecc608b.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(atecc608b, CONFIG_AKIRA_LOG_LEVEL);

#define ATECC_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(microchip_atecc608b)

#if !DT_NODE_EXISTS(ATECC_NODE)
#error "CONFIG_AKIRA_ATECC608B=y but no microchip,atecc608b node in the device tree"
#endif

/* ---- Word addresses ---------------------------------------------------- */
#define ATECC_WA_RESET 0x00
#define ATECC_WA_SLEEP 0x01
#define ATECC_WA_IDLE 0x02
#define ATECC_WA_COMMAND 0x03

/* ---- Opcodes ----------------------------------------------------------- */
#define ATECC_OP_READ 0x02
#define ATECC_OP_WRITE 0x12
#define ATECC_OP_NONCE 0x16
#define ATECC_OP_LOCK 0x17
#define ATECC_OP_RANDOM 0x1B
#define ATECC_OP_INFO 0x30
#define ATECC_OP_GENKEY 0x40
#define ATECC_OP_SIGN 0x41
#define ATECC_OP_SHA 0x47

/* Mode / param values used below. */
#define ATECC_GENKEY_PRIVATE 0x04   /* create a key, return its public key */
#define ATECC_GENKEY_PUBLIC 0x00    /* recompute the public key of a slot */
#define ATECC_NONCE_PASSTHROUGH 0x03
#define ATECC_SIGN_EXTERNAL 0x80    /* sign the digest held in TempKey */
#define ATECC_SHA_UPDATE 0x01
#define ATECC_SHA_HMAC_START 0x04
#define ATECC_SHA_HMAC_END 0x05
#define ATECC_LOCK_NO_CRC 0x80      /* skip the summary CRC check */
#define ATECC_LOCK_ZONE_CONFIG 0x00
#define ATECC_LOCK_ZONE_DATA 0x01
#define ATECC_LOCK_ZONE_SLOT 0x02
#define ATECC_ZONE_DATA 0x02

/* Info modes */
#define ATECC_INFO_MODE_REVISION 0x00

/* Read param1 flags: bit 7 selects a 32-byte read, bits 0-1 the zone. */
#define ATECC_READ_32 0x80
#define ATECC_ZONE_CONFIG 0x00

/* Worst-case execution times from the datasheet, rounded up. The part NACKs
 * reads until it is done, but polling costs more bus time than simply waiting
 * the documented maximum for the two commands used here. */
#define ATECC_EXEC_INFO_MS 5
#define ATECC_EXEC_READ_MS 5
#define ATECC_EXEC_RANDOM_MS 25
#define ATECC_EXEC_GENKEY_MS 120
#define ATECC_EXEC_SIGN_MS 120
#define ATECC_EXEC_NONCE_MS 20
#define ATECC_EXEC_SHA_MS 25
#define ATECC_EXEC_WRITE_MS 50
#define ATECC_EXEC_LOCK_MS 50

/* Wake timing. */
#define ATECC_WAKE_DELAY_US 1500
#define ATECC_WAKE_RESPONSE_LEN 4

/* Config-zone byte offsets. */
#define ATECC_CFG_LOCK_VALUE 86  /* data/OTP zone: 0x55 unlocked */
#define ATECC_CFG_LOCK_CONFIG 87 /* config zone:   0x55 unlocked */
#define ATECC_LOCK_UNLOCKED 0x55

/* Largest packet either direction: a 64-byte SHA block or public key plus
 * framing. */
#define ATECC_BUF_MAX 80

/* ------------------------------------------------------------------------ */

static const struct i2c_dt_spec s_bus = I2C_DT_SPEC_GET(ATECC_NODE);
static const uint8_t s_wake_retries = DT_PROP(ATECC_NODE, wake_retries);

static struct {
	bool present;
	uint32_t revision;
	uint8_t serial[ATECC608B_SERIAL_LEN];
} s_dev;

static K_MUTEX_DEFINE(s_lock);

/* ------------------------------------------------------------------------
 * ATCA CRC-16
 * --------------------------------------------------------------------- */

/* Polynomial 0x8005, fed least-significant-bit first per byte. This is the
 * algorithm in the CryptoAuthentication datasheet; it is not any of the
 * common named CRC-16 variants, so it is spelled out rather than borrowed. */
static uint16_t atca_crc16(const uint8_t *data, size_t len)
{
	uint16_t crc = 0;

	for (size_t i = 0; i < len; i++) {
		for (uint8_t shift = 0x01; shift > 0x00; shift <<= 1) {
			uint8_t data_bit = (data[i] & shift) ? 1 : 0;
			uint8_t crc_bit = crc >> 15;

			crc <<= 1;
			if (data_bit != crc_bit) {
				crc ^= 0x8005;
			}
		}
	}

	return crc;
}

/* ------------------------------------------------------------------------
 * Transport
 * --------------------------------------------------------------------- */

static void atecc_sleep(void)
{
	uint8_t wa = ATECC_WA_SLEEP;

	(void)i2c_write_dt(&s_bus, &wa, 1);
}

static int atecc_wake(void)
{
	uint8_t zero = 0x00;
	uint8_t resp[ATECC_WAKE_RESPONSE_LEN];

	for (uint8_t attempt = 0; attempt < s_wake_retries; attempt++) {
		/* Address 0x00 with a zero byte. Nothing ACKs this — the point
		 * is the ~80 µs SDA-low pulse it produces, not the transfer, so
		 * the error return is discarded on purpose. */
		(void)i2c_write(s_bus.bus, &zero, 1, 0x00);

		k_busy_wait(ATECC_WAKE_DELAY_US);

		if (i2c_read_dt(&s_bus, resp, sizeof(resp)) != 0) {
			continue;
		}

		/* The documented wake response, CRC and all. */
		if (resp[0] == 0x04 && resp[1] == 0x11 && resp[2] == 0x33 &&
		    resp[3] == 0x43) {
			return 0;
		}

		LOG_DBG("unexpected wake response %02x %02x %02x %02x", resp[0],
			resp[1], resp[2], resp[3]);
	}

	return -ENODEV;
}

/**
 * Send one command and collect its response. The part must be awake.
 *
 * @param opcode   Command opcode.
 * @param param1   Command param1.
 * @param param2   Command param2 (little-endian on the wire).
 * @param data     Command data, or NULL.
 * @param data_len Bytes of @p data.
 * @param exec_ms  Documented worst-case execution time.
 * @param out      Response body (without count or CRC), or NULL for commands
 *                 that answer with a status byte only.
 * @param out_len  Bytes expected in @p out (0 for status-only commands).
 */
static int atecc_command(uint8_t opcode, uint8_t param1, uint16_t param2,
			 const uint8_t *data, size_t data_len, uint32_t exec_ms,
			 uint8_t *out, size_t out_len)
{
	uint8_t tx[ATECC_BUF_MAX];
	uint8_t rx[ATECC_BUF_MAX];
	size_t body = out_len ? out_len : 1; /* status-only: one status byte */

	if (body + 3 > sizeof(rx) || data_len + 8 > sizeof(tx)) {
		return -EINVAL;
	}

	/* Word address, then the packet the CRC is computed over. */
	tx[0] = ATECC_WA_COMMAND;
	tx[1] = (uint8_t)(7 + data_len); /* count .. crc */
	tx[2] = opcode;
	tx[3] = param1;
	tx[4] = (uint8_t)(param2 & 0xFF);
	tx[5] = (uint8_t)(param2 >> 8);
	if (data_len) {
		memcpy(&tx[6], data, data_len);
	}

	uint16_t crc = atca_crc16(&tx[1], 5 + data_len);

	tx[6 + data_len] = (uint8_t)(crc & 0xFF);
	tx[7 + data_len] = (uint8_t)(crc >> 8);

	int rc = i2c_write_dt(&s_bus, tx, 8 + data_len);

	memset(tx, 0, sizeof(tx)); /* may have carried key material */
	if (rc != 0) {
		LOG_ERR("command 0x%02x write failed: %d", opcode, rc);
		return rc;
	}

	k_msleep(exec_ms);

	size_t resp_len = body + 3; /* count + body + crc16 */

	rc = i2c_read_dt(&s_bus, rx, resp_len);
	if (rc != 0) {
		LOG_ERR("command 0x%02x read failed: %d", opcode, rc);
		return rc;
	}

	/* A 4-byte packet carrying a single body byte is a status code, not
	 * data — that is how the part reports errors. */
	if (rx[0] == 4 && resp_len != 4) {
		LOG_ERR("command 0x%02x returned status 0x%02x", opcode, rx[1]);
		return -EIO;
	}

	if (rx[0] != resp_len) {
		LOG_ERR("command 0x%02x: expected %zu bytes, packet claims %u",
			opcode, resp_len, rx[0]);
		return -EIO;
	}

	uint16_t want = atca_crc16(rx, resp_len - 2);
	uint16_t got = (uint16_t)rx[resp_len - 2] | ((uint16_t)rx[resp_len - 1] << 8);

	if (want != got) {
		LOG_ERR("command 0x%02x CRC mismatch (%04x != %04x)", opcode, got,
			want);
		return -EIO;
	}

	if (out_len == 0) {
		if (rx[1] != 0x00) {
			LOG_ERR("command 0x%02x returned status 0x%02x", opcode, rx[1]);
			return -EIO;
		}
		return 0;
	}

	memcpy(out, &rx[1], out_len);
	memset(rx, 0, sizeof(rx));
	return 0;
}

/** Wake, run one command, sleep. */
static int atecc_transact_data(uint8_t opcode, uint8_t param1, uint16_t param2,
			       const uint8_t *data, size_t data_len, uint32_t exec_ms,
			       uint8_t *out, size_t out_len)
{
	k_mutex_lock(&s_lock, K_FOREVER);

	int rc = atecc_wake();

	if (rc == 0) {
		rc = atecc_command(opcode, param1, param2, data, data_len, exec_ms,
				   out, out_len);
		atecc_sleep();
	}

	k_mutex_unlock(&s_lock);
	return rc;
}

static int atecc_transact(uint8_t opcode, uint8_t param1, uint16_t param2,
			  uint32_t exec_ms, uint8_t *out, size_t out_len)
{
	return atecc_transact_data(opcode, param1, param2, NULL, 0, exec_ms, out,
				   out_len);
}

/* ------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

int atecc608b_get_revision(uint32_t *revision)
{
	if (!revision) {
		return -EINVAL;
	}

	uint8_t buf[4];
	int rc = atecc_transact(ATECC_OP_INFO, ATECC_INFO_MODE_REVISION, 0,
				ATECC_EXEC_INFO_MS, buf, sizeof(buf));

	if (rc != 0) {
		return rc;
	}

	*revision = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
		    ((uint32_t)buf[2] << 8) | buf[3];
	return 0;
}

int atecc608b_get_serial(uint8_t *out)
{
	if (!out) {
		return -EINVAL;
	}
	if (!s_dev.present) {
		return -ENODEV;
	}

	memcpy(out, s_dev.serial, ATECC608B_SERIAL_LEN);
	return 0;
}

int atecc608b_get_lock_state(bool *config_locked, bool *data_locked)
{
	/* Lock bytes live at config offsets 86 and 87, i.e. block 2, word 5
	 * (bytes 84-87). Address = (block << 3) | word. */
	uint8_t buf[4];
	int rc = atecc_transact(ATECC_OP_READ, ATECC_ZONE_CONFIG, (2 << 3) | 5,
				ATECC_EXEC_READ_MS, buf, sizeof(buf));

	if (rc != 0) {
		return rc;
	}

	if (data_locked) {
		*data_locked = (buf[ATECC_CFG_LOCK_VALUE - 84] != ATECC_LOCK_UNLOCKED);
	}
	if (config_locked) {
		*config_locked = (buf[ATECC_CFG_LOCK_CONFIG - 84] != ATECC_LOCK_UNLOCKED);
	}

	return 0;
}

int atecc608b_random(uint8_t *out, size_t len)
{
	if (!out || len == 0 || len > ATECC608B_RANDOM_LEN) {
		return -EINVAL;
	}

	uint8_t buf[ATECC608B_RANDOM_LEN];
	int rc = atecc_transact(ATECC_OP_RANDOM, 0x00, 0, ATECC_EXEC_RANDOM_MS, buf,
				sizeof(buf));

	if (rc != 0) {
		return rc;
	}

	memcpy(out, buf, len);
	return (int)len;
}

bool atecc608b_is_present(void)
{
	return s_dev.present;
}

/* ------------------------------------------------------------------------
 * Keys, signing, HMAC (not yet validated on hardware)
 * --------------------------------------------------------------------- */

int atecc608b_genkey(uint8_t slot, uint8_t pubkey[ATECC608B_PUBKEY_LEN])
{
	uint8_t buf[ATECC608B_PUBKEY_LEN];

	if (slot >= ATECC608B_SLOTS) {
		return -EINVAL;
	}
	int rc = atecc_transact(ATECC_OP_GENKEY, ATECC_GENKEY_PRIVATE, slot,
				ATECC_EXEC_GENKEY_MS, buf, sizeof(buf));

	if (rc == 0 && pubkey) {
		memcpy(pubkey, buf, sizeof(buf));
	}
	return rc;
}

int atecc608b_get_pubkey(uint8_t slot, uint8_t pubkey[ATECC608B_PUBKEY_LEN])
{
	if (slot >= ATECC608B_SLOTS || !pubkey) {
		return -EINVAL;
	}
	return atecc_transact(ATECC_OP_GENKEY, ATECC_GENKEY_PUBLIC, slot,
			      ATECC_EXEC_GENKEY_MS, pubkey, ATECC608B_PUBKEY_LEN);
}

int atecc608b_sign(uint8_t slot, const uint8_t digest[32],
		   uint8_t sig[ATECC608B_SIG_LEN])
{
	if (slot >= ATECC608B_SLOTS || !digest || !sig) {
		return -EINVAL;
	}

	k_mutex_lock(&s_lock, K_FOREVER);
	int rc = atecc_wake();

	if (rc == 0) {
		/* TempKey only survives while the part is awake. */
		rc = atecc_command(ATECC_OP_NONCE, ATECC_NONCE_PASSTHROUGH, 0,
				   digest, 32, ATECC_EXEC_NONCE_MS, NULL, 0);
		if (rc == 0) {
			rc = atecc_command(ATECC_OP_SIGN, ATECC_SIGN_EXTERNAL, slot,
					   NULL, 0, ATECC_EXEC_SIGN_MS, sig,
					   ATECC608B_SIG_LEN);
		}
		atecc_sleep();
	}
	k_mutex_unlock(&s_lock);
	return rc;
}

int atecc608b_hmac_sha256(uint8_t slot, const uint8_t *data, size_t len,
			  uint8_t mac[32])
{
	if (slot >= ATECC608B_SLOTS || (!data && len) || !mac) {
		return -EINVAL;
	}

	k_mutex_lock(&s_lock, K_FOREVER);
	int rc = atecc_wake();

	if (rc == 0) {
		/* The SHA context is lost in sleep: start, 64-byte updates and
		 * end in one wake. Param2 is the key slot for start and the data
		 * length for update/end. */
		rc = atecc_command(ATECC_OP_SHA, ATECC_SHA_HMAC_START, slot, NULL, 0,
				   ATECC_EXEC_SHA_MS, NULL, 0);
		while (rc == 0 && len >= 64) {
			rc = atecc_command(ATECC_OP_SHA, ATECC_SHA_UPDATE, 64, data, 64,
					   ATECC_EXEC_SHA_MS, NULL, 0);
			data += 64;
			len -= 64;
		}
		if (rc == 0) {
			rc = atecc_command(ATECC_OP_SHA, ATECC_SHA_HMAC_END,
					   (uint16_t)len, data, len,
					   ATECC_EXEC_SHA_MS, mac, 32);
		}
		atecc_sleep();
	}
	k_mutex_unlock(&s_lock);
	return rc;
}

/* ------------------------------------------------------------------------
 * Provisioning: writes, configuration, locks (irreversible; not yet
 * validated on hardware)
 * --------------------------------------------------------------------- */

int atecc608b_write_slot_block(uint8_t slot, uint8_t block, const uint8_t data[32])
{
	if (slot >= ATECC608B_SLOTS || !data) {
		return -EINVAL;
	}
	/* Data-zone address: block in bits 15:8, slot in bits 6:3. */
	uint16_t addr = ((uint16_t)block << 8) | ((uint16_t)slot << 3);

	return atecc_transact_data(ATECC_OP_WRITE, ATECC_ZONE_DATA | ATECC_READ_32,
				   addr, data, 32, ATECC_EXEC_WRITE_MS, NULL, 0);
}

int atecc608b_read_config(uint8_t config[ATECC608B_CONFIG_LEN])
{
	if (!config) {
		return -EINVAL;
	}
	for (uint8_t block = 0; block < 4; block++) {
		int rc = atecc_transact(ATECC_OP_READ, ATECC_ZONE_CONFIG | ATECC_READ_32,
					(uint16_t)block << 3, ATECC_EXEC_READ_MS,
					&config[block * 32], 32);
		if (rc != 0) {
			return rc;
		}
	}
	return 0;
}

int atecc608b_write_config(const uint8_t config[ATECC608B_CONFIG_LEN])
{
	if (!config) {
		return -EINVAL;
	}
	for (size_t off = 16; off < ATECC608B_CONFIG_LEN; off += 4) {
		if (off == 84) {
			continue; /* UserExtra, Selector, LockValue, LockConfig */
		}
		/* Config-zone address: block in bits 4:3, word in bits 2:0. */
		uint16_t addr = (uint16_t)(((off / 32) << 3) | ((off % 32) / 4));
		int rc = atecc_transact_data(ATECC_OP_WRITE, ATECC_ZONE_CONFIG, addr,
					     &config[off], 4, ATECC_EXEC_WRITE_MS,
					     NULL, 0);
		if (rc != 0) {
			LOG_ERR("config write at byte %zu failed: %d", off, rc);
			return rc;
		}
	}
	return 0;
}

int atecc608b_lock_config(void)
{
	LOG_WRN("locking the configuration zone (irreversible)");
	return atecc_transact(ATECC_OP_LOCK, ATECC_LOCK_NO_CRC | ATECC_LOCK_ZONE_CONFIG,
			      0, ATECC_EXEC_LOCK_MS, NULL, 0);
}

int atecc608b_lock_data(void)
{
	LOG_WRN("locking the data and OTP zones (irreversible)");
	return atecc_transact(ATECC_OP_LOCK, ATECC_LOCK_NO_CRC | ATECC_LOCK_ZONE_DATA,
			      0, ATECC_EXEC_LOCK_MS, NULL, 0);
}

int atecc608b_lock_slot(uint8_t slot)
{
	if (slot >= ATECC608B_SLOTS) {
		return -EINVAL;
	}
	LOG_WRN("locking slot %u (irreversible)", slot);
	return atecc_transact(ATECC_OP_LOCK,
			      ATECC_LOCK_NO_CRC | (uint8_t)(slot << 2) | ATECC_LOCK_ZONE_SLOT,
			      0, ATECC_EXEC_LOCK_MS, NULL, 0);
}

/* ------------------------------------------------------------------------
 * Init
 * --------------------------------------------------------------------- */

int atecc608b_init(void)
{
	if (s_dev.present) {
		return 0;
	}

	if (!device_is_ready(s_bus.bus)) {
		LOG_ERR("I2C bus %s not ready", s_bus.bus->name);
		return -ENODEV;
	}

	int rc = atecc608b_get_revision(&s_dev.revision);

	if (rc != 0) {
		LOG_WRN("ATECC608B not responding at 0x%02x (%d) — "
			"device identity unavailable", s_bus.addr, rc);
		return rc;
	}

	/* Serial number is config zone bytes 0-3 and 8-12; one 32-byte read of
	 * block 0 covers both runs. */
	uint8_t cfg[32];

	rc = atecc_transact(ATECC_OP_READ, ATECC_ZONE_CONFIG | ATECC_READ_32, 0,
			    ATECC_EXEC_READ_MS, cfg, sizeof(cfg));
	if (rc == 0) {
		memcpy(&s_dev.serial[0], &cfg[0], 4);
		memcpy(&s_dev.serial[4], &cfg[8], 5);
	} else {
		LOG_WRN("could not read serial number: %d", rc);
	}

	s_dev.present = true;

	bool cfg_locked = false;
	bool data_locked = false;

	(void)atecc608b_get_lock_state(&cfg_locked, &data_locked);

	LOG_INF("ATECC608B rev 0x%08x at 0x%02x", s_dev.revision, s_bus.addr);
	LOG_INF("  serial %02x%02x%02x%02x%02x%02x%02x%02x%02x", s_dev.serial[0],
		s_dev.serial[1], s_dev.serial[2], s_dev.serial[3], s_dev.serial[4],
		s_dev.serial[5], s_dev.serial[6], s_dev.serial[7], s_dev.serial[8]);
	LOG_INF("  zones: config=%s data=%s", cfg_locked ? "locked" : "UNLOCKED",
		data_locked ? "locked" : "UNLOCKED");

	if (!cfg_locked || !data_locked) {
		/* Worth stating loudly: a blank part has no keys, so anything
		 * it "signs" proves nothing. */
		LOG_WRN("part is blank — no provisioned keys, signatures are "
			"not yet meaningful");
	}

	return 0;
}

static int atecc608b_auto_init(void)
{
	/* A missing secure element must not stop the device booting: a Latch
	 * with no hardware identity still detects and logs, it just cannot
	 * attest to what it recorded. */
	(void)atecc608b_init();
	return 0;
}

SYS_INIT(atecc608b_auto_init, APPLICATION, CONFIG_AKIRA_ATECC608B_INIT_PRIORITY);
