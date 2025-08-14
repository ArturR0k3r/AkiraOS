#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
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

// Display dimensions
#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  320

// GPIO pin definitions
#define DC_GPIO_PIN     2
#define RESET_GPIO_PIN  4
#define BACKLIGHT_GPIO_PIN 16

// ILI9341 Commands
#define ILI9341_SWRESET     0x01
#define ILI9341_SLPOUT      0x11
#define ILI9341_DISPOFF     0x28
#define ILI9341_DISPON      0x29
#define ILI9341_CASET       0x2A
#define ILI9341_PASET       0x2B
#define ILI9341_RAMWR       0x2C
#define ILI9341_MADCTL      0x36
#define ILI9341_COLMOD      0x3A
#define ILI9341_PWCTR1      0xC0
#define ILI9341_PWCTR2      0xC1
#define ILI9341_VMCTR1      0xC5
#define ILI9341_VMCTR2      0xC7
#define ILI9341_GMCTRP1     0xE0
#define ILI9341_GMCTRN1     0xE1

static const struct device *spi_dev;
static const struct device *gpio_dev;
static struct spi_config spi_cfg;
static struct spi_cs_control cs_ctrl;

// Function to send command to display
static int ili9341_send_cmd(uint8_t cmd)
{
    struct spi_buf tx_buf = {
        .buf = &cmd,
        .len = 1
    };
    struct spi_buf_set tx_bufs = {
        .buffers = &tx_buf,
        .count = 1
    };

    // Set D/C low for command
    gpio_pin_set(gpio_dev, DC_GPIO_PIN, 0);
    k_sleep(K_USEC(1));
    
    int ret = spi_write(spi_dev, &spi_cfg, &tx_bufs);
    k_sleep(K_USEC(1));
    
    return ret;
}

// Function to send data to display
static int ili9341_send_data(uint8_t *data, size_t len)
{
    struct spi_buf tx_buf = {
        .buf = data,
        .len = len
    };
    struct spi_buf_set tx_bufs = {
        .buffers = &tx_buf,
        .count = 1
    };

    // Set D/C high for data
    gpio_pin_set(gpio_dev, DC_GPIO_PIN, 1);
    k_sleep(K_USEC(1));
    
    int ret = spi_write(spi_dev, &spi_cfg, &tx_bufs);
    k_sleep(K_USEC(1));
    
    return ret;
}

// Function to send a single data byte
static int ili9341_send_data_byte(uint8_t data)
{
    return ili9341_send_data(&data, 1);
}

// Function to initialize the display
static int ili9341_init(void)
{
    LOG_INF("Initializing ILI9341 display...");
    
    // Hardware reset
    gpio_pin_set(gpio_dev, RESET_GPIO_PIN, 0);
    k_sleep(K_MSEC(10));
    gpio_pin_set(gpio_dev, RESET_GPIO_PIN, 1);
    k_sleep(K_MSEC(120));
    
    // Software reset
    ili9341_send_cmd(ILI9341_SWRESET);
    k_sleep(K_MSEC(150));
    
    // Exit sleep mode
    ili9341_send_cmd(ILI9341_SLPOUT);
    k_sleep(K_MSEC(120));
    
    // Power control 1
    ili9341_send_cmd(ILI9341_PWCTR1);
    ili9341_send_data_byte(0x23);
    
    // Power control 2
    ili9341_send_cmd(ILI9341_PWCTR2);
    ili9341_send_data_byte(0x10);
    
    // VCOM control 1
    ili9341_send_cmd(ILI9341_VMCTR1);
    ili9341_send_data_byte(0x3E);
    ili9341_send_data_byte(0x28);
    
    // VCOM control 2
    ili9341_send_cmd(ILI9341_VMCTR2);
    ili9341_send_data_byte(0x86);
    
    // Memory access control
    ili9341_send_cmd(ILI9341_MADCTL);
    ili9341_send_data_byte(0x48); // Portrait mode, RGB order
    
    // Pixel format
    ili9341_send_cmd(ILI9341_COLMOD);
    ili9341_send_data_byte(0x55); // 16-bit RGB565
    
    // Positive gamma correction
    ili9341_send_cmd(ILI9341_GMCTRP1);
    uint8_t gamma_p[] = {0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1,
                         0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00};
    ili9341_send_data(gamma_p, sizeof(gamma_p));
    
    // Negative gamma correction
    ili9341_send_cmd(ILI9341_GMCTRN1);
    uint8_t gamma_n[] = {0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1,
                         0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F};
    ili9341_send_data(gamma_n, sizeof(gamma_n));
    
    // Display on
    ili9341_send_cmd(ILI9341_DISPON);
    k_sleep(K_MSEC(100));
    
    LOG_INF("ILI9341 display initialized successfully");
    return 0;
}

