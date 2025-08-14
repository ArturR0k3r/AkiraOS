#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

// Display colors (RGB565 format)
#define WHITE_COLOR     0xFFFF
#define RED_COLOR       0xF800
#define GREEN_COLOR     0x07E0
#define BLUE_COLOR      0x001F
#define BLACK_COLOR     0x0000
#define YELLOW_COLOR    0xFFE0
#define MAGENTA_COLOR   0xF81F
#define CYAN_COLOR      0x07FF

static const struct device *display_dev;
static const struct device *gpio_dev;

static struct display_capabilities caps;

// Backlight control (GPIO16)
#define BACKLIGHT_GPIO_PIN 16

// Function to control backlight
static int backlight_init(void)
{
    gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
    if (!device_is_ready(gpio_dev)) {
        LOG_ERR("GPIO device not ready");
        return -ENODEV;
    }
    
    int ret = gpio_pin_configure(gpio_dev, BACKLIGHT_GPIO_PIN, GPIO_OUTPUT_ACTIVE);
    if (ret < 0) {
        LOG_ERR("Failed to configure backlight GPIO: %d", ret);
        return ret;
    }
    
    // Turn on backlight
    gpio_pin_set(gpio_dev, BACKLIGHT_GPIO_PIN, 1);
    LOG_INF("Backlight turned on");
    return 0;
}

// Function to fill entire display with a color
static int fill_display_color(uint16_t color)
{
    struct display_buffer_descriptor buf_desc;
    uint16_t *buf;
    int ret;

    // Calculate buffer size
    size_t buf_size = caps.x_resolution * caps.y_resolution * sizeof(uint16_t);
    
    // Allocate buffer
    buf = k_malloc(buf_size);
    if (!buf) {
        LOG_ERR("Failed to allocate display buffer (size: %zu bytes)", buf_size);
        return -ENOMEM;
    }

    LOG_DBG("Filling buffer with color 0x%04X", color);
    // Fill buffer with color
    for (int i = 0; i < caps.x_resolution * caps.y_resolution; i++) {
        buf[i] = color;
    }

    // Setup buffer descriptor
    buf_desc.buf_size = buf_size;
    buf_desc.width = caps.x_resolution;
    buf_desc.height = caps.y_resolution;
    buf_desc.pitch = caps.x_resolution;

    LOG_DBG("Writing buffer to display");
    // Write to display
    ret = display_write(display_dev, 0, 0, &buf_desc, buf);
    if (ret < 0) {
        LOG_ERR("Failed to write to display: %d", ret);
    } else {
        LOG_INF("Display filled with color 0x%04X", color);
    }

    // Free buffer
    k_free(buf);
    return ret;
}

// Function to draw color bars
static int draw_color_bars(void)
{
    struct display_buffer_descriptor buf_desc;
    uint16_t *buf;
    int ret;
    
    size_t buf_size = caps.x_resolution * caps.y_resolution * sizeof(uint16_t);
    
    buf = k_malloc(buf_size);
    if (!buf) {
        LOG_ERR("Failed to allocate buffer for color bars");
        return -ENOMEM;
    }

    // Create color bars - divide display into 8 horizontal bands
    uint16_t colors[] = {WHITE_COLOR, RED_COLOR, GREEN_COLOR, BLUE_COLOR, 
                        YELLOW_COLOR, MAGENTA_COLOR, CYAN_COLOR, BLACK_COLOR};
    int bar_height = caps.y_resolution / 8;
    
    LOG_DBG("Creating color bars, bar height: %d", bar_height);
    
    for (int y = 0; y < caps.y_resolution; y++) {
        int color_index = y / bar_height;
        if (color_index >= 8) color_index = 7; // Clamp to last color
        
        for (int x = 0; x < caps.x_resolution; x++) {
            buf[y * caps.x_resolution + x] = colors[color_index];
        }
    }

    buf_desc.buf_size = buf_size;
    buf_desc.width = caps.x_resolution;
    buf_desc.height = caps.y_resolution;
    buf_desc.pitch = caps.x_resolution;

    ret = display_write(display_dev, 0, 0, &buf_desc, buf);
    if (ret < 0) {
        LOG_ERR("Failed to write color bars: %d", ret);
    } else {
        LOG_INF("Color bars displayed");
    }

    k_free(buf);
    return ret;
}

