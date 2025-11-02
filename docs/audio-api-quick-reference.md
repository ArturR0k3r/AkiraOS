# AkiraOS Audio API Quick Reference

## C API Functions

### Initialization & Status

```c
/* Initialize audio subsystem */
int akira_audio_init(void);

/* Check if audio is available */
bool akira_audio_is_available(void);

/* Get audio status */
int akira_audio_get_status(akira_audio_status_t *status);
```

### Basic Tone Generation

```c
/* Play simple tone (square wave) */
int akira_audio_play_tone(uint16_t frequency_hz, 
                          uint32_t duration_ms, 
                          uint8_t volume);

/* Play tone with waveform selection */
int akira_audio_play_tone_waveform(uint16_t frequency_hz, 
                                    uint32_t duration_ms,
                                    uint8_t volume, 
                                    akira_audio_waveform_t waveform);
```

**Waveform Types:**
- `AKIRA_AUDIO_WAVE_SINE` - Smooth, pure tone
- `AKIRA_AUDIO_WAVE_SQUARE` - Classic 8-bit sound (default)
- `AKIRA_AUDIO_WAVE_TRIANGLE` - Softer than square
- `AKIRA_AUDIO_WAVE_SAWTOOTH` - Harsh, buzzy
- `AKIRA_AUDIO_WAVE_NOISE` - White noise

### Sequence & Waveform Playback

```c
/* Play sequence of tones */
int akira_audio_play_sequence(const akira_audio_tone_t *tones, 
                               size_t count);

/* Play tone with ADSR envelope */
int akira_audio_play_tone_envelope(uint16_t frequency_hz, 
                                    uint32_t duration_ms,
                                    uint8_t volume, 
                                    const akira_audio_envelope_t *envelope);

/* Play raw samples (PCM) */
int akira_audio_play_waveform(const int16_t *samples, 
                               size_t count,
                               uint16_t sample_rate, 
                               uint8_t volume);
```

### Control Functions

```c
/* Stop audio playback */
int akira_audio_stop(void);

/* Set master volume (0-100) */
int akira_audio_set_volume(uint8_t volume);
```

---

## Predefined Sound Effects

### Game Sounds

```c
void akira_audio_sfx_coin(void);        /* Coin collect - rising pitch */
void akira_audio_sfx_jump(void);        /* Jump - quick sweep up */
void akira_audio_sfx_explosion(void);   /* Explosion - descending noise */
void akira_audio_sfx_powerup(void);     /* Power-up - ascending arpeggio */
void akira_audio_sfx_victory(void);     /* Victory fanfare */
void akira_audio_sfx_game_over(void);   /* Game over - sad melody */
```

### UI Sounds

```c
void akira_audio_sfx_menu_beep(void);   /* Navigation beep */
void akira_audio_sfx_menu_select(void); /* Selection confirmation */
void akira_audio_sfx_error(void);       /* Error - descending tone */
```

### System Sounds

```c
void akira_audio_sfx_startup(void);     /* Power-on - rising sweep */
void akira_audio_sfx_low_battery(void); /* Battery warning - pulsing */
void akira_audio_sfx_wifi_connected(void); /* WiFi connected - double beep */
```

---

## Shell Commands

### Basic Commands

```bash
audio init                    # Initialize audio
audio status                  # Show status
audio test_tone               # Play test tone (1 kHz, 1 sec)
audio stop                    # Stop playback
```

### Tone Generation

```bash
audio tone <freq> <dur> <vol>  # Play tone
# Example: audio tone 1000 500 80
# Plays 1000 Hz for 500ms at 80% volume

audio sweep <start> <end> <step>  # Frequency sweep
# Example: audio sweep 500 5000 100
# Sweeps from 500 Hz to 5000 Hz in 100 Hz steps
```

### Volume Control

```bash
audio volume <0-100>          # Set master volume
# Example: audio volume 70
```

### Sound Effects

```bash
audio sfx coin                # Coin collect
audio sfx jump                # Jump
audio sfx explosion           # Explosion
audio sfx powerup             # Power-up
audio sfx menu_beep           # Menu beep
audio sfx menu_select         # Menu select
audio sfx error               # Error
audio sfx victory             # Victory
audio sfx game_over           # Game over
audio sfx low_battery         # Low battery
audio sfx startup             # Startup
audio sfx wifi                # WiFi connected
audio sfx demo                # Play all effects
```

