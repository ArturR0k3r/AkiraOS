/**
 * @file akira_audio.c
 * @brief AkiraOS Audio Driver Implementation
 *
 * PWM-based audio driver for piezoelectric MEMS micro-speakers
 */

#include "akira_audio.h"
#include "akira_hal.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>
#include <math.h>

LOG_MODULE_REGISTER(akira_audio, CONFIG_LOG_DEFAULT_LEVEL);

/* PWM device configuration - using devicetree */
#if DT_NODE_HAS_STATUS(DT_ALIAS(pwm_audio), okay)
#define PWM_AUDIO_NODE DT_ALIAS(pwm_audio)
#else
/* Fallback: use PWM on GPIO21 for ESP32-S3 */
#define PWM_AUDIO_NODE DT_NODELABEL(ledc0)
#endif

/* Audio state */
static struct {
    const struct device *pwm_dev;
    uint32_t pwm_channel;
    uint8_t master_volume;
    bool initialized;
    bool playing;
    uint16_t current_frequency;
    uint32_t samples_played;
} audio_state = {
    .master_volume = 70,  /* Default 70% volume */
    .initialized = false,
    .playing = false,
    .current_frequency = 0,
    .samples_played = 0,
};

/* Math constants */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Waveform generation lookup table size */
#define WAVE_TABLE_SIZE 256

/* Static waveform tables (pre-computed for efficiency) */
static int16_t sine_table[WAVE_TABLE_SIZE];
static int16_t square_table[WAVE_TABLE_SIZE];
static int16_t triangle_table[WAVE_TABLE_SIZE];
static int16_t sawtooth_table[WAVE_TABLE_SIZE];

/* Mutex for thread-safe audio operations */
K_MUTEX_DEFINE(audio_mutex);

/**
 * @brief Initialize waveform lookup tables
 */
static void init_waveform_tables(void)
{
    for (int i = 0; i < WAVE_TABLE_SIZE; i++) {
        float phase = (float)i / WAVE_TABLE_SIZE;
        
        /* Sine wave: smooth, pure tone */
        sine_table[i] = (int16_t)(sin(2.0 * M_PI * phase) * INT16_MAX);
        
        /* Square wave: 8-bit game sound */
        square_table[i] = (phase < 0.5f) ? INT16_MAX : -INT16_MAX;
        
        /* Triangle wave: softer than square */
        if (phase < 0.25f) {
            triangle_table[i] = (int16_t)(4.0f * phase * INT16_MAX);
        } else if (phase < 0.75f) {
            triangle_table[i] = (int16_t)((2.0f - 4.0f * phase) * INT16_MAX);
        } else {
            triangle_table[i] = (int16_t)((4.0f * phase - 4.0f) * INT16_MAX);
        }
        
        /* Sawtooth wave: harsh, buzzy */
        sawtooth_table[i] = (int16_t)((2.0f * phase - 1.0f) * INT16_MAX);
    }
}

/**
 * @brief Set PWM duty cycle for audio output
 * 
 * @param duty_cycle Duty cycle in percent (0-100)
 */
static int set_pwm_duty_cycle(uint8_t duty_cycle)
{
    if (!audio_state.pwm_dev) {
        return -ENODEV;
    }
    
    /* Clamp duty cycle */
    if (duty_cycle > 100) {
        duty_cycle = 100;
    }
    
    /* Calculate PWM period in nanoseconds */
    uint64_t period_ns = NSEC_PER_SEC / AKIRA_AUDIO_PWM_FREQUENCY;
    uint64_t pulse_ns = (period_ns * duty_cycle) / 100;
    
    return pwm_set(audio_state.pwm_dev, audio_state.pwm_channel,
                   period_ns, pulse_ns, 0);
}

/**
 * @brief Generate tone using PWM frequency modulation
 * 
 * For piezo speakers, we can directly modulate the PWM frequency
 * to generate audio tones more efficiently than PCM playback.
 */
