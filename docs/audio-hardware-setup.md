# AkiraOS Audio Hardware Setup Guide

## Quick Start Prototyping

### Minimal Setup (Direct Connection)

The simplest way to test the piezo audio functionality:

```
     ESP32-S3 DevKit
           │
       GPIO19 ────[ 100Ω resistor ]────┬───(+) Piezo Buzzer
                                       │
                                    (GND)────(-)────────┘
```

**Components Needed:**
- ESP32-S3 DevKit M-1 or similar
- Piezo buzzer (3-5V, any passive buzzer)
- 100Ω resistor (1/4W)
- Breadboard
- Jumper wires

**Expected Performance:**
- Audible tones from 500 Hz to 8 kHz
- Moderate volume (60-70 dB SPL @ 10cm)
- Power consumption: 5-15 mW

### Amplified Setup (Higher Volume)

For louder audio output, use a transistor amplifier:

```
                        +3.3V
                          │
                          ├────[ 100Ω ]────┐
                          │                 │
                      [Piezo Buzzer]   [NPN Transistor]
                       (+)    (-)       (Collector)
                        │      │            │
                        │      └────────────┤
                        │                (Emitter)
                        │                   │
                        └───────────────────┴─── GND
                                            │
                                        GPIO19 (PWM)
                                          ESP32-S3
```

**Additional Components:**
- NPN transistor: 2N3904, BC547, or similar
- 1kΩ resistor (base resistor)

**Expected Performance:**
- Louder audio output (75-85 dB SPL @ 10cm)
- Better drive capability
- Power consumption: 15-30 mW

## Detailed Connection Instructions

### Step 1: ESP32-S3 Preparation

1. **Connect USB-C cable** to ESP32-S3 DevKit
2. **Verify power LED** is illuminated
3. **Note GPIO19 location** on your specific board
   - On ESP32-S3-DevKitM-1: GPIO19 is typically labeled on the board
   - Check pinout diagram if needed

### Step 2: Breadboard Assembly (Simple Version)

1. **Insert ESP32-S3** into breadboard
   - Place across center divide for stability
   - Ensure all pins are properly seated

2. **Connect Ground**
   - Use breadboard's ground rail
   - Connect ESP32 GND pin to ground rail

3. **Connect Piezo Buzzer**
   - Identify polarity (red/marked wire is positive)
   - Insert positive lead into breadboard
   - Connect negative lead to ground rail

4. **Add Current Limiting Resistor**
   - Connect 100Ω resistor between GPIO19 and piezo positive
   - This protects the GPIO pin

5. **Final Check**
   - Verify no shorts with multimeter
   - Check piezo buzzer continuity (~ohms resistance expected)

### Step 3: Breadboard Assembly (Amplified Version)

1. **Follow Steps 1-2** from simple version

2. **Place NPN Transistor**
   - Identify pins: Emitter (E), Base (B), Collector (C)
   - For 2N3904: Flat side facing you, left=E, middle=B, right=C
   - Insert into breadboard with each pin in separate row

3. **Wire Transistor Base**
   - GPIO19 → 1kΩ resistor → Transistor Base
   - This is the control signal

4. **Wire Transistor Collector**
   - Piezo positive (+) → Transistor Collector

5. **Wire Transistor Emitter**
   - Transistor Emitter → Ground rail

6. **Connect Piezo Negative**
   - Piezo negative (-) → +3.3V through 100Ω resistor
   - Or: Piezo negative → Ground (alternative, quieter)

## Pin Configuration

### Default GPIO Assignment

| Signal | GPIO Pin | ESP32-S3 Function | Notes |
|--------|----------|-------------------|-------|
| PWM Audio | GPIO19 | LEDC Channel 0 | Configurable in device tree |
| GND | GND | Common ground | Multiple GND pins available |

### Alternative GPIO Options

If GPIO19 conflicts with other peripherals, you can reconfigure in device tree overlay:

```dts
/* In boards/esp32s3_devkitm.overlay */
&ledc0 {
    /* Change to different GPIO */
    pinmux = <LEDC_CH0_GPIO10>;  /* Example: GPIO10 instead */
};
```

Available GPIO pins on ESP32-S3 for PWM:
- GPIO0-GPIO21 (most digital pins)
- Avoid: GPIO26-GPIO32 (used for flash/PSRAM)
- Avoid: GPIO43-GPIO46 (strapping pins)

## Component Selection Guide

### Piezo Buzzers

**Recommended Models:**

| Model | Type | Resonance | Voltage | SPL | Price |
|-------|------|-----------|---------|-----|-------|
| Murata 7BB-27-4 | Buzzer | 2.7 kHz | 3-5V | 85 dB | $2-3 |
| TDK PS1240 | Buzzer | 4 kHz | 3-5V | 90 dB | $2-4 |
| CUI CMT-8504-100-SMT | Transducer | Broadband | 3-5V | 75 dB | $1-2 |
| Generic 12mm disc | Disc | 3 kHz | 3-5V | 70 dB | $0.50 |

**Selection Criteria:**
- **Resonance frequency**: 2-4 kHz ideal for gaming sounds
- **Voltage rating**: 3-5V compatible with ESP32 (3.3V logic)
- **Size**: 12-20mm diameter fits handheld console
- **Type**: Passive buzzer (not active/self-oscillating)

### Transistors

**Recommended NPN Transistors:**
- **2N3904**: General purpose, common, cheap ($0.10)
- **BC547**: Similar to 2N3904, widely available
- **2N2222**: Higher current capability if needed
- **MMBT3904**: SMD version for compact designs

**Specifications Needed:**
- Collector current: > 50 mA
- Collector-emitter voltage: > 10V
- Gain (hFE): > 100

### Resistors

| Resistor | Value | Purpose | Power |
|----------|-------|---------|-------|
| R1 | 100Ω | Current limit (GPIO protection) | 1/4W |
| R2 | 1kΩ | Base resistor (amplified version) | 1/4W |

## Testing Procedure

### 1. Visual Inspection
- [ ] All connections match circuit diagram
- [ ] No exposed wire shorts
- [ ] Piezo polarity correct (red = positive)
- [ ] Transistor pins correctly identified

### 2. Multimeter Checks
- [ ] Measure GPIO19 to GND: Should be high-Z (open) when idle
- [ ] Measure piezo resistance: ~hundreds of ohms
- [ ] Check for shorts: GPIO to GND should be open circuit

### 3. Initial Power-On
- [ ] Connect USB to ESP32
- [ ] Power LED illuminates
- [ ] No smoke or unusual smells (if so, disconnect immediately!)

### 4. Audio Test
```bash
# Flash AkiraOS firmware
west flash -d build_esp32s3

# Connect serial monitor
west espressif monitor

# Test audio
akira:~$ audio test_tone
```