// Function to draw a simple test pattern
static int draw_test_pattern(void)
{
    struct display_buffer_descriptor buf_desc;
    uint16_t *buf;
    int ret;
    
    size_t buf_size = caps.x_resolution * caps.y_resolution * sizeof(uint16_t);
    
    buf = k_malloc(buf_size);
    if (!buf) {
        LOG_ERR("Failed to allocate buffer for test pattern");
        return -ENOMEM;
    }

    // Create a checkerboard pattern
    for (int y = 0; y < caps.y_resolution; y++) {
        for (int x = 0; x < caps.x_resolution; x++) {
            bool checker = ((x / 20) + (y / 20)) % 2;
            buf[y * caps.x_resolution + x] = checker ? WHITE_COLOR : BLACK_COLOR;
        }
    }

    buf_desc.buf_size = buf_size;
    buf_desc.width = caps.x_resolution;
    buf_desc.height = caps.y_resolution;
    buf_desc.pitch = caps.x_resolution;

    ret = display_write(display_dev, 0, 0, &buf_desc, buf);
    if (ret < 0) {
        LOG_ERR("Failed to write test pattern: %d", ret);
    } else {
        LOG_INF("Test pattern displayed");
    }

    k_free(buf);
    return ret;
}

int main(void)
{
    int ret;
    
    printk("=== ESP32 ILI9341 Display Test Starting ===\n");
    LOG_INF("ESP32 ILI9341 Display Test Starting...");
    
    // Get display device using device tree
    display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    if (!device_is_ready(display_dev)) {
        LOG_ERR("Display device not ready");
        printk("ERROR: Display device not ready!\n");
        return -ENODEV;
    }
    
    LOG_INF("Display device ready");
    printk("Display device ready\n");
    
    // Initialize backlight
    ret = backlight_init();
    if (ret < 0) {
        LOG_WRN("Failed to initialize backlight: %d", ret);
        printk("Warning: Backlight initialization failed\n");
    } else {
        printk("Backlight initialized\n");
    }

    // Get display capabilities
    display_get_capabilities(display_dev, &caps);
    LOG_INF("Display capabilities:");
    LOG_INF("  Resolution: %dx%d", caps.x_resolution, caps.y_resolution);
    LOG_INF("  Supported pixel formats: 0x%02X", caps.supported_pixel_formats);
    LOG_INF("  Current pixel format: 0x%02X", caps.current_pixel_format);
    LOG_INF("  Current orientation: %d", caps.current_orientation);
    
    printk("Display: %dx%d pixels\n", caps.x_resolution, caps.y_resolution);

    // Enable display (turn off blanking)
    ret = display_blanking_off(display_dev);
    if (ret < 0) {
        LOG_ERR("Failed to turn on display: %d", ret);
        printk("ERROR: Failed to turn on display: %d\n", ret);
        return ret;
    }

    LOG_INF("Display turned on, starting test sequence...");
    printk("Display turned on, starting test sequence...\n");

    int test_cycle = 0;
    while (1) {
        test_cycle++;
        LOG_INF("=== Test cycle %d ===", test_cycle);
        printk("=== Test cycle %d ===\n", test_cycle);
        
        // Fill with white
        printk("Filling with WHITE...\n");
        ret = fill_display_color(WHITE_COLOR);
        if (ret < 0) {
            LOG_ERR("Failed to fill with white: %d", ret);
            printk("ERROR: Failed to fill with white\n");
        }
        k_sleep(K_SECONDS(2));

        // Fill with red
        printk("Filling with RED...\n");
        ret = fill_display_color(RED_COLOR);
        if (ret < 0) {
            LOG_ERR("Failed to fill with red: %d", ret);
            printk("ERROR: Failed to fill with red\n");
        }
        k_sleep(K_SECONDS(2));

        // Fill with green
        printk("Filling with GREEN...\n");
        ret = fill_display_color(GREEN_COLOR);
        if (ret < 0) {
            LOG_ERR("Failed to fill with green: %d", ret);
            printk("ERROR: Failed to fill with green\n");
        }
        k_sleep(K_SECONDS(2));

        // Fill with blue
        printk("Filling with BLUE...\n");
        ret = fill_display_color(BLUE_COLOR);
        if (ret < 0) {
            LOG_ERR("Failed to fill with blue: %d", ret);
            printk("ERROR: Failed to fill with blue\n");
        }
        k_sleep(K_SECONDS(2));

        // Show color bars
        printk("Displaying color bars...\n");
        ret = draw_color_bars();
        if (ret < 0) {
            LOG_ERR("Failed to draw color bars: %d", ret);
            printk("ERROR: Failed to draw color bars\n");
        }
        k_sleep(K_SECONDS(3));

        // Show test pattern
        printk("Displaying checkerboard pattern...\n");
        ret = draw_test_pattern();
        if (ret < 0) {
            LOG_ERR("Failed to draw test pattern: %d", ret);
            printk("ERROR: Failed to draw test pattern\n");
        }
        k_sleep(K_SECONDS(2));

        // Test backlight toggle
        if (gpio_dev) {
            printk("Testing backlight toggle...\n");
            LOG_INF("Testing backlight toggle");
            gpio_pin_set(gpio_dev, BACKLIGHT_GPIO_PIN, 0); // Off
            k_sleep(K_MSEC(500));
            gpio_pin_set(gpio_dev, BACKLIGHT_GPIO_PIN, 1); // On
        }

        // Small pause before next cycle
        k_sleep(K_SECONDS(1));
    }

    return 0;
}