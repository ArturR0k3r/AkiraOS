/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_ble_api
#include <zephyr/logging/log.h>
#include <crystal/akira_seal.h>
LOG_MODULE_REGISTER(akira_ble_api, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_ble_api.c
 * @brief Arduino-style BLE WASM native API implementation.
 *
 * Bridges WASM BLE calls to the ble_app_service layer.
 * All functions are guarded by AKIRA_CAP_BLE.
 *
 * There is one app BLE layer (GATT service pool, BLE_APP mode, the app
 * advert, the device name), so it belongs to one app at a time: the first app
 * that sets anything up owns it until it calls ble_deinit() or ends; other
 * apps get -EBUSY. When the owner ends, however it ends, ble_release() stops
 * its advert, removes its GATT services, and restores the device name,
 * bonding and BT mode. Operations that reach the controller or flash
 * (advertising, service registration, renames) are rate-limited.
 */

#include "akira_api.h"
#include "akira_ble_api.h"
#include "runtime/security.h"

#ifdef CONFIG_AKIRA_WASM_BLE

#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <akira_app_containment.h>
#include "akira_wasm_mem.h"
#include "mem_helper.h"
#include "connectivity/bluetooth/ble_app_service.h"
#include "connectivity/bluetooth/bt_manager.h"

#ifdef CONFIG_BT
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#endif

#ifdef CONFIG_BT_DEVICE_NAME_MAX
#define BLE_NAME_MAX CONFIG_BT_DEVICE_NAME_MAX
#else
#define BLE_NAME_MAX 32
#endif

#define BLE_UUID_STR_LEN          36    /* "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" */
#define BLE_PROP_ALL              0x3F  /* BLE_PROP_BROADCAST..BLE_PROP_INDICATE   */
#define BLE_NAME_MIN_INTERVAL_MS  2000  /* a rename is a flash write on prod      */

/* GATT database changes (each one is a flash write of the DB hash with GATT
 * caching): a burst of one per service, then one every 2 s. */
#define BLE_DB_BURST              CONFIG_AKIRA_BLE_MAX_SERVICES
#define BLE_DB_PER_MIN            30
/* Advertising start/stop (HCI commands): 8 at once, then 4 a second. */
#define BLE_ADV_BURST             8
#define BLE_ADV_PER_MIN           240

/*
 * Track which service UUID should be included in the advertisement.
 * Set by ble_set_advertised_service(); consumed by ble_advertise().
 */
static int g_adv_svc_handle = -1;

/*
 * Owner of the app BLE layer: app slot + 1, 0 = nobody. Claimed atomically,
 * so two apps can never both own it. Uses the slot, not the module instance
 * (the next app loaded into the slot may get the same instance address).
 * The state below is touched only by the owner's natives and, once it is
 * gone, by ble_release().
 */
static atomic_t g_ble_owner;

static bool g_mode_set;        /* we put the stack in BT_MODE_BLE_APP        */
static bool g_adv_started;     /* the advert running is the owner's          */
static bool g_scan_started;    /* the owner started the BT_MODE_BLE_SCAN scan */
static bool g_spam_started;    /* the owner started BT_MODE_BLE_SPAM          */
static bool g_bondable_off;    /* ble_init() turned bonding off              */
static bool g_name_changed;    /* g_saved_name holds the name to restore     */
static int64_t g_last_name_ms;
static AKIRA_BULK_BSS char g_saved_name[BLE_NAME_MAX + 1];

/* Token buckets, in 1/1000 token. Never reset on teardown, so an app cannot
 * refill them by releasing and re-claiming BLE in a loop. */
struct ble_bucket {
	int32_t tokens;
	int64_t last;   /* 0 = never used: full */
};

static struct ble_bucket g_db_bucket;
static struct ble_bucket g_adv_bucket;

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

/* WASM pointer validation lives in akira_wasm_mem.h (akira_wasm_ptr_to_native). */
#define wasm_ptr_to_native(env, ptr, len) akira_wasm_ptr_to_native((env), (ptr), (len))

/* App-triggered warnings: at most one a second. */
static bool ble_log_allowed(void)
{
	static uint32_t last;
	uint32_t now = k_uptime_get_32();

	if (last != 0U && (now - last) < 1000U) {
		return false;
	}
	last = now ? now : 1U;
	return true;
}

#define BLE_WRN(...)                                                           \
	do {                                                                   \
		if (ble_log_allowed()) {                                       \
			LOG_WRN(__VA_ARGS__);                                  \
		}                                                              \
	} while (0)

static bool bucket_take(struct ble_bucket *b, int burst, int per_min)
{
	const int32_t cap = burst * 1000;
	int64_t now = k_uptime_get();

	if (b->last == 0) {
		b->tokens = cap;
	} else {
		int64_t refill = (now - b->last) * per_min / 60;

		b->tokens = (int32_t)MIN((int64_t)cap, b->tokens + refill);
	}
	b->last = now ? now : 1;

	if (b->tokens < 1000) {
		return false;
	}
	b->tokens -= 1000;
	return true;
}

/* Claim the app BLE layer for the caller, or confirm it already owns it.
 * @return 0, -EBUSY if another app owns it, -EACCES if not an app. */
static int ble_claim(wasm_exec_env_t exec_env)
{
	int slot = akira_runtime_slot_of_exec_env(exec_env);

	if (slot < 0) {
		return -EACCES;
	}

	atomic_val_t me = (atomic_val_t)slot + 1;

	if (atomic_cas(&g_ble_owner, 0, me) || atomic_get(&g_ble_owner) == me) {
		return 0;
	}
	BLE_WRN("BLE is in use by another app (slot %d)", (int)atomic_get(&g_ble_owner) - 1);
	return -EBUSY;
}

/* The caller must already own the app BLE layer.
 * @return 0, -EBUSY if another app owns it, -ENOENT if nobody does. */
static int ble_owned(wasm_exec_env_t exec_env)
{
	int slot = akira_runtime_slot_of_exec_env(exec_env);
	atomic_val_t owner = atomic_get(&g_ble_owner);

	if (slot < 0) {
		return -EACCES;
	}
	if (owner == 0) {
		return -ENOENT;
	}
	return (owner == (atomic_val_t)slot + 1) ? 0 : -EBUSY;
}

static bool uuid_str_ok(const char *s)
{
	/* "$" is NUL-terminated inside linear memory; bound the scan anyway. */
	return strnlen(s, BLE_UUID_STR_LEN + 1) == BLE_UUID_STR_LEN;
}

/*
 * Undo everything the owner set up. Runs on the owner's thread (ble_deinit)
 * or from ble_release() once the owner is gone — then possibly on the system
 * workqueue: every step here is a bounded stack call (HCI command, GATT DB
 * change, settings write).
 */
static void ble_teardown(void)
{
	/* Stop a scan or advertisement spam only if the owner started it and it
	 * is still the active mode: bt_manager_scan_stop()/spam_stop() drop the
	 * stack to BT_MODE_NONE and stop advertising, which must never knock out
	 * another mode (companion, HID, mesh, a shell scan). */
	if (g_spam_started && bt_manager_get_mode() == BT_MODE_BLE_SPAM) {
		bt_manager_spam_stop();
	}
	g_spam_started = false;
	if (g_scan_started && bt_manager_get_mode() == BT_MODE_BLE_SCAN) {
		bt_manager_scan_stop();
	}
	g_scan_started = false;

	if (g_adv_started) {
		bt_manager_stop_advertising();
		g_adv_started = false;
	}

	/* Unregisters the app's GATT services and drops queued events. */
	ble_app_svc_deinit();

#ifdef CONFIG_BT
	/* An app-set name must not outlive the app (it is persisted). */
	if (g_name_changed) {
		(void)bt_set_name(g_saved_name);
		g_name_changed = false;
	}
#if defined(CONFIG_BT_SMP)
	if (g_bondable_off) {
		bt_set_bondable(IS_ENABLED(CONFIG_BT_BONDABLE));
	}
#endif
#endif
	g_bondable_off = false;

	/* Release BLE_APP only if we took it: another mode (companion, mesh,
	 * scan) must never be knocked out by an app. NONE restores HID if it
	 * was running. */
	if (g_mode_set && bt_manager_get_mode() == BT_MODE_BLE_APP) {
		bt_manager_set_mode(BT_MODE_NONE);
	}
	g_mode_set = false;
	g_adv_svc_handle = -1;
}

/* ------------------------------------------------------------------ */
/* WASM-exported native functions                                      */
/* ------------------------------------------------------------------ */

int akira_native_ble_init(wasm_exec_env_t exec_env)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE, -EPERM);

	int ret = ble_claim(exec_env);

	if (ret < 0) {
		return ret;
	}
	if (akira_runtime_stop_requested(exec_env)) {
		return -ECANCELED;
	}

	/* Mode switch (and lazy BT start), settings: system locks. */
	akira_native_critical_enter(exec_env);

	ret = bt_manager_set_mode(BT_MODE_BLE_APP);
	if (ret < 0) {
		akira_native_critical_exit(exec_env);
		BLE_WRN("ble_init: set_mode failed: %d", ret);
		return ret;
	}
	g_mode_set = true;

