/**
 * @file akira_audio.h
 * @brief AkiraOS Audio Driver for Piezo MEMS Micro-Speaker
 *
 * This driver provides audio output capabilities using PWM-driven piezoelectric
 * actuators. It supports both discrete piezo buzzers and custom MEMS micro-speakers.
 *
 * Features:
 * - Simple tone generation (sine, square, triangle waves)
 * - Multi-tone playback for sound effects
 * - Volume control (0-100%)
 * - Envelope shaping (ADSR)
 * - Low-power operation
 *
 * Hardware Requirements:
 * - PWM-capable GPIO pin (ESP32-S3 LEDC recommended)
 * - Piezoelectric actuator (buzzer or MEMS speaker)
 * - Optional: amplifier circuit for higher SPL
 *
 * ST Piezo MEMS Design Challenge Integration:
 * This driver is designed to support custom piezoelectric MEMS micro-speakers
 * developed for the ST MEMS Design Challenge, while maintaining compatibility
 * with standard piezo buzzers for prototyping.
 */

#ifndef AKIRA_AUDIO_H
#define AKIRA_AUDIO_H

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Audio subsystem configuration */
#define AKIRA_AUDIO_PWM_FREQUENCY 100000  /**< PWM carrier frequency (100 kHz) */
#define AKIRA_AUDIO_MAX_VOLUME 100        /**< Maximum volume level */
#define AKIRA_AUDIO_MIN_FREQUENCY 50      /**< Minimum audio frequency (Hz) */
#define AKIRA_AUDIO_MAX_FREQUENCY 10000   /**< Maximum audio frequency (Hz) */
#define AKIRA_AUDIO_SAMPLE_RATE 8000      /**< Default sample rate for waveform playback */

/* Waveform types */
typedef enum {
    AKIRA_AUDIO_WAVE_SINE,      /**< Sine wave (smoothest, best for tones) */
    AKIRA_AUDIO_WAVE_SQUARE,    /**< Square wave (classic 8-bit sound) */
    AKIRA_AUDIO_WAVE_TRIANGLE,  /**< Triangle wave (softer than square) */
    AKIRA_AUDIO_WAVE_SAWTOOTH,  /**< Sawtooth wave (harsh, buzzy) */
    AKIRA_AUDIO_WAVE_NOISE      /**< White noise (explosions, wind) */
} akira_audio_waveform_t;

/* Envelope parameters (ADSR) */
typedef struct {
    uint16_t attack_ms;   /**< Attack time (0-1000 ms) */
    uint16_t decay_ms;    /**< Decay time (0-1000 ms) */
    uint8_t sustain_level; /**< Sustain level (0-100%) */
    uint16_t release_ms;  /**< Release time (0-1000 ms) */
} akira_audio_envelope_t;

/* Tone descriptor */
typedef struct {
    uint16_t frequency_hz;  /**< Frequency in Hz (50-10000) */
    uint32_t duration_ms;   /**< Duration in milliseconds */
    uint8_t volume;         /**< Volume (0-100) */
    akira_audio_waveform_t waveform; /**< Waveform type */
} akira_audio_tone_t;

/* Audio status */
typedef struct {
    bool initialized;       /**< Driver initialization status */
    bool playing;           /**< Currently playing audio */
    uint16_t current_freq;  /**< Current playback frequency */
    uint8_t current_volume; /**< Current volume level */
    uint32_t samples_played; /**< Total samples played (for diagnostics) */
} akira_audio_status_t;

/**
 * @brief Initialize the audio subsystem
 *
 * Sets up PWM peripheral and configures audio output pin.
 * Must be called before any other audio functions.
 *
 * @return 0 on success, negative errno on error
 */
int akira_audio_init(void);

/**
 * @brief Check if audio subsystem is available
 *
 * @return true if audio hardware is present and initialized, false otherwise
 */
bool akira_audio_is_available(void);

/**
 * @brief Play a simple tone
 *
 * Plays a single tone at the specified frequency and volume for the given duration.
 * This is a blocking call - it will return after the tone completes.
 *
 * @param frequency_hz Frequency in Hz (50-10000)
 * @param duration_ms Duration in milliseconds
 * @param volume Volume level (0-100)
 * @return 0 on success, negative errno on error
 */
