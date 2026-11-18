/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_ble_svc
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_ble_svc, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file ble_app_service.c
 * @brief Dynamic GATT service pool for WASM BLE apps.
 *
 * Slots are reused, but an attribute table that has been registered is
 * never cleared, only rebuilt: Zephyr walks the GATT database without a lock
 * (on the BT RX thread), so a walk that started before a service was
 * unregistered may still be inside its attributes. Unregistering removes the
 * service from the database and flips our flags; every attribute keeps
 * pointing at valid objects, and the read/write callbacks refuse once the
 * characteristic is no longer used. See ble_app_svc_register() for rebuild.
 */

#include "ble_app_service.h"

#ifdef CONFIG_AKIRA_WASM_BLE

#include <zephyr/kernel.h>
#include <string.h>
#include <errno.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include "lib/mem_helper.h"

/* Number of GATT attrs per characteristic slot (decl + value + CCC) */
#define ATTRS_PER_CHAR   3
/* Pool lock wait: -EBUSY after this. Teardown runs from the app release
 * hook, possibly on the system workqueue, so it must never wait forever. */
#define POOL_LOCK_TIMEOUT K_MSEC(1000)
/* Total attrs per service: 1 primary decl + all char attrs */
#define ATTRS_PER_SERVICE (1 + CONFIG_AKIRA_BLE_MAX_CHARS * ATTRS_PER_CHAR)

/*===========================================================================*/
/* Internal Structures                                                       */
/*===========================================================================*/

/** Per-characteristic slot */
struct ble_char_slot {
	struct bt_uuid_128 uuid;
	struct bt_gatt_chrc chrc_decl;  /**< Zephyr characteristic declaration */
	struct bt_gatt_ccc_managed_user_data ccc; /**< CCC state (for NOTIFY) */
	uint8_t props;
	uint8_t value[CONFIG_AKIRA_BLE_CHAR_MAX_LEN];
	uint16_t value_len;
	uint16_t max_len;
	bool used;
	bool subscribed;     /**< Any peer has enabled notifications */
};

/** Per-service slot */
struct ble_service_slot {
	struct bt_uuid_128 uuid;
	struct bt_gatt_attr  attrs[ATTRS_PER_SERVICE];
	uint8_t              attr_count;
	struct bt_gatt_service svc;
	int   char_handles[CONFIG_AKIRA_BLE_MAX_CHARS];
	uint8_t char_count;
	bool registered;
	bool used;
};

/*===========================================================================*/
/* Static Pools                                                              */
/*===========================================================================*/

static struct ble_char_slot   AKIRA_BULK_BSS g_chars[CONFIG_AKIRA_BLE_MAX_CHARS];
static struct ble_service_slot AKIRA_BULK_BSS g_svcs[CONFIG_AKIRA_BLE_MAX_SERVICES];

/* Event queue — ring buffer in PSRAM; ble_event is plain data, and every
 * producer is a GATT callback running on the BT RX thread, never an ISR. */
AKIRA_BULK_MSGQ_DEFINE(g_evt_q, sizeof(struct ble_event),
		       CONFIG_AKIRA_BLE_EVENT_QUEUE_DEPTH, 4);

static K_MUTEX_DEFINE(g_mutex);
static bool g_initialized;

/* Persistent UUID objects for the standard declaration attributes. The Zephyr
 * BT_UUID_GATT_* macros expand to compound literals; used inside a function
 * they have automatic (stack) storage, so storing their address in a
 * long-lived attr table leaves a dangling pointer once the function returns
 * (the service decl then reads garbage and no client can discover it). These
 * file-scope statics have static storage and are safe to point at. */
static const struct bt_uuid_16 g_uuid_primary =
	BT_UUID_INIT_16(BT_UUID_GATT_PRIMARY_VAL);
static const struct bt_uuid_16 g_uuid_chrc =
	BT_UUID_INIT_16(BT_UUID_GATT_CHRC_VAL);
static const struct bt_uuid_16 g_uuid_ccc =
	BT_UUID_INIT_16(BT_UUID_GATT_CCC_VAL);

/*===========================================================================*/
/* Internal Helpers                                                          */
/*===========================================================================*/

static int pool_lock(void)
{
	return (k_mutex_lock(&g_mutex, POOL_LOCK_TIMEOUT) == 0) ? 0 : -EBUSY;
}

/**
 * @brief Parse "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" into 16-byte LE array.
 *
 * BLE UUIDs are stored little-endian (last group first).
 */
