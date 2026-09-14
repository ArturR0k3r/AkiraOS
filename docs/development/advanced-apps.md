---
layout: default
title: Advanced Sample Apps
parent: Development
nav_order: 6
---

# Advanced Sample Applications

AkiraOS's `AkiraSDK/wasm_apps/` folder contains advanced sample applications functioning as reference architecture for deeply embedded mathematical interfaces, high-speed input logic, networking, and HID routing. These examples demonstrate best practices for WASM app development on resource-constrained devices.

## 1. Hello World (`hello_world/main.c`)

Minimal starter app demonstrating the `printf()` logging API. Shows basic WASM execution flow and output formatting.

```c
int main(void) {
    printf("=================================");
    printf("  Hello from AkiraOS WASM!      ");
    printf("=================================");
    printf("");
    printf("[INFO]  This is an info message");
    printf("[WARN]  This is a warning message");
    printf("[ERROR] This is an error message");
    return 0;
}
```

## 2. IMU Orientation Visualizer (`imu_3d/main.c`)

Retrieves accelerometer data, computes pitch/roll, and renders a 3D wireframe bounding box showing device orientation. 
**Key Takeaway**: Uses Q14 fixed-point math (no floating-point) for matrix transformations. Derives sine/cosine directly from normalized accelerometer ratios without lookup tables.

```c
// 1. Helper converts raw accelerometer value to Q14 normalized coordinate
static int to_q14(int raw, int mag) {
    return (mag != 0) ? (int)(((int64_t)raw << 14) / mag) : 0;
}

// 2. Normalize accelerometer vector and extract pitch sine/cosine
int mag = isqrt64(ax*ax + ay*ay + az*az);
int sp = to_q14(-ax, mag);  // sin(pitch) derived from gravity vector
int cp = to_q14(cross, mag); // cos(pitch) from cross product

// 3. Apply 3×3 rotation matrix with Q14 fixed-point multiply
int x_rot = (x * m00 + y * m01 + z * m02) >> 14;
int y_rot = (x * m10 + y * m11 + z * m12) >> 14;

// 4. Project to orthogonal display coordinates
int proj_x = center_x + ((x_rot * SCALE) >> 14);
int proj_y = center_y - ((y_rot * SCALE) >> 14);

// 5. Draw filled triangles with painter's algorithm (back-to-front)
display_triangle_fill(x0, y0, x1, y1, x2, y2, color);
```

## 3. Macro Pad Keyboard Configurator (`macro_pad/main.c`)

Showcases the power of abstracting Bluetooth LE HID into standardized `akira` bindings over 60Hz polling delays. Validates button edges utilizing hardware bitmasks and immediately translates them into Consumer/Media and standard Keycode streams without blocking.

```c
// Button logic polled continuously inside thread logic at 16.6ms intervals
int edge_mask = buttons_edge();
if (edge_mask) {
    if (edge_mask & (1 << BTN_UP)) {
        // Fire custom OS registry string into target device over BLE
        hid_type_string("akira_run_script\n");
    }
    if (edge_mask & (1 << BTN_LEFT)) {
        // Multimedia / Consumer interface control 
        hid_consumer_send(HID_CONSUMER_PLAY_PAUSE);
    }
    if (edge_mask & (1 << BTN_A)) {
        // Absolute HID state manipulation
        hid_key_press(HID_KEY_A);
        delay(50);
        hid_key_release_all();
    }
}
```

## 4. Logic Analyzer (`logic_analyzer/main.c`)

A fast GPIO polling engine executing array bit-packing methodologies to construct high-density logic waves on the display subsystem without causing out-of-bounds heap overflows. The interface maps state-tracking via ring buffers.

```c
// Pre-mapping array pointer write-indexes 
#define SAMPLES 272
static uint8_t smp[4][SAMPLES];

// Sample buffer injection with rollover boundary
smp[0][wr] = gpio_read(PIN_0);
smp[1][wr] = gpio_read(PIN_1);

// Redraw logic signals based on previous-state checks avoiding duplicate paint routines
if (prev_state != current_state) {
    display_rect(x, wave_y, 1, h, COLOR_YELLOW);
}
wr = (wr + 1) % SAMPLES; // Advance ring pointer logically
```

## 5. 3D Spinning Cube (`cube3d/main.c`)

Real-time 3D renderer with perspective projection, back-face culling, and diffuse shading. Gyroscope controls spin speed dynamically — flick the board to accelerate rotation.

