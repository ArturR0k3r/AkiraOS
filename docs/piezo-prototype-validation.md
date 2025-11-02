# Piezo MEMS Micro-Speaker Prototype Validation Guide

## Overview

This document provides step-by-step instructions for prototyping and validating the piezoelectric micro-speaker integration with AkiraOS using off-the-shelf components before fabricating custom MEMS devices.

---

## Hardware Setup

### Required Components

| Component | Specification | Example Part Number | Cost (USD) |
|-----------|---------------|---------------------|------------|
| ESP32-S3 DevKit | ESP32-S3-WROOM-1 | ESP32-S3-DevKitM-1 | $10-15 |
| Piezo Buzzer | 3-5V, 2-4 kHz resonance | Murata 7BB-27-4, TDK PS1240 | $1-3 |
| NPN Transistor | Small signal, > 50mA | 2N3904, BC547 | $0.10 |
| Resistor | 100Ω, 1/4W | Generic | $0.02 |
| Breadboard | Standard | Generic | $3-5 |
| Jumper Wires | Male-to-male | Generic | $2-3 |
| USB Cable | USB-C (ESP32-S3) | Generic | $3-5 |

**Optional (for advanced testing):**
- Sound Pressure Level (SPL) meter: REED R8050 (~$50-100)
- Oscilloscope: For signal verification
- Multimeter: For current/voltage measurements

### Circuit Diagram

```
                     +3.3V (ESP32-S3)
                          │
                          ├────[ 100Ω ]────┬
                          │                 │
                      [Piezo Buzzer]   [NPN Transistor]
                       (+)    (-)       (Collector)
                        │      │            │
                        │      └────────────┤
                        │               (Emitter)
                        │                   │
                        └───────────────────┴─── GND
                                            │
                                        GPIO21 (PWM)
                                      ESP32-S3 Pin


Alternative Simpler Circuit (Direct Connection - Lower Volume):

    ESP32-S3 GPIO21 ──[ 100Ω ]──┬──(+) Piezo Buzzer (-)──┐
                                 │                         │
                                 └─────────────────────────┴── GND
```

### Step-by-Step Hardware Assembly

#### Option 1: Transistor Amplified (Recommended for Louder Audio)

1. **Prepare the Breadboard**
   - Insert ESP32-S3 DevKit into breadboard
   - Ensure power LED lights up when connected via USB

2. **Place the Piezo Buzzer**
   - Identify polarity: Red/marked wire = positive (+)
   - Insert buzzer legs into breadboard with ~5-10 rows spacing

3. **Add the NPN Transistor**
   - Identify pins: Collector (C), Base (B), Emitter (E)
   - 2N3904: Flat side facing you, left=E, middle=B, right=C
   - Insert transistor with each pin in separate breadboard row

4. **Wire the Connections**
   ```
   Connection Map:
   - ESP32-S3 GPIO21 → Transistor Base (via 1kΩ resistor)
   - Transistor Collector → Piezo (+) [Red wire]
   - Transistor Emitter → GND
   - Piezo (-) → ESP32-S3 +3.3V (through 100Ω resistor for protection)
   - ESP32-S3 GND → Breadboard ground rail
   ```

5. **Power Connection**
   - Connect ESP32-S3 to computer via USB-C cable
   - Verify power LED is on
   - No external power supply needed (USB provides 5V)

#### Option 2: Direct Connection (Simpler, Quieter)

1. **Connect Components**
   ```
   - ESP32-S3 GPIO21 → 100Ω resistor → Piezo (+)
   - Piezo (-) → ESP32-S3 GND
   ```

2. **Verify Connections**
   - Check for shorts with multimeter
   - Ensure piezo polarity is correct

### Pin Configuration for ESP32-S3

| Signal | GPIO Pin | Function |
|--------|----------|----------|
| PWM Audio Out | GPIO21 | LEDC Channel 0 |
| Ground | GND | Common ground |
| Power | 3.3V | Optional (for direct drive) |

**Note:** GPIO21 is used because it's mapped to LEDC peripheral in the AkiraOS device tree configuration.

---

## Software Setup

### 1. Install AkiraOS Firmware

#### Prerequisites
- Zephyr SDK installed (follow [AkiraOS README](../README.md))
- West tool configured
- ESP-IDF tools available

#### Build and Flash