// Function to set drawing area
static int ili9341_set_area(uint16_t x_start, uint16_t y_start, uint16_t x_end, uint16_t y_end)
{
    // Column address set
    ili9341_send_cmd(ILI9341_CASET);
    ili9341_send_data_byte(x_start >> 8);
    ili9341_send_data_byte(x_start & 0xFF);
    ili9341_send_data_byte(x_end >> 8);
    ili9341_send_data_byte(x_end & 0xFF);
    
    // Page address set
    ili9341_send_cmd(ILI9341_PASET);
    ili9341_send_data_byte(y_start >> 8);
    ili9341_send_data_byte(y_start & 0xFF);
    ili9341_send_data_byte(y_end >> 8);
    ili9341_send_data_byte(y_end & 0xFF);
    
    return 0;
}

// Function to fill entire display with a color
static int fill_display_color(uint16_t color)
{
    LOG_DBG("Filling display with color 0x%04X", color);
    
    // Set full screen area
    ili9341_set_area(0, 0, DISPLAY_WIDTH - 1, DISPLAY_HEIGHT - 1);
    
    // Start memory write
    ili9341_send_cmd(ILI9341_RAMWR);
    
    // Convert color to big endian for transmission
    uint8_t color_bytes[2] = {color >> 8, color & 0xFF};
    
    // Fill display with color (in chunks to avoid large memory allocation)
    const size_t chunk_size = 1024; // pixels per chunk
    uint8_t *chunk_buffer = k_malloc(chunk_size * 2);
    if (!chunk_buffer) {
        LOG_ERR("Failed to allocate chunk buffer");
        return -ENOMEM;
    }
    
    // Fill chunk buffer with color
    for (size_t i = 0; i < chunk_size * 2; i += 2) {
        chunk_buffer[i] = color_bytes[0];
        chunk_buffer[i + 1] = color_bytes[1];
    }
    
    // Send color data in chunks
    size_t total_pixels = DISPLAY_WIDTH * DISPLAY_HEIGHT;
    size_t pixels_sent = 0;
    
    while (pixels_sent < total_pixels) {
        size_t pixels_to_send = (total_pixels - pixels_sent > chunk_size) ? 
                               chunk_size : (total_pixels - pixels_sent);
        
        int ret = ili9341_send_data(chunk_buffer, pixels_to_send * 2);
        if (ret < 0) {
            LOG_ERR("Failed to send pixel data: %d", ret);
            k_free(chunk_buffer);
            return ret;
        }
        
        pixels_sent += pixels_to_send;
    }
    
    k_free(chunk_buffer);
    LOG_INF("Display filled with color 0x%04X", color);
    return 0;
}

// Function to draw color bars
static int draw_color_bars(void)
{
    LOG_INF("Drawing color bars");
    
    uint16_t colors[] = {WHITE_COLOR, RED_COLOR, GREEN_COLOR, BLUE_COLOR, 
                        YELLOW_COLOR, MAGENTA_COLOR, CYAN_COLOR, BLACK_COLOR};
    int bar_height = DISPLAY_HEIGHT / 8;
    
    for (int i = 0; i < 8; i++) {
        int y_start = i * bar_height;
        int y_end = (i == 7) ? DISPLAY_HEIGHT - 1 : (i + 1) * bar_height - 1;
        
        ili9341_set_area(0, y_start, DISPLAY_WIDTH - 1, y_end);
        ili9341_send_cmd(ILI9341_RAMWR);
        
        uint8_t color_bytes[2] = {colors[i] >> 8, colors[i] & 0xFF};
        int pixels_in_bar = DISPLAY_WIDTH * (y_end - y_start + 1);
        
        for (int p = 0; p < pixels_in_bar; p++) {
            ili9341_send_data(color_bytes, 2);
        }
    }
    
    LOG_INF("Color bars displayed");
    return 0;
}