```c
// 1. Interpolated sin/cos using 64-entry Q14 lookup table
static int32_t sin_a(int ang) {
    int idx  = ang >> 6;          // Integer LUT index [0..63]
    int frac = ang & 63;          // Fractional sub-step [0..63]
    int32_t s0 = s64[idx];
    int32_t s1 = s64[(idx + 1) & 63];
    return s0 + ((s1 - s0) * frac >> 6); // Linear interpolation
}

// 2. Read gyroscope channels and accumulate rotation angles
int32_t gx = sensor_read(SENSOR_CHAN_GYRO_X);
int32_t gy = sensor_read(SENSOR_CHAN_GYRO_Y);
int32_t gz = sensor_read(SENSOR_CHAN_GYRO_Z);
int dx = (gx != AKIRA_SENSOR_ERROR) ? gx / GYRO_SCALE : 0;
int dy = (gy != AKIRA_SENSOR_ERROR) ? gy / GYRO_SCALE : 0;
ang_x = (ang_x + AUTO_X + dx) & ANG_MASK;
ang_y = (ang_y + AUTO_Y + dy) & ANG_MASK;

// 3. Build 3×3 rotation matrix from euler angles
int sx = sin_a(ang_x), cx = cos_a(ang_x);
int sy = sin_a(ang_y), cy = cos_a(ang_y);
int m00 = (cy * cz) >> 14;
int m01 = (cy * sz) >> 14;
// ... (9 matrix elements)

// 4. Transform cube vertices, perform perspective divide
int xr = (x * m00 + y * m01 + z * m02) >> 14;
int yr = (x * m10 + y * m11 + z * m12) >> 14;
int zr = (x * m20 + y * m21 + z * m22) >> 14;
int xs = center_x + ((xr * FOCAL) / (zr + FOCAL));
int ys = center_y - ((yr * FOCAL) / (zr + FOCAL));

// 5. Back-face culling, depth sort, then fill with diffuse shading
if (dot_product > 0) continue; // Back face — skip
qsort(face_depths, 6, sizeof(face_t), depth_compare);
for (int i = 0; i < 6; i++) {
    int shade = calculate_diffuse(normal, light_dir);
    display_triangle_fill(v0, v1, v2, shade_color(PIECE_COLOR[i], shade));
}
```

## 6. Supervisor App Launcher (`supervisor/main.c`)

Foreground app launcher displaying scrollable list of installed apps with state badges (RUNNING/STOPPED/ERROR). Uses Lifecycle IPC to reactively update status.

```c
// 1. Query installed apps via lifecycle API (returns "name:STATE\n" format)
char list_buf[LIST_BUF];
int count = app_list((uint8_t *)list_buf, sizeof(list_buf));

// 2. Parse app list buffer into local arrays
char *p = list_buf;
for (int i = 0; i < count; i++) {
    char *col = strchr(p, ':');
    int name_len = col - p;
    strncpy(apps[i].name, p, name_len);
    apps[i].state = parse_state(col + 1); // Parse "RUNNING", "STOPPED", etc.
    p = strchr(col, '\n') + 1;
}

// 3. Handle button input for navigation and control
if (btn_up_edge())   sel = (sel > 0) ? sel - 1 : count - 1;
if (btn_down_edge()) sel = (sel + 1) % count;
if (btn_a_edge()) {
    app_switch(apps[sel].name); // Launch selected app
    return 0; // Supervisor exits cleanly
}
if (btn_b_edge()) {
    app_stop(apps[sel].name); // Stop selected app
}

// 4. Subscribe to lifecycle topic and poll for events (non-blocking)
msg_subscribe("akira.lifecycle");
akira_lifecycle_event_t ev;
while (msg_try_recv("akira.lifecycle", (uint8_t *)&ev, sizeof(ev)) > 0) {
    // Update app state badges reactively
    for (int i = 0; i < count; i++) {
        if (strcmp(apps[i].name, ev.name) == 0) {
            apps[i].state = ev.state;
            break;
        }
    }
}
```

## 7. TCP Echo Client (`net_echo/main.c`)

TCP networking example connecting to echo server, sending data, and receiving reply. Uses shared-memory ring buffers for zero-copy I/O.

```c
// 1. Open TCP socket and bind TX/RX ring buffers
int h = net_open(NET_TYPE_TCP);
net_tx_bind(h, tx_ring, sizeof(tx_ring));
net_rx_bind(h, rx_ring, sizeof(rx_ring));

// 2. Async connect to server (DNS + TCP handshake in background)
net_connect(h, "127.0.0.1", 7);

// 3. Poll for NET_EVT_CONNECTED event
uint8_t evt[4];
while (!connected) {
    if (net_event_pop(evt, sizeof(evt)) > 0) {
        if (evt[0] == NET_EVT_CONNECTED) connected = 1;
    }
    delay_ms(10);
}

// 4. Write to TX ring (zero-copy), then flush
net_ring_write(tx_ring, sizeof(tx_ring), (uint8_t *)"Hello AkiraOS!\n", 15);
net_tx_flush(h);

// 5. Poll for NET_EVT_DATA_READY, then read from RX ring
while (net_event_pop(evt, sizeof(evt)) > 0) {
    if (evt[0] == NET_EVT_DATA_READY && evt[1] == (uint8_t)h) {
        char buf[64];
        int n = net_ring_read(rx_ring, sizeof(rx_ring), (uint8_t *)buf, sizeof(buf));
        printf("Echo reply: %.*s", n, buf);
    }
}
```