---

## Code Examples

### Example 1: Simple Tone

```c
#include "drivers/akira_audio.h"

void play_beep(void) {
    /* Play 1000 Hz tone for 200ms at 80% volume */
    akira_audio_play_tone(1000, 200, 80);
}
```

### Example 2: Melody

```c
void play_melody(void) {
    /* C major scale */
    const akira_audio_tone_t scale[] = {
        {.frequency_hz = 262, .duration_ms = 200, .volume = 80, .waveform = AKIRA_AUDIO_WAVE_SQUARE}, /* C */
        {.frequency_hz = 294, .duration_ms = 200, .volume = 80, .waveform = AKIRA_AUDIO_WAVE_SQUARE}, /* D */
        {.frequency_hz = 330, .duration_ms = 200, .volume = 80, .waveform = AKIRA_AUDIO_WAVE_SQUARE}, /* E */
        {.frequency_hz = 349, .duration_ms = 200, .volume = 80, .waveform = AKIRA_AUDIO_WAVE_SQUARE}, /* F */
        {.frequency_hz = 392, .duration_ms = 200, .volume = 80, .waveform = AKIRA_AUDIO_WAVE_SQUARE}, /* G */
        {.frequency_hz = 440, .duration_ms = 200, .volume = 80, .waveform = AKIRA_AUDIO_WAVE_SQUARE}, /* A */
        {.frequency_hz = 494, .duration_ms = 200, .volume = 80, .waveform = AKIRA_AUDIO_WAVE_SQUARE}, /* B */
        {.frequency_hz = 523, .duration_ms = 400, .volume = 85, .waveform = AKIRA_AUDIO_WAVE_SQUARE}, /* C */
    };
    
    akira_audio_play_sequence(scale, ARRAY_SIZE(scale));
}
```

### Example 3: Game Integration

```c
void game_collect_coin(void) {
    player.score += 10;
    akira_audio_sfx_coin();  /* Audio feedback */
    update_score_display();
}

void game_player_jump(void) {
    if (player.on_ground) {
        player.velocity_y = JUMP_VELOCITY;
        akira_audio_sfx_jump();  /* Audio feedback */
    }
}

void game_level_complete(void) {
    game_state = STATE_VICTORY;
    akira_audio_sfx_victory();  /* Victory music */
    show_level_complete_screen();
}
```

### Example 4: Volume Fade

```c
void fade_in_audio(void) {
    for (uint8_t vol = 0; vol <= 80; vol += 10) {
        akira_audio_set_volume(vol);
        akira_audio_play_tone(440, 100, 100);
        k_msleep(100);
    }
}
```

### Example 5: Alarm/Siren

```c
void play_alarm(void) {
    for (int i = 0; i < 5; i++) {
        akira_audio_play_tone(800, 200, 90);  /* High */
        akira_audio_play_tone(400, 200, 90);  /* Low */
    }
}
```

---

## Musical Notes Frequencies

### Octave 4 (Middle C)

| Note | Frequency (Hz) |
|------|----------------|
| C4   | 262 |
| C#4  | 277 |
| D4   | 294 |
| D#4  | 311 |
| E4   | 330 |
| F4   | 349 |
| F#4  | 370 |
| G4   | 392 |
| G#4  | 415 |
| A4   | 440 |
| A#4  | 466 |
| B4   | 494 |

### Octave 5

| Note | Frequency (Hz) |
|------|----------------|
| C5   | 523 |
| C#5  | 554 |
| D5   | 587 |
| D#5  | 622 |
| E5   | 659 |
| F5   | 698 |
| F#5  | 740 |
| G5   | 784 |
| G#5  | 831 |
| A5   | 880 |
| A#5  | 932 |
| B5   | 988 |

---

## Frequency Ranges

| Range | Frequency | Best For |
|-------|-----------|----------|
| Sub-bass | 50-100 Hz | Rumble, explosions (low-end) |
| Bass | 100-250 Hz | Deep tones, game over sounds |
| Low-mid | 250-500 Hz | Error tones, alarms |
| Mid | 500-2000 Hz | Most game sounds, UI feedback |
| High-mid | 2000-4000 Hz | **Resonance peak**, coin collect |
| High | 4000-8000 Hz | Bright tones, notifications |
| Very high | 8000-10000 Hz | Subtle effects, treble |