#if defined(CONFIG_BT_SMP)
	/* A WASM app's GATT services are added dynamically and differ between
	 * apps/boots. If the peer bonds, it caches this attribute table and will
	 * not re-discover our service on later connections (the classic "service
	 * not found" after a service was added). Disable bonding in app mode so
	 * every connection re-discovers the current GATT; teardown restores it. */
	bt_set_bondable(false);
	g_bondable_off = true;
#endif

	ret = ble_app_svc_init();
	akira_native_critical_exit(exec_env);

	if (ret < 0) {
		BLE_WRN("ble_init: svc_init failed: %d", ret);
		return ret;
	}

	g_adv_svc_handle = -1;
	LOG_DBG("BLE app mode initialised");
	return 0;
}

int akira_native_ble_deinit(wasm_exec_env_t exec_env)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE, -EPERM);

	int ret = ble_owned(exec_env);

	if (ret == -ENOENT) {
		return 0; /* nothing set up: never touch another mode's state */
	}
	if (ret < 0) {
		return ret;
	}

	akira_native_critical_enter(exec_env);
	ble_teardown();
	akira_native_critical_exit(exec_env);

	atomic_set(&g_ble_owner, 0);
	LOG_DBG("BLE app mode released");
	return 0;
}

int akira_native_ble_set_local_name(wasm_exec_env_t exec_env,
				    const char *name)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE, -EPERM);

	if (!name) {
		return -EINVAL;
	}
	if (strnlen(name, BLE_NAME_MAX + 1) > BLE_NAME_MAX) {
		return -ENOMEM; /* what bt_set_name() returns for a long name */
	}

	int ret = ble_claim(exec_env);

	if (ret < 0) {
		return ret;
	}

