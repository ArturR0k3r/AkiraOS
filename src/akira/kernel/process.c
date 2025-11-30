/**
 * @file process.c
 * @brief AkiraOS Process Manager Implementation
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include "process.h"

LOG_MODULE_REGISTER(akira_process, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* Configuration                                                             */
/*===========================================================================*/

#ifndef AKIRA_MAX_PROCESSES
#define AKIRA_MAX_PROCESSES 16
#endif

/*===========================================================================*/
/* Internal Structures                                                       */
/*===========================================================================*/

typedef struct
{
    akira_process_t proc;
    bool in_use;
    struct k_thread thread;
    k_thread_stack_t *stack;
    struct k_sem exit_sem;
} process_slot_t;

/*===========================================================================*/
/* Internal State                                                            */
/*===========================================================================*/

static struct
{
    bool initialized;
    process_slot_t slots[AKIRA_MAX_PROCESSES];
    akira_pid_t next_pid;
    int process_count;
    struct k_mutex mutex;
} proc_mgr;

/*===========================================================================*/
/* Internal Functions                                                        */
/*===========================================================================*/

static process_slot_t *find_slot_by_pid(akira_pid_t pid)
{
    for (int i = 0; i < AKIRA_MAX_PROCESSES; i++)
    {
        if (proc_mgr.slots[i].in_use &&
            proc_mgr.slots[i].proc.pid == pid)
        {
            return &proc_mgr.slots[i];
        }
    }
    return NULL;
}

static process_slot_t *find_free_slot(void)
{
    for (int i = 0; i < AKIRA_MAX_PROCESSES; i++)
    {
        if (!proc_mgr.slots[i].in_use)
        {
            return &proc_mgr.slots[i];
        }
    }
    return NULL;
}

static void process_thread_entry(void *p1, void *p2, void *p3)
{
    process_slot_t *slot = (process_slot_t *)p1;
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    if (!slot || !slot->proc.entry.native_entry)
    {
        return;
    }

    slot->proc.state = AKIRA_PROC_STATE_RUNNING;
    slot->proc.start_time = k_uptime_get_32();

    LOG_INF("Process '%s' (PID %u) started", slot->proc.name, slot->proc.pid);

    /* Execute process entry point */
    slot->proc.entry.native_entry(slot->proc.arg);

    /* Process exited normally */
    slot->proc.state = AKIRA_PROC_STATE_TERMINATED;
    slot->proc.exit_code = 0;

    LOG_INF("Process '%s' (PID %u) exited with code %d",
            slot->proc.name, slot->proc.pid, slot->proc.exit_code);

    k_sem_give(&slot->exit_sem);
}

/*===========================================================================*/
/* Public API                                                                */
/*===========================================================================*/

int akira_process_manager_init(void)
{
    if (proc_mgr.initialized)
    {
        return 0;
    }

    LOG_INF("Initializing process manager");

    k_mutex_init(&proc_mgr.mutex);

    memset(proc_mgr.slots, 0, sizeof(proc_mgr.slots));
    proc_mgr.next_pid = 1;
    proc_mgr.process_count = 0;

    for (int i = 0; i < AKIRA_MAX_PROCESSES; i++)
    {
        k_sem_init(&proc_mgr.slots[i].exit_sem, 0, 1);
    }

    proc_mgr.initialized = true;

    LOG_INF("Process manager initialized (max=%d)", AKIRA_MAX_PROCESSES);

    return 0;
}

