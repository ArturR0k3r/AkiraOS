/**
 * @file audio_demo.c
 * @brief AkiraOS Audio Demo Application
 *
 * Demonstrates the piezo MEMS audio capabilities with various sound effects
 * and interactive tone generation. This example showcases how to integrate
 * audio into AkiraOS applications.
 *
 * ST Piezo MEMS Design Challenge - Demo Application
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "../../src/drivers/akira_audio.h"

LOG_MODULE_REGISTER(audio_demo, CONFIG_LOG_DEFAULT_LEVEL);

/**
 * @brief Play a simple melody
 */
static void play_melody(void)
{
    /* Super Mario Bros theme (partial) */
    const akira_audio_tone_t melody[] = {
        /* E5, E5, rest, E5, rest, C5, E5, rest, G5 */
        {.frequency_hz = 659, .duration_ms = 150, .volume = 85, .waveform = AKIRA_AUDIO_WAVE_SQUARE},
        {.frequency_hz = 659, .duration_ms = 150, .volume = 85, .waveform = AKIRA_AUDIO_WAVE_SQUARE},
        {.frequency_hz = 0,   .duration_ms = 150, .volume = 0,  .waveform = AKIRA_AUDIO_WAVE_SQUARE}, /* rest */
        {.frequency_hz = 659, .duration_ms = 150, .volume = 85, .waveform = AKIRA_AUDIO_WAVE_SQUARE},
        {.frequency_hz = 0,   .duration_ms = 150, .volume = 0,  .waveform = AKIRA_AUDIO_WAVE_SQUARE}, /* rest */
        {.frequency_hz = 523, .duration_ms = 150, .volume = 85, .waveform = AKIRA_AUDIO_WAVE_SQUARE},
        {.frequency_hz = 659, .duration_ms = 150, .volume = 85, .waveform = AKIRA_AUDIO_WAVE_SQUARE},
        {.frequency_hz = 0,   .duration_ms = 150, .volume = 0,  .waveform = AKIRA_AUDIO_WAVE_SQUARE}, /* rest */
        {.frequency_hz = 784, .duration_ms = 300, .volume = 90, .waveform = AKIRA_AUDIO_WAVE_SQUARE},
    };
    
    LOG_INF("Playing melody...");
    akira_audio_play_sequence(melody, ARRAY_SIZE(melody));
}

/**
 * @brief Demonstrate all sound effects
 */
static void demo_sound_effects(void)
{
    LOG_INF("=== Sound Effects Demo ===");
    
    const struct {
        const char *name;
        void (*func)(void);
    } effects[] = {
        {"Coin Collect", akira_audio_sfx_coin},
        {"Jump", akira_audio_sfx_jump},
        {"Explosion", akira_audio_sfx_explosion},
        {"Power-Up", akira_audio_sfx_powerup},
        {"Menu Beep", akira_audio_sfx_menu_beep},
        {"Menu Select", akira_audio_sfx_menu_select},
        {"Error", akira_audio_sfx_error},
        {"Victory", akira_audio_sfx_victory},
        {"Game Over", akira_audio_sfx_game_over},
        {"Low Battery", akira_audio_sfx_low_battery},
        {"Startup", akira_audio_sfx_startup},
        {"WiFi Connected", akira_audio_sfx_wifi_connected},
    };
    
    for (size_t i = 0; i < ARRAY_SIZE(effects); i++) {
        LOG_INF("[%d/%d] %s", i + 1, ARRAY_SIZE(effects), effects[i].name);
        effects[i].func();
        k_msleep(500); /* Gap between effects */
    }
    
    LOG_INF("Sound effects demo complete!");
}

/**
 * @brief Generate sweeping tones (frequency sweep)
 */
static void frequency_sweep_demo(void)
{
    LOG_INF("=== Frequency Sweep Demo ===");
    LOG_INF("Sweeping from 500 Hz to 5000 Hz...");
    
    for (uint16_t freq = 500; freq <= 5000; freq += 250) {
        akira_audio_play_tone(freq, 200, 75);
        k_msleep(50);
    }
    
    LOG_INF("Frequency sweep complete!");
}

/**
 * @brief Demonstrate volume control
 */
static void volume_control_demo(void)
{
    LOG_INF("=== Volume Control Demo ===");
    
    const uint8_t volumes[] = {20, 40, 60, 80, 100, 80, 60, 40, 20};
    
    for (size_t i = 0; i < ARRAY_SIZE(volumes); i++) {
        LOG_INF("Volume: %d%%", volumes[i]);
        akira_audio_play_tone(1000, 300, volumes[i]);
        k_msleep(100);
    }
    
    LOG_INF("Volume control demo complete!");
}

/**
 * @brief Generate alarm/siren effect
 */
