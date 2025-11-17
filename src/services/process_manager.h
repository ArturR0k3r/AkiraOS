
#ifndef AKIRA_PROCESS_MANAGER_H
#define AKIRA_PROCESS_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_PROCESSES 8

typedef enum
{
    PROCESS_TYPE_NATIVE,
    PROCESS_TYPE_WASM
} akira_process_type_t;

typedef struct
{
    const char *name;
    akira_process_type_t type;
    void *entry;
    bool running;
    uint32_t pid;
    uint32_t memory_usage;
} akira_process_t;

int process_manager_launch(const akira_process_t *process);
int process_manager_stop(uint32_t pid);
int process_manager_status(uint32_t pid);
int process_manager_list(akira_process_t *out_list, int max_count);

#endif // AKIRA_PROCESS_MANAGER_H