akira_pid_t akira_process_create(const akira_process_options_t *options)
{
    if (!proc_mgr.initialized || !options)
    {
        return 0;
    }

    k_mutex_lock(&proc_mgr.mutex, K_FOREVER);

    process_slot_t *slot = find_free_slot();
    if (!slot)
    {
        k_mutex_unlock(&proc_mgr.mutex);
        LOG_ERR("No free process slots");
        return 0;
    }

    /* Initialize process */
    memset(&slot->proc, 0, sizeof(slot->proc));

    if (options->name)
    {
        strncpy(slot->proc.name, options->name, sizeof(slot->proc.name) - 1);
    }
    else
    {
        snprintf(slot->proc.name, sizeof(slot->proc.name),
                 "proc_%u", proc_mgr.next_pid);
    }

    slot->proc.pid = proc_mgr.next_pid++;
    slot->proc.type = options->type;
    slot->proc.state = AKIRA_PROC_STATE_CREATED;
    slot->proc.priority = options->priority;
    slot->proc.entry.native_entry = options->entry;
    slot->proc.arg = options->arg;
    slot->proc.stack_size = options->stack_size > 0 ? options->stack_size : AKIRA_PROCESS_DEFAULT_STACK;
    slot->proc.heap_size = options->heap_size > 0 ? options->heap_size : AKIRA_PROCESS_DEFAULT_HEAP;
    slot->proc.capabilities = options->capabilities;
    slot->proc.create_time = k_uptime_get_32();
    slot->proc.parent_pid = 0; /* TODO: Get current process PID */

    slot->in_use = true;
    proc_mgr.process_count++;

    k_sem_reset(&slot->exit_sem);

    k_mutex_unlock(&proc_mgr.mutex);

    LOG_INF("Created process '%s' (PID %u, type=%d)",
            slot->proc.name, slot->proc.pid, slot->proc.type);

    return slot->proc.pid;
}

int akira_process_start(akira_pid_t pid)
{
    if (!proc_mgr.initialized || pid == 0)
    {
        return -1;
    }

    k_mutex_lock(&proc_mgr.mutex, K_FOREVER);

    process_slot_t *slot = find_slot_by_pid(pid);
    if (!slot)
    {
        k_mutex_unlock(&proc_mgr.mutex);
        LOG_ERR("Process %u not found", pid);
        return -1;
    }

    if (slot->proc.state != AKIRA_PROC_STATE_CREATED &&
        slot->proc.state != AKIRA_PROC_STATE_READY)
    {
        k_mutex_unlock(&proc_mgr.mutex);
        LOG_ERR("Process %u not in startable state", pid);
        return -1;
    }

    slot->proc.state = AKIRA_PROC_STATE_READY;

    if (slot->proc.type == AKIRA_PROCESS_NATIVE)
    {
        /* Allocate stack dynamically */
        slot->stack = k_thread_stack_alloc(slot->proc.stack_size, 0);
        if (!slot->stack)
        {
            k_mutex_unlock(&proc_mgr.mutex);
            LOG_ERR("Failed to allocate stack for process %u", pid);
            return -1;
        }

        /* Map priority */
        int zephyr_priority;
        switch (slot->proc.priority)
        {
        case AKIRA_PROC_PRIORITY_REALTIME:
            zephyr_priority = -5;
            break;
        case AKIRA_PROC_PRIORITY_HIGH:
            zephyr_priority = 5;
            break;
        case AKIRA_PROC_PRIORITY_NORMAL:
            zephyr_priority = 10;
            break;
        case AKIRA_PROC_PRIORITY_LOW:
            zephyr_priority = 14;
            break;
        default:
            zephyr_priority = 15;
            break;
        }

        /* Create thread */
        k_thread_create(&slot->thread, slot->stack, slot->proc.stack_size,
                        process_thread_entry, slot, NULL, NULL,
                        zephyr_priority, 0, K_NO_WAIT);

        k_thread_name_set(&slot->thread, slot->proc.name);
    }
    else if (slot->proc.type == AKIRA_PROCESS_WASM)
    {
        /* TODO: Start WASM execution */
        LOG_WRN("WASM process start not implemented");
    }
    else if (slot->proc.type == AKIRA_PROCESS_CONTAINER)
    {
        /* TODO: Start OCRE container */
        LOG_WRN("Container start not implemented");
    }

    k_mutex_unlock(&proc_mgr.mutex);

    return 0;
}

int akira_process_stop(akira_pid_t pid)
{
    if (!proc_mgr.initialized || pid == 0)
    {
        return -1;
    }

    k_mutex_lock(&proc_mgr.mutex, K_FOREVER);

    process_slot_t *slot = find_slot_by_pid(pid);
    if (!slot)
    {
        k_mutex_unlock(&proc_mgr.mutex);
        return -1;
    }

    if (slot->proc.state != AKIRA_PROC_STATE_RUNNING &&
        slot->proc.state != AKIRA_PROC_STATE_BLOCKED)
    {
        k_mutex_unlock(&proc_mgr.mutex);
        return 0; /* Already stopped */
    }

    /* Signal process to stop */
    slot->proc.state = AKIRA_PROC_STATE_TERMINATED;

    if (slot->proc.type == AKIRA_PROCESS_NATIVE)
    {
        k_thread_abort(&slot->thread);
        if (slot->stack)
        {
            k_thread_stack_free(slot->stack);
            slot->stack = NULL;
        }
    }

    k_sem_give(&slot->exit_sem);

    k_mutex_unlock(&proc_mgr.mutex);

    LOG_INF("Stopped process '%s' (PID %u)", slot->proc.name, pid);

    return 0;
}