static int generate_tone_pwm(uint16_t frequency_hz, uint8_t volume)
{
    if (!audio_state.pwm_dev) {
        return -ENODEV;
    }
    
    if (frequency_hz < AKIRA_AUDIO_MIN_FREQUENCY || 
        frequency_hz > AKIRA_AUDIO_MAX_FREQUENCY) {
        return -EINVAL;
    }
    
    /* Apply volume scaling */
    uint8_t effective_volume = (volume * audio_state.master_volume) / 100;
    if (effective_volume > 100) {
        effective_volume = 100;
    }
    
    /* Calculate PWM period for the audio frequency */
    uint64_t period_ns = NSEC_PER_SEC / frequency_hz;
    
    /* 50% duty cycle with volume-based amplitude modulation */
    uint64_t pulse_ns = (period_ns * effective_volume) / 200; /* Divide by 200 for ~50% max */
    
    audio_state.current_frequency = frequency_hz;
    
    return pwm_set(audio_state.pwm_dev, audio_state.pwm_channel,
                   period_ns, pulse_ns, 0);
}

int akira_audio_init(void)
{
    if (audio_state.initialized) {
        LOG_WRN("Audio already initialized");
        return 0;
    }
    
    LOG_INF("Initializing AkiraOS audio subsystem");
    
    /* Initialize waveform tables */
    init_waveform_tables();
    
#if AKIRA_PLATFORM_ESP32 || AKIRA_PLATFORM_ESP32S3
    /* Get PWM device - on ESP32 this is the LEDC peripheral */
    audio_state.pwm_dev = DEVICE_DT_GET(PWM_AUDIO_NODE);
    
    if (!device_is_ready(audio_state.pwm_dev)) {
        LOG_ERR("PWM device not ready");
        return -ENODEV;
    }
    
    audio_state.pwm_channel = 0; /* Use channel 0 */
    
    /* Initialize PWM to idle state (silent) */
    set_pwm_duty_cycle(0);
    
    audio_state.initialized = true;
    LOG_INF("Audio initialized: PWM mode, %d kHz carrier", 
            AKIRA_AUDIO_PWM_FREQUENCY / 1000);
    
    return 0;
#else
    /* Native simulation - audio not supported yet */
    LOG_WRN("Audio not supported on this platform (simulation mode)");
    audio_state.initialized = false;
    return -ENOTSUP;
#endif
}

bool akira_audio_is_available(void)
{
    return audio_state.initialized;
}

int akira_audio_play_tone(uint16_t frequency_hz, uint32_t duration_ms, uint8_t volume)
{
    return akira_audio_play_tone_waveform(frequency_hz, duration_ms, volume, 
                                          AKIRA_AUDIO_WAVE_SQUARE);
}

int akira_audio_play_tone_waveform(uint16_t frequency_hz, uint32_t duration_ms,
                                    uint8_t volume, akira_audio_waveform_t waveform)
{
    if (!audio_state.initialized) {
        LOG_ERR("Audio not initialized");
        return -ENODEV;
    }
    
    if (volume > AKIRA_AUDIO_MAX_VOLUME) {
        volume = AKIRA_AUDIO_MAX_VOLUME;
    }
    
    k_mutex_lock(&audio_mutex, K_FOREVER);
    
    audio_state.playing = true;
    
    /* For square wave, use direct PWM frequency modulation (most efficient) */
    if (waveform == AKIRA_AUDIO_WAVE_SQUARE) {
        int ret = generate_tone_pwm(frequency_hz, volume);
        if (ret < 0) {
            audio_state.playing = false;
            k_mutex_unlock(&audio_mutex);
            return ret;
        }
        
        /* Play for specified duration */
        k_msleep(duration_ms);
        
        /* Stop tone */
        set_pwm_duty_cycle(0);
    } else {
        /* For other waveforms, use sample-based playback */
        /* TODO: Implement PCM playback for sine, triangle, sawtooth */
        LOG_WRN("Waveform type %d not yet implemented, using square", waveform);
        int ret = generate_tone_pwm(frequency_hz, volume);
        if (ret < 0) {
            audio_state.playing = false;
            k_mutex_unlock(&audio_mutex);
            return ret;
        }
        k_msleep(duration_ms);
        set_pwm_duty_cycle(0);
    }
    
    audio_state.playing = false;
    audio_state.samples_played += (duration_ms * frequency_hz) / 1000;
    
    k_mutex_unlock(&audio_mutex);
    
    return 0;
}

