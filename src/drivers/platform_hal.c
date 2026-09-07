/**
 * @file platform_hal.c
 * @brief Platform Hardware Abstraction Layer Implementation
 */

#include "platform_hal.h"
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <string.h>

#if defined(CONFIG_BOARD_NATIVE_SIM) || defined(CONFIG_NATIVE_SIM) || defined(__linux__)
#ifndef AKIRA_PLATFORM_NATIVE_SIM
#define AKIRA_PLATFORM_NATIVE_SIM 1
#endif
#endif

#if AKIRA_PLATFORM_NATIVE_SIM
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#endif

/* Logging module set at source level */
LOG_MODULE_REGISTER(akira_hal, LOG_LEVEL_DBG);

#if AKIRA_PLATFORM_NATIVE_SIM
/* Simulated display framebuffer (240x320 RGB565) */
/* For native simulation we keep the static framebuffer in normal RAM. */
static uint16_t sim_framebuffer[240 * 320];
static bool sim_display_dirty = false;

/* Simulated button states */
static uint32_t sim_button_state = 0;

/* Shared memory for external SDL2 viewer */
static int shm_framebuffer_fd = -1;
static int shm_buttons_fd = -1;
static uint16_t *shared_framebuffer = NULL;
static uint32_t *shared_buttons = NULL;
#endif /* AKIRA_PLATFORM_NATIVE_SIM */

/* On hardware platforms the framebuffer lives in SPIRAM/PSRAM.
 * ESP32 family uses CONFIG_ESP_SPIRAM + __attribute__((section(".ext_ram.bss"))).
 * Other platforms with Zephyr MEMC/PSRAM support use CONFIG_AKIRA_FRAMEBUFFER_IN_PSRAM.
 * 400×240×2 = 192 000 B — sized for the largest supported display (Sharp LS027B7DH01
 * 400×240).  Boards with smaller displays simply leave the tail unused.
 */
#define AKIRA_FB_MAX_PIXELS (400 * 240)
#define AKIRA_FB_NUM_BUFFERS 2
#if defined(CONFIG_ESP_SPIRAM) || \
    (defined(CONFIG_AKIRA_FRAMEBUFFER_IN_PSRAM) && defined(CONFIG_MEMC))
#define AKIRA_HAS_HW_FRAMEBUFFER 1
__attribute__((section(".ext_ram.bss"), aligned(4))) static uint16_t hw_framebuffer[AKIRA_FB_NUM_BUFFERS][AKIRA_FB_MAX_PIXELS];
#endif
/* On targets without SPIRAM/MEMC there is no buffer at all: akira_framebuffer_get()
 * returns NULL and the present/flip path below is a no-op. */

/* Double-buffer handoff to the display compositor thread. Multiple threads
 * call akira_display_flush() -> this function concurrently (akira_os_shell
 * thread, system workqueue via g_auto_flush_work/g_sd_install_work, and a
 * WASM app's own thread) — not single-producer. fb_present_mutex serializes
 * the give(frame_ready)/take(buffer_free)/write_idx-flip sequence so two
 * racing producers can't collide on the same semaphore credit (frame_ready
 * caps at 1 — a second concurrent give() is silently dropped) or desync
 * fb_write_idx, which otherwise can permanently wedge a producer in
 * take(buffer_free) forever (the compositor only ever hands back one
 * credit per real frame it consumed). */
#if defined(AKIRA_HAS_HW_FRAMEBUFFER)
static uint8_t fb_write_idx;           /* buffer the app is currently drawing into */
#endif
static const uint16_t *fb_present_buf; /* buffer handed to the compositor */
static struct k_sem fb_sem_frame_ready = Z_SEM_INITIALIZER(fb_sem_frame_ready, 0, 1);
static struct k_sem fb_sem_buffer_free = Z_SEM_INITIALIZER(fb_sem_buffer_free, 1, 1);
K_MUTEX_DEFINE(fb_present_mutex);