static void alarm_effect_demo(void)
{
    LOG_INF("=== Alarm Effect Demo ===");
    
    for (int i = 0; i < 5; i++) {
        /* Alternating high/low tones like a siren */
        akira_audio_play_tone(800, 200, 90);
        akira_audio_play_tone(400, 200, 90);
    }
    
    LOG_INF("Alarm effect demo complete!");
}

/**
 * @brief Create a simple game-like sequence
 */
static void game_sequence_demo(void)
{
    LOG_INF("=== Game Sequence Demo ===");
    LOG_INF("Simulating a simple game sequence...");
    
    /* Game start */
    LOG_INF("Game starting...");
    akira_audio_sfx_startup();
    k_msleep(1000);
    
    /* Player actions */
    LOG_INF("Player jumps...");
    akira_audio_sfx_jump();
    k_msleep(500);
    
    LOG_INF("Collecting coins...");
    akira_audio_sfx_coin();
    k_msleep(300);
    akira_audio_sfx_coin();
    k_msleep(300);
    akira_audio_sfx_coin();
    k_msleep(500);
    
    LOG_INF("Power-up collected!");
    akira_audio_sfx_powerup();
    k_msleep(1000);
    
    LOG_INF("Enemy defeated!");
    akira_audio_sfx_explosion();
    k_msleep(1000);
    
    /* Level complete */
    LOG_INF("Level complete!");
    akira_audio_sfx_victory();
    
    LOG_INF("Game sequence demo complete!");
}

/**
 * @brief Interactive demo menu
 */
static void show_demo_menu(void)
{
    LOG_INF("╔════════════════════════════════════════╗");
    LOG_INF("║   AkiraOS Audio Demo Application      ║");
    LOG_INF("║   ST Piezo MEMS Design Challenge      ║");
    LOG_INF("╚════════════════════════════════════════╝");
    LOG_INF("");
    LOG_INF("Available demos:");
    LOG_INF("  1. Sound Effects Demo");
    LOG_INF("  2. Melody Playback");
    LOG_INF("  3. Frequency Sweep");
    LOG_INF("  4. Volume Control");
    LOG_INF("  5. Alarm Effect");
    LOG_INF("  6. Game Sequence");
    LOG_INF("  7. Run All Demos");
    LOG_INF("");
    LOG_INF("Use shell command: audio_demo <number>");
}

/**
 * @brief Main demo function - runs all demos
 */
void audio_demo_run_all(void)
{
    LOG_INF("╔════════════════════════════════════════╗");
    LOG_INF("║   Running All Audio Demos              ║");
    LOG_INF("╚════════════════════════════════════════╝");
    
    /* Check if audio is available */
    if (!akira_audio_is_available()) {
        LOG_ERR("Audio subsystem not available!");
        return;
    }
    
    akira_audio_status_t status;
    akira_audio_get_status(&status);
    LOG_INF("Audio Status:");
    LOG_INF("  Initialized: %s", status.initialized ? "Yes" : "No");
    LOG_INF("  Master Volume: %u%%", status.current_volume);
    
    k_msleep(1000);
    
    /* Run all demos */
    demo_sound_effects();
    k_msleep(2000);
    
    play_melody();
    k_msleep(2000);
    
    frequency_sweep_demo();
    k_msleep(2000);
    
    volume_control_demo();
    k_msleep(2000);
    
    alarm_effect_demo();
    k_msleep(2000);
    
    game_sequence_demo();
    
    LOG_INF("╔════════════════════════════════════════╗");
    LOG_INF("║   All Demos Complete!                  ║");
    LOG_INF("╚════════════════════════════════════════╝");
}

/**
 * @brief Individual demo functions for shell commands
 */
void audio_demo_sound_effects(void)
{
    demo_sound_effects();
}

void audio_demo_melody(void)
{
    play_melody();
}

void audio_demo_frequency_sweep(void)
{
    frequency_sweep_demo();
}

void audio_demo_volume_control(void)
{
    volume_control_demo();
}

void audio_demo_alarm(void)
{
    alarm_effect_demo();
}

void audio_demo_game_sequence(void)
{
    game_sequence_demo();
}

void audio_demo_show_menu(void)
{
    show_demo_menu();
}

/**
 * @brief Demo application entry point
 */
int main(void)
{
    LOG_INF("AkiraOS Audio Demo Application");
    LOG_INF("ST Piezo MEMS Design Challenge");
    LOG_INF("");
    
    /* Initialize audio if not already done */
    int ret = akira_audio_init();
    if (ret < 0 && ret != -EEXIST) {
        LOG_ERR("Audio initialization failed: %d", ret);
        return ret;
    }
    
    /* Show menu */
    show_demo_menu();
    
    /* Wait for user interaction via shell */
    LOG_INF("Waiting for shell commands...");
    LOG_INF("Example: audio_demo 7 (to run all demos)");
    
    return 0;
}