int akira_audio_play_sequence(const akira_audio_tone_t *tones, size_t count)
{
    if (!audio_state.initialized) {
        return -ENODEV;
    }
    
    if (!tones || count == 0) {
        return -EINVAL;
    }
    
    for (size_t i = 0; i < count; i++) {
        int ret = akira_audio_play_tone_waveform(
            tones[i].frequency_hz,
            tones[i].duration_ms,
            tones[i].volume,
            tones[i].waveform
        );
        
        if (ret < 0) {
            LOG_ERR("Failed to play tone %d in sequence", i);
            return ret;
        }
        
        /* Small gap between tones for clarity */
        k_msleep(10);
    }
    
    return 0;
}

int akira_audio_play_tone_envelope(uint16_t frequency_hz, uint32_t duration_ms,
                                    uint8_t volume, const akira_audio_envelope_t *envelope)
{
    if (!audio_state.initialized || !envelope) {
        return -EINVAL;
    }
    
    /* Simplified envelope implementation */
    /* Attack phase */
    for (uint16_t t = 0; t < envelope->attack_ms; t += 10) {
        uint8_t env_volume = (volume * t) / envelope->attack_ms;
        generate_tone_pwm(frequency_hz, env_volume);
        k_msleep(10);
    }
    
    /* Decay phase */
    for (uint16_t t = 0; t < envelope->decay_ms; t += 10) {
        uint8_t env_volume = volume - ((volume - envelope->sustain_level) * t) / envelope->decay_ms;
        generate_tone_pwm(frequency_hz, env_volume);
        k_msleep(10);
    }
    
    /* Sustain phase */
    uint32_t sustain_time = duration_ms - envelope->attack_ms - envelope->decay_ms - envelope->release_ms;
    if (sustain_time > 0) {
        generate_tone_pwm(frequency_hz, envelope->sustain_level);
        k_msleep(sustain_time);
    }
    
    /* Release phase */
    for (uint16_t t = 0; t < envelope->release_ms; t += 10) {
        uint8_t env_volume = envelope->sustain_level * (envelope->release_ms - t) / envelope->release_ms;
        generate_tone_pwm(frequency_hz, env_volume);
        k_msleep(10);
    }
    
    set_pwm_duty_cycle(0);
    return 0;
}

int akira_audio_play_waveform(const int16_t *samples, size_t count,
                               uint16_t sample_rate, uint8_t volume)
{
    /* TODO: Implement PCM sample playback */
    LOG_WRN("PCM waveform playback not yet implemented");
    return -ENOTSUP;
}

int akira_audio_stop(void)
{
    if (!audio_state.initialized) {
        return -ENODEV;
    }
    
    k_mutex_lock(&audio_mutex, K_FOREVER);
    
    set_pwm_duty_cycle(0);
    audio_state.playing = false;
    audio_state.current_frequency = 0;
    
    k_mutex_unlock(&audio_mutex);
    
    return 0;
}

int akira_audio_set_volume(uint8_t volume)
{
    if (volume > AKIRA_AUDIO_MAX_VOLUME) {
        volume = AKIRA_AUDIO_MAX_VOLUME;
    }
    
    audio_state.master_volume = volume;
    LOG_DBG("Master volume set to %d%%", volume);
    
    return 0;
}

int akira_audio_get_status(akira_audio_status_t *status)
{
    if (!status) {
        return -EINVAL;
    }
    
    status->initialized = audio_state.initialized;
    status->playing = audio_state.playing;
    status->current_freq = audio_state.current_frequency;
    status->current_volume = audio_state.master_volume;
    status->samples_played = audio_state.samples_played;
    
    return 0;
}

/*
 * Predefined sound effects
 */

void akira_audio_sfx_coin(void)
{
    /* Classic coin collect: rising pitch */
    akira_audio_play_tone(1000, 50, 90);
    akira_audio_play_tone(1500, 100, 90);
}

void akira_audio_sfx_jump(void)
{
    /* Jump sound: quick rising sweep */
    akira_audio_play_tone(400, 30, 80);
    akira_audio_play_tone(600, 40, 80);
}