int akira_process_suspend(akira_pid_t pid)
{
    if (!proc_mgr.initialized || pid == 0)
    {
        return -1;
    }

    k_mutex_lock(&proc_mgr.mutex, K_FOREVER);

    process_slot_t *slot = find_slot_by_pid(pid);
    if (!slot || slot->proc.state != AKIRA_PROC_STATE_RUNNING)
    {
        k_mutex_unlock(&proc_mgr.mutex);
        return -1;
    }

    if (slot->proc.type == AKIRA_PROCESS_NATIVE)
    {
        k_thread_suspend(&slot->thread);
    }

    slot->proc.state = AKIRA_PROC_STATE_SUSPENDED;

    k_mutex_unlock(&proc_mgr.mutex);

    LOG_DBG("Suspended process '%s' (PID %u)", slot->proc.name, pid);

    return 0;
}

int akira_process_resume(akira_pid_t pid)
{
    if (!proc_mgr.initialized || pid == 0)
    {
        return -1;
    }

    k_mutex_lock(&proc_mgr.mutex, K_FOREVER);

    process_slot_t *slot = find_slot_by_pid(pid);
    if (!slot || slot->proc.state != AKIRA_PROC_STATE_SUSPENDED)
    {
        k_mutex_unlock(&proc_mgr.mutex);
        return -1;
    }

    if (slot->proc.type == AKIRA_PROCESS_NATIVE)
    {
        k_thread_resume(&slot->thread);
    }

    slot->proc.state = AKIRA_PROC_STATE_RUNNING;

    k_mutex_unlock(&proc_mgr.mutex);

    LOG_DBG("Resumed process '%s' (PID %u)", slot->proc.name, pid);

    return 0;
}

int akira_process_kill(akira_pid_t pid, int exit_code)
{
    if (!proc_mgr.initialized || pid == 0)
    {
        return -1;
    }

    k_mutex_lock(&proc_mgr.mutex, K_FOREVER);

    process_slot_t *slot = find_slot_by_pid(pid);
    if (!slot)
    {
        k_mutex_unlock(&proc_mgr.mutex);
        return -1;
    }

    slot->proc.exit_code = exit_code;

    if (slot->proc.type == AKIRA_PROCESS_NATIVE)
    {
        k_thread_abort(&slot->thread);
        if (slot->stack)
        {
            k_thread_stack_free(slot->stack);
            slot->stack = NULL;
        }
    }

    slot->proc.state = AKIRA_PROC_STATE_ZOMBIE;
    k_sem_give(&slot->exit_sem);

    k_mutex_unlock(&proc_mgr.mutex);

    LOG_INF("Killed process '%s' (PID %u) with code %d",
            slot->proc.name, pid, exit_code);

    return 0;
}

int akira_process_wait(akira_pid_t pid, int *exit_code,
                       akira_duration_t timeout_ms)
{
    if (!proc_mgr.initialized || pid == 0)
    {
        return -1;
    }

    process_slot_t *slot = find_slot_by_pid(pid);
    if (!slot)
    {
        return -1;
    }

    k_timeout_t timeout = (timeout_ms == AKIRA_WAIT_FOREVER) ? K_FOREVER : K_MSEC(timeout_ms);

    int ret = k_sem_take(&slot->exit_sem, timeout);
    if (ret != 0)
    {
        return -1; /* Timeout */
    }

    if (exit_code)
    {
        *exit_code = slot->proc.exit_code;
    }

    return 0;
}

akira_process_t *akira_process_get(akira_pid_t pid)
{
    process_slot_t *slot = find_slot_by_pid(pid);
    return slot ? &slot->proc : NULL;
}