#ifdef CONFIG_BT
	/* bt_set_name() persists to flash on a prod build (BT_SETTINGS +
	 * BT_DEVICE_NAME_DYNAMIC). Skip a rename to the current name, and
	 * rate-limit changes so an app cannot wear out flash by spamming
	 * renames. */
	if (strcmp(name, bt_get_name()) == 0) {
		return 0;
	}

	int64_t now = k_uptime_get();

	if (g_last_name_ms != 0 && (now - g_last_name_ms) < BLE_NAME_MIN_INTERVAL_MS) {
		return -EBUSY;
	}

	if (!g_name_changed) {
		strncpy(g_saved_name, bt_get_name(), sizeof(g_saved_name) - 1);
		g_saved_name[sizeof(g_saved_name) - 1] = '\0';
	}

	akira_native_critical_enter(exec_env);
	ret = bt_set_name(name);
	akira_native_critical_exit(exec_env);

	if (ret < 0) {
		BLE_WRN("bt_set_name failed: %d", ret);
		return ret;
	}
	g_name_changed = true;
	g_last_name_ms = now ? now : 1;
#endif

	LOG_DBG("BLE local name set to \"%s\"", name);
	return 0;
}

int akira_native_ble_service_create(wasm_exec_env_t exec_env,
				    const char *uuid128_str)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE, -EPERM);

	if (!uuid128_str || !uuid_str_ok(uuid128_str)) {
		return -EINVAL;
	}

	int ret = ble_claim(exec_env);

	if (ret < 0) {
		return ret;
	}

	akira_native_critical_enter(exec_env);
	ret = ble_app_svc_alloc(uuid128_str);
	akira_native_critical_exit(exec_env);
	return ret;
}

