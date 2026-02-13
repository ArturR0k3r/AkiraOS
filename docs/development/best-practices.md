# ⭐ Akira SDK Best Practices

Write better, more efficient, and more maintainable code with these proven patterns and guidelines.

---

## 📑 Table of Contents

- [Event Loop Best Practices](#-event-loop-best-practices)
- [Memory Management](#-memory-management)
- [Display Optimization](#-display-optimization)
- [Callback Design](#-callback-design)
- [Error Handling](#-error-handling)
- [Power Efficiency](#-power-efficiency)
- [Code Organization](#-code-organization)
- [Security Considerations](#-security-considerations)
- [Testing & Debugging](#-testing--debugging)

---

## 🔄 Event Loop Best Practices

### ✅ DO: Keep Your Main Loop Simple

```c
int main(void) {
    // Initialization
    setup_display();
    setup_callbacks();

    // Simple, clean event loop
    while(1) {
        akira_process_events();
    }

    return 0;
}
```

### ❌ DON'T: Put Business Logic in Main Loop

```c
// BAD - cluttered main loop
int main(void) {
    while(1) {
        read_sensors();
        update_display();
        check_buttons();
        process_network();
        akira_process_events();
    }
}
```

**Why?** `akira_process_events()` already dispatches to your callbacks. Keep the main loop focused on event processing.

---

### ✅ DO: Use Callbacks for Asynchronous Events

```c
void on_timer() {
    update_sensor_display();
}

void on_button(uint32_t buttons) {
    handle_user_input(buttons);
}

int main(void) {
    akira_register_timer_callback(0, on_timer);
    akira_input_set_callback(on_button);

    while(1) {
        akira_process_events();
    }
}
```

---

### ❌ DON'T: Block in Callbacks

```c
// BAD - blocks event processing
void on_button(uint32_t buttons) {
    for(int i = 0; i < 1000; i++) {
        akira_system_sleep(10);  // Blocks for 10 seconds!
        update_animation();
    }
}
```

**Why?** Callbacks must return quickly. Use timers or state machines for long operations.

---

### ✅ DO: Use State Machines for Complex Logic

```c
typedef enum {
    STATE_IDLE,
    STATE_READING,
    STATE_PROCESSING,
    STATE_COMPLETE
} AppState;

static AppState state = STATE_IDLE;

void on_timer() {
    switch(state) {
        case STATE_IDLE:
            start_sensor_read();
            state = STATE_READING;
            break;

        case STATE_READING:
            if (sensor_ready()) {
                state = STATE_PROCESSING;
            }
            break;

        case STATE_PROCESSING:
            process_data();
            state = STATE_COMPLETE;
            break;

        case STATE_COMPLETE:
            display_results();
            state = STATE_IDLE;
            break;
    }
}
```

---

## 💾 Memory Management

### ✅ DO: Use Static Buffers for Fixed-Size Data

```c
static char display_buffer[256];
static uint8_t sensor_readings[MAX_SAMPLES];
```

**Benefits:**
- No heap fragmentation
- Predictable memory usage
- No allocation failures

---

### ❌ DON'T: Use Large Stack Buffers

```c
void process_data() {
    char huge_buffer[4096];  // BAD - may overflow stack
    // ...
}
```

**Why?** WASM has limited stack space. Use static allocation for large buffers.

---

### ✅ DO: Check Array Bounds

```c
static int values[MAX_VALUES];
static int count = 0;

void add_value(int val) {
    if (count < MAX_VALUES) {
        values[count++] = val;
    } else {
        akira_log(1, "Buffer full!");
    }
}
```

---

### ✅ DO: Free Resources When Done

```c
void cleanup() {
    akira_rf_deinit();
    akira_unregister_timer_callback(0);
    akira_unregister_gpio_callback(0, 5);
}
```

---

## 🎨 Display Optimization

### ✅ DO: Batch Display Updates

```c
// GOOD - single flush
void update_screen() {
    akira_display_clear(0x0000);
    akira_display_text(10, 10, "Line 1", 0xFFFF);
    akira_display_text(10, 30, "Line 2", 0xFFFF);
    akira_display_text(10, 50, "Line 3", 0xFFFF);
    akira_display_flush();  // Flush once
}
```

### ❌ DON'T: Flush After Every Draw

```c
// BAD - too many flushes
akira_display_text(10, 10, "Line 1", 0xFFFF);
akira_display_flush();
akira_display_text(10, 30, "Line 2", 0xFFFF);
akira_display_flush();
akira_display_text(10, 50, "Line 3", 0xFFFF);
akira_display_flush();
```

**Why?** Each flush is expensive. Batch all updates, then flush once.

---

### ✅ DO: Only Update Changed Regions

```c
static int last_value = -1;

void update_value(int new_value) {
    if (new_value != last_value) {
        akira_display_rect(100, 50, 50, 20, 0x0000);  // Clear old
        char text[16];
        snprintf(text, sizeof(text), "%d", new_value);
        akira_display_text(100, 50, text, 0xFFFF);
        akira_display_flush();
        last_value = new_value;
    }
}
```

---

### ✅ DO: Limit Frame Rate

```c
void animation_loop() {
    while(1) {
        update_animation();
        akira_system_sleep(16);  // ~60 FPS
        akira_process_events();
    }
}
```

**Why?** Higher frame rates waste power and CPU.

---

### 💡 Color Palette Tip

```c
#define COLOR_BG      0x0000
#define COLOR_TEXT     0xFFFF
#define COLOR_SUCCESS  0x07E0
#define COLOR_ERROR    0xF800
#define COLOR_WARNING  0xFFE0

void show_status(bool success) {
    uint16_t color = success ? COLOR_SUCCESS : COLOR_ERROR;
    akira_display_text(10, 10, "Status", color);
}
```

---

## 🎯 Callback Design

### ✅ DO: Keep Callbacks Short and Fast

```c
void on_button(uint32_t buttons) {
    if (buttons & AKIRA_BTN_A) {
        flag_action_needed = true;  // Set flag, process later
    }
}

void on_timer() {
    if (flag_action_needed) {
        perform_long_operation();
        flag_action_needed = false;
    }
}
```

---

### ✅ DO: Check for NULL Callbacks

```c
int my_register_callback(callback_func_t cb) {
    if (!cb) {
        akira_log(0, "NULL callback!");
        return -1;
    }
    // Register...
}
```

---

### ❌ DON'T: Call Callbacks Recursively

```c
// BAD - can cause stack overflow
void on_timer() {
    process_data();
    on_timer();  // Don't do this!
}
```

---

### ✅ DO: Handle All Event Cases

```c
void on_button(uint32_t buttons) {
    if (buttons & AKIRA_BTN_A) {
        handle_a();
    } else if (buttons & AKIRA_BTN_B) {
        handle_b();
    } else if (buttons == 0) {
        handle_release();
    } else {
        akira_log(3, "Unknown button combo");
    }
}
```

---

## ⚠️ Error Handling

### ✅ DO: Check Return Values

```c
if (akira_sensor_read(SENSOR_TYPE_TEMP, &temp) != 0) {
    akira_log(0, "Sensor read failed!");
    return;
}
process_temperature(temp);
```

### ❌ DON'T: Ignore Errors

```c
// BAD - ignores potential failure
akira_sensor_read(SENSOR_TYPE_TEMP, &temp);
process_temperature(temp);  // temp might be uninitialized!
```

---

### ✅ DO: Provide User Feedback

```c
void show_error(const char *message) {
    akira_display_clear(0x0000);
    akira_display_text(10, 10, "Error!", 0xF800);
    akira_display_text(10, 30, message, 0xFFFF);
    akira_display_flush();
    akira_log(0, message);
}
```

---

### ✅ DO: Implement Retry Logic

```c
int retry_sensor_read(float *value, int max_retries) {
    for (int i = 0; i < max_retries; i++) {
        if (akira_sensor_read(SENSOR_TYPE_TEMP, value) == 0) {
            return 0;
        }
        akira_system_sleep(100);
    }
    return -1;
}
```

---

### ✅ DO: Use Defensive Programming

```c
void process_message(const char *topic, const void *payload, uint32_t len) {
    if (!topic || !payload || len == 0) {
        akira_log(1, "Invalid message parameters");
        return;
    }

    if (len > MAX_PAYLOAD_SIZE) {
        akira_log(1, "Payload too large");
        return;
    }

    handle_message(topic, payload, len);
}
```

---

## 🔋 Power Efficiency

### ✅ DO: Sleep When Idle

```c
void main_loop() {
    while(1) {
        if (has_work()) {
            do_work();
        } else {
            akira_system_sleep(100);  // Sleep when idle
        }
        akira_process_events();
    }
}
```

---

### ✅ DO: Reduce Sensor Polling

```c
// GOOD - read every 5 seconds
#define SENSOR_INTERVAL_MS 5000

void on_timer() {
    read_sensor();
}

// BAD - constant polling
void main_loop() {
    while(1) {
        read_sensor();  // Too frequent!
        akira_system_sleep(10);
    }
}
```

---

### ✅ DO: Disable Unused Features

```c
void cleanup_unused_features() {
    akira_rf_deinit();  // Power down RF if not needed
    akira_unregister_timer_callback(unused_timer_id);
}
```

---

### ✅ DO: Optimize Display Updates

```c
static float last_temp = -999.0;

void update_display(float temp) {
    if (fabs(temp - last_temp) > 0.1) {  // Threshold
        // Display update logic
        last_temp = temp;
    }
}
```

---

## 📁 Code Organization

### ✅ DO: Use Meaningful Names

```c
// GOOD
void update_temperature_display(float celsius);
void handle_button_press(uint32_t button_mask);

// BAD
void upd(float t);
void hndl(uint32_t b);
```

---

### ✅ DO: Group Related Functions

```c
// sensor.c
void sensor_init(void);
float sensor_read_temp(void);
float sensor_read_humidity(void);

// display.c
void display_init(void);
void display_update(void);
void display_clear(void);

// main.c
int main(void) {
    sensor_init();
    display_init();
    // ...
}
```

---

### ✅ DO: Use Constants Instead of Magic Numbers

```c
// GOOD
#define TEMP_THRESHOLD_LOW   18.0
#define TEMP_THRESHOLD_HIGH  25.0
#define UPDATE_INTERVAL_MS   1000

if (temp < TEMP_THRESHOLD_LOW) {
    set_color(COLOR_BLUE);
}

// BAD
if (temp < 18.0) {
    set_color(0x001F);
}
```

---

### ✅ DO: Comment Complex Logic

```c
void calculate_moving_average() {
    // Use circular buffer to maintain last N samples
    // When buffer is full, overwrite oldest sample
    int index = sample_count % BUFFER_SIZE;
    buffer[index] = new_sample;
    sample_count++;

    // Calculate average of all valid samples
    float sum = 0;
    int count = (sample_count < BUFFER_SIZE) ? sample_count : BUFFER_SIZE;
    for (int i = 0; i < count; i++) {
        sum += buffer[i];
    }
    return sum / count;
}
```

---

## 🔒 Security Considerations

### ✅ DO: Validate Input Sizes

```c
void handle_message(const char *topic, const void *payload, uint32_t len) {
    if (len > MAX_SAFE_SIZE) {
        akira_log(0, "Payload too large - rejected");
        return;
    }
    process_payload(payload, len);
}
```

---

### ✅ DO: Use Bounds-Checked Functions

```c
// GOOD
strncpy(dest, src, sizeof(dest) - 1);
dest[sizeof(dest) - 1] = '\0';

// BAD
strcpy(dest, src);  // No bounds checking!
```

---

### ✅ DO: Validate Pointers

```c
void process_data(const char *data) {
    if (!data) {
        akira_log(0, "NULL pointer!");
        return;
    }
    do_something(data);
}
```

---

### ✅ DO: Sanitize User Input

```c
void handle_rf_packet(uint8_t *data, size_t len) {
    if (len < sizeof(PacketHeader)) {
        akira_log(1, "Invalid packet size");
        return;
    }

    PacketHeader *hdr = (PacketHeader*)data;

    if (!verify_checksum(hdr)) {
        akira_log(1, "Checksum failed");
        return;
    }

    process_valid_packet(hdr);
}
```

---

## 🧪 Testing & Debugging

### ✅ DO: Use Log Levels Appropriately

```c
akira_log(0, "Critical error occurred!");      // Error
akira_log(1, "Warning: retrying operation");   // Warning
akira_log(2, "Sensor read successful");        // Info
akira_log(3, "Debug: value = 42");             // Debug
```

---

### ✅ DO: Add Debug Visualizations

```c
#ifdef DEBUG
void show_debug_info() {
    akira_display_text(200, 5, "DBG", 0xF800);

    char dbg[32];
    snprintf(dbg, sizeof(dbg), "M:%zu", akira_system_free_memory());
    akira_display_text(180, 110, dbg, 0x7BEF);
}
#endif
```

---

### ✅ DO: Test Error Paths

```c
void test_sensor_failure() {
    float dummy;
    int result = akira_sensor_read(SENSOR_TYPE_INVALID, &dummy);
    if (result != 0) {
        akira_log(2, "Error handling works!");
    }
}
```

---

### ✅ DO: Monitor Resource Usage

```c
void log_resources() {
    size_t free_mem = akira_system_free_memory();
    uint64_t uptime = akira_system_uptime_ms();

    char msg[128];
    snprintf(msg, sizeof(msg),
             "Mem: %zu bytes | Uptime: %llu ms",
             free_mem, uptime);
    akira_log(3, msg);
}
```

---

## 🎯 Quick Reference Checklist

Before deploying your app, verify:

- [ ] Main loop calls `akira_process_events()`
- [ ] Callbacks return quickly
- [ ] All return values checked
- [ ] No large stack allocations
- [ ] Display updates batched
- [ ] Errors logged and handled
- [ ] Resource cleanup on exit
- [ ] Input validation present
- [ ] Power-saving measures used
- [ ] Code is commented
- [ ] Constants used instead of magic numbers
- [ ] Memory leaks checked

---

## Related Documentation

- [SDK API Reference](sdk-api-reference.md) - Complete API documentation
- [SDK Troubleshooting](sdk-troubleshooting.md) - Common issues
- [Building Apps](building-apps.md) - WASM compilation workflow
- [First App Tutorial](../getting-started/first-app.md) - Hello World

---

[⬆ Back to Top](#-akira-sdk-best-practices)