```bash
# Navigate to AkiraOS directory
cd /path/to/AkiraOS

# Initialize West workspace (if not done)
west init -l .
west update

# Build for ESP32-S3 with audio support
west build -b esp32s3_devkitm/esp32s3/procpu -d build_esp32s3 -- \
    -DCONFIG_AKIRA_AUDIO=y

# Flash to device
west flash -d build_esp32s3

# Monitor serial output
west espressif monitor -d build_esp32s3
```

### 2. Enable Audio in Configuration

Add to `prj.conf` (or create `prj_audio.conf` overlay):

```ini
# Enable PWM for audio output
CONFIG_PWM=y
CONFIG_PWM_ESP32_LEDC=y

# Enable audio subsystem
CONFIG_AKIRA_AUDIO=y

# Enable shell commands for testing
CONFIG_SHELL=y
```

### 3. Device Tree Configuration

Add to `boards/esp32s3_devkitm.overlay`:

```dts
/ {
    aliases {
        pwm-audio = &ledc0;
    };
};

&ledc0 {
    status = "okay";
    pinctrl-0 = <&ledc0_default>;
    pinctrl-names = "default";
    #address-cells = <1>;
    #size-cells = <0>;
    
    channel0@0 {
        reg = <0>;
        timer = <0>;
    };
};

&pinctrl {
    ledc0_default: ledc0_default {
        group1 {
            pinmux = <LEDC_CH0_GPIO21>;
            output-enable;
        };
    };
};
```

---

## Testing Procedures

### Test 1: Basic Functionality Check

**Objective:** Verify audio output is working

**Procedure:**
1. Power on ESP32-S3 with piezo connected
2. Open serial terminal (115200 baud)
3. Run shell command:
   ```
   akira:~$ audio test_tone
   ```
4. Listen for 1 kHz tone (should be clearly audible)

**Expected Result:**
- Clear tone audible from piezo
- No distortion or crackling
- Serial output confirms tone playback

**Pass Criteria:** Audible tone at expected frequency

---

### Test 2: Frequency Response Sweep

**Objective:** Measure frequency response and find resonance peak

**Equipment:**
- SPL meter (optional but recommended)
- Ruler (for consistent 10cm measurement distance)

**Procedure:**
1. Position SPL meter 10 cm from piezo buzzer
2. Run frequency sweep test:
   ```
   akira:~$ audio sweep 100 10000 100
   ```
   (Sweep from 100 Hz to 10 kHz in 100 Hz steps)

3. Record SPL at each frequency
4. Create frequency response plot

**Manual Alternative (without SPL meter):**
```bash
# Test individual frequencies and note subjective loudness
akira:~$ audio tone 500 1000 80    # 500 Hz, 1 sec, 80% volume
akira:~$ audio tone 1000 1000 80   # 1 kHz
akira:~$ audio tone 2000 1000 80   # 2 kHz
akira:~$ audio tone 4000 1000 80   # 4 kHz
akira:~$ audio tone 8000 1000 80   # 8 kHz
```

**Expected Results:**
- Peak response at buzzer's resonant frequency (typically 2-4 kHz)
- Usable response from 500 Hz to 8 kHz
- Q-factor (resonance sharpness): 20-50

**Data Recording Template:**

| Frequency (Hz) | SPL (dB) @ 10cm | Notes |
|----------------|-----------------|-------|
| 100 | | |
| 500 | | |
| 1000 | | |
| 2000 | | |
| 3000 | | Peak expected here |
| 4000 | | |
| 6000 | | |
| 8000 | | |
| 10000 | | |

---

### Test 3: Volume Control Linearity

**Objective:** Verify volume control works correctly

**Procedure:**
1. Set fixed frequency (1 kHz - mid-range)
2. Vary volume from 0% to 100% in 10% steps
3. Measure SPL at each setting

```bash
akira:~$ audio tone 1000 2000 10   # 10% volume
akira:~$ audio tone 1000 2000 20   # 20% volume
akira:~$ audio tone 1000 2000 30   # 30% volume
# ... continue to 100%
```

**Expected Results:**
- SPL increases with volume setting
- ~40 dB dynamic range (min to max)
- No distortion at maximum volume

**Data Recording:**

| Volume (%) | SPL (dB) | Current (mA) |
|------------|----------|--------------|
| 10 | | |
| 20 | | |
| 30 | | |
| 40 | | |
| 50 | | |
| 60 | | |
| 70 | | |
| 80 | | |
| 90 | | |
| 100 | | |

---

### Test 4: Power Consumption Analysis

**Objective:** Measure current draw during audio playback

**Equipment:**
- Multimeter (DC current mode)
- Or: USB power monitor (e.g., UM25C)