// Function to draw a checkerboard pattern
static int draw_test_pattern(void)
{
    LOG_INF("Drawing checkerboard pattern");
    
    ili9341_set_area(0, 0, DISPLAY_WIDTH - 1, DISPLAY_HEIGHT - 1);
    ili9341_send_cmd(ILI9341_RAMWR);
    
    uint8_t white_bytes[2] = {WHITE_COLOR >> 8, WHITE_COLOR & 0xFF};
    uint8_t black_bytes[2] = {BLACK_COLOR >> 8, BLACK_COLOR & 0xFF};
    
    for (int y = 0; y < DISPLAY_HEIGHT; y++) {
        for (int x = 0; x < DISPLAY_WIDTH; x++) {
            bool checker = ((x / 20) + (y / 20)) % 2;
            uint8_t *color_bytes = checker ? white_bytes : black_bytes;
            ili9341_send_data(color_bytes, 2);
        }
    }
    
    LOG_INF("Test pattern displayed");
    return 0;
}

// Function to control backlight
static int backlight_init(void)
{
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

int main(void)
{
    int ret;
    
    printk("=== ESP32 ILI9341 Display Test Starting ===\n");
    LOG_INF("ESP32 ILI9341 Display Test Starting...");
    
    // Get GPIO device
    gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
    if (!device_is_ready(gpio_dev)) {
        LOG_ERR("GPIO device not ready");
        printk("ERROR: GPIO device not ready!\n");
        return -ENODEV;
    }
    
    // Get SPI device
    spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi2));
    if (!device_is_ready(spi_dev)) {
        LOG_ERR("SPI device not ready");
        printk("ERROR: SPI device not ready!\n");
        return -ENODEV;
    }
    
    LOG_INF("SPI and GPIO devices ready");
    printk("SPI and GPIO devices ready\n");
    
    // Configure GPIO pins
    ret = gpio_pin_configure(gpio_dev, DC_GPIO_PIN, GPIO_OUTPUT_ACTIVE);
    if (ret < 0) {
        LOG_ERR("Failed to configure DC GPIO: %d", ret);
        return ret;
    }
    
    ret = gpio_pin_configure(gpio_dev, RESET_GPIO_PIN, GPIO_OUTPUT_ACTIVE);
    if (ret < 0) {
        LOG_ERR("Failed to configure RESET GPIO: %d", ret);
        return ret;
    }
    
    // Configure SPI
    spi_cfg.operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_MODE_CPOL | SPI_MODE_CPHA;
    spi_cfg.frequency = 25000000; // 25MHz
    spi_cfg.slave = 0;
    
    // Initialize backlight
    ret = backlight_init();
    if (ret < 0) {
        LOG_WRN("Failed to initialize backlight: %d", ret);
        printk("Warning: Backlight initialization failed\n");
    }
    
    // Initialize display
    ret = ili9341_init();
    if (ret < 0) {
        LOG_ERR("Failed to initialize ILI9341: %d", ret);
        printk("ERROR: Failed to initialize display: %d\n", ret);
        return ret;
    }
    
    printk("Display initialized successfully\n");
    LOG_INF("Starting test sequence...");
    
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
        printk("Testing backlight toggle...\n");
        LOG_INF("Testing backlight toggle");
        gpio_pin_set(gpio_dev, BACKLIGHT_GPIO_PIN, 0); // Off
        k_sleep(K_MSEC(500));
        gpio_pin_set(gpio_dev, BACKLIGHT_GPIO_PIN, 1); // On

        // Small pause before next cycle
        k_sleep(K_SECONDS(1));
    }

    return 0;
}