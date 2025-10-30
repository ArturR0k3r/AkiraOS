/**
 * @file event_system.h
 * @brief AkiraOS Event System API
 */

#ifndef AKIRA_EVENT_SYSTEM_H
#define AKIRA_EVENT_SYSTEM_H

#include <stdint.h>
#include <stddef.h>

typedef enum
{
    EVENT_BUTTON_PRESS,
    EVENT_OTA_PROGRESS,
    EVENT_NETWORK_STATUS,
    EVENT_APP_INSTALL,
    EVENT_SHELL_COMMAND,
    EVENT_BLE_CONNECT,
    EVENT_BLE_DISCONNECT,
    EVENT_WASM_UPLOAD,
    EVENT_WASM_UPDATE,
    EVENT_PROCESS_START,
    EVENT_PROCESS_STOP,
    EVENT_CUSTOM
} akira_event_type_t;

typedef struct
{
    akira_event_type_t type;
    void *data;
    size_t data_size;
} akira_event_t;

typedef void (*akira_event_handler_t)(const akira_event_t *event);

int event_system_publish(const akira_event_t *event);
int event_system_subscribe(akira_event_type_t type, akira_event_handler_t handler);
int event_system_unsubscribe(akira_event_type_t type, akira_event_handler_t handler);

#endif // AKIRA_EVENT_SYSTEM_H
