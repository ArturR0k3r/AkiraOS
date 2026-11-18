/**
 * @file akira_gpio_api.c
 * @brief GPIO API implementation for WASM exports
 */

#include "akira_api.h"
#include "akira_gpio_api.h"
#include <runtime/security.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>
#include <errno.h>

LOG_MODULE_REGISTER(akira_gpio_api, CONFIG_AKIRA_LOG_LEVEL);

/* ---------------------------------------------------------------------------
 * Pin policy
 *
 * Pin numbering is the SoC GPIO number: 0-31 on gpio0, 32+ on gpio1.
 *
 * 1. SoC-reserved pins are refused to EVERY caller (shell included):
 *    reconfiguring them kills the system outright (ESP32-S3: SPI flash and
 *    octal PSRAM are on GPIO26-37 — touching them stalls the cache and the
 *    CPU; GPIO19/20 are the USB PHY, GPIO43/44 the UART0 console;
 *    GPIO22-25 and 49+ do not exist).
 *
 * 2. WASM apps (whose capabilities are self-declared, so not a security
 *    boundary) are further limited to:
 *      CONFIG_AKIRA_WASM_GPIO_APP_PINS          (per-board allowlist)
 *      minus every pin the devicetree gives the system: any okay node's
 *      gpios / cs-gpios / int-gpios / irq-gpios / reset-gpios / ... (see
 *      GPIO_DT_NODE_GPIO_PINS) on gpio0/gpio1, and every pad muxed by an
 *      okay node's pinctrl-0 (ESP32 pinmux: I2C, SPI, UART, LEDC, ...)
 *      minus the strapping pins (ESP32-S3: GPIO45/46).
 *    A pin configured by one app cannot be touched by another, an app may
 *    read or write only pins it configured, and every pin an app configured
 *    is put back to GPIO_DISCONNECTED when it ends, however it ends.
 *
 * The masks are compile-time constants built from devicetree macros, so the
 * policy costs no RAM and cannot drift from the board files.
 * ------------------------------------------------------------------------- */

#if defined(CONFIG_SOC_SERIES_ESP32S3)
#define GPIO_SOC_RESERVED (BIT64(19) | BIT64(20) |                  /* USB D-/D+     */ \
                           GENMASK64(25, 22) |                     /* not bonded    */ \
                           GENMASK64(37, 26) |                     /* flash + PSRAM */ \
                           BIT64(43) | BIT64(44) |                 /* UART0 console */ \
                           GENMASK64(63, 49))                      /* no such pin   */
#define GPIO_APP_EXTRA_DENY (BIT64(45) | BIT64(46))                /* strapping     */
#elif defined(CONFIG_SOC_SERIES_ESP32)
#define GPIO_SOC_RESERVED (GENMASK64(11, 6) |                      /* SPI flash     */ \
                           BIT64(1) | BIT64(3) |                   /* UART0 console */ \
                           BIT64(20) | BIT64(24) |                 /* no such pin   */ \
                           GENMASK64(31, 28) | GENMASK64(63, 40))
#define GPIO_APP_EXTRA_DENY (BIT64(0) | BIT64(2) | BIT64(5) |      /* strapping     */ \
                             BIT64(12) | BIT64(15))
#elif defined(CONFIG_SOC_SERIES_ESP32C3)
#define GPIO_SOC_RESERVED (GENMASK64(17, 12) |                     /* SPI flash     */ \
                           GENMASK64(63, 22))                      /* no such pin   */
#define GPIO_APP_EXTRA_DENY (BIT64(18) | BIT64(19) |               /* USB D-/D+     */ \
                             BIT64(20) | BIT64(21) |               /* UART0 console */ \
                             BIT64(2) | BIT64(8) | BIT64(9))       /* strapping     */
#elif defined(CONFIG_SOC_SERIES_ESP32C6)
#define GPIO_SOC_RESERVED (GENMASK64(30, 24) |                     /* SPI flash     */ \
                           GENMASK64(63, 31))                      /* no such pin   */
#define GPIO_APP_EXTRA_DENY (BIT64(12) | BIT64(13) |               /* USB D-/D+     */ \
                             BIT64(16) | BIT64(17) |               /* UART0 console */ \
                             BIT64(8) | BIT64(9) | BIT64(15))      /* strapping     */
#else
#define GPIO_SOC_RESERVED 0ULL
#define GPIO_APP_EXTRA_DENY 0ULL
#endif