## 8. Inclinometer (`inclinometer/main.c`)

Accelerometer-based tilt sensor displaying direction and angle on compass-style rose. No magnetometer required — pure gravity vector analysis.

```c
// 1. Read accelerometer gravity vector (separate channels)
int32_t ax = sensor_read(SENSOR_CHAN_ACCEL_X);
int32_t ay = sensor_read(SENSOR_CHAN_ACCEL_Y);
int32_t az = sensor_read(SENSOR_CHAN_ACCEL_Z);
if (ax == AKIRA_SENSOR_ERROR) { ax = 0; ay = 0; az = 9812; }

// 2. Calculate total magnitude and horizontal tilt component
int32_t mag = isqrt64((int64_t)ax*ax + (int64_t)ay*ay + (int64_t)az*az);
int32_t horiz = isqrt64((int64_t)ax*ax + (int64_t)ay*ay);
int32_t tilt_q14 = (int32_t)((int64_t)horiz * 16384 / mag); // sin(tilt) in Q14

// 3. Compute tilt direction with integer atan2 (0-3600 tenths of degrees)
int tilt_dir = atan2_dd((int)ax, (int)ay); // CCW from +Y axis

// 4. Convert tilt angle to needle length and draw compass
int tilt_deg = (int32_t)((int64_t)tilt_q14 * 90 >> 14); // 0-90 degrees
int needle_len = (tilt_q14 * RADIUS) >> 14; // Scale to display radius
int lut_idx = (tilt_dir * 64) / 3600; // Map to 64-entry LUT
int nx = center_x + ((needle_len * COS64(lut_idx)) >> 14);
int ny = center_y - ((needle_len * SIN64(lut_idx)) >> 14);
display_line(center_x, center_y, nx, ny, COLOR_RED);
```

## 9. Tetris (`tetris/main.c`)

Full-featured Tetris implementation with classic gameplay, scoring, and level progression. Demonstrates efficient game state management and incremental rendering.

```c
// 1. Piece collision detection using bit-packed 4×4 shapes
static int collides(int piece, int rot, int x, int y) {
    uint16_t shape = SHAPES[piece][rot];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (shape & (0x8000 >> (r * 4 + c))) {
                int bx = x + c, by = y + r;
                if (bx < 0 || bx >= BOARD_WIDTH || by >= BOARD_HEIGHT)
                    return 1;
                if (by >= 0 && g.board[by][bx]) return 1;
            }
        }
    }
    return 0;
}

// 2. Line clear detection and board compaction
int lines_cleared = 0;
for (int y = BOARD_HEIGHT - 1; y >= 0; y--) {
    int full = 1;
    for (int x = 0; x < BOARD_WIDTH; x++) {
        if (!g.board[y][x]) { full = 0; break; }
    }
    if (full) {
        lines_cleared++;
        // Shift all rows above down by one
        for (int yy = y; yy > 0; yy--) {
            for (int x = 0; x < BOARD_WIDTH; x++) {
                g.board[yy][x] = g.board[yy - 1][x];
            }
        }
        // Clear top row
        for (int x = 0; x < BOARD_WIDTH; x++) g.board[0][x] = 0;
        y++; // Re-check this row (shifted down)
    }
}

// 3. Score calculation: pts[1]=100, pts[2]=300, pts[3]=500, pts[4]=800
static const uint32_t pts[5] = {0, 100, 300, 500, 800};
g.score += pts[lines_cleared] * g.level;

// 4. Incremental rendering — only redraw changed cells
if (g.piece_moved || g.board_changed) {
    for (int y = 0; y < BOARD_HEIGHT; y++) {
        for (int x = 0; x < BOARD_WIDTH; x++) {
            if (g.board[y][x] != prev_board[y][x]) {
                draw_block(x, y, PIECE_COLOR[g.board[y][x] - 1]);
            }
        }
    }
    g.piece_moved = 0;
    g.board_changed = 0;
}
```

Additional sample apps available in `AkiraSDK/wasm_apps/`: `ble_led`, `compass`, `display_test`, `gpio`, `imu_timer_test`, `net_server`, `storage_test`. Refer to each app's `main.c` for implementation details and required capabilities.

---

*Last updated: 2026-09-14 (AkiraOS v1.6.4)*
