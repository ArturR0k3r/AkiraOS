/*
 * SPDX-License-Identifier: Apache-2.0
 * 
 * Akira Module Template
 * 
 * Copy this file and customize it to create your own module!
 * Replace "template" with your module name throughout.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <akira_module.h>

/* Configure logging for your module */
LOG_MODULE_REGISTER(my_custom_module, LOG_LEVEL_INF);

/* 
 * Your module's private data structure
 * Store any state or configuration here
 */
struct my_module_data {
	bool initialized;
	uint32_t counter;
	/* Add your fields here */
};

static struct my_module_data module_data = {
	.initialized = false,
	.counter = 0,
};

/*
 * Module initialization callback
 * Called once when module is registered
 * 
 * @param user_data Pointer to your module data
 * @return 0 on success, negative errno on failure
 */
static int my_module_init(void *user_data)
{
	struct my_module_data *data = (struct my_module_data *)user_data;
	
	LOG_INF("Initializing my custom module");
	
	/* TODO: Add your initialization code here */
	/* Example: Initialize hardware, allocate resources, etc. */
	
	data->initialized = true;
	data->counter = 0;
	
	LOG_INF("My custom module initialized successfully");
	return 0;
}

/*
 * Module deinitialization callback
 * Called when module is unregistered
 * 
 * @param user_data Pointer to your module data
 * @return 0 on success, negative errno on failure
 */
static int my_module_deinit(void *user_data)
{
	struct my_module_data *data = (struct my_module_data *)user_data;
	
	LOG_INF("Deinitializing my custom module");
	
	/* TODO: Add your cleanup code here */
	/* Example: Release resources, stop timers, etc. */
	
	data->initialized = false;
	
	LOG_INF("My custom module deinitialized");
	return 0;
}

/*
 * Command handler callback
 * Process commands sent to your module
 * 
 * @param command Command string
 * @param data Command data buffer
 * @param len Data buffer length
 * @param user_data Pointer to your module data
 * @return 0 on success, negative errno on failure
 */
static int my_module_command(const char *command, void *data,
                              size_t len, void *user_data)
{
	struct my_module_data *mod_data = (struct my_module_data *)user_data;
	
	if (!mod_data->initialized) {
		LOG_ERR("Module not initialized");
		return -EAGAIN;
	}
	
	/* Handle your commands here */
	
	if (strcmp(command, "hello") == 0) {
		LOG_INF("Hello command received!");
		/* TODO: Implement hello command */
		return 0;
	}
	else if (strcmp(command, "get_counter") == 0) {
		if (data && len >= sizeof(uint32_t)) {
			*(uint32_t *)data = mod_data->counter;
			LOG_INF("Counter value: %u", mod_data->counter);
			return 0;
		}
		return -EINVAL;
	}
	else if (strcmp(command, "increment") == 0) {
		mod_data->counter++;
		LOG_INF("Counter incremented to: %u", mod_data->counter);
		
		/* Broadcast event when counter changes */
		akira_module_broadcast_event("counter_changed", 
		                              &mod_data->counter,
		                              sizeof(mod_data->counter));
		return 0;
	}
	else if (strcmp(command, "reset") == 0) {
		mod_data->counter = 0;
		LOG_INF("Counter reset");
		return 0;
	}
	
	/* TODO: Add more commands here */
	
	LOG_WRN("Unknown command: %s", command);
	return -ENOTSUP;
}

/*
 * Event handler callback
 * Process events from other modules
 * 
 * @param event Event name
 * @param data Event data buffer
 * @param len Data buffer length
 * @param user_data Pointer to your module data
 * @return 0 if event was handled, negative errno otherwise
 */
static int my_module_event(const char *event, void *data,
                            size_t len, void *user_data)
{
	struct my_module_data *mod_data = (struct my_module_data *)user_data;
	
	/* Handle events from other modules */
	
	if (strcmp(event, "system_ready") == 0) {
		LOG_INF("System is ready!");
		/* TODO: React to system ready event */
		return 0;
	}
	else if (strcmp(event, "button_pressed") == 0) {
		LOG_INF("Button was pressed!");
		/* TODO: React to button press */
		mod_data->counter++;
		return 0;
	}
	
	/* TODO: Add more event handlers here */
	
	/* Return non-zero if event was not handled */
	return -ENOTSUP;
}

/*
 * Register your module
 * This macro creates the module and registers it at boot time
 * 
 * Parameters:
 *   - Module name (no quotes)
 *   - Module type (AKIRA_MODULE_TYPE_xxx)
 *   - Init callback
 *   - Deinit callback
 *   - Command callback
 *   - Event callback
 *   - User data pointer
 */
AKIRA_MODULE_DEFINE(my_custom_module,
                    AKIRA_MODULE_TYPE_CUSTOM,
                    my_module_init,
                    my_module_deinit,
                    my_module_command,
                    my_module_event,
                    &module_data);

/*
 * ============================================================================
 * USAGE EXAMPLES
 * ============================================================================
 * 
 * From another module or main application:
 * 
 * 1. Send a command:
 *    akira_module_send_command("my_custom_module", "hello", NULL, 0);
 * 
 * 2. Get counter value:
 *    uint32_t counter;
 *    akira_module_send_command("my_custom_module", "get_counter", 
 *                               &counter, sizeof(counter));
 * 
 * 3. Increment counter:
 *    akira_module_send_command("my_custom_module", "increment", NULL, 0);
 * 
 * From external device via JSON:
 * 
 * 1. Send command:
 *    {"module":"my_custom_module","command":"increment"}
 * 
 * 2. Get counter:
 *    {"module":"my_custom_module","command":"get_counter"}
 * 
 * ============================================================================
 * CUSTOMIZATION CHECKLIST
 * ============================================================================
 * 
 * [ ] Replace "my_custom_module" with your module name
 * [ ] Update LOG_MODULE_REGISTER with your module name
 * [ ] Define your module data structure
 * [ ] Implement initialization in my_module_init()
 * [ ] Implement cleanup in my_module_deinit()
 * [ ] Add your commands in my_module_command()
 * [ ] Add your event handlers in my_module_event()
 * [ ] Choose appropriate AKIRA_MODULE_TYPE_xxx
 * [ ] Test with akira_module_send_command()
 * [ ] Document your commands and events
 * [ ] Share with the community!
 * 
 * ============================================================================
 */