static int parse_uuid128(const char *str, uint8_t out[16])
{
	if (!str || strlen(str) != 36) {
		return -EINVAL;
	}
	if (str[8] != '-' || str[13] != '-' || str[18] != '-' || str[23] != '-') {
		return -EINVAL;
	}

	/* Assemble in big-endian byte order first, then reverse */
	uint8_t be[16];
	const char *p = str;
	int idx = 0;

	for (int i = 0; i < 36; i++) {
		if (str[i] == '-') {
			continue;
		}
		char hi = str[i];
		char lo = str[++i];

		uint8_t h = (hi >= 'a') ? hi - 'a' + 10 :
			    (hi >= 'A') ? hi - 'A' + 10 : hi - '0';
		uint8_t l = (lo >= 'a') ? lo - 'a' + 10 :
			    (lo >= 'A') ? lo - 'A' + 10 : lo - '0';

		if (idx >= 16) {
			return -EINVAL;
		}
		be[idx++] = (h << 4) | l;
	}

	if (idx != 16) {
		return -EINVAL;
	}

	/* BT UUID128 is stored LE — reverse the big-endian bytes */
	for (int i = 0; i < 16; i++) {
		out[i] = be[15 - i];
	}

	(void)p; /* avoid warning */
	return 0;
}

/** GATT read callback — copies characteristic value to ATT response */
static ssize_t char_read_cb(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr,
			     void *buf, uint16_t len, uint16_t offset)
{
	struct ble_char_slot *ch = (struct ble_char_slot *)attr->user_data;

	if (!ch || !ch->used) {
		return BT_GATT_ERR(BT_ATT_ERR_READ_NOT_PERMITTED);
	}

	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 ch->value, ch->value_len);
}

/** GATT write callback — copies peer data into slot and queues event */
static ssize_t char_write_cb(struct bt_conn *conn,
			      const struct bt_gatt_attr *attr,
			      const void *buf, uint16_t len,
			      uint16_t offset, uint8_t flags)
{
	struct ble_char_slot *ch = (struct ble_char_slot *)attr->user_data;

	if (!ch || !ch->used) {
		return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
	}
	if (offset + len > ch->max_len) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	memcpy(ch->value + offset, buf, len);
	ch->value_len = offset + len;

	/* Identify handle index for the event */
	int char_h = -1;

	for (int i = 0; i < CONFIG_AKIRA_BLE_MAX_CHARS; i++) {
		if (&g_chars[i] == ch) {
			char_h = i;
			break;
		}
	}

	struct ble_event evt = {
		.type        = BLE_EVT_CHAR_WRITTEN,
		.char_handle = (uint8_t)char_h,
		.data_len    = len,
	};
	uint16_t copy_len = (len > CONFIG_AKIRA_BLE_CHAR_MAX_LEN)
			    ? CONFIG_AKIRA_BLE_CHAR_MAX_LEN : len;

	memcpy(evt.data, buf, copy_len);

	if (k_msgq_put(&g_evt_q, &evt, K_NO_WAIT) != 0) {
		LOG_WRN("BLE event queue full — dropping CHAR_WRITTEN event");
	}

	return len;
}

/** CCC change callback — tracks whether any peer has enabled notifications */
static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	/*
	 * Walk through all char slots and find the one whose CCC attr this is.
	 * We look in every registered service's attr array.
	 */
	for (int si = 0; si < CONFIG_AKIRA_BLE_MAX_SERVICES; si++) {
		struct ble_service_slot *sv = &g_svcs[si];

		if (!sv->used || !sv->registered) {
			continue;
		}
		for (int ai = 0; ai < sv->attr_count; ai++) {
			if (&sv->attrs[ai] == attr) {
				/* Found — the char slot is the one before this CCC */
				/* A CCC sits at char_decl + value + CCC = every 3 attrs
				 * starting from attr[1]. So char index = (ai - 3) / 3 */
				int char_attr_base = ai - 2; /* value attr */
				if (char_attr_base < 1) {
					return;
				}
				/* Find which char handle has this value attr */
				for (int ci = 0; ci < sv->char_count; ci++) {
					int ch = sv->char_handles[ci];

					if (ch < 0 || ch >= CONFIG_AKIRA_BLE_MAX_CHARS) {
						continue;
					}
					if (sv->attrs[char_attr_base].user_data ==
					    &g_chars[ch]) {
						g_chars[ch].subscribed =
							(value == BT_GATT_CCC_NOTIFY);
						LOG_DBG("char[%d] notify %s",
							ch,
							g_chars[ch].subscribed ?
							"enabled" : "disabled");
						return;
					}
				}
			}
		}
	}
}

