# AkiraOS Audio Demo Application

This example application demonstrates the piezo MEMS micro-speaker capabilities integrated into AkiraOS for the ST Piezo MEMS Design Challenge.

## Overview

The audio demo application showcases:
- **Sound Effects Library**: All 12 predefined game sound effects
- **Melody Playback**: Multi-tone sequence (Super Mario Bros theme)
- **Frequency Sweep**: Test audio response across frequency range
- **Volume Control**: Dynamic volume adjustment demonstration
- **Alarm Effects**: Siren-like alternating tones
- **Game Sequence**: Realistic game audio scenario

## Features Demonstrated

### 1. Sound Effects Demo
Plays all predefined sound effects:
- Coin collect
- Jump
- Explosion
- Power-up
- Menu beep/select
- Error
- Victory fanfare
- Game over
- Low battery warning
- Startup sound
- WiFi connected notification

### 2. Melody Playback
Demonstrates multi-tone sequence playback with the `akira_audio_play_sequence()` API. Plays the opening notes of the Super Mario Bros theme as an example of creating retro game music.

### 3. Frequency Sweep
Sweeps through the audio frequency range (500 Hz - 5000 Hz) to demonstrate the speaker's frequency response. Useful for:
- Testing prototype piezo buzzers
- Measuring resonance peaks
- Validating frequency response vs design specifications

### 4. Volume Control
Demonstrates dynamic volume adjustment from 20% to 100% and back down. Shows the effectiveness of the PWM duty cycle modulation for amplitude control.

### 5. Alarm Effect
Creates a siren-like effect with alternating high and low frequency tones. Demonstrates rapid tone switching capabilities.

### 6. Game Sequence
Simulates a realistic game audio scenario:
1. Game startup sound
2. Player jumps
3. Collects coins (3x)
4. Picks up power-up
5. Defeats enemy (explosion)
6. Level complete (victory fanfare)

## Building and Running

### Option 1: Standalone Application (Not yet implemented)

To build as a standalone WASM application:

```bash
# TODO: WASM compilation example
# This requires WASM runtime API bindings (future work)
```

### Option 2: Shell-Based Demo (Current Implementation)

The demo functions are available through the AkiraOS shell:

```bash
# Build AkiraOS with audio support
cd /path/to/AkiraOS
west build -b esp32s3_devkitm/esp32s3/procpu

# Flash to device
west flash

# Connect to serial console
west espressif monitor

# Run individual demos
akira:~$ audio sfx demo          # Sound effects
akira:~$ audio tone 659 150 85   # Single tone (E5 note)
akira:~$ audio sweep 500 5000 250 # Frequency sweep

# Or use the integrated demo in main app
# (if audio_demo.c is linked into main application)
```

## API Usage Examples

### Playing a Simple Tone

```c
#include "drivers/akira_audio.h"

/* Play 1000 Hz tone for 500ms at 80% volume */
akira_audio_play_tone(1000, 500, 80);
```

### Playing a Sequence

```c
const akira_audio_tone_t melody[] = {
    {.frequency_hz = 262, .duration_ms = 200, .volume = 85, .waveform = AKIRA_AUDIO_WAVE_SQUARE},
    {.frequency_hz = 294, .duration_ms = 200, .volume = 85, .waveform = AKIRA_AUDIO_WAVE_SQUARE},
    {.frequency_hz = 330, .duration_ms = 200, .volume = 85, .waveform = AKIRA_AUDIO_WAVE_SQUARE},
    {.frequency_hz = 349, .duration_ms = 400, .volume = 90, .waveform = AKIRA_AUDIO_WAVE_SQUARE},
};

akira_audio_play_sequence(melody, ARRAY_SIZE(melody));
```

### Using Predefined Sound Effects

```c
/* Coin collect sound */
akira_audio_sfx_coin();

/* Victory fanfare */
akira_audio_sfx_victory();

/* Low battery warning */
akira_audio_sfx_low_battery();
```

### Volume Control

```c
/* Set master volume to 70% */
akira_audio_set_volume(70);

/* Get current status */
akira_audio_status_t status;
akira_audio_get_status(&status);
printk("Current volume: %u%%\n", status.current_volume);
```

## Hardware Setup

### For Discrete Piezo Buzzer Prototype

```
ESP32-S3 GPIO9 ──[ 100Ω ]──┬──(+) Piezo Buzzer (-)──┐
                            │                         │
                            └─────────────────────────┴── GND
```

### For Custom MEMS Device