void akira_framebuffer_present(void)
{
#if !defined(AKIRA_HAS_HW_FRAMEBUFFER)
    /* No framebuffer to hand over, and no compositor consuming one — giving
     * frame_ready here would strand the next caller in take(buffer_free). */
    return;
#else
    k_mutex_lock(&fb_present_mutex, K_FOREVER);
    fb_present_buf = hw_framebuffer[fb_write_idx];
    k_sem_give(&fb_sem_frame_ready);
    /* Blocks only if the compositor hasn't finished the previous buffer yet
     * — bounds producers to at most one frame ahead of the blit. */
    k_sem_take(&fb_sem_buffer_free, K_FOREVER);
    uint8_t presented_idx = fb_write_idx;
    fb_write_idx ^= 1U;
    /* Seed the new write buffer with the frame just presented so apps that
     * draw deltas (not a full redraw every frame) don't alternate onto a
     * stale/uninitialized buffer every other present(). */
    memcpy(hw_framebuffer[fb_write_idx], hw_framebuffer[presented_idx],
           sizeof(hw_framebuffer[0]));
    k_mutex_unlock(&fb_present_mutex);
#endif
}

const uint16_t *akira_framebuffer_wait_present(void)
{
    k_sem_take(&fb_sem_frame_ready, K_FOREVER);
    return fb_present_buf;
}

void akira_framebuffer_present_done(void)
{
    k_sem_give(&fb_sem_buffer_free);
}

int akira_hal_init(void)
{
    LOG_INF("Akira HAL initializing for: %s", akira_get_platform_name());

#if AKIRA_PLATFORM_NATIVE_SIM
    LOG_INF("Running in SIMULATION mode with display and button emulation");

    /* Initialize simulated framebuffer to black */
    memset(sim_framebuffer, 0, sizeof(sim_framebuffer));

    /* Create shared memory for external SDL2 viewer */
    shm_framebuffer_fd = open("/tmp/akira_framebuffer", O_CREAT | O_RDWR, 0666);
    if (shm_framebuffer_fd >= 0)
    {
        ftruncate(shm_framebuffer_fd, 240 * 320 * 2);
        shared_framebuffer = mmap(NULL, 240 * 320 * 2,
                                  PROT_READ | PROT_WRITE,
                                  MAP_SHARED, shm_framebuffer_fd, 0);
        if (shared_framebuffer != MAP_FAILED)
        {
            /* Initialize shared framebuffer to black */
            memset(shared_framebuffer, 0, 240 * 320 * 2);
            LOG_INF("✅ Framebuffer file mapped (/tmp/akira_framebuffer)");
        }
        else
        {
            LOG_WRN("⚠️  Failed to mmap framebuffer file: errno=%d (%s)", errno, strerror(errno));
            shared_framebuffer = NULL;
        }
    }
    else
    {
        LOG_WRN("⚠️  Failed to create framebuffer file: errno=%d (%s)", errno, strerror(errno));
    }

    /* Create shared memory for buttons */
    shm_buttons_fd = open("/tmp/akira_buttons", O_CREAT | O_RDWR, 0666);
    if (shm_buttons_fd >= 0)
    {
        ftruncate(shm_buttons_fd, sizeof(uint32_t));
        shared_buttons = mmap(NULL, sizeof(uint32_t),
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED, shm_buttons_fd, 0);
        if (shared_buttons != MAP_FAILED)
        {
            *shared_buttons = 0;
            LOG_INF("✅ Button file mapped (/tmp/akira_buttons)");
        }
        else
        {
            LOG_WRN("⚠️  Failed to mmap button file: errno=%d (%s)", errno, strerror(errno));
            shared_buttons = NULL;
        }
    }
    else
    {
        LOG_WRN("⚠️  Failed to create button file: errno=%d (%s)", errno, strerror(errno));
    }

    if (shared_framebuffer || shared_buttons)
    {
        LOG_INF("📺 Ready for external SDL2 viewer connection");
        LOG_INF("   Run: tools/akira_viewer &");
    }

    LOG_INF("Simulated 240x320 display framebuffer initialized");
    LOG_INF("Simulated buttons active");

#elif AKIRA_PLATFORM_ESP32S3
    LOG_INF("Running on ESP32-S3 - full hardware support");

    /* Initialize hardware display — runs for any board with CONFIG_DISPLAY=y.
     * display_hal will report ENOTSUP if no zephyr,display is in DT. */
#if defined(CONFIG_DISPLAY)
    {
        int ret = akira_display_hal_init();
        if (ret < 0 && ret != -ENOTSUP)
        {
            LOG_WRN("Display HAL initialization failed: %d", ret);
        }
    }
#endif

#elif AKIRA_PLATFORM_ESP32C6
    LOG_INF("Running on ESP32-C6 - RISC-V hardware support (WiFi 6 + BLE + 802.15.4)");

#if defined(CONFIG_DISPLAY)
    {
        int ret = akira_display_hal_init();
        if (ret < 0 && ret != -ENOTSUP)
        {
            LOG_WRN("Display HAL initialization failed: %d", ret);
        }
    }
#endif

#elif AKIRA_PLATFORM_ESP32H2
    LOG_INF("Running on ESP32-H2 - RISC-V minimal hardware support (BLE + 802.15.4, no WiFi)");

#if defined(CONFIG_DISPLAY)
    {
        int ret = akira_display_hal_init();
        if (ret < 0 && ret != -ENOTSUP)
        {
            LOG_WRN("Display HAL initialization failed: %d", ret);
        }
    }
#endif

#else
    LOG_INF("Running on ESP32 - full hardware support");

#if defined(CONFIG_DISPLAY)
    {
        int ret = akira_display_hal_init();
        if (ret < 0 && ret != -ENOTSUP)
        {
            LOG_WRN("Display HAL initialization failed: %d", ret);
        }
    }
#endif
#endif /* Platform-specific initialization */
    return 0;
}