/*===========================================================================*/
/* Public API                                                                */
/*===========================================================================*/

int ble_app_svc_init(void)
{
	if (g_initialized) {
		return 0;
	}

	/* No clearing of the pools: they start zeroed, teardown leaves every
	 * slot unused, and a registered attribute table must never be wiped
	 * (see the file comment). */
	k_msgq_purge(&g_evt_q);
	g_initialized = true;

	LOG_INF("BLE app service layer initialised "
		"(max_svc=%d max_char=%d evt_depth=%d)",
		CONFIG_AKIRA_BLE_MAX_SERVICES,
		CONFIG_AKIRA_BLE_MAX_CHARS,
		CONFIG_AKIRA_BLE_EVENT_QUEUE_DEPTH);
	return 0;
}

int ble_app_svc_deinit(void)
{
	int ret = ble_app_svc_unregister_all();

	if (ret < 0) {
		return ret;
	}
	k_msgq_purge(&g_evt_q);
	g_initialized = false;
	return 0;
}

int ble_app_svc_alloc(const char *uuid128_str)
{
	if (!uuid128_str) {
		return -EINVAL;
	}

	/* Initialize the service layer on first use. The Arduino-style app flow
	 * (ble_led sample) creates and registers services BEFORE calling
	 * ble_init() — which also runs ble_app_svc_init(). Init used to wipe
	 * the pools, which made an already registered service vanish from the
	 * GATT; it no longer clears anything, and stays idempotent via
	 * g_initialized. */
	ble_app_svc_init();

	uint8_t val[16];

	if (parse_uuid128(uuid128_str, val) < 0) {
		LOG_DBG("Invalid service UUID");
		return -EINVAL;
	}

	if (pool_lock() < 0) {
		return -EBUSY;
	}

	for (int i = 0; i < CONFIG_AKIRA_BLE_MAX_SERVICES; i++) {
		struct ble_service_slot *sv = &g_svcs[i];

		if (!sv->used) {
			/* Field by field, never memset: the attribute table may
			 * still be under a walk of an earlier registration. */
			sv->uuid.uuid.type = BT_UUID_TYPE_128;
			memcpy(sv->uuid.val, val, sizeof(val));
			sv->svc.attrs      = sv->attrs;
			sv->svc.attr_count = 0;
			sv->attr_count     = 0;
			sv->char_count     = 0;
			sv->registered     = false;
			sv->used           = true;
			k_mutex_unlock(&g_mutex);
			LOG_DBG("Service slot %d allocated", i);
			return i;
		}
	}

	k_mutex_unlock(&g_mutex);
	LOG_ERR("No free service slots (max=%d)", CONFIG_AKIRA_BLE_MAX_SERVICES);
	return -ENOSPC;
}

int ble_app_char_alloc(const char *uuid128_str, uint8_t props, uint16_t max_len)
{
	if (!uuid128_str || max_len == 0 ||
	    max_len > CONFIG_AKIRA_BLE_CHAR_MAX_LEN) {
		return -EINVAL;
	}

	ble_app_svc_init(); /* idempotent */

	uint8_t val[16];

	if (parse_uuid128(uuid128_str, val) < 0) {
		LOG_DBG("Invalid char UUID");
		return -EINVAL;
	}

	if (pool_lock() < 0) {
		return -EBUSY;
	}

	for (int i = 0; i < CONFIG_AKIRA_BLE_MAX_CHARS; i++) {
		struct ble_char_slot *ch = &g_chars[i];

		if (!ch->used) {
			/* Field by field, never memset: an earlier registration's
			 * attributes still point here, and a declaration's uuid
			 * pointer must never be NULL. */
			ch->uuid.uuid.type = BT_UUID_TYPE_128;
			memcpy(ch->uuid.val, val, sizeof(val));

			/* Build characteristic declaration */
			ch->chrc_decl.uuid         = &ch->uuid.uuid;
			ch->chrc_decl.value_handle = 0;
			ch->chrc_decl.properties   = props;

			/* All-zero is a valid CCC (nobody subscribed) */
			memset(&ch->ccc, 0, sizeof(ch->ccc));
			ch->ccc.cfg_changed = ccc_changed;

			ch->props      = props;
			ch->max_len    = max_len;
			ch->value_len  = 0;
			ch->subscribed = false;
			ch->used       = true;

			k_mutex_unlock(&g_mutex);
			LOG_DBG("Char slot %d allocated: props=0x%02x", i, props);
			return i;
		}
	}

	k_mutex_unlock(&g_mutex);
	LOG_ERR("No free char slots (max=%d)", CONFIG_AKIRA_BLE_MAX_CHARS);
	return -ENOSPC;
}