**Note**: Piezo buzzers typically resonate at 2-4 kHz (highest efficiency).

---

## Data Structures

### akira_audio_tone_t

```c
typedef struct {
    uint16_t frequency_hz;        /* 50-10000 Hz */
    uint32_t duration_ms;         /* Duration in milliseconds */
    uint8_t volume;               /* 0-100 */
    akira_audio_waveform_t waveform;  /* Waveform type */
} akira_audio_tone_t;
```

### akira_audio_envelope_t

```c
typedef struct {
    uint16_t attack_ms;   /* Attack time (0-1000 ms) */
    uint16_t decay_ms;    /* Decay time (0-1000 ms) */
    uint8_t sustain_level; /* Sustain level (0-100%) */
    uint16_t release_ms;  /* Release time (0-1000 ms) */
} akira_audio_envelope_t;
```

### akira_audio_status_t

```c
typedef struct {
    bool initialized;       /* Driver initialized? */
    bool playing;           /* Currently playing? */
    uint16_t current_freq;  /* Current frequency */
    uint8_t current_volume; /* Master volume */
    uint32_t samples_played; /* Total samples played */
} akira_audio_status_t;
```

---

## Configuration

### Kconfig Options

```kconfig
CONFIG_AKIRA_AUDIO=y              # Enable audio subsystem
CONFIG_PWM=y                      # Enable PWM peripheral
CONFIG_AKIRA_AUDIO_PWM_CHANNEL=0  # PWM channel number
```

### Device Tree (ESP32-S3)

```dts
&ledc0 {
    status = "okay";
    pinctrl-0 = <&ledc0_default>;
    pinctrl-names = "default";
    
    channel@0 {
        reg = <0x0>;
        timer = <0>;
    };
};

&pinctrl {
    ledc0_default: ledc0_default {
        group2 {
            pinmux = <LEDC_CH0_GPIO9>;  /* GPIO9 for audio */
            output-enable;
        };
    };
};
```

---

## Hardware Connection

### Simple (Direct)

```
ESP32-S3 GPIO9 ──[ 100Ω ]── (+) Piezo Buzzer (─) ── GND
```

### Amplified

```
GPIO9 ──[ 1kΩ ]── Base (NPN Transistor)
                  Collector ── (+) Piezo
                  Emitter ── GND
                  
+3.3V ──[ 100Ω ]── (─) Piezo
```

---

## Performance

| Parameter | Typical Value |
|-----------|---------------|
| Latency | < 5 ms |
| CPU usage | < 1% (during playback) |
| Power consumption | 30-50 mW @ 80% volume |
| Frequency accuracy | ±1 Hz |
| Volume resolution | 1% steps (0-100) |
| Max playback time | Unlimited (blocking calls) |

---

## Troubleshooting

| Symptom | Possible Cause | Solution |
|---------|----------------|----------|
| No sound | GPIO not configured | Check device tree overlay |
| | Piezo polarity reversed | Try flipping connections |
| | Audio not initialized | Call `akira_audio_init()` |
| Quiet sound | Volume too low | `audio volume 100` |
| | Frequency off-resonance | Test 2-4 kHz range |
| | Resistor too high | Use 47-100Ω |
| Distorted | Overdriving | Reduce volume to 70-80% |
| | Resistor too low | Increase to 100Ω |

---

## References

- **Full Documentation**: [docs/piezo-mems-speaker-design.md](piezo-mems-speaker-design.md)
- **Hardware Setup**: [docs/audio-hardware-setup.md](audio-hardware-setup.md)
- **Validation Guide**: [docs/piezo-prototype-validation.md](piezo-prototype-validation.md)
- **Example App**: [examples/audio_demo/README.md](../examples/audio_demo/README.md)

---

**Quick Start:**
1. Connect piezo buzzer to GPIO9 (with 100Ω resistor)
2. Flash AkiraOS: `west flash -d build_esp32s3`
3. Test audio: `audio test_tone`
4. Play effects: `audio sfx demo`

**Pro Tip:** Piezo buzzers sound best at their resonance frequency (usually 2-4 kHz). Run `audio sweep 1000 5000 100` to find the loudest frequency!