/* Public accessor for hardware framebuffer (if present). Returns NULL when
 * no dedicated hw framebuffer is configured for this platform.
 */
uint16_t *akira_framebuffer_get(void)
{
#if defined(AKIRA_HAS_HW_FRAMEBUFFER)
    return hw_framebuffer[fb_write_idx];
#else
    LOG_WRN("akira_framebuffer_get: no SPIRAM framebuffer (need CONFIG_ESP_SPIRAM)");
    return NULL;
#endif
}

bool akira_has_display(void)
{
    return IS_ENABLED(CONFIG_DISPLAY);
}

bool akira_has_wifi(void)
{
    return IS_ENABLED(CONFIG_WIFI);
}

bool akira_has_spi(void)
{
    return IS_ENABLED(CONFIG_SPI);
}

bool akira_has_gpio(void)
{
    /* native_sim uses simulated GPIO, not real hardware pins */
    return !AKIRA_PLATFORM_NATIVE_SIM;
}

const char *akira_get_platform_name(void)
{
#if AKIRA_PLATFORM_NATIVE_SIM
    return "native_sim";
#elif AKIRA_PLATFORM_ESP32S3
    return "ESP32-S3";
#elif AKIRA_PLATFORM_ESP32C6
    return "ESP32-C6";
#elif AKIRA_PLATFORM_ESP32H2
    return "ESP32-H2";
#elif AKIRA_PLATFORM_ESP32
    return "ESP32";
#elif AKIRA_PLATFORM_STM32
    return "STM32";
#elif AKIRA_PLATFORM_NORDIC
    return "Nordic";
#else
    return "unknown";
#endif
}

const struct device *akira_get_gpio_device(const char *label)
{
#if AKIRA_PLATFORM_NATIVE_SIM
    /* On native_sim, return dummy pointer for simulation */
    static const struct device sim_gpio_dev;
    return &sim_gpio_dev;
#elif AKIRA_PLATFORM_STM32
    /* STM32 uses gpioa, gpiob, gpioc, etc. Try DT alias for generic gpio0 */
    if (label && strcmp(label, "gpio0") == 0)
    {
        const struct device *dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(gpio0));
        if (dev && device_is_ready(dev))
        {
            return dev;
        }
        LOG_WRN("gpio0 alias not available on this board");
        return NULL;
    }
    ARG_UNUSED(label);
    return NULL;
#elif AKIRA_PLATFORM_NORDIC
    /* Nordic uses gpio0, gpio1, etc. via device tree */
    ARG_UNUSED(label);
    return NULL;
#else
    /* On ESP32/ESP32-S3, use device tree */
    if (strcmp(label, "gpio0") == 0)
    {
        const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
        if (!device_is_ready(dev))
        {
            LOG_ERR("GPIO device not ready");
            return NULL;
        }
        return dev;
    }
    return NULL;
#endif
}

