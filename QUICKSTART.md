# 🚀 Quick Start Guide - Akira Console Simulator

## First Time Setup & Run

### One Command (Builds Everything & Runs)

```bash
cd /home/artur_ubuntu/Akira/AkiraOS
chmod +x build_and_run.sh
./build_and_run.sh
```

This will:
1. ✅ Build SDL2 viewer (if needed)
2. ✅ Build AkiraOS for native_sim
3. ✅ Start simulator window
4. ✅ Run AkiraOS

---

## Step by Step (Manual)

### Step 1: Build AkiraOS for Native Sim

```bash
cd /home/artur_ubuntu/Akira/AkiraOS
west build --pristine -b native_sim . -d ../build_native_sim
```

**Output:** Creates `../build_native_sim/zephyr/zephyr.exe`

### Step 2: Build SDL2 Viewer (One Time)

```bash
cd tools
make
cd ..
```

**Output:** Creates `tools/akira_viewer`

### Step 3: Run Simulator

**Terminal 1 - SDL2 Viewer:**
```bash
cd /home/artur_ubuntu/Akira/AkiraOS/tools
./akira_viewer
```

**Terminal 2 - AkiraOS:**
```bash
cd /home/artur_ubuntu/Akira/build_native_sim/zephyr
./zephyr.exe
```

---

## Subsequent Runs (After First Build)

### Quick Run (If Already Built)

```bash
cd /home/artur_ubuntu/Akira/AkiraOS
./run_simulator.sh
```

### Rebuild After Code Changes

```bash
cd /home/artur_ubuntu/Akira/AkiraOS

# Rebuild AkiraOS
west build -b native_sim . -d ../build_native_sim

# Run (viewer stays open, just restart zephyr.exe)
cd ../build_native_sim/zephyr
./zephyr.exe
```

---

## Troubleshooting

### Error: "build_native_sim/zephyr: No such file or directory"

**Solution:** You need to build first!
```bash
cd /home/artur_ubuntu/Akira/AkiraOS
west build --pristine -b native_sim . -d ../build_native_sim
```

### Error: "akira_viewer: command not found"

**Solution:** Build the viewer!
```bash
cd /home/artur_ubuntu/Akira/AkiraOS/tools
make
```

### Simulator window doesn't open

**Check SDL2:**
```bash
pkg-config --exists sdl2 && echo "SDL2 OK" || echo "Install SDL2"
```

**Install if needed:**
```bash
sudo apt-get install libsdl2-dev
```

---

## Quick Command Reference

| Task | Command |
|------|---------|
| **Full build & run** | `./build_and_run.sh` |
| **Build AkiraOS** | `west build -b native_sim . -d ../build_native_sim` |
| **Clean build** | `west build --pristine -b native_sim . -d ../build_native_sim` |
| **Run viewer** | `tools/akira_viewer &` |
| **Run AkiraOS** | `../build_native_sim/zephyr/zephyr.exe` |
| **Quick run (if built)** | `./run_simulator.sh` |

---

## Controls in Simulator

| Key | Button |
|-----|--------|
| `W` `S` `A` `D` | D-Pad |
| `I` `K` `J` `L` | X, B, Y, A buttons |
| `ESC` | Power |
| `ENTER` | Settings |
| **Mouse** | Click buttons |

---

## Expected Output

When you run `./zephyr.exe`, you should see:

```
*** Booting Zephyr OS build v4.2.0 ***
=== AkiraOS main() started ===
[00:00:00.000,000] <inf> akira_hal: Akira HAL initializing for: native_sim
[00:00:00.000,000] <inf> akira_hal: Running in SIMULATION mode
[00:00:00.000,000] <inf> akira_hal: ✅ Framebuffer shared memory created
[00:00:00.000,000] <inf> akira_hal: ✅ Button shared memory created
[00:00:00.000,000] <inf> akira_hal: 📺 Ready for external SDL2 viewer
[00:00:02.610,000] <inf> ili9341: ILI9341 initialization completed
[00:00:02.610,000] <inf> akira_main: ✅ ILI9341 display initialized
```

And the simulator window shows the Akira Console with display and buttons!

---

## Directory Structure

```
/home/artur_ubuntu/Akira/
├── AkiraOS/                          ← Your code
│   ├── src/
│   ├── tools/
│   │   ├── akira_viewer              ← SDL2 executable
│   │   └── akira_simulator_viewer.c
│   ├── build_and_run.sh              ← Complete build & run
│   └── run_simulator.sh              ← Quick run (if built)
└── build_native_sim/                 ← Build output
    └── zephyr/
        └── zephyr.exe                ← AkiraOS executable
```

---

## Pro Tips

### Keep Viewer Open

The SDL2 viewer can stay open. Just restart `zephyr.exe` when you rebuild:

```bash
# Terminal 1: Leave viewer running
tools/akira_viewer

# Terminal 2: Edit, rebuild, rerun
vim src/main.c
west build -b native_sim . -d ../build_native_sim
cd ../build_native_sim/zephyr
./zephyr.exe
```

### Watch Mode

Use `inotifywait` for auto-rebuild:

```bash
while inotifywait -e modify src/**/*.c; do
    west build -b native_sim . -d ../build_native_sim
done
```

---

**Ready to start? Run:** `./build_and_run.sh`
