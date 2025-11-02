/**
 * @file akira_audio_shell.c
 * @brief Shell commands for AkiraOS audio subsystem
 *
 * Provides command-line interface for testing and controlling
 * the piezo MEMS micro-speaker.
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <stdlib.h>
#include <string.h>
#include "../drivers/akira_audio.h"

/**
 * @brief Audio initialization command
 */
static int cmd_audio_init(const struct shell *sh, size_t argc, char **argv)
{
    int ret = akira_audio_init();
    if (ret < 0) {
        shell_error(sh, "Failed to initialize audio: %d", ret);
        return ret;
    }
    
    shell_print(sh, "Audio subsystem initialized successfully");
    return 0;
}

/**
 * @brief Audio status command
 */
static int cmd_audio_status(const struct shell *sh, size_t argc, char **argv)
{
    akira_audio_status_t status;
    int ret = akira_audio_get_status(&status);
    
    if (ret < 0) {
        shell_error(sh, "Failed to get status: %d", ret);
        return ret;
    }
    
    shell_print(sh, "Audio Status:");
    shell_print(sh, "  Initialized: %s", status.initialized ? "Yes" : "No");
    shell_print(sh, "  Playing: %s", status.playing ? "Yes" : "No");
    shell_print(sh, "  Current Frequency: %u Hz", status.current_freq);
    shell_print(sh, "  Master Volume: %u%%", status.current_volume);
    shell_print(sh, "  Samples Played: %u", status.samples_played);
    
    return 0;
}

/**
 * @brief Play simple tone command
 * Usage: audio tone <frequency> <duration> <volume>
 */
static int cmd_audio_tone(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 4) {
        shell_error(sh, "Usage: audio tone <frequency_hz> <duration_ms> <volume_0-100>");
        shell_print(sh, "Example: audio tone 1000 500 80");
        return -EINVAL;
    }
    
    uint16_t frequency = (uint16_t)atoi(argv[1]);
    uint32_t duration = (uint32_t)atoi(argv[2]);
    uint8_t volume = (uint8_t)atoi(argv[3]);
    
    if (frequency < AKIRA_AUDIO_MIN_FREQUENCY || frequency > AKIRA_AUDIO_MAX_FREQUENCY) {
        shell_error(sh, "Frequency must be between %d and %d Hz",
                    AKIRA_AUDIO_MIN_FREQUENCY, AKIRA_AUDIO_MAX_FREQUENCY);
        return -EINVAL;
    }
    
    if (volume > 100) {
        shell_error(sh, "Volume must be between 0 and 100");
        return -EINVAL;
    }
    
    shell_print(sh, "Playing %u Hz tone for %u ms at %u%% volume...",
                frequency, duration, volume);
    
    int ret = akira_audio_play_tone(frequency, duration, volume);
    if (ret < 0) {
        shell_error(sh, "Failed to play tone: %d", ret);
        return ret;
    }
    
    shell_print(sh, "Done");
    return 0;
}

/**
 * @brief Frequency sweep command for testing
 * Usage: audio sweep <start_freq> <end_freq> <step>
 */
static int cmd_audio_sweep(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 4) {
        shell_error(sh, "Usage: audio sweep <start_hz> <end_hz> <step_hz>");
        shell_print(sh, "Example: audio sweep 100 10000 100");
        return -EINVAL;
    }
    
    uint16_t start_freq = (uint16_t)atoi(argv[1]);
    uint16_t end_freq = (uint16_t)atoi(argv[2]);
    uint16_t step = (uint16_t)atoi(argv[3]);
    
    if (start_freq >= end_freq) {
        shell_error(sh, "Start frequency must be less than end frequency");
        return -EINVAL;
    }
    
    if (step == 0) {
        shell_error(sh, "Step must be greater than 0");
        return -EINVAL;
    }
    
    shell_print(sh, "Frequency sweep: %u Hz to %u Hz, step %u Hz",
                start_freq, end_freq, step);
    shell_print(sh, "Press Ctrl+C to stop...");
    
    for (uint16_t freq = start_freq; freq <= end_freq; freq += step) {
        shell_print(sh, "  %u Hz", freq);
        
        int ret = akira_audio_play_tone(freq, 500, 80);
        if (ret < 0) {
            shell_error(sh, "Failed at %u Hz: %d", freq, ret);
            return ret;
        }
        
        k_msleep(100); /* Small gap between tones */
    }
    
    shell_print(sh, "Sweep complete");
    return 0;
}

/**
 * @brief Set master volume
 * Usage: audio volume <0-100>
 */
static int cmd_audio_volume(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "Usage: audio volume <0-100>");
        return -EINVAL;
    }
    
    uint8_t volume = (uint8_t)atoi(argv[1]);
    
    if (volume > 100) {
        shell_error(sh, "Volume must be between 0 and 100");
        return -EINVAL;
    }
    
    int ret = akira_audio_set_volume(volume);
    if (ret < 0) {
        shell_error(sh, "Failed to set volume: %d", ret);
        return ret;
    }
    
    shell_print(sh, "Master volume set to %u%%", volume);
    return 0;
}

/**
 * @brief Stop audio playback
 */
static int cmd_audio_stop(const struct shell *sh, size_t argc, char **argv)
{
    int ret = akira_audio_stop();
    if (ret < 0) {
        shell_error(sh, "Failed to stop audio: %d", ret);
        return ret;
    }
    
    shell_print(sh, "Audio stopped");
    return 0;
}

/**
 * @brief Test tone (1 kHz for 1 second)
 */
static int cmd_audio_test(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "Playing test tone: 1000 Hz, 1 second, 80%% volume");
    
    int ret = akira_audio_play_tone(1000, 1000, 80);
    if (ret < 0) {
        shell_error(sh, "Failed to play test tone: %d", ret);
        return ret;
    }
    
    shell_print(sh, "Test complete");
    return 0;
}