int akira_audio_play_tone(uint16_t frequency_hz, uint32_t duration_ms, uint8_t volume);

/**
 * @brief Play a tone with specified waveform
 *
 * Similar to akira_audio_play_tone but allows waveform selection.
 *
 * @param frequency_hz Frequency in Hz (50-10000)
 * @param duration_ms Duration in milliseconds
 * @param volume Volume level (0-100)
 * @param waveform Waveform type (sine, square, triangle, etc.)
 * @return 0 on success, negative errno on error
 */
int akira_audio_play_tone_waveform(uint16_t frequency_hz, uint32_t duration_ms, 
                                    uint8_t volume, akira_audio_waveform_t waveform);

/**
 * @brief Play a sequence of tones
 *
 * Plays multiple tones in sequence. Useful for melodies and sound effects.
 *
 * @param tones Array of tone descriptors
 * @param count Number of tones in the array
 * @return 0 on success, negative errno on error
 */
int akira_audio_play_sequence(const akira_audio_tone_t *tones, size_t count);

/**
 * @brief Play a tone with envelope shaping
 *
 * Plays a tone with ADSR (Attack, Decay, Sustain, Release) envelope.
 * Creates more natural-sounding audio with smooth onset and fadeout.
 *
 * @param frequency_hz Frequency in Hz (50-10000)
 * @param duration_ms Total duration in milliseconds
 * @param volume Base volume level (0-100)
 * @param envelope Envelope parameters
 * @return 0 on success, negative errno on error
 */
int akira_audio_play_tone_envelope(uint16_t frequency_hz, uint32_t duration_ms,
                                    uint8_t volume, const akira_audio_envelope_t *envelope);

/**
 * @brief Play raw waveform samples
 *
 * Plays arbitrary waveform data. Samples are 16-bit signed integers.
 * Useful for playing recorded sounds or complex waveforms.
 *
 * @param samples Pointer to sample data (16-bit signed)
 * @param count Number of samples
 * @param sample_rate Sample rate in Hz (e.g., 8000, 16000)
 * @param volume Volume level (0-100)
 * @return 0 on success, negative errno on error
 */
int akira_audio_play_waveform(const int16_t *samples, size_t count, 
                               uint16_t sample_rate, uint8_t volume);

/**
 * @brief Stop audio playback
 *
 * Immediately stops any ongoing audio playback.
 *
 * @return 0 on success, negative errno on error
 */
int akira_audio_stop(void);

/**
 * @brief Set global volume
 *
 * Sets the master volume level. Affects all subsequent audio playback.
 *
 * @param volume Volume level (0-100)
 * @return 0 on success, negative errno on error
 */
int akira_audio_set_volume(uint8_t volume);

/**
 * @brief Get current audio status
 *
 * @param status Pointer to status structure to fill
 * @return 0 on success, negative errno on error
 */
int akira_audio_get_status(akira_audio_status_t *status);

/*
 * Predefined sound effects for common game audio needs
 */

/**
 * @brief Play coin/item collect sound
 */
void akira_audio_sfx_coin(void);

/**
 * @brief Play jump sound
 */
void akira_audio_sfx_jump(void);

/**
 * @brief Play explosion sound
 */
void akira_audio_sfx_explosion(void);

/**
 * @brief Play power-up sound
 */
void akira_audio_sfx_powerup(void);

/**
 * @brief Play menu navigation beep
 */
void akira_audio_sfx_menu_beep(void);

/**
 * @brief Play menu select confirmation
 */
void akira_audio_sfx_menu_select(void);

/**
 * @brief Play error/damage sound
 */
void akira_audio_sfx_error(void);

/**
 * @brief Play victory fanfare
 */
void akira_audio_sfx_victory(void);

/**
 * @brief Play game over sound
 */
void akira_audio_sfx_game_over(void);

/**
 * @brief Play low battery warning
 */
void akira_audio_sfx_low_battery(void);

/**
 * @brief Play power-on startup sound
 */
void akira_audio_sfx_startup(void);

/**
 * @brief Play WiFi connected notification
 */
void akira_audio_sfx_wifi_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_AUDIO_H */