#if DT_NODE_EXISTS(DT_NODELABEL(gpio0))
#define GPIO_PROP_ON_GPIO0(node, prop, idx) \
    DT_SAME_NODE(DT_GPIO_CTLR_BY_IDX(node, prop, idx), DT_NODELABEL(gpio0))
#else
#define GPIO_PROP_ON_GPIO0(node, prop, idx) 0
#endif
#if DT_NODE_EXISTS(DT_NODELABEL(gpio1))
#define GPIO_PROP_ON_GPIO1(node, prop, idx) \
    DT_SAME_NODE(DT_GPIO_CTLR_BY_IDX(node, prop, idx), DT_NODELABEL(gpio1))
#else
#define GPIO_PROP_ON_GPIO1(node, prop, idx) 0
#endif

#define GPIO_DT_PHA_BIT(node, prop, idx)                                                  \
    ((GPIO_PROP_ON_GPIO0(node, prop, idx)                                                 \
          ? BIT64(DT_GPIO_PIN_BY_IDX(node, prop, idx) & 63) : 0ULL) |                     \
     (GPIO_PROP_ON_GPIO1(node, prop, idx)                                                 \
          ? BIT64((32 + DT_GPIO_PIN_BY_IDX(node, prop, idx)) & 63) : 0ULL))

/* Only entries that really are <&controller pin flags> (skips gpio-hog
 * "gpios" arrays and <0> placeholders in cs-gpios). */
#define GPIO_DT_PIN_BIT(node, prop, idx)                                                  \
    | COND_CODE_1(DT_PHA_HAS_CELL_AT_IDX(node, prop, idx, pin),                           \
                  (GPIO_DT_PHA_BIT(node, prop, idx)), (0ULL))

#define GPIO_DT_PROP_PINS(node, prop) \
    COND_CODE_1(DT_NODE_HAS_PROP(node, prop), (DT_FOREACH_PROP_ELEM(node, prop, GPIO_DT_PIN_BIT)), ())

#if defined(CONFIG_SOC_FAMILY_ESPRESSIF_ESP32)
/* ESP32 pinmux cells carry the pad number in bits [5:0] (ESP32_PIN_NUM_MASK).
 * The _SEP variant: a nested DT_FOREACH_PROP_ELEM would not expand. */
#define GPIO_PINMUX_BIT(node, prop, idx) | BIT64(DT_PROP_BY_IDX(node, prop, idx) & 0x3F)
#define GPIO_PINCTRL_GROUP(group) \
    COND_CODE_1(DT_NODE_HAS_PROP(group, pinmux),                                          \
                (DT_FOREACH_PROP_ELEM_SEP(group, pinmux, GPIO_PINMUX_BIT, ())), ())
#define GPIO_PINCTRL_STATE(node, prop, idx) \
    DT_FOREACH_CHILD(DT_PHANDLE_BY_IDX(node, prop, idx), GPIO_PINCTRL_GROUP)
#define GPIO_DT_PINCTRL_PINS(node) \
    COND_CODE_1(DT_NODE_HAS_PROP(node, pinctrl_0),                                        \
                (DT_FOREACH_PROP_ELEM(node, pinctrl_0, GPIO_PINCTRL_STATE)), ())
#else
#define GPIO_DT_PINCTRL_PINS(node)
#endif

/* Devicetree has no way to walk every "*-gpios" property, so this lists the
 * names used by the AkiraOS boards, snippets and bindings plus the common
 * Zephyr radio/power/storage ones. A pin wired only through a property not
 * listed here is still kept from apps by the board allowlist
 * (CONFIG_AKIRA_WASM_GPIO_APP_PINS), which should name only free pins. */