**Procedure:**
1. **Idle State:**
   - Measure current with no audio playing
   - Record baseline consumption

2. **Quiet Audio (50% volume, 1 kHz):**
   ```
   akira:~$ audio tone 1000 10000 50
   ```
   - Measure current during playback
   - Calculate power: P = V × I (V = 3.3V for ESP32-S3)

3. **Maximum Volume (100%, 2 kHz resonance):**
   ```
   akira:~$ audio tone 2000 10000 100
   ```
   - Measure peak current
   - Calculate maximum power

**Expected Results:**

| Condition | Current (mA) | Power (mW) | Notes |
|-----------|--------------|------------|-------|
| Idle (no audio) | < 1 | < 3 | Piezo capacitive load |
| 50% volume, 1 kHz | 5-10 | 16-33 | Typical gaming audio |
| 100% volume, 2 kHz | 15-20 | 50-66 | Maximum output |

**Pass Criteria:**
- Idle current < 1 mA
- Maximum current < 25 mA (within ESP32 GPIO limits)
- Power efficiency > 1 mW per dB SPL

---

### Test 5: Audio Quality Assessment

**Objective:** Subjective evaluation of sound quality for gaming

**Procedure:**
1. **Test Predefined Sound Effects:**
   ```bash
   akira:~$ audio sfx coin        # Coin collect
   akira:~$ audio sfx jump        # Jump
   akira:~$ audio sfx explosion   # Explosion
   akira:~$ audio sfx powerup     # Power-up
   akira:~$ audio sfx menu_beep   # Menu navigation
   akira:~$ audio sfx victory     # Victory fanfare
   akira:~$ audio sfx game_over   # Game over
   ```

2. **Evaluate Each Sound:**
   - Clarity: Is the sound recognizable?
   - Tone: Does it match the intended emotion?
   - Distortion: Any crackling or buzzing?
   - Volume: Appropriate loudness?

3. **Record Subjective Ratings (1-5 scale):**

| Sound Effect | Clarity | Tone Quality | Volume | Overall |
|--------------|---------|--------------|--------|---------|
| Coin Collect | | | | |
| Jump | | | | |
| Explosion | | | | |
| Power-up | | | | |
| Menu Beep | | | | |
| Victory | | | | |
| Game Over | | | | |

**Pass Criteria:** Average rating > 3.5/5 for retro gaming applications

---

### Test 6: Response Time / Latency

**Objective:** Measure audio onset delay (critical for gaming)

**Equipment:**
- Oscilloscope (optional)
- Smartphone with slow-motion video (120+ fps)

**Procedure:**
1. **Visual Trigger Method:**
   - Film ESP32-S3 with smartphone slow-motion camera
   - Trigger tone via button press (visible LED change)
   - Analyze video frame-by-frame to measure delay

2. **Oscilloscope Method (if available):**
   - Connect scope to GPIO21 (PWM output)
   - Trigger tone via shell command
   - Measure time from command to PWM signal start

```bash
akira:~$ audio tone 1000 100 80
```

**Expected Results:**
- Response time: < 10 ms (imperceptible to humans)
- Acceptable for gaming: < 50 ms
- 10-90% rise time: < 1 ms

**Pass Criteria:** Total latency < 20 ms from API call to audible sound

---

## Troubleshooting

### Problem: No Sound Output

**Possible Causes:**
1. Piezo polarity reversed (try flipping connections)
2. GPIO pin not configured correctly (check device tree)
3. PWM not enabled (verify `CONFIG_PWM=y` in prj.conf)
4. Piezo defective (test with multimeter: should show ~ohms resistance)

**Debug Steps:**
```bash
# Check if audio driver initialized
akira:~$ audio status

# Verify PWM device
akira:~$ kernel device list | grep pwm

# Test with maximum volume
akira:~$ audio tone 2000 5000 100
```

### Problem: Very Quiet Output

**Possible Causes:**
1. Missing transistor amplification (use amplified circuit)
2. Volume set too low (check master volume)
3. Frequency not at resonance peak (try 2-4 kHz)

**Solutions:**
- Increase volume: `audio volume 100`
- Test at resonance: `audio tone 3000 2000 100`
- Add transistor amplifier circuit

### Problem: Distorted Sound

**Possible Causes:**
1. Current limiting resistor too high (reduce to 47-100Ω)
2. Voltage too high (piezo may be overdriven)
3. PWM frequency incorrect

**Solutions:**
- Reduce volume: `audio volume 70`
- Check resistor value (should be 47-100Ω)
- Verify PWM carrier frequency (100 kHz in code)