**Expected Result:**
- 1 kHz tone for 1 second
- Clear, steady tone (not crackling or buzzing)
- Moderate volume (audible at arm's length)

## Troubleshooting

### No Sound Output

**Check:**
1. Piezo buzzer polarity (try reversing)
2. GPIO19 is not used by another peripheral
3. Resistor connections are secure
4. USB provides adequate power (try different USB port/cable)

**Test:**
```bash
akira:~$ audio tone 2000 1000 100  # Max volume at resonance
```

### Very Quiet Sound

**Possible Causes:**
- Frequency not at piezo resonance (try 2-4 kHz range)
- Volume too low (increase: `audio volume 100`)
- Resistor value too high (should be 47-100Ω)
- Poor breadboard connections

**Solution:**
```bash
akira:~$ audio sweep 1000 5000 100  # Find resonance peak
akira:~$ audio volume 100            # Maximum volume
```

### Distorted Sound

**Possible Causes:**
- Overdrive (voltage too high)
- Resistor value too low
- Transistor saturated (if using amplified version)

**Solution:**
- Increase resistor value to 150-220Ω
- Reduce volume: `audio volume 70`
- Check transistor base resistor (should be 1kΩ)

### Intermittent Sound

**Possible Causes:**
- Loose breadboard connections
- Damaged jumper wires
- Defective piezo buzzer

**Solution:**
- Re-seat all connections firmly
- Replace jumper wires
- Test with different piezo buzzer

## Advanced: Custom MEMS Integration

For custom piezoelectric MEMS devices fabricated through MPW:

### Electrical Interface

```
ESP32-S3 GPIO19 ──[ Series Resistor ]──┬── Top Electrode
                                       │
                                    (PCB)── Bottom Electrode (GND)
```

**Design Considerations:**
- **Capacitance**: MEMS devices are highly capacitive (10-100 pF)
- **Impedance**: Very high impedance (MΩ range at low frequencies)
- **Drive voltage**: 3.3V PWM sufficient for piezoelectric actuation
- **Matching**: May require impedance matching network for optimal SPL

### PCB Design

For production integration:

1. **Copper landing pads**: 1mm x 1mm (wire bonding) or 0.5mm x 0.5mm (flip-chip)
2. **Acoustic port**: 3-4mm diameter hole in PCB under MEMS die
3. **Protection**: TVS diode on GPIO line for ESD protection
4. **Decoupling**: 100nF capacitor near MEMS power pins

### Packaging

- **COB (Chip-on-Board)**: Direct die attach to PCB, wire bonding
- **LGA (Land Grid Array)**: Custom ceramic carrier with acoustic port
- **Protective mesh**: Dust filter over acoustic opening

## Safety Notes

- **Voltage**: ESP32-S3 operates at 3.3V (safe)
- **Current**: GPIO pins limited to 40mA (within safe limits)
- **Audio volume**: Piezo buzzers can be loud at max volume
  - Use hearing protection for extended testing
  - Keep volume at 70-80% for prolonged use
- **Polarity**: Reversing piezo polarity won't damage it, just reduces efficiency
- **ESD**: Handle MEMS devices with anti-static precautions

## Bill of Materials (BOM)

### Basic Prototype

| Item | Quantity | Unit Price | Total |
|------|----------|------------|-------|
| ESP32-S3 DevKit M-1 | 1 | $12 | $12 |
| Piezo buzzer (Murata 7BB-27-4) | 1 | $2.50 | $2.50 |
| 100Ω resistor, 1/4W | 1 | $0.02 | $0.02 |
| Breadboard | 1 | $3 | $3 |
| Jumper wires (pack) | 1 | $2 | $2 |
| USB-C cable | 1 | $3 | $3 |
| **Total** | | | **$22.52** |

### Amplified Version (Add to Basic)

| Item | Quantity | Unit Price | Total |
|------|----------|------------|-------|
| 2N3904 transistor | 1 | $0.10 | $0.10 |
| 1kΩ resistor, 1/4W | 1 | $0.02 | $0.02 |
| **Additional Cost** | | | **$0.12** |

### Optional Test Equipment

| Item | Quantity | Unit Price | Total |
|------|----------|------------|-------|
| SPL meter (REED R8050) | 1 | $75 | $75 |
| Multimeter | 1 | $20 | $20 |
| Oscilloscope (USB, DSO138) | 1 | $30 | $30 |

---

## References

- **ESP32-S3 Datasheet**: https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf
- **LEDC PWM Documentation**: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/ledc.html
- **Murata Piezo Buzzers**: https://www.murata.com/en-us/products/sound
- **Zephyr PWM API**: https://docs.zephyrproject.org/latest/hardware/peripherals/pwm.html

---

**Document Version:** 1.0  
**Last Updated:** November 2025  
**For:** ST Piezo MEMS Design Challenge