#define GPIO_DT_NODE_GPIO_PINS(node)                                                      \
    GPIO_DT_PROP_PINS(node, gpios) GPIO_DT_PROP_PINS(node, cs_gpios)                      \
    GPIO_DT_PROP_PINS(node, int_gpios) GPIO_DT_PROP_PINS(node, irq_gpios)                 \
    GPIO_DT_PROP_PINS(node, enable_gpios) GPIO_DT_PROP_PINS(node, dc_gpios)               \
    GPIO_DT_PROP_PINS(node, reset_gpios) GPIO_DT_PROP_PINS(node, busy_gpios)              \
    GPIO_DT_PROP_PINS(node, gpo_gpios) GPIO_DT_PROP_PINS(node, detect_gpios)              \
    GPIO_DT_PROP_PINS(node, cd_gpios) GPIO_DT_PROP_PINS(node, wp_gpios)                   \
    GPIO_DT_PROP_PINS(node, drdy_gpios) GPIO_DT_PROP_PINS(node, int_n_gpios)              \
    GPIO_DT_PROP_PINS(node, ldo_enable_gpios) GPIO_DT_PROP_PINS(node, select_gpios)       \
    GPIO_DT_PROP_PINS(node, usb_mux_sel_gpios) GPIO_DT_PROP_PINS(node, shdnz_gpios)       \
    GPIO_DT_PROP_PINS(node, sda_gpios) GPIO_DT_PROP_PINS(node, scl_gpios)                 \
    GPIO_DT_PROP_PINS(node, wkup_gpios) GPIO_DT_PROP_PINS(node, wakeup_gpios)             \
    GPIO_DT_PROP_PINS(node, power_gpios) GPIO_DT_PROP_PINS(node, pwr_gpios)               \
    GPIO_DT_PROP_PINS(node, en_gpios) GPIO_DT_PROP_PINS(node, standby_gpios)              \
    GPIO_DT_PROP_PINS(node, ready_gpios) GPIO_DT_PROP_PINS(node, ce_gpios)                \
    GPIO_DT_PROP_PINS(node, dio_gpios) GPIO_DT_PROP_PINS(node, dio1_gpios)                \
    GPIO_DT_PROP_PINS(node, antenna_enable_gpios)                                         \
    GPIO_DT_PROP_PINS(node, tx_enable_gpios) GPIO_DT_PROP_PINS(node, rx_enable_gpios)     \
    GPIO_DT_PROP_PINS(node, mute_gpios)

#define GPIO_DT_NODE_PINS(node) GPIO_DT_NODE_GPIO_PINS(node) GPIO_DT_PINCTRL_PINS(node)

/* Same, but a LEDC controller's own output pads do not count: the PWM API
 * vets those per channel. */
#define GPIO_DT_NODE_PINS_NO_LEDC(node)                                                   \
    GPIO_DT_NODE_GPIO_PINS(node)                                                          \
    COND_CODE_1(DT_NODE_HAS_COMPAT(node, espressif_esp32_ledc), (),                       \
                (GPIO_DT_PINCTRL_PINS(node)))

/* Every pin the devicetree hands to a system device (compile-time constant). */
#define GPIO_DT_SYSTEM_PINS (0ULL DT_FOREACH_STATUS_OKAY_NODE(GPIO_DT_NODE_PINS))
#define GPIO_DT_SYSTEM_PINS_NO_LEDC (0ULL DT_FOREACH_STATUS_OKAY_NODE(GPIO_DT_NODE_PINS_NO_LEDC))

#ifndef CONFIG_AKIRA_WASM_GPIO_APP_PINS
#define CONFIG_AKIRA_WASM_GPIO_APP_PINS 0xFFFFFFFFFFFFFFFFULL
#endif

/** Pins a WASM app may configure / read / write. */
static const uint64_t s_gpio_app_pins =
    (uint64_t)CONFIG_AKIRA_WASM_GPIO_APP_PINS &
    ~(GPIO_SOC_RESERVED | GPIO_APP_EXTRA_DENY | GPIO_DT_SYSTEM_PINS);

/** Pins that may carry an app-driven PWM channel's output. */
static const uint64_t s_gpio_pwm_pads =
    (uint64_t)CONFIG_AKIRA_WASM_GPIO_APP_PINS &
    ~(GPIO_SOC_RESERVED | GPIO_APP_EXTRA_DENY | GPIO_DT_SYSTEM_PINS_NO_LEDC);

static inline bool gpio_pin_soc_reserved(uint32_t pin)
{
    return pin >= 64 || (GPIO_SOC_RESERVED & BIT64(pin)) != 0;
}

bool akira_gpio_pin_app_allowed(uint32_t pin)
{
    return pin < 64 && (s_gpio_app_pins & BIT64(pin)) != 0;
}

bool akira_gpio_pin_is_system(uint32_t pin)
{
    return gpio_pin_soc_reserved(pin) ||
           ((GPIO_APP_EXTRA_DENY | GPIO_DT_SYSTEM_PINS) & BIT64(pin)) != 0;
}

bool akira_gpio_pin_pwm_pad_allowed(uint32_t pin)
{
    return pin < 64 && (s_gpio_pwm_pads & BIT64(pin)) != 0;
}

