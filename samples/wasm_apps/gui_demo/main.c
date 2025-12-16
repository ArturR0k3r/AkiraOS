/**
 * @file main.c
 * @brief LVGL GUI Demo - Interactive GUI with buttons and slider
 * 
 * Demonstrates the LVGL graphics layer integration with:
 * - Modern UI widgets
 * - Event handling
 * - Smooth animations
 * - Real-time updates
 */

#include "akira_api.h"
#include "akira_gui_api.h"

/* Color definitions */
#define COLOR_PRIMARY   0x2196F3  // Blue
#define COLOR_SUCCESS   0x4CAF50  // Green
#define COLOR_DANGER    0xF44336  // Red
#define COLOR_DARK      0x212121  // Dark gray
#define COLOR_LIGHT     0xF5F5F5  // Light gray

/* Application state */
static struct {
    gui_obj_t screen;
    gui_obj_t title_label;
    gui_obj_t counter_label;
    gui_obj_t status_label;
    gui_obj_t btn_increment;
    gui_obj_t btn_decrement;
    gui_obj_t btn_reset;
    gui_obj_t slider;
    gui_obj_t slider_label;
    
    int32_t counter;
    int32_t slider_value;
} app_state = {0};

/**
 * @brief Increment button clicked
 */
void on_increment_clicked(gui_obj_t obj, gui_event_type_t event)
{
    if (event != GUI_EVENT_CLICKED) return;
    
    app_state.counter++;
    gui_label_set_text_fmt(app_state.counter_label, "Count: %d", app_state.counter);
    gui_label_set_text(app_state.status_label, "Incremented!");
    
    akira_log("Counter incremented: %d", app_state.counter);
}

/**
 * @brief Decrement button clicked
 */
void on_decrement_clicked(gui_obj_t obj, gui_event_type_t event)
{
    if (event != GUI_EVENT_CLICKED) return;
    
    app_state.counter--;
    gui_label_set_text_fmt(app_state.counter_label, "Count: %d", app_state.counter);
    gui_label_set_text(app_state.status_label, "Decremented!");
    
    akira_log("Counter decremented: %d", app_state.counter);
}

/**
 * @brief Reset button clicked
 */
void on_reset_clicked(gui_obj_t obj, gui_event_type_t event)
{
    if (event != GUI_EVENT_CLICKED) return;
    
    app_state.counter = 0;
    gui_label_set_text_fmt(app_state.counter_label, "Count: %d", app_state.counter);
    gui_label_set_text(app_state.status_label, "Reset to zero!");
    
    /* Animate counter label */
    gui_obj_fade_out(app_state.counter_label, 200);
    akira_system_sleep_ms(200);
    gui_obj_fade_in(app_state.counter_label, 200);
    
    akira_log("Counter reset");
}

/**
 * @brief Slider value changed
 */
void on_slider_changed(gui_obj_t obj, gui_event_type_t event)
{
    if (event != GUI_EVENT_VALUE_CHANGED) return;
    
    app_state.slider_value = gui_slider_get_value(app_state.slider);
    gui_label_set_text_fmt(app_state.slider_label, "Volume: %d%%", app_state.slider_value);
    
    akira_log("Slider changed: %d", app_state.slider_value);
}

/**
 * @brief Create UI layout
 */
void create_ui(void)
{
    /* Create screen */
    app_state.screen = gui_screen_create();
    gui_screen_load(app_state.screen);
    
    /* Title */
    app_state.title_label = gui_label_create(app_state.screen);
    gui_label_set_text(app_state.title_label, "LVGL GUI Demo");
    gui_obj_set_pos(app_state.title_label, 80, 10);
    gui_obj_set_style_text_color(app_state.title_label, COLOR_PRIMARY);
    
    /* Counter display */
    app_state.counter_label = gui_label_create(app_state.screen);
    gui_label_set_text(app_state.counter_label, "Count: 0");
    gui_obj_set_pos(app_state.counter_label, 120, 50);
    
    /* Increment button */
    app_state.btn_increment = gui_button_create(app_state.screen);
    gui_obj_set_size(app_state.btn_increment, 80, 40);
    gui_obj_set_pos(app_state.btn_increment, 30, 90);
    gui_button_set_label(app_state.btn_increment, "+ Inc");
    gui_button_add_event_cb(app_state.btn_increment, on_increment_clicked);
    gui_obj_set_style_bg_color(app_state.btn_increment, COLOR_SUCCESS);
    
    /* Decrement button */
    app_state.btn_decrement = gui_button_create(app_state.screen);
    gui_obj_set_size(app_state.btn_decrement, 80, 40);
    gui_obj_set_pos(app_state.btn_decrement, 120, 90);
    gui_button_set_label(app_state.btn_decrement, "- Dec");
    gui_button_add_event_cb(app_state.btn_decrement, on_decrement_clicked);
    gui_obj_set_style_bg_color(app_state.btn_decrement, COLOR_DANGER);
    
    /* Reset button */
    app_state.btn_reset = gui_button_create(app_state.screen);
    gui_obj_set_size(app_state.btn_reset, 80, 40);
    gui_obj_set_pos(app_state.btn_reset, 210, 90);
    gui_button_set_label(app_state.btn_reset, "Reset");
    gui_button_add_event_cb(app_state.btn_reset, on_reset_clicked);
    gui_obj_set_style_bg_color(app_state.btn_reset, COLOR_DARK);
    
    /* Slider label */
    app_state.slider_label = gui_label_create(app_state.screen);
    gui_label_set_text(app_state.slider_label, "Volume: 50%");
    gui_obj_set_pos(app_state.slider_label, 110, 150);
    
    /* Slider */
    app_state.slider = gui_slider_create(app_state.screen);
    gui_obj_set_size(app_state.slider, 200, 20);
    gui_obj_set_pos(app_state.slider, 60, 175);
    gui_slider_set_range(app_state.slider, 0, 100);
    gui_slider_set_value(app_state.slider, 50, false);
    gui_slider_add_event_cb(app_state.slider, on_slider_changed);
    
    /* Status label */
    app_state.status_label = gui_label_create(app_state.screen);
    gui_label_set_text(app_state.status_label, "Ready");
    gui_obj_set_pos(app_state.status_label, 120, 215);
    gui_obj_set_style_text_color(app_state.status_label, 0x666666);
    
    akira_log("UI created successfully");
}

/**
 * @brief Main entry point
 */
void _start()
{
    akira_log("=================================");
    akira_log("   LVGL GUI Demo Starting");
    akira_log("=================================");
    
    /* Initialize state */
    app_state.counter = 0;
    app_state.slider_value = 50;
    
    /* Create UI */
    create_ui();
    
    akira_log("Entering main loop...");
    
    /* Main loop */
    uint32_t frame = 0;
    while (1) {
        /* Process LVGL tasks */
        gui_task_handler();
        
        /* Update status every 5 seconds */
        if ((frame % 500) == 0) {
            uint64_t uptime = akira_system_uptime_ms() / 1000;
            gui_label_set_text_fmt(app_state.status_label, 
                                   "Uptime: %llu s", uptime);
        }
        
        /* Frame rate control (100 FPS for smooth UI) */
        akira_system_sleep_ms(10);
        frame++;
    }
}