akira_process_t *akira_process_find(const char *name)
{
    if (!name)
        return NULL;

    for (int i = 0; i < AKIRA_MAX_PROCESSES; i++)
    {
        if (proc_mgr.slots[i].in_use &&
            strcmp(proc_mgr.slots[i].proc.name, name) == 0)
        {
            return &proc_mgr.slots[i].proc;
        }
    }
    return NULL;
}

akira_pid_t akira_process_current(void)
{
    /* TODO: Map Zephyr thread to process */
    return 0;
}

akira_process_state_t akira_process_get_state(akira_pid_t pid)
{
    akira_process_t *proc = akira_process_get(pid);
    return proc ? proc->state : AKIRA_PROC_STATE_NONE;
}

int akira_process_set_priority(akira_pid_t pid, akira_process_priority_t priority)
{
    process_slot_t *slot = find_slot_by_pid(pid);
    if (!slot)
        return -1;

    slot->proc.priority = priority;

    if (slot->proc.type == AKIRA_PROCESS_NATIVE &&
        slot->proc.state == AKIRA_PROC_STATE_RUNNING)
    {
        /* Map and set thread priority */
        int zephyr_priority = 10; /* Default */
        switch (priority)
        {
        case AKIRA_PROC_PRIORITY_REALTIME:
            zephyr_priority = -5;
            break;
        case AKIRA_PROC_PRIORITY_HIGH:
            zephyr_priority = 5;
            break;
        case AKIRA_PROC_PRIORITY_NORMAL:
            zephyr_priority = 10;
            break;
        case AKIRA_PROC_PRIORITY_LOW:
            zephyr_priority = 14;
            break;
        default:
            zephyr_priority = 15;
            break;
        }
        k_thread_priority_set(&slot->thread, zephyr_priority);
    }

    return 0;
}

int akira_process_list(akira_pid_t *pids, int max_count)
{
    if (!pids || max_count <= 0)
        return 0;

    int count = 0;
    for (int i = 0; i < AKIRA_MAX_PROCESSES && count < max_count; i++)
    {
        if (proc_mgr.slots[i].in_use)
        {
            pids[count++] = proc_mgr.slots[i].proc.pid;
        }
    }
    return count;
}

int akira_process_count(void)
{
    return proc_mgr.process_count;
}

int akira_process_cleanup(void)
{
    int cleaned = 0;

    k_mutex_lock(&proc_mgr.mutex, K_FOREVER);

    for (int i = 0; i < AKIRA_MAX_PROCESSES; i++)
    {
        if (proc_mgr.slots[i].in_use &&
            proc_mgr.slots[i].proc.state == AKIRA_PROC_STATE_ZOMBIE)
        {
            LOG_DBG("Cleaning up zombie process '%s' (PID %u)",
                    proc_mgr.slots[i].proc.name,
                    proc_mgr.slots[i].proc.pid);

            proc_mgr.slots[i].in_use = false;
            proc_mgr.process_count--;
            cleaned++;
        }
    }

    k_mutex_unlock(&proc_mgr.mutex);

    return cleaned;
}

void akira_process_print_table(void)
{
    LOG_INF("=== Process Table ===");
    LOG_INF("Active processes: %d/%d", proc_mgr.process_count, AKIRA_MAX_PROCESSES);

    static const char *state_names[] = {
        "NONE", "CREATED", "READY", "RUNNING",
        "BLOCKED", "SUSPENDED", "TERMINATED", "ZOMBIE"};

    static const char *type_names[] = {
        "NATIVE", "WASM", "CONTAINER"};

    for (int i = 0; i < AKIRA_MAX_PROCESSES; i++)
    {
        if (proc_mgr.slots[i].in_use)
        {
            akira_process_t *p = &proc_mgr.slots[i].proc;
            LOG_INF("  PID %u: %s [%s] %s pri=%d mem=%u",
                    p->pid, p->name,
                    type_names[p->type],
                    state_names[p->state],
                    p->priority,
                    p->memory_usage);
        }
    }
}

uint32_t akira_process_memory_usage(akira_pid_t pid)
{
    akira_process_t *proc = akira_process_get(pid);
    return proc ? proc->memory_usage : 0;
}

uint32_t akira_process_cpu_time(akira_pid_t pid)
{
    akira_process_t *proc = akira_process_get(pid);
    return proc ? proc->cpu_time_us : 0;
}
