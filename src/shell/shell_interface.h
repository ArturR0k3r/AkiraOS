/**
 * @file shell_interface.h
 * @brief Shell Interface - Dependency Injection for Shell Commands
 * 
 * Allows shell to be reused without coupling to specific modules.
 * Main application provides concrete implementations.
 */

#ifndef AKIRA_SHELL_INTERFACE_H
#define AKIRA_SHELL_INTERFACE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
struct app_info;

/**
 * @brief Shell operation callbacks
 * 
 * Application provides these implementations to shell.
 * Shell calls them without knowing internal module structure.
 */
typedef struct {
    /* System operations */
    int (*get_system_info)(char *buffer, size_t size);
    int (*get_memory_info)(size_t *used, size_t *free, size_t *total);
    int (*system_reboot)(void);
    
    /* Button/Input operations */
    uint32_t (*get_button_state)(void);
    
    /* App management operations */
    int (*app_list)(struct app_info *apps, int max_count);
    int (*app_info_get)(const char *name, struct app_info *info);
    int (*app_start)(const char *name);
    int (*app_stop)(const char *name);
    int (*app_restart)(const char *name);
    int (*app_uninstall)(const char *name);
    
    /* WiFi operations */
    int (*wifi_get_status)(char *buffer, size_t size);
    int (*wifi_connect)(const char *ssid, const char *password);
    int (*wifi_disconnect)(void);
    
    /* Settings operations */
    int (*settings_get)(const char *key, char *value, size_t max_len);
    int (*settings_set)(const char *key, const char *value);
    int (*settings_list)(char *buffer, size_t size);
    
    /* Storage operations */
    int (*storage_info)(char *buffer, size_t size);
    int (*storage_list)(const char *path, char **files, int max_count);
    
    /* OTA operations */
    int (*ota_status)(char *buffer, size_t size);
    int (*ota_trigger)(const char *url);
    
    /* Optional: Custom command handler */
    int (*custom_command)(const char *cmd, char *response, size_t size);
    
} shell_ops_t;

/**
 * @brief Initialize shell with operation callbacks
 * @param ops Pointer to operations structure
 * @return 0 on success, negative on error
 */
int akira_shell_init_with_ops(const shell_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_SHELL_INTERFACE_H */