int ble_app_svc_add_char(int svc_h, int char_h)
{
	if (svc_h < 0 || svc_h >= CONFIG_AKIRA_BLE_MAX_SERVICES ||
	    char_h < 0 || char_h >= CONFIG_AKIRA_BLE_MAX_CHARS) {
		return -EINVAL;
	}

	if (pool_lock() < 0) {
		return -EBUSY;
	}

	struct ble_service_slot *sv = &g_svcs[svc_h];
	struct ble_char_slot    *ch = &g_chars[char_h];

	if (!sv->used || !ch->used) {
		k_mutex_unlock(&g_mutex);
		return -ENOENT;
	}
	if (sv->registered) {
		k_mutex_unlock(&g_mutex);
		LOG_ERR("Cannot add char to already-registered service %d",
			svc_h);
		return -EBUSY;
	}
	if (sv->char_count >= CONFIG_AKIRA_BLE_MAX_CHARS) {
		k_mutex_unlock(&g_mutex);
		return -ENOSPC;
	}

	sv->char_handles[sv->char_count++] = char_h;
	k_mutex_unlock(&g_mutex);
	LOG_DBG("char %d added to service %d", char_h, svc_h);
	return 0;
}

int ble_app_svc_register(int svc_h)
{
	if (svc_h < 0 || svc_h >= CONFIG_AKIRA_BLE_MAX_SERVICES) {
		return -EINVAL;
	}

	if (pool_lock() < 0) {
		return -EBUSY;
	}

	struct ble_service_slot *sv = &g_svcs[svc_h];

	if (!sv->used) {
		k_mutex_unlock(&g_mutex);
		return -ENOENT;
	}
	if (sv->registered) {
		k_mutex_unlock(&g_mutex);
		return 0;
	}

	/*----------- Build the bt_gatt_attr array ---------------------------------
	 * The table may still be walked from an earlier registration (see the
	 * file comment), so it is rebuilt in place with every field valid at
	 * every moment: uuid and user_data never NULL, handle 0 (auto-assign),
	 * and attr_count published only once the entries below it are filled.
	 * Rebuilds run on app threads, below the BT RX thread's priority, so on
	 * this single-core SoC a walk is never actually in progress meanwhile.
	 *
	 * Layout per service:
	 *   [0]     Primary Service declaration
	 *   [1+N*0] Characteristic declaration  (char N)
	 *   [2+N*0] Characteristic value        (char N)
	 *   [3+N*0] CCC descriptor              (char N, if NOTIFY)
	 *   ... (repeat for each char)
	 *-------------------------------------------------------------------------*/

	uint8_t ai = 0;

	/* Service declaration attr */
	sv->attrs[ai].handle    = 0;
	sv->attrs[ai].uuid      = (struct bt_uuid *)&g_uuid_primary;
	sv->attrs[ai].perm      = BT_GATT_PERM_READ;
	sv->attrs[ai].read      = bt_gatt_attr_read_service;
	sv->attrs[ai].write     = NULL;
	sv->attrs[ai].user_data = &sv->uuid.uuid;
	ai++;

	for (int ci = 0; ci < sv->char_count; ci++) {
		int ch = sv->char_handles[ci];

		if (ch < 0 || ch >= CONFIG_AKIRA_BLE_MAX_CHARS) {
			continue;
		}

		struct ble_char_slot *slot = &g_chars[ch];
		uint8_t props = slot->props;

		/* Determine permission bits */
		uint8_t perm = 0;

		if (props & BLE_PROP_READ) {
			perm |= BT_GATT_PERM_READ;
		}
		if (props & (BLE_PROP_WRITE | BLE_PROP_WRITE_WO_RSP)) {
			perm |= BT_GATT_PERM_WRITE;
		}

		/* 1. Characteristic declaration */
		if (ai >= ATTRS_PER_SERVICE) {
			LOG_ERR("Attr pool overflow for service %d", svc_h);
			break;
		}
		sv->attrs[ai].handle    = 0;
		sv->attrs[ai].uuid      = (struct bt_uuid *)&g_uuid_chrc;
		sv->attrs[ai].perm      = BT_GATT_PERM_READ;
		sv->attrs[ai].read      = bt_gatt_attr_read_chrc;
		sv->attrs[ai].write     = NULL;
		sv->attrs[ai].user_data = &slot->chrc_decl;
		ai++;

		/* 2. Characteristic value */
		if (ai >= ATTRS_PER_SERVICE) {
			LOG_ERR("Attr pool overflow for service %d", svc_h);
			break;
		}
		sv->attrs[ai].handle    = 0;
		sv->attrs[ai].uuid      = &slot->uuid.uuid;
		sv->attrs[ai].perm      = perm;
		sv->attrs[ai].read      = (props & BLE_PROP_READ)
					  ? char_read_cb  : NULL;
		sv->attrs[ai].write     = (props & (BLE_PROP_WRITE |
					   BLE_PROP_WRITE_WO_RSP))
					  ? char_write_cb : NULL;
		sv->attrs[ai].user_data = slot;
		ai++;

		/* 3. CCC descriptor (only if NOTIFY or INDICATE) */
		if (props & (BLE_PROP_NOTIFY | BLE_PROP_INDICATE)) {
			if (ai >= ATTRS_PER_SERVICE) {
				LOG_ERR("Attr pool overflow for service %d", svc_h);
				break;
			}
			sv->attrs[ai].handle    = 0;
			sv->attrs[ai].uuid      = (struct bt_uuid *)&g_uuid_ccc;
			sv->attrs[ai].perm      = BT_GATT_PERM_READ |
						  BT_GATT_PERM_WRITE;
			sv->attrs[ai].read      = bt_gatt_attr_read_ccc;
			sv->attrs[ai].write     = bt_gatt_attr_write_ccc;
			sv->attrs[ai].user_data = &slot->ccc;
			ai++;
		}
	}

	sv->attr_count = ai;
	sv->svc.attrs      = sv->attrs;
	sv->svc.attr_count = ai;

	int err = bt_gatt_service_register(&sv->svc);

	if (err) {
		LOG_ERR("bt_gatt_service_register failed: %d", err);
		k_mutex_unlock(&g_mutex);
		return err;
	}

	sv->registered = true;
	k_mutex_unlock(&g_mutex);

	LOG_INF("Service %d registered (%d attrs, %d chars) svc_handle=0x%04x @%p",
		svc_h, ai, sv->char_count, sv->attrs[0].handle, (void *)&sv->svc);
	return 0;
}