int akira_native_ble_char_create(wasm_exec_env_t exec_env,
				 const char *uuid128_str,
				 int32_t props,
				 int32_t max_len)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE, -EPERM);

	if (!uuid128_str || !uuid_str_ok(uuid128_str) ||
	    max_len <= 0 || max_len > CONFIG_AKIRA_BLE_CHAR_MAX_LEN ||
	    (props & ~BLE_PROP_ALL) != 0) {
		return -EINVAL;
	}

	int ret = ble_claim(exec_env);

	if (ret < 0) {
		return ret;
	}

	akira_native_critical_enter(exec_env);
	ret = ble_app_char_alloc(uuid128_str, (uint8_t)props, (uint16_t)max_len);
	akira_native_critical_exit(exec_env);
	return ret;
}

int akira_native_ble_service_add_char(wasm_exec_env_t exec_env,
				      int32_t svc_h, int32_t char_h)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE, -EPERM);

	int ret = ble_owned(exec_env);

	if (ret < 0) {
		return ret;
	}

	akira_native_critical_enter(exec_env);
	ret = ble_app_svc_add_char(svc_h, char_h);
	akira_native_critical_exit(exec_env);
	return ret;
}

int akira_native_ble_add_service(wasm_exec_env_t exec_env, int32_t svc_h)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE, -EPERM);

	int ret = ble_owned(exec_env);

	if (ret < 0) {
		return ret;
	}
	if (akira_runtime_stop_requested(exec_env)) {
		return -ECANCELED;
	}
	if (!bucket_take(&g_db_bucket, BLE_DB_BURST, BLE_DB_PER_MIN)) {
		BLE_WRN("ble_add_service: rate-limited");
		return -EBUSY;
	}

	/* GATT DB change: GATT lock, settings write. */
	akira_native_critical_enter(exec_env);
	ret = ble_app_svc_register((int)svc_h);
	akira_native_critical_exit(exec_env);
	return ret;
}

int akira_native_ble_set_advertised_service(wasm_exec_env_t exec_env,
					    int32_t svc_h)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE, -EPERM);

	int ret = ble_claim(exec_env);

	if (ret < 0) {
		return ret;
	}

	g_adv_svc_handle = (int)svc_h;
	LOG_DBG("Advertised service handle set to %d", g_adv_svc_handle);
	return 0;
}

