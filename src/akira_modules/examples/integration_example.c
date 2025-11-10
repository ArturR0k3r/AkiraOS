/*
 * SPDX-License-Identifier: Apache-2.0
 * 
 * Akira Module Integration Example
 * 
 * This example shows how to integrate Akira into your project
 * and control it from external devices.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <akira_module.h>

LOG_MODULE_REGISTER(akira_integration_example, LOG_LEVEL_INF);

/* Example: Custom sensor module */
struct sensor_data {
	float temperature;
	float humidity;
	uint32_t timestamp;
};

static struct sensor_data current_sensor_data;

static int sensor_module_init(void *user_data)
{
	LOG_INF("Sensor module initialized");
	current_sensor_data.temperature = 25.0f;
	current_sensor_data.humidity = 50.0f;
	return 0;
}

static int sensor_module_command(const char *command, void *data,
                                  size_t len, void *user_data)
{
	if (strcmp(command, "read") == 0) {
		if (data && len >= sizeof(struct sensor_data)) {
			memcpy(data, &current_sensor_data, sizeof(struct sensor_data));
			LOG_INF("Sensor data read: temp=%.1f, humidity=%.1f",
			        current_sensor_data.temperature,
			        current_sensor_data.humidity);
			return 0;
		}
		return -EINVAL;
	}
	else if (strcmp(command, "update") == 0) {
		/* Simulate sensor update */
		current_sensor_data.temperature += (float)(sys_rand32_get() % 10 - 5) / 10.0f;
		current_sensor_data.humidity += (float)(sys_rand32_get() % 10 - 5) / 10.0f;
		current_sensor_data.timestamp = k_uptime_get_32();
		
		/* Broadcast sensor update event */
		akira_module_broadcast_event("sensor_updated", &current_sensor_data,
		                              sizeof(current_sensor_data));
		return 0;
	}
	
	return -ENOTSUP;
}

/* Register sensor module */
AKIRA_MODULE_DEFINE(sensor,
                    AKIRA_MODULE_TYPE_SENSOR,
                    sensor_module_init,
                    NULL,
                    sensor_module_command,
                    NULL,
                    NULL);

/* Example: Display module that shows sensor data */
static int display_sensor_event(const char *event, void *data,
                                 size_t len, void *user_data)
{
	if (strcmp(event, "sensor_updated") == 0 && len >= sizeof(struct sensor_data)) {
		struct sensor_data *sensor = (struct sensor_data *)data;
		
		/* Update display with sensor data */
		char temp_str[32];
		snprintf(temp_str, sizeof(temp_str), "Temp: %.1fC", sensor->temperature);
		
		char hum_str[32];
		snprintf(hum_str, sizeof(hum_str), "Humidity: %.1f%%", sensor->humidity);
		
		/* Send commands to display module */
		akira_module_send_command("display", "text", temp_str, strlen(temp_str) + 1);
		
		LOG_INF("Display updated with sensor data");
		return 0;
	}
	return 0;
}

/* Custom display controller module */
AKIRA_MODULE_DEFINE(display_controller,
                    AKIRA_MODULE_TYPE_CUSTOM,
                    NULL,
                    NULL,
                    NULL,
                    display_sensor_event,
                    NULL);

/* Main application thread */
static void integration_thread(void *p1, void *p2, void *p3)
{
	LOG_INF("=== Akira Module Integration Example ===");
	
	/* Initialize module system */
	akira_module_init();
	
	/* Start communication */
	akira_module_start_comm();
	
	/* Wait for modules to be ready */
	k_sleep(K_MSEC(500));
	
	LOG_INF("Modules registered:");
	LOG_INF("  - sensor: Read temperature and humidity");
	LOG_INF("  - display: Show information on screen");
	LOG_INF("  - display_controller: Update display with sensor data");
	LOG_INF("  - buttons: Read button inputs");
	LOG_INF("  - gpio: Control GPIO pins");
	
	/* Broadcast system ready event */
	akira_module_broadcast_event("system_ready", NULL, 0);
	
	/* Periodic sensor updates */
	while (1) {
		/* Trigger sensor update */
		akira_module_send_command("sensor", "update", NULL, 0);
		
		/* Read button state */
		uint32_t buttons = 0;
		akira_module_send_command("buttons", "read", &buttons, sizeof(buttons));
		
		if (buttons != 0) {
			LOG_INF("Button pressed: 0x%08x", buttons);
			
			/* Broadcast button event */
			akira_module_broadcast_event("button_pressed", &buttons, sizeof(buttons));
		}
		
		k_sleep(K_SECONDS(2));
	}
}

K_THREAD_DEFINE(integration_tid, 4096,
                integration_thread, NULL, NULL, NULL,
                7, 0, 0);

/* 
 * Integration Examples:
 * 
 * 1. Arduino Integration (UART):
 *    - Connect Akira UART to Arduino Serial
 *    - Send JSON commands: {"module":"display","command":"text","data":"Hello"}
 *    - Receive sensor data: {"type":"event","event":"sensor_updated","data":{...}}
 * 
 * 2. Raspberry Pi Integration (Network):
 *    - Connect to Akira WiFi network
 *    - HTTP POST to http://akira.local/api/command
 *    - WebSocket connection for real-time events
 * 
 * 3. ESP32 Integration (SPI):
 *    - Connect SPI between ESP32 and Akira
 *    - High-speed data transfer for display updates
 *    - Low latency for gaming applications
 * 
 * 4. Custom Hardware Integration (I2C):
 *    - Use Akira as I2C master
 *    - Control multiple I2C slave devices
 *    - Display I2C sensor data on screen
 */