int ble_app_svc_unregister_all(void)
{
	if (pool_lock() < 0) {
		LOG_ERR("BLE app service pool busy: services left registered");
		return -EBUSY;
	}

	for (int i = 0; i < CONFIG_AKIRA_BLE_MAX_SERVICES; i++) {
		if (g_svcs[i].registered) {
			/* Out of the database under Zephyr's own lock; the
			 * attributes stay intact for any walk still inside. */
			int err = bt_gatt_service_unregister(&g_svcs[i].svc);

			if (err) {
				LOG_ERR("Unregister svc %d failed: %d", i, err);
			}
			g_svcs[i].registered = false;
		}
		g_svcs[i].used = false;
	}

	/* From here the read/write callbacks refuse these characteristics. */
	for (int i = 0; i < CONFIG_AKIRA_BLE_MAX_CHARS; i++) {
		g_chars[i].used       = false;
		g_chars[i].subscribed = false;
	}

	k_msgq_purge(&g_evt_q);
	k_mutex_unlock(&g_mutex);
	LOG_DBG("All BLE app services unregistered");
	return 0;
}

int ble_app_char_write(int char_h, const uint8_t *data, uint16_t len)
{
	if (char_h < 0 || char_h >= CONFIG_AKIRA_BLE_MAX_CHARS ||
	    !data || len == 0) {
		return -EINVAL;
	}

	if (pool_lock() < 0) {
		return -EBUSY;
	}

	struct ble_char_slot *ch = &g_chars[char_h];
	const struct bt_gatt_attr *notify_attr = NULL;

	if (!ch->used) {
		k_mutex_unlock(&g_mutex);
		return -ENOENT;
	}
	if (len > ch->max_len) {
		k_mutex_unlock(&g_mutex);
		return -EMSGSIZE;
	}

	memcpy(ch->value, data, len);
	ch->value_len = len;

	/* Find the value attr to notify if any peer subscribed */
	if ((ch->props & BLE_PROP_NOTIFY) && ch->subscribed) {
		for (int si = 0; si < CONFIG_AKIRA_BLE_MAX_SERVICES && !notify_attr; si++) {
			if (!g_svcs[si].registered) {
				continue;
			}
			for (int ci = 0; ci < g_svcs[si].char_count; ci++) {
				if (g_svcs[si].char_handles[ci] == char_h) {
					/* Value attr is at char_decl_index + 1.
					 * Each char takes 2 or 3 attrs; sum up
					 * to find this char's offset. */
					int attr_idx = 1; /* skip svc decl */
					for (int k = 0; k < ci; k++) {
						int h = g_svcs[si].char_handles[k];
						attr_idx += 2;
						if (g_chars[h].props &
						    (BLE_PROP_NOTIFY |
						     BLE_PROP_INDICATE)) {
							attr_idx++;
						}
					}
					attr_idx++; /* skip decl to value */
					if (attr_idx < g_svcs[si].attr_count) {
						notify_attr = &g_svcs[si].attrs[attr_idx];
					}
					break;
				}
			}
		}
	}
	k_mutex_unlock(&g_mutex);

	/* Outside the pool lock: the notify may wait for an ATT buffer, and
	 * teardown must not wait behind it. The attribute is static memory; if
	 * the service was unregistered meanwhile the stack refuses it. */
	if (notify_attr) {
		bt_gatt_notify(NULL, notify_attr, data, len);
	}
	return 0;
}