int akira_native_ble_advertise(wasm_exec_env_t exec_env)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE, -EPERM);

	int ret = ble_owned(exec_env);

	if (ret < 0) {
		return (ret == -ENOENT) ? -EINVAL : ret;
	}
	if (bt_manager_get_mode() != BT_MODE_BLE_APP) {
		BLE_WRN("ble_advertise: not in BLE_APP mode (call ble_init first)");
		return -EINVAL;
	}
	if (akira_runtime_stop_requested(exec_env)) {
		return -ECANCELED;
	}
	if (!bucket_take(&g_adv_bucket, BLE_ADV_BURST, BLE_ADV_PER_MIN)) {
		return -EBUSY;
	}

	/* Copied by the stack: the UUID lives in the service pool, not in app
	 * memory. */
	const uint8_t *uuid128 = NULL;

	if (g_adv_svc_handle >= 0) {
		uuid128 = ble_app_svc_get_uuid128(g_adv_svc_handle);
	}

	/* Only an advert we started is ours to stop later. */
	bool already = bt_manager_get_state() == BT_STATE_ADVERTISING;

	akira_native_critical_enter(exec_env);
	ret = bt_manager_start_advertising_custom(uuid128);
	akira_native_critical_exit(exec_env);

	if (ret == 0 && !already) {
		g_adv_started = true;
	}
	return ret;
}

int akira_native_ble_stop_advertise(wasm_exec_env_t exec_env)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE, -EPERM);

	int ret = ble_owned(exec_env);

	if (ret == -ENOENT) {
		return 0;
	}
	if (ret < 0) {
		return ret;
	}
	if (!g_adv_started) {
		return 0; /* HID / companion adverts are not the app's to stop */
	}
	if (!bucket_take(&g_adv_bucket, BLE_ADV_BURST, BLE_ADV_PER_MIN)) {
		return -EBUSY;
	}

	akira_native_critical_enter(exec_env);
	ret = bt_manager_stop_advertising();
	akira_native_critical_exit(exec_env);
	g_adv_started = false;
	return ret;
}

int akira_native_ble_is_connected(wasm_exec_env_t exec_env)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE, -EPERM);

	return bt_manager_is_connected() ? 1 : 0;
}

int akira_native_ble_char_write(wasm_exec_env_t exec_env,
				int32_t char_h,
				uint32_t data_ptr, uint32_t len)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE, -EPERM);

	if (len == 0) {
		return -EINVAL;
	}
	if (len > CONFIG_AKIRA_BLE_CHAR_MAX_LEN) {
		return -EMSGSIZE;
	}

	const uint8_t *data = (const uint8_t *)wasm_ptr_to_native(
		exec_env, data_ptr, len);

	if (!data) {
		BLE_WRN("ble_char_write: invalid WASM pointer 0x%x len=%u",
			data_ptr, len);
		return -EFAULT;
	}

	int ret = ble_owned(exec_env);

	if (ret < 0) {
		return ret;
	}

	/* A notification waits for an ATT buffer inside the stack (holding a
	 * connection reference): never abort the thread in there. The data is
	 * copied into the slot and the buffer before this returns. */
	akira_native_critical_enter(exec_env);
	ret = ble_app_char_write((int)char_h, data, (uint16_t)len);
	akira_native_critical_exit(exec_env);
	return ret;
}

int akira_native_ble_char_read(wasm_exec_env_t exec_env,
			       int32_t char_h,
			       uint32_t buf_ptr, uint32_t len)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE, -EPERM);

	if (len == 0) {
		return -EINVAL;
	}

	uint8_t *buf = (uint8_t *)wasm_ptr_to_native(exec_env, buf_ptr, len);

	if (!buf) {
		BLE_WRN("ble_char_read: invalid WASM pointer");
		return -EFAULT;
	}

	int ret = ble_owned(exec_env);

	if (ret < 0) {
		return ret;
	}

	return ble_app_char_read((int)char_h, buf, (uint16_t)MIN(len, UINT16_MAX));
}