const struct device *akira_get_spi_device(const char *label)
{
#if AKIRA_PLATFORM_NATIVE_SIM
    /* On native_sim, return dummy pointer for simulation */
    static const struct device sim_spi_dev;
    return &sim_spi_dev;
#elif AKIRA_PLATFORM_ESP32 || AKIRA_PLATFORM_ESP32S3 || \
      AKIRA_PLATFORM_ESP32C6 || AKIRA_PLATFORM_ESP32H2
    /* On ESP32 variants, look up spi2 from the device tree */
    if (strcmp(label, "spi2") == 0)
    {
        const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(spi2));
        if (!device_is_ready(dev))
        {
            LOG_ERR("SPI device not ready");
            return NULL;
        }
        return dev;
    }
    return NULL;
#else
    /* All other platforms (RP2040, STM32, Nordic, etc.): caller uses DT macros */
    ARG_UNUSED(label);
    return NULL;
#endif
}

int akira_gpio_pin_configure(const struct device *dev, gpio_pin_t pin, gpio_flags_t flags)
{
    if (!dev)
    {
        return -ENODEV;
    }

#if AKIRA_PLATFORM_NATIVE_SIM
    /* On native_sim, simulate GPIO configuration */
    LOG_DBG("Simulated GPIO configure: pin %d, flags 0x%x", pin, flags);
    return 0;
#else
    return gpio_pin_configure(dev, pin, flags);
#endif
}

int akira_gpio_pin_set(const struct device *dev, gpio_pin_t pin, int value)
{
    if (!dev)
    {
        return -ENODEV;
    }

#if AKIRA_PLATFORM_NATIVE_SIM
    /* On native_sim, simulate GPIO write */
    LOG_DBG("Simulated GPIO set: pin %d = %d", pin, value);
    return 0;
#else
    return gpio_pin_set(dev, pin, value);
#endif
}

int akira_gpio_pin_get(const struct device *dev, gpio_pin_t pin)
{
    if (!dev)
    {
        return 0;
    }

#if AKIRA_PLATFORM_NATIVE_SIM
    /* On native_sim, return simulated button state */
    return (sim_button_state & (1 << pin)) ? 0 : 1; /* Active low simulation */
#else
    return gpio_pin_get(dev, pin);
#endif
}

int akira_spi_write(const struct device *dev, const struct spi_config *config,
                    const struct spi_buf_set *tx_bufs)
{
    if (!dev || !config || !tx_bufs)
    {
        return -EINVAL;
    }

#if AKIRA_PLATFORM_NATIVE_SIM
    /* On native_sim, update simulated display */
    LOG_DBG("Simulated SPI write: %d bytes", tx_bufs->buffers[0].len);
    sim_display_dirty = true;
    return 0;
#else
    return spi_write(dev, config, tx_bufs);
#endif
}

/* Simulation functions for native_sim */
#if AKIRA_PLATFORM_NATIVE_SIM

uint32_t akira_sim_read_buttons(void)
{
    /* Read button state from shared memory (written by SDL2 viewer) */
    if (shared_buttons)
    {
        sim_button_state = *shared_buttons;
    }
    return sim_button_state;
}

void akira_sim_draw_pixel(int x, int y, uint16_t color)
{
    if (x >= 0 && x < 240 && y >= 0 && y < 320)
    {
        sim_framebuffer[y * 240 + x] = color;
        sim_display_dirty = true;
    }
}

void akira_sim_show_display(void)
{
    if (!sim_display_dirty)
    {
        return;
    }

    /* Copy framebuffer to shared memory for SDL2 viewer */
    if (shared_framebuffer)
    {
        memcpy(shared_framebuffer, sim_framebuffer, 240 * 320 * 2);
    }

    /* Log periodic updates */
    static uint32_t update_count = 0;
    if (++update_count % 100 == 0)
    {
        LOG_DBG("Display updated (%u frames)", update_count);
    }

    sim_display_dirty = false;
}

#else

/* Stub implementations for hardware platforms */
uint32_t akira_sim_read_buttons(void)
{
    return 0;
}

void akira_sim_draw_pixel(int x, int y, uint16_t color)
{
    /* Not used on hardware platforms */
}

void akira_sim_show_display(void)
{
    /* Not used on hardware platforms */
}

#endif

void akira_hal_reset(void)
{
    sys_reboot(SYS_REBOOT_COLD);
}

const char *akira_hal_platform(void)
{
    return akira_get_platform_name();
}