/* GPIO device handles */
static const struct device *gpio0_dev = NULL;
static const struct device *gpio1_dev = NULL;

/* Initialize GPIO devices on first use */
static int akira_gpio_init(void)
{
    static bool initialized = false;

    if (initialized) {
        return 0;
    }

#if defined(CONFIG_GPIO)
#if DT_NODE_HAS_STATUS(DT_NODELABEL(gpio0), okay)
    gpio0_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
    if (!device_is_ready(gpio0_dev)) {
        LOG_ERR("GPIO0 device not ready");
        gpio0_dev = NULL;
    }
#else
    gpio0_dev = NULL;
    LOG_DBG("GPIO0 not defined in device tree");
#endif

#if DT_NODE_HAS_STATUS(DT_NODELABEL(gpio1), okay)
    gpio1_dev = DEVICE_DT_GET(DT_NODELABEL(gpio1));
    if (!device_is_ready(gpio1_dev)) {
        LOG_DBG("GPIO1 device not available (normal for some platforms)");
        gpio1_dev = NULL;
    }
#else
    gpio1_dev = NULL;
    LOG_DBG("GPIO1 not defined in device tree");
#endif

    if (!gpio0_dev && !gpio1_dev) {
        LOG_ERR("No GPIO devices available");
        return -ENODEV;
    }
#else
    gpio0_dev = NULL;
    gpio1_dev = NULL;
    LOG_WRN("GPIO subsystem not enabled (CONFIG_GPIO=n)");
    return -ENOTSUP;
#endif

    initialized = true;
    LOG_INF("GPIO API initialized");
    return 0;
}

/* Get GPIO device based on pin number */
static const struct device *akira_gpio_get_device(uint32_t pin, gpio_pin_t *pin_out)
{
    const struct device *dev = NULL;

    if (akira_gpio_init() != 0) {
        return NULL;
    }

    /* ESP32-S3: GPIO0-31 on gpio0, GPIO32-48 on gpio1 */
    if (pin < 32) {
        *pin_out = pin;
        dev = gpio0_dev;
    } else if (pin < 64) {
        *pin_out = pin - 32;
        dev = gpio1_dev;
    }

    /* The port must really have that pin: the driver API asserts on it,
     * and some drivers index tables with it. */
    if (dev) {
        const struct gpio_driver_config *cfg =
            (const struct gpio_driver_config *)dev->config;

        if ((cfg->port_pin_mask & (gpio_port_pins_t)BIT(*pin_out)) == 0U) {
            dev = NULL;
        }
    }
    if (!dev) {
        LOG_DBG("Invalid GPIO pin: %u", pin);
    }
    return dev;
}

/* Convert AKIRA GPIO flags to Zephyr GPIO flags */
static gpio_flags_t akira_gpio_flags_to_zephyr(uint32_t akira_flags)
{
    gpio_flags_t zephyr_flags = 0;

    if (akira_flags & AKIRA_GPIO_INPUT) {
        zephyr_flags |= GPIO_INPUT;
    }
    if (akira_flags & AKIRA_GPIO_OUTPUT) {
        zephyr_flags |= GPIO_OUTPUT;
    }
    if (akira_flags & AKIRA_GPIO_OUTPUT_INIT_LOW) {
        zephyr_flags |= GPIO_OUTPUT_INIT_LOW;
    }
    if (akira_flags & AKIRA_GPIO_OUTPUT_INIT_HIGH) {
        zephyr_flags |= GPIO_OUTPUT_INIT_HIGH;
    }
    if (akira_flags & AKIRA_GPIO_PULL_UP) {
        zephyr_flags |= GPIO_PULL_UP;
    }
    if (akira_flags & AKIRA_GPIO_PULL_DOWN) {
        zephyr_flags |= GPIO_PULL_DOWN;
    }
    if (akira_flags & AKIRA_GPIO_ACTIVE_LOW) {
        zephyr_flags |= GPIO_ACTIVE_LOW;
    }
    if (akira_flags & AKIRA_GPIO_ACTIVE_HIGH) {
        zephyr_flags |= GPIO_ACTIVE_HIGH;
    }

    return zephyr_flags;
}

/* Combinations gpio_pin_configure() asserts on (a kernel panic with
 * CONFIG_ASSERT) or that drivers handle inconsistently. */