int akira_native_ble_event_pop(wasm_exec_env_t exec_env,
			       uint32_t buf_ptr, uint32_t len)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE, -EPERM);

	/* Minimum buffer: 1 (type) + 1 (char_h) + 2 (data_len) = 4 bytes */
	if (len < 4) {
		return -EINVAL;
	}

	uint8_t *buf = (uint8_t *)wasm_ptr_to_native(exec_env, buf_ptr, len);

	if (!buf) {
		return -EFAULT;
	}

	/* Only the owner reads the app layer's events. */
	int ret = ble_owned(exec_env);

	if (ret == -ENOENT) {
		return BLE_EVT_NONE;
	}
	if (ret < 0) {
		return ret;
	}

	struct ble_event evt;
	int evt_type = ble_app_event_pop(&evt);

	if (evt_type <= BLE_EVT_NONE) {
		return evt_type; /* 0 = empty, -errno = error */
	}

	/* Serialise event into WASM buffer:
	 *   [0]   type  (uint8)
	 *   [1]   char_handle (uint8)
	 *   [2-3] data_len LE (uint16)
	 *   [4..] data bytes
	 */
	buf[0] = evt.type;
	buf[1] = evt.char_handle;
	buf[2] = (uint8_t)(evt.data_len & 0xFF);
	buf[3] = (uint8_t)(evt.data_len >> 8);

	if (evt.data_len > 0) {
		size_t copy = MIN((size_t)(len - 4), (size_t)evt.data_len);

		copy = MIN(copy, sizeof(evt.data));
		memcpy(buf + 4, evt.data, copy);
	}

	return evt_type;
}

/* ------------------------------------------------------------------ */
/* Passive BLE scan and advertisement spam (console features)           */
/* ------------------------------------------------------------------ */

/*
 * Both run in their own BT mode (BT_MODE_BLE_SCAN / BT_MODE_BLE_SPAM) and,
 * like the GATT layer, belong to the app that owns the app BLE layer: the
 * starter claims it, only the owner stops them or reads scan reports, and
 * ble_teardown() stops whichever of them the owner left running.
 */

int akira_native_ble_scan_start(wasm_exec_env_t exec_env, int32_t active)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE_SCAN, -EPERM);

	int ret = ble_claim(exec_env);

	if (ret < 0) {
		return ret;
	}
	if (akira_runtime_stop_requested(exec_env)) {
		return -ECANCELED;
	}

	/* Mode switch (and lazy BT start), HCI commands: system locks. */
	akira_native_critical_enter(exec_env);
	ret = bt_manager_scan_start(active != 0);
	akira_native_critical_exit(exec_env);

	if (ret == 0) {
		g_scan_started = true;
	}
	return ret;
}

int akira_native_ble_scan_stop(wasm_exec_env_t exec_env)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE_SCAN, -EPERM);

	int ret = ble_owned(exec_env);

	if (ret == -ENOENT) {
		return 0; /* nothing set up: never stop another mode's scan */
	}
	if (ret < 0) {
		return ret;
	}
	if (!g_scan_started) {
		return 0;
	}

	akira_native_critical_enter(exec_env);
	if (bt_manager_get_mode() == BT_MODE_BLE_SCAN) {
		ret = bt_manager_scan_stop();
	}
	akira_native_critical_exit(exec_env);
	g_scan_started = false;
	return ret;
}

int akira_native_ble_scan_pop(wasm_exec_env_t exec_env,
			      uint32_t buf_ptr, uint32_t len)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE_SCAN, -EPERM);

	if (len < sizeof(struct ble_scan_report)) {
		return -EINVAL;
	}

	uint8_t *buf = (uint8_t *)wasm_ptr_to_native(exec_env, buf_ptr, len);

	if (!buf) {
		return -EFAULT;
	}

	/* Only the owner reads the reports of the scan it started. */
	int ret = ble_owned(exec_env);

	if (ret == -ENOENT) {
		return 0;
	}
	if (ret < 0) {
		return ret;
	}

	struct ble_scan_report rep;

	ret = bt_manager_scan_pop(&rep);
	if (ret <= 0) {
		return ret; /* 0 = empty, <0 = errno */
	}

	memcpy(buf, &rep, sizeof(rep));
	return 1;
}