void akira_audio_sfx_explosion(void)
{
    /* Explosion: descending noise-like sweep */
    for (int freq = 2000; freq > 100; freq -= 100) {
        akira_audio_play_tone(freq, 8, 70);
    }
}

void akira_audio_sfx_powerup(void)
{
    /* Power-up: ascending arpeggio */
    const akira_audio_tone_t tones[] = {
        {.frequency_hz = 262, .duration_ms = 80, .volume = 85, .waveform = AKIRA_AUDIO_WAVE_SQUARE},
        {.frequency_hz = 330, .duration_ms = 80, .volume = 85, .waveform = AKIRA_AUDIO_WAVE_SQUARE},
        {.frequency_hz = 392, .duration_ms = 80, .volume = 85, .waveform = AKIRA_AUDIO_WAVE_SQUARE},
        {.frequency_hz = 523, .duration_ms = 200, .volume = 90, .waveform = AKIRA_AUDIO_WAVE_SQUARE},
    };
    akira_audio_play_sequence(tones, ARRAY_SIZE(tones));
}

void akira_audio_sfx_menu_beep(void)
{
    /* Short navigation beep */
    akira_audio_play_tone(800, 30, 60);
}

void akira_audio_sfx_menu_select(void)
{
    /* Double beep confirmation */
    akira_audio_play_tone(1200, 50, 70);
    k_msleep(30);
    akira_audio_play_tone(1200, 50, 70);
}

void akira_audio_sfx_error(void)
{
    /* Error: descending low tone */
    akira_audio_play_tone(400, 100, 80);
    akira_audio_play_tone(200, 150, 80);
}

void akira_audio_sfx_victory(void)
{
    /* Victory fanfare: ascending triumphant melody */
    const akira_audio_tone_t tones[] = {
        {.frequency_hz = 523, .duration_ms = 150, .volume = 90, .waveform = AKIRA_AUDIO_WAVE_SQUARE},
        {.frequency_hz = 659, .duration_ms = 150, .volume = 90, .waveform = AKIRA_AUDIO_WAVE_SQUARE},
        {.frequency_hz = 784, .duration_ms = 150, .volume = 90, .waveform = AKIRA_AUDIO_WAVE_SQUARE},
        {.frequency_hz = 1047, .duration_ms = 400, .volume = 95, .waveform = AKIRA_AUDIO_WAVE_SQUARE},
    };
    akira_audio_play_sequence(tones, ARRAY_SIZE(tones));
}

void akira_audio_sfx_game_over(void)
{
    /* Game over: descending sad melody */
    const akira_audio_tone_t tones[] = {
        {.frequency_hz = 523, .duration_ms = 200, .volume = 85, .waveform = AKIRA_AUDIO_WAVE_SQUARE},
        {.frequency_hz = 494, .duration_ms = 200, .volume = 85, .waveform = AKIRA_AUDIO_WAVE_SQUARE},
        {.frequency_hz = 440, .duration_ms = 200, .volume = 85, .waveform = AKIRA_AUDIO_WAVE_SQUARE},
        {.frequency_hz = 392, .duration_ms = 500, .volume = 80, .waveform = AKIRA_AUDIO_WAVE_SQUARE},
    };
    akira_audio_play_sequence(tones, ARRAY_SIZE(tones));
}

void akira_audio_sfx_low_battery(void)
{
    /* Low battery: pulsing warning tone */
    for (int i = 0; i < 3; i++) {
        akira_audio_play_tone(400, 200, 70);
        k_msleep(200);
    }
}

void akira_audio_sfx_startup(void)
{
    /* Power-on startup: rising sweep */
    for (int freq = 500; freq <= 2000; freq += 100) {
        akira_audio_play_tone(freq, 20, 70);
    }
    akira_audio_play_tone(2000, 150, 80);
}

void akira_audio_sfx_wifi_connected(void)
{
    /* WiFi connected: double rising beep */
    akira_audio_play_tone(800, 80, 75);
    k_msleep(50);
    akira_audio_play_tone(1200, 80, 75);
}
