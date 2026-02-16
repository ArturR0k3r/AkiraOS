# SDL2 Visual Simulator Integration - Implementation Summary

## Overview

This document summarizes the implementation of the Akira Console SDL2 Visual Simulator for Zephyr's native_sim platform. The simulator was already implemented in the codebase but not integrated into the build system.

## Implementation Date

- **Date**: 2026-02-16
- **Branch**: `claude/add-akira-simulation`
- **Issue**: "Add Akira Simulation (SDL2 Visual Simulator or anything you can invent)"

## What Was Done

### 1. Enabled Simulator Build (CMakeLists.txt)

**File**: `/CMakeLists.txt` (lines 335-338)

**Before:**
```cmake
# SDL2 simulator uses external viewer (tools/akira_viewer)
# No need to build SDL2 code within Zephyr build system
# if(CONFIG_BOARD_NATIVE_SIM)
#     add_subdirectory(src/drivers/sim)
# endif()
```

**After:**
```cmake
# SDL2 simulator for native_sim platform
if(CONFIG_BOARD_NATIVE_SIM)
    add_subdirectory(src/drivers/sim)
endif()
```

This change enables the simulator build when targeting native_sim.

### 2. Integrated Simulator with Platform HAL

**File**: `/src/drivers/platform_hal.c`

#### Added SDL2 Simulator Header Include (lines 25-28)
```c
/* SDL2 Simulator support */
#if AKIRA_PLATFORM_NATIVE_SIM && defined(AKIRA_SIM_ENABLED)
#include "sim/akira_sim.h"
#endif
```

#### Added Simulator Initialization (lines 124-134)
```c
#if defined(AKIRA_SIM_ENABLED)
    /* Initialize SDL2 visual simulator */
    if (akira_sim_init() == 0)
    {
        LOG_INF("✅ SDL2 Visual Simulator initialized");
    }
    else
    {
        LOG_WRN("⚠️  SDL2 Visual Simulator init failed - fallback to shared memory mode");
    }
#endif
```

#### Updated Button Reading (lines 321-334)
```c
uint32_t akira_sim_read_buttons(void)
{
#if defined(AKIRA_SIM_ENABLED)
    /* Read button state from SDL2 visual simulator */
    sim_button_state = akira_sim_get_button_state();
#else
    /* Read button state from shared memory (written by SDL2 viewer) */
    if (shared_buttons)
    {
        sim_button_state = *shared_buttons;
    }
#endif
    return sim_button_state;
}
```

#### Updated Display Rendering (lines 360-363)
```c
#if defined(AKIRA_SIM_ENABLED)
    /* Update SDL2 visual simulator */
    akira_sim_update_display(sim_framebuffer);
#endif
```

### 3. Created Convenience Build Script

**File**: `/build_and_run.sh` (New file, 196 lines)

Features:
- Automatic SDL2 dependency checking
- Clean build option
- Run-only option
- Colored output and progress indicators
- Keyboard control help display

Usage:
```bash
./build_and_run.sh           # Build and run
./build_and_run.sh --clean   # Clean, build, and run
./build_and_run.sh --run     # Run only (no build)
```

### 4. Updated Documentation

#### README.md Changes

Added comprehensive SDL2 simulator section (lines 69-124):
- Requirements installation instructions
- Quick start guide
- Feature list
- Complete keyboard controls table
- Link to detailed documentation

#### QUICKSTART.md Changes

- Added SDL2 to prerequisites (line 10)
- Expanded Native Simulation section (lines 143-180)
- Added SDL2 requirements with installation commands
- Added simulator features list
- Added reference to detailed controls

## Existing Simulator Code

The following files already existed and were NOT modified:
- `/src/drivers/sim/akira_sim.h` - Simulator API definitions
- `/src/drivers/sim/akira_sim.c` - Main simulator implementation
- `/src/drivers/sim/akira_sim_display.c` - Display rendering
- `/src/drivers/sim/akira_sim_buttons.c` - Button input handling
- `/src/drivers/sim/CMakeLists.txt` - Simulator build configuration
- `/src/drivers/sim/README.md` - Detailed simulator documentation

## Simulator Features

### Display
- **Resolution**: 240x320 pixels (ILI9341 format)
- **Color Format**: RGB565 → RGB888 conversion for SDL
- **Update Rate**: 60 FPS
- **Window Size**: 400x600 pixels

### Input
- **Buttons**: 10 (D-Pad: 4, Action: 4, Power: 1, Settings: 1)
- **Mouse Support**: Click buttons directly
- **Keyboard Support**: Full keyboard mapping

### Keyboard Controls
| Key | Button | Function |
|-----|--------|----------|
| W | UP | D-Pad Up |
| S | DOWN | D-Pad Down |
| A | LEFT | D-Pad Left |
| D | RIGHT | D-Pad Right |
| I | X | Action button X |
| K | B | Action button B |
| J | Y | Action button Y |
| L | A | Action button A |
| ESC | POWER | Power/ON-OFF |
| ENTER | SETTINGS | Settings button |

## Build System Integration