int akira_native_ble_spam_start(wasm_exec_env_t exec_env, int32_t preset)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE_SPAM, -EPERM);
	AKIRA_SEAL_GATE_OR_RETURN(exec_env, AKIRA_SEAL_BLE_SPAM, NULL, -EACCES);

	int ret = ble_claim(exec_env);

	if (ret < 0) {
		return ret;
	}
	if (akira_runtime_stop_requested(exec_env)) {
		return -ECANCELED;
	}

	akira_native_critical_enter(exec_env);
	ret = bt_manager_spam_start((int)preset);
	akira_native_critical_exit(exec_env);

	if (ret == 0) {
		g_spam_started = true;
	}
	return ret;
}

int akira_native_ble_spam_stop(wasm_exec_env_t exec_env)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE_SPAM, -EPERM);

	int ret = ble_owned(exec_env);

	if (ret == -ENOENT) {
		return 0;
	}
	if (ret < 0) {
		return ret;
	}
	if (!g_spam_started) {
		return 0;
	}

	/* Cancels the spam work synchronously and stops the advert. */
	akira_native_critical_enter(exec_env);
	if (bt_manager_get_mode() == BT_MODE_BLE_SPAM) {
		ret = bt_manager_spam_stop();
	}
	akira_native_critical_exit(exec_env);
	g_spam_started = false;
	return ret;
}

int akira_native_ble_spam_packet_count(wasm_exec_env_t exec_env)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE_SPAM, -EPERM);

	return (int)bt_manager_spam_packet_count();
}

/* ------------------------------------------------------------------ */
/* App teardown                                                        */
/* ------------------------------------------------------------------ */

/*
 * Only the owner's BLE state is torn down — another app's advert and
 * services survive this app's exit. Idempotent: ownership is cleared last,
 * and a second call finds nothing owned.
 */
static void ble_release(int slot, const char *app_name)
{
	ARG_UNUSED(app_name);

	if (slot < 0 || atomic_get(&g_ble_owner) != (atomic_val_t)slot + 1) {
		return;
	}

	ble_teardown();
	atomic_set(&g_ble_owner, 0);
	LOG_INF("BLE released for app slot %d", slot);
}

AKIRA_APP_RESOURCE_DEFINE(ble, 50, NULL, ble_release);

#else /* !CONFIG_AKIRA_WASM_BLE */

/* Stubs so the linker does not complain if the header is included elsewhere */
int akira_native_ble_init(wasm_exec_env_t e)
{ (void)e; return -ENOTSUP; }
int akira_native_ble_deinit(wasm_exec_env_t e)
{ (void)e; return -ENOTSUP; }
int akira_native_ble_set_local_name(wasm_exec_env_t e, const char *n)
{ (void)e; (void)n; return -ENOTSUP; }
int akira_native_ble_service_create(wasm_exec_env_t e, const char *u)
{ (void)e; (void)u; return -ENOTSUP; }
int akira_native_ble_char_create(wasm_exec_env_t e, const char *u,
				 int32_t p, int32_t m)
{ (void)e; (void)u; (void)p; (void)m; return -ENOTSUP; }
int akira_native_ble_service_add_char(wasm_exec_env_t e, int32_t s, int32_t c)
{ (void)e; (void)s; (void)c; return -ENOTSUP; }
int akira_native_ble_add_service(wasm_exec_env_t e, int32_t s)
{ (void)e; (void)s; return -ENOTSUP; }
int akira_native_ble_set_advertised_service(wasm_exec_env_t e, int32_t s)
{ (void)e; (void)s; return -ENOTSUP; }
int akira_native_ble_advertise(wasm_exec_env_t e)    { (void)e; return -ENOTSUP; }
int akira_native_ble_stop_advertise(wasm_exec_env_t e) { (void)e; return -ENOTSUP; }
int akira_native_ble_is_connected(wasm_exec_env_t e) { (void)e; return -ENOTSUP; }
int akira_native_ble_char_write(wasm_exec_env_t e, int32_t h,
				uint32_t p, uint32_t l)
{ (void)e; (void)h; (void)p; (void)l; return -ENOTSUP; }
int akira_native_ble_char_read(wasm_exec_env_t e, int32_t h,
			       uint32_t p, uint32_t l)
{ (void)e; (void)h; (void)p; (void)l; return -ENOTSUP; }
int akira_native_ble_event_pop(wasm_exec_env_t e, uint32_t p, uint32_t l)
{ (void)e; (void)p; (void)l; return -ENOTSUP; }
int akira_native_ble_scan_start(wasm_exec_env_t e, int32_t a)
{ (void)e; (void)a; return -ENOTSUP; }
int akira_native_ble_scan_stop(wasm_exec_env_t e) { (void)e; return -ENOTSUP; }
int akira_native_ble_scan_pop(wasm_exec_env_t e, uint32_t p, uint32_t l)
{ (void)e; (void)p; (void)l; return -ENOTSUP; }
int akira_native_ble_spam_start(wasm_exec_env_t e, int32_t p)
{ (void)e; (void)p; return -ENOTSUP; }
int akira_native_ble_spam_stop(wasm_exec_env_t e) { (void)e; return -ENOTSUP; }
int akira_native_ble_spam_packet_count(wasm_exec_env_t e) { (void)e; return -ENOTSUP; }

