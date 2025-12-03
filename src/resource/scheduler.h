/**
 * @file scheduler.h
 * @brief WASM App/Container Scheduler
 * 
 * Cooperative scheduler for WASM applications with:
 * - Priority-based scheduling
 * - Time slicing
 * - Power-aware scheduling
 * - Fair share scheduling
 */

#ifndef AKIRA_SCHEDULER_H
#define AKIRA_SCHEDULER_H

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Maximum schedulable tasks
 */
#define SCHED_MAX_TASKS         16

/**
 * @brief Scheduler priorities
 */
typedef enum {
	SCHED_PRIORITY_IDLE     = 0,   // Background tasks
	SCHED_PRIORITY_LOW      = 1,   // Low priority
	SCHED_PRIORITY_NORMAL   = 2,   // Default priority
	SCHED_PRIORITY_HIGH     = 3,   // High priority
	SCHED_PRIORITY_REALTIME = 4    // Real-time (minimal preemption)
} sched_priority_t;

/**
 * @brief Task states
 */
typedef enum {
	TASK_STATE_INACTIVE = 0,   // Not started
	TASK_STATE_READY,          // Ready to run
	TASK_STATE_RUNNING,        // Currently executing
	TASK_STATE_BLOCKED,        // Waiting for resource
	TASK_STATE_SUSPENDED,      // Manually suspended
	TASK_STATE_TERMINATED      // Finished execution
} task_state_t;

/**
 * @brief Task handle
 */
typedef int task_handle_t;

/**
 * @brief Task entry function
 */
typedef void (*task_entry_t)(void *arg);

/**
 * @brief Task configuration
 */
struct task_config {
	const char *name;
	task_entry_t entry;
	void *arg;
	sched_priority_t priority;
	uint32_t time_slice_ms;     // Max execution time per slice
	uint32_t stack_size;        // Stack size for native tasks
	uint32_t app_id;            // Associated WASM app ID
};

/**
 * @brief Task statistics
 */
struct task_stats {
	uint32_t total_runtime_us;
	uint32_t num_slices;
	uint32_t num_preemptions;
	uint32_t num_yields;
	uint32_t last_run_us;
	uint32_t avg_slice_us;
};

/**
 * @brief Initialize scheduler
 * @return 0 on success
 */
int scheduler_init(void);

/**
 * @brief Create a new task
 * @param config Task configuration
 * @return Task handle or negative error
 */
task_handle_t scheduler_create_task(const struct task_config *config);

/**
 * @brief Destroy task
 * @param handle Task handle
 * @return 0 on success
 */
int scheduler_destroy_task(task_handle_t handle);

/**
 * @brief Start task execution
 * @param handle Task handle
 * @return 0 on success
 */
int scheduler_start_task(task_handle_t handle);

/**
 * @brief Suspend task
 * @param handle Task handle
 * @return 0 on success
 */
int scheduler_suspend_task(task_handle_t handle);

/**
 * @brief Resume suspended task
 * @param handle Task handle
 * @return 0 on success
 */
int scheduler_resume_task(task_handle_t handle);

/**
 * @brief Set task priority
 * @param handle Task handle
 * @param priority New priority
 * @return 0 on success
 */
int scheduler_set_priority(task_handle_t handle, sched_priority_t priority);

/**
 * @brief Get task state
 * @param handle Task handle
 * @return Task state
 */
task_state_t scheduler_get_state(task_handle_t handle);

/**
 * @brief Get task statistics
 * @param handle Task handle
 * @param stats Output for statistics
 * @return 0 on success
 */
int scheduler_get_stats(task_handle_t handle, struct task_stats *stats);

/**
 * @brief Yield current task's time slice
 */
void scheduler_yield(void);

/**
 * @brief Block current task
 * @param reason Block reason (for debugging)
 */
void scheduler_block(const char *reason);

/**
 * @brief Unblock task
 * @param handle Task handle
 * @return 0 on success
 */
int scheduler_unblock(task_handle_t handle);

/**
 * @brief Run scheduler tick (call from timer interrupt)
 */
void scheduler_tick(void);

/**
 * @brief Run one scheduling cycle
 * @return Number of tasks executed
 */
int scheduler_run(void);

/**
 * @brief Enable/disable power-aware scheduling
 * @param enable true to enable
 */
void scheduler_set_power_aware(bool enable);

/**
 * @brief Get current running task
 * @return Task handle or -1 if none
 */
task_handle_t scheduler_current_task(void);

/**
 * @brief Print scheduler debug info
 */
void scheduler_print_debug(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_SCHEDULER_H */