### Problem: Inconsistent Behavior

**Possible Causes:**
1. Loose breadboard connections
2. Insufficient power supply
3. EMI from other circuits

**Solutions:**
- Re-seat all connections firmly
- Use USB power supply with adequate current (500mA+)
- Keep piezo wires short and away from ESP32 antenna

---

## Data Collection & Analysis

### Recommended Measurements

1. **Frequency Response Data**
   - Export to CSV: frequency (Hz), SPL (dB)
   - Plot in Excel/Python/MATLAB
   - Identify -3dB bandwidth

2. **Power Consumption Profile**
   - Create current vs. volume chart
   - Calculate efficiency: dB per mW

3. **Audio Recordings**
   - Use smartphone or USB mic to record sound effects
   - Analyze waveforms in Audacity (free software)
   - Check for harmonic distortion

### Sample Data Format (CSV)

```csv
Test,Frequency_Hz,Duration_ms,Volume_%,SPL_dB,Current_mA,Notes
FreqSweep,500,1000,80,72,8,Low frequency roll-off
FreqSweep,1000,1000,80,78,10,Mid-range
FreqSweep,2000,1000,80,85,12,Near resonance
FreqSweep,3000,1000,80,88,14,Peak response
FreqSweep,4000,1000,80,84,12,Falling response
FreqSweep,6000,1000,80,76,9,High frequency
FreqSweep,8000,1000,80,68,7,Upper limit
```

---

## Expected Performance Summary

Based on typical piezo buzzer specifications, you should achieve:

| Metric | Target | Acceptable Range |
|--------|--------|------------------|
| Peak SPL @ 10cm | 85-90 dB | 80-95 dB |
| Frequency Range (-6dB) | 500 Hz - 8 kHz | 300 Hz - 10 kHz |
| Resonance Frequency | 2-4 kHz | 1-6 kHz |
| Power Consumption (max) | 40-50 mW | < 100 mW |
| Response Latency | < 10 ms | < 50 ms |
| Dynamic Range | 40-50 dB | > 30 dB |

---

## Next Steps After Validation

### 1. Document Results
- Compile all measurement data
- Create plots and charts
- Write summary report

### 2. Design Custom MEMS Device
- Use prototype data to optimize MEMS design
- Adjust membrane dimensions for desired resonance
- Select piezoelectric material (ScAlN recommended)

### 3. Prepare for MPW Submission
- Finalize device layout and mask designs
- Submit to ST MEMS foundry or other MPW service
- Include test structures for characterization

### 4. Software Optimization
- Fine-tune PWM parameters based on measurements
- Implement advanced features (PCM playback, effects)
- Optimize for power efficiency

---

## Photo Documentation Checklist

For challenge submission and publication:

- [ ] Complete hardware setup (breadboard view)
- [ ] Close-up of piezo buzzer connections
- [ ] ESP32-S3 DevKit with cables
- [ ] SPL meter during measurement
- [ ] Oscilloscope traces (PWM waveform)
- [ ] Serial terminal with test commands
- [ ] Frequency response graph
- [ ] Power consumption chart

---

## Safety Notes

- **Voltage:** ESP32-S3 operates at 3.3V (safe)
- **Current:** GPIO pins limited to 40mA max (piezo draws < 20mA)
- **Audio Volume:** Piezo buzzers can be loud at max volume (use ear protection if testing for extended periods)
- **Polarity:** Reversing piezo polarity won't damage it, but may reduce efficiency
- **Heat:** Components should stay cool; if anything gets hot, disconnect and check for shorts

---

## References & Resources

**Hardware Datasheets:**
- ESP32-S3 Technical Reference: https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf
- Murata Piezo Buzzers: https://www.murata.com/en-us/products/sound
- 2N3904 Transistor Datasheet

**Software Documentation:**
- Zephyr PWM API: https://docs.zephyrproject.org/latest/hardware/peripherals/pwm.html
- ESP-IDF LEDC Driver: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/ledc.html

**Audio Analysis Tools:**
- Audacity (free audio editor): https://www.audacityteam.org/
- REW (Room EQ Wizard): https://www.roomeqwizard.com/
- Python Scipy for signal processing

**SPL Measurement Apps (Smartphone):**
- Sound Meter (Android/iOS) - reasonable accuracy for relative measurements
- Decibel X (iOS) - calibrated with reference SPL meter

---

**Document Version:** 1.0  
**Last Updated:** November 2025  
**Maintainer:** AkiraOS Development Team
