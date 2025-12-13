/**
 * @file akira_micro_test.c
 * @brief Integration test for Akira-Micro hardware
 * 
 * Tests:
 * - 6 buttons (KEY_1 through KEY_6)
 * - SD card support
 * - Status LED
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fatfs_ff.h>
#include <string.h>

LOG_MODULE_REGISTER(akira_micro_test, LOG_LEVEL_INF);

/* GPIO pins for Akira-Micro buttons (ESP32) */
#define KEY_1_PIN  35  /* IO35 */
#define KEY_2_PIN  34  /* IO34 */
#define KEY_3_PIN  39  /* IO39 */
#define KEY_4_PIN  36  /* IO36 */
#define KEY_5_PIN  14  /* IO14 */
#define KEY_6_PIN  13  /* IO13 */

/* Status LED */
#define STATUS_LED_PIN  32  /* IO32 - Blue LED */

static const struct device *gpio_dev;

/* Button states */
static bool button_pressed[6] = {false};

/**
 * @brief Initialize GPIO for buttons and LED
 */
static int init_gpio(void)
{
    gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
    if (!device_is_ready(gpio_dev)) {
        LOG_ERR("GPIO device not ready");
        return -ENODEV;
    }

    /* Configure buttons as inputs with pull-up (active low) */
    gpio_pin_configure(gpio_dev, KEY_1_PIN, GPIO_INPUT | GPIO_PULL_UP);
    gpio_pin_configure(gpio_dev, KEY_2_PIN, GPIO_INPUT | GPIO_PULL_UP);
    gpio_pin_configure(gpio_dev, KEY_3_PIN, GPIO_INPUT | GPIO_PULL_UP);
    gpio_pin_configure(gpio_dev, KEY_4_PIN, GPIO_INPUT | GPIO_PULL_UP);
    gpio_pin_configure(gpio_dev, KEY_5_PIN, GPIO_INPUT | GPIO_PULL_UP);
    gpio_pin_configure(gpio_dev, KEY_6_PIN, GPIO_INPUT | GPIO_PULL_UP);

    /* Configure status LED as output */
    gpio_pin_configure(gpio_dev, STATUS_LED_PIN, GPIO_OUTPUT_ACTIVE);
    gpio_pin_set(gpio_dev, STATUS_LED_PIN, 0);

    LOG_INF("GPIO initialized successfully");
    return 0;
}

/**
 * @brief Read button states
 */
static void read_buttons(void)
{
    /* Buttons are active low (pressed = 0) */
    button_pressed[0] = !gpio_pin_get(gpio_dev, KEY_1_PIN);
    button_pressed[1] = !gpio_pin_get(gpio_dev, KEY_2_PIN);
    button_pressed[2] = !gpio_pin_get(gpio_dev, KEY_3_PIN);
    button_pressed[3] = !gpio_pin_get(gpio_dev, KEY_4_PIN);
    button_pressed[4] = !gpio_pin_get(gpio_dev, KEY_5_PIN);
    button_pressed[5] = !gpio_pin_get(gpio_dev, KEY_6_PIN);
}

/**
 * @brief Print button states
 */
static void print_button_states(void)
{
    LOG_INF("Buttons: KEY1=%d KEY2=%d KEY3=%d KEY4=%d KEY5=%d KEY6=%d",
            button_pressed[0], button_pressed[1], button_pressed[2],
            button_pressed[3], button_pressed[4], button_pressed[5]);
}

/**
 * @brief Blink LED pattern based on button presses
 */
static void update_led(void)
{
    static bool led_state = false;
    static uint32_t last_blink = 0;
    uint32_t now = k_uptime_get_32();

    /* Count pressed buttons */
    int pressed_count = 0;
    for (int i = 0; i < 6; i++) {
        if (button_pressed[i]) {
            pressed_count++;
        }
    }

    /* Blink faster when buttons are pressed */
    uint32_t blink_interval = pressed_count > 0 ? (200 / pressed_count) : 1000;

    if (now - last_blink >= blink_interval) {
        led_state = !led_state;
        gpio_pin_set(gpio_dev, STATUS_LED_PIN, led_state);
        last_blink = now;
    }
}

/**
 * @brief Test SD card functionality
 */
static int test_sd_card(void)
{
    FATFS fat_fs;
    FIL file;
    FRESULT res;
    UINT bytes_written;
    const char *disk_mount_pt = "/SD:";
    const char *test_file = "/SD:/akira_test.txt";
    const char *test_data = "Akira-Micro SD Card Test\n";

    LOG_INF("=== SD Card Test ===");

    /* Mount SD card */
    res = f_mount(&fat_fs, disk_mount_pt, 1);
    if (res != FR_OK) {
        LOG_ERR("Failed to mount SD card: %d", res);
        return -EIO;
    }
    LOG_INF("SD card mounted successfully");

    /* Create and write test file */
    res = f_open(&file, test_file, FA_CREATE_ALWAYS | FA_WRITE);
    if (res != FR_OK) {
        LOG_ERR("Failed to create file: %d", res);
        f_unmount(disk_mount_pt);
        return -EIO;
    }

    res = f_write(&file, test_data, strlen(test_data), &bytes_written);
    if (res != FR_OK) {
        LOG_ERR("Failed to write file: %d", res);
        f_close(&file);
        f_unmount(disk_mount_pt);
        return -EIO;
    }

    f_close(&file);
    LOG_INF("Wrote %d bytes to %s", bytes_written, test_file);

    /* Read back the file */
    res = f_open(&file, test_file, FA_READ);
    if (res != FR_OK) {
        LOG_ERR("Failed to open file for reading: %d", res);
        f_unmount(disk_mount_pt);
        return -EIO;
    }

    char read_buffer[64];
    UINT bytes_read;
    res = f_read(&file, read_buffer, sizeof(read_buffer) - 1, &bytes_read);
    if (res != FR_OK) {
        LOG_ERR("Failed to read file: %d", res);
        f_close(&file);
        f_unmount(disk_mount_pt);
        return -EIO;
    }

    read_buffer[bytes_read] = '\0';
    f_close(&file);
    LOG_INF("Read %d bytes: %s", bytes_read, read_buffer);

    /* Verify data */
    if (strcmp(read_buffer, test_data) == 0) {
        LOG_INF("SD card test PASSED!");
    } else {
        LOG_ERR("SD card test FAILED - data mismatch");
        f_unmount(disk_mount_pt);
        return -EIO;
    }

    f_unmount(disk_mount_pt);
    LOG_INF("SD card unmounted");
    return 0;
}

/**
 * @brief Main test thread
 */
int main(void)
{
    LOG_INF("===========================================");
    LOG_INF("  Akira-Micro Hardware Integration Test");
    LOG_INF("===========================================");

    /* Initialize GPIO */
    if (init_gpio() != 0) {
        LOG_ERR("Failed to initialize GPIO");
        return -1;
    }

    /* Test SD card */
    k_sleep(K_MSEC(1000));  /* Give SD card time to initialize */
    int sd_result = test_sd_card();
    if (sd_result == 0) {
        LOG_INF("✓ SD Card: PASS");
    } else {
        LOG_ERR("✗ SD Card: FAIL");
    }

    LOG_INF("");
    LOG_INF("=== Button Test (press buttons to see status) ===");
    LOG_INF("Press any combination of KEY1-KEY6 buttons");
    LOG_INF("LED will blink faster when buttons are pressed");
    LOG_INF("");

    /* Main loop - monitor buttons and control LED */
    while (1) {
        read_buttons();
        print_button_states();
        update_led();
        k_sleep(K_MSEC(100));
    }

    return 0;
}