/**
 * @brief Sound effects commands
 */
static int cmd_audio_sfx_coin(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "Playing coin collect sound...");
    akira_audio_sfx_coin();
    return 0;
}

static int cmd_audio_sfx_jump(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "Playing jump sound...");
    akira_audio_sfx_jump();
    return 0;
}

static int cmd_audio_sfx_explosion(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "Playing explosion sound...");
    akira_audio_sfx_explosion();
    return 0;
}

static int cmd_audio_sfx_powerup(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "Playing power-up sound...");
    akira_audio_sfx_powerup();
    return 0;
}

static int cmd_audio_sfx_menu_beep(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "Playing menu beep...");
    akira_audio_sfx_menu_beep();
    return 0;
}

static int cmd_audio_sfx_menu_select(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "Playing menu select...");
    akira_audio_sfx_menu_select();
    return 0;
}

static int cmd_audio_sfx_error(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "Playing error sound...");
    akira_audio_sfx_error();
    return 0;
}

static int cmd_audio_sfx_victory(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "Playing victory fanfare...");
    akira_audio_sfx_victory();
    return 0;
}

static int cmd_audio_sfx_game_over(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "Playing game over sound...");
    akira_audio_sfx_game_over();
    return 0;
}

static int cmd_audio_sfx_low_battery(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "Playing low battery warning...");
    akira_audio_sfx_low_battery();
    return 0;
}

static int cmd_audio_sfx_startup(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "Playing startup sound...");
    akira_audio_sfx_startup();
    return 0;
}

static int cmd_audio_sfx_wifi(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "Playing WiFi connected sound...");
    akira_audio_sfx_wifi_connected();
    return 0;
}

/**
 * @brief Sound effects demo - play all effects
 */
static int cmd_audio_sfx_demo(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "Sound Effects Demo - Playing all effects...");
    
    const char *effects[] = {
        "Coin Collect", "Jump", "Explosion", "Power-Up",
        "Menu Beep", "Menu Select", "Error", "Victory",
        "Game Over", "Low Battery", "Startup", "WiFi Connected"
    };
    
    void (*sfx_functions[])(void) = {
        akira_audio_sfx_coin,
        akira_audio_sfx_jump,
        akira_audio_sfx_explosion,
        akira_audio_sfx_powerup,
        akira_audio_sfx_menu_beep,
        akira_audio_sfx_menu_select,
        akira_audio_sfx_error,
        akira_audio_sfx_victory,
        akira_audio_sfx_game_over,
        akira_audio_sfx_low_battery,
        akira_audio_sfx_startup,
        akira_audio_sfx_wifi_connected
    };
    
    for (size_t i = 0; i < ARRAY_SIZE(effects); i++) {
        shell_print(sh, "  [%d/%d] %s", i + 1, ARRAY_SIZE(effects), effects[i]);
        sfx_functions[i]();
        k_msleep(500); /* Gap between effects */
    }
    
    shell_print(sh, "Demo complete!");
    return 0;
}

/* Sound effects subcommands */
SHELL_STATIC_SUBCMD_SET_CREATE(sub_audio_sfx,
    SHELL_CMD(coin, NULL, "Coin collect sound", cmd_audio_sfx_coin),
    SHELL_CMD(jump, NULL, "Jump sound", cmd_audio_sfx_jump),
    SHELL_CMD(explosion, NULL, "Explosion sound", cmd_audio_sfx_explosion),
    SHELL_CMD(powerup, NULL, "Power-up sound", cmd_audio_sfx_powerup),
    SHELL_CMD(menu_beep, NULL, "Menu navigation beep", cmd_audio_sfx_menu_beep),
    SHELL_CMD(menu_select, NULL, "Menu select sound", cmd_audio_sfx_menu_select),
    SHELL_CMD(error, NULL, "Error/damage sound", cmd_audio_sfx_error),
    SHELL_CMD(victory, NULL, "Victory fanfare", cmd_audio_sfx_victory),
    SHELL_CMD(game_over, NULL, "Game over sound", cmd_audio_sfx_game_over),
    SHELL_CMD(low_battery, NULL, "Low battery warning", cmd_audio_sfx_low_battery),
    SHELL_CMD(startup, NULL, "Power-on startup sound", cmd_audio_sfx_startup),
    SHELL_CMD(wifi, NULL, "WiFi connected notification", cmd_audio_sfx_wifi),
    SHELL_CMD(demo, NULL, "Play all sound effects", cmd_audio_sfx_demo),
    SHELL_SUBCMD_SET_END
);

/* Main audio commands */
SHELL_STATIC_SUBCMD_SET_CREATE(sub_audio,
    SHELL_CMD(init, NULL, "Initialize audio subsystem", cmd_audio_init),
    SHELL_CMD(status, NULL, "Show audio status", cmd_audio_status),
    SHELL_CMD(tone, NULL, "Play tone <freq> <duration> <volume>", cmd_audio_tone),
    SHELL_CMD(sweep, NULL, "Frequency sweep <start> <end> <step>", cmd_audio_sweep),
    SHELL_CMD(volume, NULL, "Set master volume <0-100>", cmd_audio_volume),
    SHELL_CMD(stop, NULL, "Stop audio playback", cmd_audio_stop),
    SHELL_CMD(test_tone, NULL, "Play test tone (1 kHz, 1 sec)", cmd_audio_test),
    SHELL_CMD(sfx, &sub_audio_sfx, "Sound effects", NULL),
    SHELL_SUBCMD_SET_END
);

/* Register audio command */
SHELL_CMD_REGISTER(audio, &sub_audio, "Audio subsystem commands", NULL);