#endif /* CONFIG_AKIRA_WASM_BLE */

/* ===== WASM exports =====
 * Registered with WAMR by the native API registry (akira_native_registry.h).
 * Import names and signatures are WASM ABI: see docs/api-stability-policy.md. */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && (defined(CONFIG_AKIRA_WASM_BLE))
#include <akira_native_registry.h>

static const NativeSymbol akira_ble_natives[] = {
    {"ble_init", (void *)akira_native_ble_init, "()i", NULL},
    {"ble_deinit", (void *)akira_native_ble_deinit, "()i", NULL},
    {"ble_set_local_name", (void *)akira_native_ble_set_local_name, "($)i", NULL},
    {"ble_service_create", (void *)akira_native_ble_service_create, "($)i", NULL},
    {"ble_char_create", (void *)akira_native_ble_char_create, "($ii)i", NULL},
    {"ble_service_add_char", (void *)akira_native_ble_service_add_char, "(ii)i", NULL},
    {"ble_add_service", (void *)akira_native_ble_add_service, "(i)i", NULL},
    {"ble_set_advertised_service", (void *)akira_native_ble_set_advertised_service, "(i)i", NULL},
    {"ble_advertise", (void *)akira_native_ble_advertise, "()i", NULL},
    {"ble_stop_advertise", (void *)akira_native_ble_stop_advertise, "()i", NULL},
    {"ble_is_connected", (void *)akira_native_ble_is_connected, "()i", NULL},
    {"ble_char_write", (void *)akira_native_ble_char_write, "(iii)i", NULL},
    {"ble_char_read", (void *)akira_native_ble_char_read, "(iii)i", NULL},
    {"ble_event_pop", (void *)akira_native_ble_event_pop, "(ii)i", NULL},
    /* Passive scan + advertisement spam (console features; spam is Seal-gated) */
    {"ble_scan_start", (void *)akira_native_ble_scan_start, "(i)i", NULL},
    {"ble_scan_stop", (void *)akira_native_ble_scan_stop, "()i", NULL},
    {"ble_scan_pop", (void *)akira_native_ble_scan_pop, "(ii)i", NULL},
    {"ble_spam_start", (void *)akira_native_ble_spam_start, "(i)i", NULL},
    {"ble_spam_stop", (void *)akira_native_ble_spam_stop, "()i", NULL},
    {"ble_spam_packet_count", (void *)akira_native_ble_spam_packet_count, "()i", NULL},
};

AKIRA_NATIVE_API_DEFINE(akira_ble_api, "env", akira_ble_natives);
#endif