static int akira_gpio_flags_check(uint32_t flags)
{
    const uint32_t init = AKIRA_GPIO_OUTPUT_INIT_LOW | AKIRA_GPIO_OUTPUT_INIT_HIGH;
    const uint32_t pulls = AKIRA_GPIO_PULL_UP | AKIRA_GPIO_PULL_DOWN;

    if ((flags & pulls) == pulls || (flags & init) == init) {
        return -EINVAL;
    }
    if ((flags & init) != 0 && (flags & AKIRA_GPIO_OUTPUT) == 0) {
        return -EINVAL;
    }
    return 0;
}

int akira_gpio_configure(uint32_t pin, uint32_t flags)
{
    if (gpio_pin_soc_reserved(pin)) {
        return -EACCES;
    }
    int ret = akira_gpio_flags_check(flags);
    if (ret < 0) {
        return ret;
    }

    gpio_pin_t gpio_pin = 0;
    const struct device *dev = akira_gpio_get_device(pin, &gpio_pin);

    if (!dev) {
        return -EINVAL;
    }

    gpio_flags_t zephyr_flags = akira_gpio_flags_to_zephyr(flags);
    ret = gpio_pin_configure(dev, gpio_pin, zephyr_flags);

    if (ret < 0) {
        LOG_DBG("Failed to configure GPIO %u: %d", pin, ret);
        return ret;
    }

    LOG_DBG("Configured GPIO %u with flags 0x%08x", pin, flags);
    return 0;
}

int akira_gpio_read(uint32_t pin)
{
    if (gpio_pin_soc_reserved(pin)) {
        return -EACCES;
    }
    gpio_pin_t gpio_pin = 0;
    const struct device *dev = akira_gpio_get_device(pin, &gpio_pin);

    if (!dev) {
        return -EINVAL;
    }

    int ret = gpio_pin_get(dev, gpio_pin);

    if (ret < 0) {
        LOG_DBG("Failed to read GPIO %u: %d", pin, ret);
        return ret;
    }

    return ret;
}

int akira_gpio_write(uint32_t pin, uint32_t value)
{
    if (gpio_pin_soc_reserved(pin)) {
        return -EACCES;
    }
    gpio_pin_t gpio_pin = 0;
    const struct device *dev = akira_gpio_get_device(pin, &gpio_pin);

    if (!dev) {
        return -EINVAL;
    }

    int ret = gpio_pin_set(dev, gpio_pin, value ? 1 : 0);

    if (ret < 0) {
        LOG_DBG("Failed to write GPIO %u: %d", pin, ret);
        return ret;
    }

    return 0;
}

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <runtime/akira_runtime.h>
#include <lib/mem_helper.h>

/* WASM Native export API */

/* Pins each app slot has configured, disconnected again by gpio_release()
 * when the app ends. s_gpio_own_lock keeps the cross-slot ownership check
 * atomic; it is never held across a driver call. */
static uint64_t AKIRA_BULK_BSS s_gpio_slot_pins[AKIRA_MAX_WASM_INSTANCES];
static struct k_spinlock s_gpio_own_lock;

static bool gpio_slot_ok(int slot)
{
    return slot >= 0 && slot < AKIRA_MAX_WASM_INSTANCES;
}

/* Claim @p pin for @p slot: -EBUSY if another app slot has it configured. */
static int gpio_claim(int slot, uint32_t pin)
{
    int ret = 0;
    k_spinlock_key_t key = k_spin_lock(&s_gpio_own_lock);

    for (int i = 0; i < AKIRA_MAX_WASM_INSTANCES; i++) {
        if (i != slot && (s_gpio_slot_pins[i] & BIT64(pin))) {
            ret = -EBUSY;
            break;
        }
    }
    if (ret == 0) {
        s_gpio_slot_pins[slot] |= BIT64(pin);
    }
    k_spin_unlock(&s_gpio_own_lock, key);
    return ret;
}

static void gpio_unclaim(int slot, uint32_t pin)
{
    k_spinlock_key_t key = k_spin_lock(&s_gpio_own_lock);

    s_gpio_slot_pins[slot] &= ~BIT64(pin);
    k_spin_unlock(&s_gpio_own_lock, key);
}

static bool gpio_owned_by(int slot, uint32_t pin)
{
    return gpio_slot_ok(slot) && pin < 64 && (s_gpio_slot_pins[slot] & BIT64(pin)) != 0;
}