int ble_app_char_read(int char_h, uint8_t *buf, uint16_t buf_len)
{
	if (char_h < 0 || char_h >= CONFIG_AKIRA_BLE_MAX_CHARS ||
	    !buf || buf_len == 0) {
		return -EINVAL;
	}

	if (pool_lock() < 0) {
		return -EBUSY;
	}

	struct ble_char_slot *ch = &g_chars[char_h];

	if (!ch->used) {
		k_mutex_unlock(&g_mutex);
		return -ENOENT;
	}

	uint16_t copy = MIN(MIN(ch->value_len, buf_len), (uint16_t)sizeof(ch->value));

	memcpy(buf, ch->value, copy);
	k_mutex_unlock(&g_mutex);
	return copy;
}

int ble_app_event_pop(struct ble_event *evt)
{
	if (!evt) {
		return -EINVAL;
	}

	if (k_msgq_get(&g_evt_q, evt, K_NO_WAIT) == 0) {
		return evt->type;
	}

	return BLE_EVT_NONE;
}

void ble_app_push_conn_event(uint8_t type)
{
	struct ble_event evt = {
		.type        = type,
		.char_handle = 0,
		.data_len    = 0,
	};

	if (k_msgq_put(&g_evt_q, &evt, K_NO_WAIT) != 0) {
		LOG_WRN("BLE event queue full — dropping conn event %d", type);
	}
}

const uint8_t *ble_app_svc_get_uuid128(int svc_h)
{
	if (svc_h < 0 || svc_h >= CONFIG_AKIRA_BLE_MAX_SERVICES ||
	    !g_svcs[svc_h].used) {
		return NULL;
	}
	return g_svcs[svc_h].uuid.val;
}

#else /* !CONFIG_AKIRA_WASM_BLE */

/* Stubs: keep the translation unit non-empty */
int ble_app_svc_init(void)        { return -ENOTSUP; }
int ble_app_svc_deinit(void)      { return -ENOTSUP; }
int ble_app_svc_unregister_all(void) { return -ENOTSUP; }
int ble_app_svc_alloc(const char *u)   { (void)u; return -ENOTSUP; }
int ble_app_char_alloc(const char *u, uint8_t p, uint16_t m)
{ (void)u; (void)p; (void)m; return -ENOTSUP; }
int ble_app_svc_add_char(int s, int c) { (void)s; (void)c; return -ENOTSUP; }
int ble_app_svc_register(int s)   { (void)s; return -ENOTSUP; }
int ble_app_char_write(int h, const uint8_t *d, uint16_t l)
{ (void)h; (void)d; (void)l; return -ENOTSUP; }
int ble_app_char_read(int h, uint8_t *b, uint16_t l)
{ (void)h; (void)b; (void)l; return -ENOTSUP; }
int ble_app_event_pop(struct ble_event *e) { (void)e; return -ENOTSUP; }
void ble_app_push_conn_event(uint8_t t) { (void)t; }
const uint8_t *ble_app_svc_get_uuid128(int s) { (void)s; return NULL; }

#endif /* CONFIG_AKIRA_WASM_BLE */