Connect the piezo MEMS actuator pads to:
- Positive terminal: GPIO9 (PWM output)
- Negative terminal: GND

For higher output, use an amplifier circuit (see [prototype validation guide](../../docs/piezo-prototype-validation.md)).

## Expected Output

When running the demo, you should observe:

**Console Output:**
```
AkiraOS Audio Demo Application
ST Piezo MEMS Design Challenge

Available demos:
  1. Sound Effects Demo
  2. Melody Playback
  3. Frequency Sweep
  4. Volume Control
  5. Alarm Effect
  6. Game Sequence
  7. Run All Demos

Use shell command: audio_demo <number>
```

**Audio Output:**
- Clear tones at specified frequencies
- Distinct sound effects recognizable as game audio
- Smooth frequency transitions in sweep mode
- Volume changes audible in volume control demo

## Testing and Validation

### Sound Quality Checklist

- [ ] All sound effects are clearly audible
- [ ] Tones match expected frequencies (verify with audio analyzer app)
- [ ] No audible distortion at 80% volume
- [ ] Volume control changes are smooth and proportional
- [ ] Frequency sweep shows resonance peak (typically 2-4 kHz for discrete buzzer)

### Performance Metrics

Record the following for prototype validation:

| Test | Expected | Actual | Notes |
|------|----------|--------|-------|
| Startup sound duration | ~1-2 sec | | |
| Coin collect sound | Clear, rising pitch | | |
| Max SPL @ resonance | > 80 dB @ 10cm | | |
| Power consumption @ 80% volume | < 50 mW | | |
| Response latency | < 10 ms | | |

### Troubleshooting

**No sound output:**
- Check piezo buzzer polarity (try reversing)
- Verify GPIO9 is free and not used by another peripheral
- Confirm `CONFIG_AKIRA_AUDIO=y` in prj.conf
- Test with maximum volume: `audio tone 2000 1000 100`

**Quiet or distorted sound:**
- Increase volume: `audio volume 100`
- Test at resonance frequency (typically 2-4 kHz): `audio tone 3000 1000 100`
- Check resistor value (should be 47-100Ω)
- Ensure adequate power supply (USB should provide 500mA+)

**Inconsistent behavior:**
- Re-seat breadboard connections
- Try different piezo buzzer (may be defective)
- Check for EMI (keep wires away from ESP32 antenna)

## Integration with Games

### Example: Adding Audio to a Game

```c
/* In your game loop */

void game_collect_coin(void) {
    player_score += 10;
    akira_audio_sfx_coin();  /* Audio feedback */
}

void game_player_jump(void) {
    player_velocity_y = JUMP_VELOCITY;
    akira_audio_sfx_jump();  /* Audio feedback */
}

void game_level_complete(void) {
    game_state = STATE_VICTORY;
    akira_audio_sfx_victory();  /* Victory music */
}
```

## Future Enhancements

### WASM API Integration (Planned)

```javascript
// JavaScript/WASM API (future implementation)
export function onPlayerJump() {
    audio.playTone(400, 40, 80);
}

export function onCoinCollect() {
    audio.playSFX('coin');
}

export function playBackgroundMusic() {
    const melody = [
        { freq: 262, dur: 200, vol: 70 },
        { freq: 294, dur: 200, vol: 70 },
        { freq: 330, dur: 200, vol: 70 },
        { freq: 349, dur: 400, vol: 75 }
    ];
    audio.playSequence(melody);
}
```

### Advanced Features (TODO)

- [ ] PCM waveform playback for recorded sounds
- [ ] ADSR envelope shaping for more natural sounds
- [ ] Background music looping
- [ ] Multiple channel mixing
- [ ] Audio compression/decompression

## Documentation References

- [Piezo MEMS Speaker Design](../../docs/piezo-mems-speaker-design.md)
- [Prototype Validation Guide](../../docs/piezo-prototype-validation.md)
- [ST Challenge Submission Guide](../../docs/st-challenge-submission-guide.md)
- [Audio Driver API](../../src/drivers/akira_audio.h)

## License

This example is part of AkiraOS and is licensed under the MIT License.

## Contributing

Contributions are welcome! If you add new sound effects or demo modes:
1. Follow the existing code style
2. Add documentation for new features
3. Test on actual hardware (ESP32-S3 + piezo buzzer)
4. Submit a pull request

---

**ST Piezo MEMS Design Challenge** - Demonstrating MEMS audio in embedded gaming systems

For questions or support, see the [main AkiraOS repository](https://github.com/ArturR0k3r/AkiraOS).