/* Stop driving @p pin: no output, no pulls. Plain input on drivers without
 * GPIO_DISCONNECTED support. Non-blocking (SoC GPIO register writes). */
static void gpio_disconnect(uint32_t pin)
{
    gpio_pin_t gpio_pin = 0;
    const struct device *dev = akira_gpio_get_device(pin, &gpio_pin);

    if (!dev) {
        return;
    }
    if (gpio_pin_configure(dev, gpio_pin, GPIO_DISCONNECTED) < 0) {
        (void)gpio_pin_configure(dev, gpio_pin, GPIO_INPUT);
    }
}

/* Release hook: runs on every exit path, never concurrently with the slot's
 * own natives. Pins are disconnected before ownership is dropped, so another
 * app cannot claim one in between and have it undone. */
static void gpio_release(int slot, const char *app_name)
{
    ARG_UNUSED(app_name);

    if (!gpio_slot_ok(slot)) {
        return;
    }

    uint64_t pins = s_gpio_slot_pins[slot];

    while (pins != 0) {
        uint32_t pin = (uint32_t)__builtin_ctzll(pins);

        pins &= pins - 1;
        gpio_disconnect(pin);
    }

    k_spinlock_key_t key = k_spin_lock(&s_gpio_own_lock);
    s_gpio_slot_pins[slot] = 0;
    k_spin_unlock(&s_gpio_own_lock, key);
}

AKIRA_APP_RESOURCE_DEFINE(gpio_pins, 50, NULL, gpio_release);

int akira_native_gpio_configure(wasm_exec_env_t exec_env, uint32_t pin, uint32_t flags)
{
    /* Output pin requires write capability; input pin requires read capability */
    uint64_t required_cap = (flags & AKIRA_GPIO_OUTPUT) ?
                            AKIRA_CAP_GPIO_WRITE : AKIRA_CAP_GPIO_READ;

    AKIRA_CHECK_CAP_OR_RETURN(exec_env, required_cap, -EPERM);

    /* Hard rule before any device access: even configuring a system pin as
     * an input detaches it from its peripheral (bus, chip select, IRQ). */
    if (!akira_gpio_pin_app_allowed(pin)) {
        return -EACCES;
    }
    int ret = akira_gpio_flags_check(flags);
    if (ret < 0) {
        return ret;
    }
    int slot = akira_runtime_slot_of_exec_env(exec_env);
    if (!gpio_slot_ok(slot)) {
        return -EPERM;
    }
    bool had = gpio_owned_by(slot, pin);
    ret = gpio_claim(slot, pin);
    if (ret < 0) {
        return ret;
    }
    ret = akira_gpio_configure(pin, flags);
    if (ret < 0 && !had) {
        gpio_unclaim(slot, pin);
    }
    return ret;
}

int akira_native_gpio_read(wasm_exec_env_t exec_env, uint32_t pin)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_GPIO_READ, -EPERM);
    if (!akira_gpio_pin_app_allowed(pin)) {
        return -EACCES;
    }
    /* Only a pin this app configured (and so owns). */
    if (!gpio_owned_by(akira_runtime_slot_of_exec_env(exec_env), pin)) {
        return -EPERM;
    }
    return akira_gpio_read(pin);
}

int akira_native_gpio_write(wasm_exec_env_t exec_env, uint32_t pin, uint32_t value)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_GPIO_WRITE, -EPERM);
    if (!akira_gpio_pin_app_allowed(pin)) {
        return -EACCES;
    }
    /* Only a pin this app configured (and so owns) may be driven. */
    if (!gpio_owned_by(akira_runtime_slot_of_exec_env(exec_env), pin)) {
        return -EPERM;
    }
    return akira_gpio_write(pin, value);
}

#endif

/* ===== WASM exports =====
 * Registered with WAMR by the native API registry (akira_native_registry.h).
 * Import names and signatures are WASM ABI: see docs/api-stability-policy.md. */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && (defined(CONFIG_AKIRA_WASM_API) && defined(CONFIG_GPIO))
#include <akira_native_registry.h>

static const NativeSymbol akira_gpio_natives[] = {
    {"gpio_configure", (void *)akira_native_gpio_configure, "(ii)i", NULL},
    {"gpio_read", (void *)akira_native_gpio_read, "(i)i", NULL},
    {"gpio_write", (void *)akira_native_gpio_write, "(ii)i", NULL},
};

AKIRA_NATIVE_API_DEFINE(akira_gpio_api, "env", akira_gpio_natives);
#endif
