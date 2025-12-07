/**
 * @file process.h
 * @brief AkiraOS Process Manager
 *
 * Manages native and WASM processes with lifecycle control,
 * resource tracking, and inter-process communication.
 */

#ifndef AKIRA_KERNEL_PROCESS_H
#define AKIRA_KERNEL_PROCESS_H

#include "types.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /*===========================================================================*/
    /* Process Types                                                             */
    /*===========================================================================*/

    typedef enum
    {
        AKIRA_PROCESS_NATIVE = 0, /**< Native Zephyr thread */
        AKIRA_PROCESS_WASM,       /**< WASM application */
        AKIRA_PROCESS_CONTAINER   /**< OCRE container */
    } akira_process_type_t;

    /*===========================================================================*/
    /* Process States                                                            */
    /*===========================================================================*/

    typedef enum
    {
        AKIRA_PROC_STATE_NONE = 0,
        AKIRA_PROC_STATE_CREATED,
        AKIRA_PROC_STATE_READY,
        AKIRA_PROC_STATE_RUNNING,
        AKIRA_PROC_STATE_BLOCKED,
        AKIRA_PROC_STATE_SUSPENDED,
        AKIRA_PROC_STATE_TERMINATED,
        AKIRA_PROC_STATE_ZOMBIE
    } akira_process_state_t;

    /*===========================================================================*/
    /* Process Priority                                                          */
    /*===========================================================================*/

    typedef enum
    {
        AKIRA_PROC_PRIORITY_IDLE = 0,
        AKIRA_PROC_PRIORITY_LOW = 1,
        AKIRA_PROC_PRIORITY_NORMAL = 2,
        AKIRA_PROC_PRIORITY_HIGH = 3,
        AKIRA_PROC_PRIORITY_REALTIME = 4
    } akira_process_priority_t;

    /*===========================================================================*/
    /* Process Entry Point                                                       */
    /*===========================================================================*/

    /**
     * @brief Native process entry point
     */
    typedef void (*akira_process_entry_t)(void *arg);

    /*===========================================================================*/
    /* Process Structure                                                         */
    /*===========================================================================*/

    /**
     * @brief Process descriptor
     */
    typedef struct akira_process
    {
        /* Identification */
        char name[32];
        akira_pid_t pid;
        akira_process_type_t type;
        akira_process_state_t state;
        akira_process_priority_t priority;

        /* Entry point */
        union
        {
            akira_process_entry_t native_entry;
            void *wasm_module;
        } entry;
        void *arg;

        /* Resources */
        uint32_t memory_usage;
        uint32_t cpu_time_us;
        uint32_t stack_size;
        uint32_t heap_size;

        /* Capabilities */
        uint64_t capabilities;

        /* Timing */
        uint32_t create_time;
        uint32_t start_time;
        uint32_t exit_code;

        /* Parent/child relationships */
        akira_pid_t parent_pid;

        /* Internal */
        void *_internal;
    } akira_process_t;

    /*===========================================================================*/
    /* Process Creation Options                                                  */
    /*===========================================================================*/

    typedef struct
    {
        const char *name;
        akira_process_type_t type;
        akira_process_priority_t priority;
        akira_process_entry_t entry;
        void *arg;
        uint32_t stack_size;
        uint32_t heap_size;
        uint64_t capabilities;
    } akira_process_options_t;

    /*===========================================================================*/
    /* Default Options                                                           */
    /*===========================================================================*/

#define AKIRA_PROCESS_DEFAULT_STACK 4096
#define AKIRA_PROCESS_DEFAULT_HEAP 8192

#define AKIRA_PROCESS_OPTIONS_DEFAULT {        \
    .name = "process",                         \
    .type = AKIRA_PROCESS_NATIVE,              \
    .priority = AKIRA_PROC_PRIORITY_NORMAL,    \
    .entry = NULL,                             \
    .arg = NULL,                               \
    .stack_size = AKIRA_PROCESS_DEFAULT_STACK, \
    .heap_size = AKIRA_PROCESS_DEFAULT_HEAP,   \
    .capabilities = 0}

    /*===========================================================================*/
    /* Process Manager API                                                       */
    /*===========================================================================*/

    /**
     * @brief Initialize process manager
     * @return 0 on success
     */
    int akira_process_manager_init(void);

    /**
     * @brief Create a new process
     * @param options Process options
     * @return PID or negative error
     */
    akira_pid_t akira_process_create(const akira_process_options_t *options);

    /**
     * @brief Start a process
     * @param pid Process ID
     * @return 0 on success
     */
    int akira_process_start(akira_pid_t pid);

    /**
     * @brief Stop a process
     * @param pid Process ID
     * @return 0 on success
     */
    int akira_process_stop(akira_pid_t pid);

    /**
     * @brief Suspend a process
     * @param pid Process ID
     * @return 0 on success
     */
    int akira_process_suspend(akira_pid_t pid);

    /**
     * @brief Resume a suspended process
     * @param pid Process ID
     * @return 0 on success
     */
    int akira_process_resume(akira_pid_t pid);

    /**
     * @brief Kill a process immediately
     * @param pid Process ID
     * @param exit_code Exit code to set
     * @return 0 on success
     */
    int akira_process_kill(akira_pid_t pid, int exit_code);

    /**
     * @brief Wait for process to terminate
     * @param pid Process ID
     * @param exit_code Output for exit code (may be NULL)
     * @param timeout_ms Timeout in milliseconds
     * @return 0 on success, -1 on timeout
     */
    int akira_process_wait(akira_pid_t pid, int *exit_code,
                           akira_duration_t timeout_ms);

    /**
     * @brief Get process by PID
     * @param pid Process ID
     * @return Process pointer or NULL
     */
    akira_process_t *akira_process_get(akira_pid_t pid);

    /**
     * @brief Get process by name
     * @param name Process name
     * @return Process pointer or NULL
     */
    akira_process_t *akira_process_find(const char *name);

    /**
     * @brief Get current process PID
     * @return Current PID
     */
    akira_pid_t akira_process_current(void);

    /**
     * @brief Get process state
     * @param pid Process ID
     * @return Process state
     */
    akira_process_state_t akira_process_get_state(akira_pid_t pid);

    /**
     * @brief Set process priority
     * @param pid Process ID
     * @param priority New priority
     * @return 0 on success
     */
    int akira_process_set_priority(akira_pid_t pid, akira_process_priority_t priority);

    /**
     * @brief List all processes
     * @param pids Output array for PIDs
     * @param max_count Maximum entries
     * @return Number of processes
     */
    int akira_process_list(akira_pid_t *pids, int max_count);

    /**
     * @brief Get process count
     * @return Number of active processes
     */
    int akira_process_count(void);

    /**
     * @brief Clean up zombie processes
     * @return Number of processes cleaned
     */
    int akira_process_cleanup(void);

    /**
     * @brief Print process table (debug)
     */
    void akira_process_print_table(void);

    /*===========================================================================*/
    /* Process Memory API                                                        */
    /*===========================================================================*/

    /**
     * @brief Get process memory usage
     * @param pid Process ID
     * @return Memory usage in bytes
     */
    uint32_t akira_process_memory_usage(akira_pid_t pid);

    /**
     * @brief Get process CPU time
     * @param pid Process ID
     * @return CPU time in microseconds
     */
    uint32_t akira_process_cpu_time(akira_pid_t pid);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_KERNEL_PROCESS_H */
