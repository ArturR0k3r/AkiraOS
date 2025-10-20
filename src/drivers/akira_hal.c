/**
 * @file akira_hal.c
 * @brief Akira Hardware Abstraction Layer Implementation
 */

#include "akira_hal.h"
#include <zephyr/logging/log.h>
#include <string.h>

#if AKIRA_PLATFORM_NATIVE_SIM
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#endif

LOG_MODULE_REGISTER(akira_hal, LOG_LEVEL_INF);

#if AKIRA_PLATFORM_NATIVE_SIM
/* Simulated display framebuffer (240x320 RGB565) */
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

int akira_hal_init(void)
{
    LOG_INF("Akira HAL initializing for: %s", akira_get_platform_name());

#if AKIRA_PLATFORM_NATIVE_SIM
    LOG_INF("Running in SIMULATION mode with display and button emulation");

    /* Initialize simulated framebuffer to black */
    memset(sim_framebuffer, 0, sizeof(sim_framebuffer));

    /* Create shared memory for external SDL2 viewer */
    shm_framebuffer_fd = shm_open("/akira_framebuffer",
                                  O_CREAT | O_RDWR, 0666);
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
            LOG_INF("✅ Framebuffer shared memory created (/akira_framebuffer)");
        }
        else
        {
            LOG_WRN("⚠️  Failed to mmap framebuffer shared memory");
            shared_framebuffer = NULL;
        }
    }
    else
    {
        LOG_WRN("⚠️  Failed to create framebuffer shared memory");
    }

    /* Create shared memory for buttons */
    shm_buttons_fd = shm_open("/akira_buttons",
                              O_CREAT | O_RDWR, 0666);
    if (shm_buttons_fd >= 0)
    {
        ftruncate(shm_buttons_fd, sizeof(uint32_t));
        shared_buttons = mmap(NULL, sizeof(uint32_t),
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED, shm_buttons_fd, 0);
        if (shared_buttons != MAP_FAILED)
        {
            *shared_buttons = 0;
            LOG_INF("✅ Button shared memory created (/akira_buttons)");
        }
        else
        {
            LOG_WRN("⚠️  Failed to mmap button shared memory");
            shared_buttons = NULL;
        }
    }
    else
    {
        LOG_WRN("⚠️  Failed to create button shared memory");
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
#elif AKIRA_PLATFORM_ESP32
    LOG_INF("Running on ESP32 - full hardware support");
#else
    LOG_WRN("Running on unknown platform");
#endif

    return 0;
}

bool akira_has_display(void)
{
    return AKIRA_HAS_DISPLAY;
}

bool akira_has_wifi(void)
{
    return AKIRA_HAS_WIFI;
}

bool akira_has_spi(void)
{
    return AKIRA_HAS_SPI;
}

bool akira_has_gpio(void)
{
    return AKIRA_HAS_REAL_GPIO;
}

const char *akira_get_platform_name(void)
{
#if AKIRA_PLATFORM_NATIVE_SIM
    return "native_sim";
#elif AKIRA_PLATFORM_ESP32S3
    return "ESP32-S3";
#elif AKIRA_PLATFORM_ESP32
    return "ESP32";
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
#else
    /* On ESP32/ESP32-S3, use device tree */
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
