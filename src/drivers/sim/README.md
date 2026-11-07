> **Status on v1.6.x: not built.** Nothing adds this directory to the build
> (`add_subdirectory()`), and `snippets/akira-board/boards/native_sim.conf` sets
> `CONFIG_DISPLAY=n`, so a `native_sim` build does not open this window. The
> firmware's native_sim HAL (`src/drivers/platform_hal.c`) maps its framebuffer
> and button state to `/tmp/akira_framebuffer` and `/tmp/akira_buttons` instead.
> Treat the rest of this page as design notes until the local simulator is reworked.

# ℹ️ About

The Akira Console SDL2 Visual Simulator provides a faithful, interactive simulation of the Akira Console hardware for Zephyr's `native_sim` platform. It enables developers to run and test AkiraOS firmware on a PC with a graphical window that closely mimics the real device, including:

- A live, hardware-accurate 240x320 TFT display simulation
- Interactive button input (D-Pad, action, power, settings) via mouse and keyboard
- Visual layout and controls matching the physical console
- Real-time updates and low-latency input for rapid development and debugging

This simulator is ideal for firmware development, UI prototyping, and automated testing—no physical hardware required. It is tightly integrated with the Zephyr build system and automatically included when building for `native_sim`.

# 🎮 Akira Console SDL2 Visual Simulator

This module provides a graphical simulation of the Akira Console hardware when building for Zephyr's `native_sim` platform.

## ✨ Features

- **Visual Display**: Simulates the 240x320 ILI9341 TFT display in a window
- **Interactive Buttons**: 10 buttons (D-Pad, Action buttons, Power, Settings)
- **Mouse Input**: Click buttons with the mouse
- **Keyboard Input**: Use WASD, IJKL, and other keys
- **Real-time Updates**: Display updates as the firmware draws
- **Hardware-accurate**: Behaves identically to real hardware

## 📦 Requirements

### Install SDL2

```bash
# Ubuntu/Debian
sudo apt-get install libsdl2-dev

# Fedora
sudo dnf install SDL2-devel

# macOS
brew install sdl2
```

## 🔨 Building

From the root of your AkiraOS checkout:

```bash
./build.sh            # builds native_sim, then runs it
```

The build lands in `../build-native-sim` (the workspace root).

## 🚀 Running

```bash
../build-native-sim/zephyr/zephyr.exe
```

A window will open showing the Akira Console!

## 🎮 Controls

### Keyboard Mapping

| Key(s) | Button | Description |
|--------|--------|-------------|
| `W` | UP | D-Pad Up |
| `S` | DOWN | D-Pad Down |
| `A` | LEFT | D-Pad Left |
| `D` | RIGHT | D-Pad Right |
| `I` | X | Action button X |
| `K` | B | Action button B |
| `J` | Y | Action button Y |
| `L` | A | Action button A |
| `ESC` | POWER | Power/ON-OFF |
| `ENTER` | SETTINGS | Settings button |

### Mouse Input

- **Left Click**: Press buttons
- **Release**: Release buttons
- Click directly on the button circles in the simulator window

## 🖼️ Window Layout

```
┌─────────────────────────────────────┐
│   Akira Console Simulator           │  ← Title bar
├─────────────────────────────────────┤
│                                     │
│     ┌─────────────────────┐        │
│     │                     │        │  ← Display
│     │   240x320 Display   │        │     (red frame)
│     │                     │        │
│     └─────────────────────┘        │
│                                     │
│  [SET]                      [PWR]  │  ← Top buttons
│                                     │
│   ○   ○                    ○   ○   │  ← Button layout
│ ○   ○   ○                ○   ○   ○ │     D-Pad + ABXY
│   ○   ○                    ○   ○   │
│                                     │
│          ┌──────────┐              │  ← PCB/Logo area
│          │  AKIRA   │              │
│          └──────────┘              │
└─────────────────────────────────────┘
```

## 📁 Architecture

### Files

- **akira_sim.h** - Public API and definitions
- **akira_sim.c** - Main simulator logic and threading
- **akira_sim_display.c** - Display rendering (RGB565 → SDL)
- **akira_sim_buttons.c** - Button handling (mouse + keyboard)
- **CMakeLists.txt** - Build configuration

### Integration

The simulator integrates with `akira_hal.c`:

```c
// In akira_hal.c
#if AKIRA_PLATFORM_NATIVE_SIM && defined(AKIRA_SIM_ENABLED)
    akira_sim_init();                      // Initialize SDL2 window
    akira_sim_update_display(framebuffer); // Update display
    state = akira_sim_get_button_state();  // Read buttons
#endif
```

### Threading

The simulator runs in a separate pthread:
- **Main thread**: Runs Zephyr RTOS and application code
- **Simulator thread**: Handles SDL2 events and rendering (~60 FPS)

## 🔧 Customization

### Change Window Size

Edit `akira_sim.h`:

```c
#define SIM_WINDOW_WIDTH   400
#define SIM_WINDOW_HEIGHT  600
```

### Change Button Layout

Edit button positions in `akira_sim_buttons.c`:

```c
static akira_button_t buttons[SIM_NUM_BUTTONS] = {
    {x, y, radius, false, AKIRA_BTN_UP, "↑"},
    // ...
};
```

### Change Keyboard Mapping

Edit key mapping in `akira_sim_buttons.c`:

```c
static const struct {
    SDL_Keycode key;
    akira_button_id_t button;
} key_mapping[] = {
    {SDLK_w, AKIRA_BTN_UP},
    // ...
};
```

## 🐛 Troubleshooting

### SDL2 Not Found

```
CMake Error: Could not find SDL2
```

**Solution**: Install SDL2 development libraries (see Requirements above)

### Simulator Window Doesn't Open

Check logs:
```
[00:00:00.000,000] <inf> akira_sim: Initializing Akira Console Simulator
[00:00:00.010,000] <inf> akira_sim: ✅ Akira Console Simulator initialized
```

If not present, SDL2 initialization failed. Check:
- SDL2 is installed
- Display server is available (X11/Wayland)
- Running in graphical environment (not SSH without X forwarding)

### Buttons Not Responding

**Keyboard**: Make sure simulator window has focus (click on it)

**Mouse**: Click directly on button circles

**Check logs**:
```
[00:00:05.123,000] <dbg> akira_sim_buttons: Button A pressed (keyboard)
```

## 🎯 Fallback Mode

If SDL2 is not available, the HAL automatically falls back to text-mode simulation:

```
[00:00:00.000,000] <inf> akira_hal: Using text-mode simulation (no SDL2)
```

In this mode:
- Display updates are logged
- Buttons cycle automatically (timer-based)
- No graphical window

## 🚧 Future Enhancements

- [ ] Add FPS counter
- [ ] Add screenshot capability
- [ ] Record display output to video
- [ ] Add audio simulation
- [ ] Add WiFi status indicator
- [ ] Add SD card slot visualization
- [ ] Add battery level indicator
- [ ] Export framebuffer to PNG

## 📝 Technical Details

### Display Rendering

1. Firmware writes to `sim_framebuffer` (RGB565)
2. `akira_sim_draw_pixel()` updates framebuffer
3. `akira_sim_show_display()` triggers update
4. Simulator converts RGB565 → RGB888
5. SDL2 texture is updated
6. Rendered at 60 FPS

### Button Input

1. SDL2 captures mouse/keyboard events
2. Events mapped to button IDs
3. Button state bitmask updated
4. HAL reads state via `akira_sim_read_buttons()`
5. Application sees identical interface as hardware

### Performance

- **60 FPS rendering** (16ms frame time)
- **Sub-millisecond button latency**
- **Zero-copy display updates** (direct framebuffer access)
- **Minimal CPU usage** (~5% on modern systems)

## ✅ Testing

The simulator is automatically tested when building:

```bash
# Build
west build -b native_sim . -d ../build_native_sim

# Run
cd ../build_native_sim/zephyr
./zephyr.exe

# Expected output:
# [00:00:00.000,000] <inf> akira_sim: ✅ Akira Console Simulator initialized
# Window opens showing Akira Console
# Display updates appear in window
# Buttons respond to mouse/keyboard
```

## 📚 API Reference

See `akira_sim.h` for complete API documentation.

### Key Functions

```c
/* Initialize simulator */
int akira_sim_init(void);

/* Update display from framebuffer */
void akira_sim_update_display(const uint16_t *framebuffer);

/* Get current button states */
uint32_t akira_sim_get_button_state(void);

/* Check if running */
bool akira_sim_is_running(void);

/* Cleanup */
void akira_sim_shutdown(void);
```

---

**Enjoy testing your Akira Console firmware on PC! 🎮✨**