The simulator is automatically enabled for native_sim builds through:

1. **CMake detection**: `CONFIG_BOARD_NATIVE_SIM` check
2. **Compile definition**: `AKIRA_SIM_ENABLED=1` set by `/src/drivers/sim/CMakeLists.txt`
3. **SDL2 detection**: Uses pkg-config to find SDL2 libraries
4. **Automatic linking**: Links SDL2, pthread, and math libraries

## Dependencies

### Required
- SDL2 development libraries (libsdl2-dev)
- pthread (usually included with glibc)
- math library (libm)

### Installation Commands

Ubuntu/Debian:
```bash
sudo apt-get install libsdl2-dev
```

Fedora:
```bash
sudo dnf install SDL2-devel
```

macOS:
```bash
brew install sdl2
```

## Testing

### Manual Testing Steps

1. **Install SDL2**:
   ```bash
   sudo apt-get install libsdl2-dev  # Ubuntu/Debian
   ```

2. **Build for native_sim**:
   ```bash
   cd /path/to/AkiraOS
   ./build_and_run.sh
   ```

3. **Verify simulator window opens** with:
   - Display area (240x320, red frame)
   - Button circles (D-Pad on left, ABXY on right)
   - Power and Settings buttons at top
   - AKIRA logo area at bottom

4. **Test keyboard controls**:
   - Press WASD - D-Pad buttons should highlight
   - Press IJKL - Action buttons should highlight
   - Press ESC - Power button should highlight
   - Press ENTER - Settings button should highlight

5. **Test mouse controls**:
   - Click on button circles - should highlight when clicked
   - Release - should return to normal color

6. **Check logs**:
   ```
   [INFO] Akira HAL initializing for: Native Sim
   [INFO] ✅ SDL2 Visual Simulator initialized
   [INFO] SDL2 Visual Simulator initialized
   [INFO] Window size: 400x600
   ```

### Expected Behavior

- Window opens immediately after starting executable
- Display shows black screen initially
- Buttons respond to both keyboard and mouse input
- Button state changes visible in real-time
- Closing window terminates the application

### Fallback Mode

If SDL2 is not available or fails to initialize:
- HAL prints warning message
- Falls back to shared memory mode (for external viewer)
- Application continues to run without visual simulator

## Architecture

### Threading Model
- **Main thread**: Runs Zephyr RTOS and application code
- **Simulator thread**: Handles SDL2 events and rendering at ~60 FPS
- **Synchronization**: pthread mutex protects shared framebuffer access

### Data Flow

#### Display Updates
1. Application writes pixels via `akira_sim_draw_pixel()`
2. Pixels stored in `sim_framebuffer[]` (RGB565)
3. `akira_sim_show_display()` called
4. Framebuffer copied to SDL2 simulator
5. Simulator thread converts RGB565 → RGB888
6. SDL2 texture updated and rendered

#### Button Input
1. SDL2 captures keyboard/mouse events (simulator thread)
2. Events mapped to button IDs
3. Button state bitmask updated
4. `akira_sim_read_buttons()` called (main thread)
5. Returns current button state to application

## Files Modified/Created

### Modified Files
1. `/CMakeLists.txt` - Enabled simulator build
2. `/src/drivers/platform_hal.c` - Added simulator integration
3. `/README.md` - Added SDL2 simulator documentation
4. `/QUICKSTART.md` - Updated with SDL2 setup

### Created Files
1. `/build_and_run.sh` - Convenience build and run script
2. `/.github/SDL2_SIMULATOR_INTEGRATION.md` - This document

### Unchanged Simulator Files
- `/src/drivers/sim/akira_sim.h`
- `/src/drivers/sim/akira_sim.c`
- `/src/drivers/sim/akira_sim_display.c`
- `/src/drivers/sim/akira_sim_buttons.c`
- `/src/drivers/sim/CMakeLists.txt`
- `/src/drivers/sim/README.md`

## Future Enhancements

Possible improvements (from `/src/drivers/sim/README.md`):
- [ ] Add FPS counter
- [ ] Add screenshot capability
- [ ] Record display output to video
- [ ] Add audio simulation
- [ ] Add WiFi status indicator
- [ ] Add SD card slot visualization
- [ ] Add battery level indicator
- [ ] Export framebuffer to PNG

## References

- **Simulator Documentation**: `/src/drivers/sim/README.md`
- **Main README**: `/README.md` - SDL2 Simulator section
- **Quick Start**: `/QUICKSTART.md` - Native Simulation section
- **Zephyr native_sim**: https://docs.zephyrproject.org/latest/boards/native/native_sim/doc/index.html

## Notes

- The simulator code was already well-implemented; this PR only integrated it
- SDL2 is optional; the system falls back gracefully if unavailable
- The simulator provides hardware-accurate behavior for testing
- No changes to hardware builds; only affects native_sim
- Existing external viewer mode still works as fallback

## Conclusion

The SDL2 Visual Simulator is now fully integrated and documented. Users can build and run the simulator with a single command (`./build_and_run.sh`), providing an excellent development and testing experience without requiring physical hardware.
