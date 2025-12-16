/**
 * @file scheduler.c
 * @brief WASM App/Container Scheduler Implementation
 * 
 * Priority-based cooperative scheduler for WASM applications.
 * Provides fair CPU time distribution with power awareness.
 */

#include "scheduler.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(scheduler, CONFIG_AKIRA_LOG_LEVEL);

/* Default time slice */
#define DEFAULT_TIME_SLICE_MS   10

/* Task control block */
struct task_cb {
	bool in_use;
	char name[32];
	task_entry_t entry;
	void *arg;
	sched_priority_t priority;
	task_state_t state;
	uint32_t time_slice_ms;
	uint32_t app_id;
	
	/* Runtime tracking */
	uint64_t start_time;
	uint64_t total_runtime;
	uint32_t slice_count;
	uint32_t preemption_count;
	uint32_t yield_count;
	
	/* Blocking */
	const char *block_reason;
};

/* Scheduler state */
static struct {
	bool initialized;
	bool running;
	struct task_cb tasks[SCHED_MAX_TASKS];
	task_handle_t current_task;
	struct k_mutex mutex;
	
	/* Timing */
	uint64_t last_tick;
	uint32_t tick_count;
	
	/* Power awareness */
	bool power_aware;
	
	/* Ready queue (simple priority array) */
	task_handle_t ready_queue[SCHED_MAX_TASKS];
	int ready_count;
} sched_state;

/**
 * @brief Get task by handle
 */
static struct task_cb *get_task(task_handle_t handle)
{
	if (handle < 0 || handle >= SCHED_MAX_TASKS) {
		return NULL;
	}
	if (!sched_state.tasks[handle].in_use) {
		return NULL;
	}
	return &sched_state.tasks[handle];
}

/**
 * @brief Find free task slot
 */
static task_handle_t find_free_slot(void)
{
	for (int i = 0; i < SCHED_MAX_TASKS; i++) {
		if (!sched_state.tasks[i].in_use) {
			return i;
		}
	}
	return -1;
}

/**
 * @brief Add task to ready queue
 */
static void add_to_ready_queue(task_handle_t handle)
{
	if (sched_state.ready_count >= SCHED_MAX_TASKS) {
		return;
	}
	
	struct task_cb *task = get_task(handle);
	if (!task) return;
	
	/* Insert in priority order (higher priority first) */
	int insert_pos = sched_state.ready_count;
	for (int i = 0; i < sched_state.ready_count; i++) {
		struct task_cb *other = get_task(sched_state.ready_queue[i]);
		if (other && task->priority > other->priority) {
			insert_pos = i;
			break;
		}
	}
	
	/* Shift elements */
	for (int i = sched_state.ready_count; i > insert_pos; i--) {
		sched_state.ready_queue[i] = sched_state.ready_queue[i - 1];
	}
	
	sched_state.ready_queue[insert_pos] = handle;
	sched_state.ready_count++;
}

/**
 * @brief Remove task from ready queue
 */
static void remove_from_ready_queue(task_handle_t handle)
{
	for (int i = 0; i < sched_state.ready_count; i++) {
		if (sched_state.ready_queue[i] == handle) {
			/* Shift elements */
			for (int j = i; j < sched_state.ready_count - 1; j++) {
				sched_state.ready_queue[j] = sched_state.ready_queue[j + 1];
			}
			sched_state.ready_count--;
			return;
		}
	}
}

/**
 * @brief Select next task to run
 */
static task_handle_t select_next_task(void)
{
	// Priority-based round-robin scheduler with power awareness
	// 1. Select highest priority ready task
	// 2. Within same priority, round-robin
	// 3. Consider power state if enabled
	
	if (sched_state.ready_count == 0) {
		return -1;
	}
	
	// Find highest priority task
	int best_priority = -1;
	for (int i = 0; i < sched_state.ready_count; i++) {
		struct task_cb *task = get_task(sched_state.ready_queue[i]);
		if (task && task->priority > best_priority) {
			best_priority = task->priority;
		}
	}
	
	// Round-robin among tasks with highest priority
	// Find first task with highest priority after current position
	task_handle_t selected = -1;
	bool found_current = false;
	
	for (int pass = 0; pass < 2 && selected < 0; pass++) {
		for (int i = 0; i < sched_state.ready_count; i++) {
			task_handle_t handle = sched_state.ready_queue[i];
			struct task_cb *task = get_task(handle);
			
			if (!task || task->priority != best_priority) {
				continue;
			}
			
			// In first pass, look for task after current
			if (pass == 0) {
				if (!found_current) {
					if (handle == sched_state.current_task) {
						found_current = true;
					}
					continue;
				}
			}
			
			selected = handle;
			break;
		}
	}
	
	// If no task found, pick first with highest priority
	if (selected < 0) {
		for (int i = 0; i < sched_state.ready_count; i++) {
			struct task_cb *task = get_task(sched_state.ready_queue[i]);
			if (task && task->priority == best_priority) {
				selected = sched_state.ready_queue[i];
				break;
			}
		}
	}
	
	return selected;
}

int scheduler_init(void)
{
	if (sched_state.initialized) {
		return 0;
	}
	
	LOG_INF("Initializing scheduler");
	
	k_mutex_init(&sched_state.mutex);
	
	for (int i = 0; i < SCHED_MAX_TASKS; i++) {
		sched_state.tasks[i].in_use = false;
	}
	
	sched_state.current_task = -1;
	sched_state.ready_count = 0;
	sched_state.running = false;
	sched_state.power_aware = false;
	sched_state.tick_count = 0;
	sched_state.last_tick = k_uptime_get();
	
	sched_state.initialized = true;
	
	LOG_INF("Scheduler initialized");
	return 0;
}

task_handle_t scheduler_create_task(const struct task_config *config)
{
	if (!sched_state.initialized || !config || !config->entry) {
		return -EINVAL;
	}
	
	k_mutex_lock(&sched_state.mutex, K_FOREVER);
	
	task_handle_t handle = find_free_slot();
	if (handle < 0) {
		k_mutex_unlock(&sched_state.mutex);
		LOG_ERR("No free task slots");
		return -ENOMEM;
	}
	
	struct task_cb *task = &sched_state.tasks[handle];
	task->in_use = true;
	if (config->name) {
		strncpy(task->name, config->name, sizeof(task->name) - 1);
		task->name[sizeof(task->name) - 1] = '\0';
	} else {
		snprintf(task->name, sizeof(task->name), "task_%d", handle);
	}
	task->entry = config->entry;
	task->arg = config->arg;
	task->priority = config->priority;
	task->state = TASK_STATE_INACTIVE;
	task->time_slice_ms = config->time_slice_ms > 0 ? 
	                      config->time_slice_ms : DEFAULT_TIME_SLICE_MS;
	task->app_id = config->app_id;
	task->total_runtime = 0;
	task->slice_count = 0;
	task->preemption_count = 0;
	task->yield_count = 0;
	task->block_reason = NULL;
	
	k_mutex_unlock(&sched_state.mutex);
	
	LOG_INF("Created task '%s' (handle=%d, priority=%d)", 
	        task->name, handle, task->priority);
	
	return handle;
}

int scheduler_destroy_task(task_handle_t handle)
{
	k_mutex_lock(&sched_state.mutex, K_FOREVER);
	
	struct task_cb *task = get_task(handle);
	if (!task) {
		k_mutex_unlock(&sched_state.mutex);
		return -ENOENT;
	}
	
	remove_from_ready_queue(handle);
	
	if (sched_state.current_task == handle) {
		sched_state.current_task = -1;
	}
	
	LOG_INF("Destroyed task '%s'", task->name);
	
	task->in_use = false;
	
	k_mutex_unlock(&sched_state.mutex);
	return 0;
}

int scheduler_start_task(task_handle_t handle)
{
	k_mutex_lock(&sched_state.mutex, K_FOREVER);
	
	struct task_cb *task = get_task(handle);
	if (!task) {
		k_mutex_unlock(&sched_state.mutex);
		return -ENOENT;
	}
	
	if (task->state != TASK_STATE_INACTIVE && 
	    task->state != TASK_STATE_SUSPENDED) {
		k_mutex_unlock(&sched_state.mutex);
		return -EINVAL;
	}
	
	task->state = TASK_STATE_READY;
	add_to_ready_queue(handle);
	
	k_mutex_unlock(&sched_state.mutex);
	
	LOG_DBG("Started task '%s'", task->name);
	return 0;
}

int scheduler_suspend_task(task_handle_t handle)
{
	k_mutex_lock(&sched_state.mutex, K_FOREVER);
	
	struct task_cb *task = get_task(handle);
	if (!task) {
		k_mutex_unlock(&sched_state.mutex);
		return -ENOENT;
	}
	
	task->state = TASK_STATE_SUSPENDED;
	remove_from_ready_queue(handle);
	
	k_mutex_unlock(&sched_state.mutex);
	
	LOG_DBG("Suspended task '%s'", task->name);
	return 0;
}

int scheduler_resume_task(task_handle_t handle)
{
	k_mutex_lock(&sched_state.mutex, K_FOREVER);
	
	struct task_cb *task = get_task(handle);
	if (!task) {
		k_mutex_unlock(&sched_state.mutex);
		return -ENOENT;
	}
	
	if (task->state != TASK_STATE_SUSPENDED) {
		k_mutex_unlock(&sched_state.mutex);
		return -EINVAL;
	}
	
	task->state = TASK_STATE_READY;
	add_to_ready_queue(handle);
	
	k_mutex_unlock(&sched_state.mutex);
	
	LOG_DBG("Resumed task '%s'", task->name);
	return 0;
}

int scheduler_set_priority(task_handle_t handle, sched_priority_t priority)
{
	k_mutex_lock(&sched_state.mutex, K_FOREVER);
	
	struct task_cb *task = get_task(handle);
	if (!task) {
		k_mutex_unlock(&sched_state.mutex);
		return -ENOENT;
	}
	
	task->priority = priority;
	
	/* Re-sort ready queue if task is ready */
	if (task->state == TASK_STATE_READY) {
		remove_from_ready_queue(handle);
		add_to_ready_queue(handle);
	}
	
	k_mutex_unlock(&sched_state.mutex);
	return 0;
}

task_state_t scheduler_get_state(task_handle_t handle)
{
	struct task_cb *task = get_task(handle);
	if (!task) {
		return TASK_STATE_INACTIVE;
	}
	return task->state;
}

int scheduler_get_stats(task_handle_t handle, struct task_stats *stats)
{
	if (!stats) {
		return -EINVAL;
	}
	
	struct task_cb *task = get_task(handle);
	if (!task) {
		return -ENOENT;
	}
	
	stats->total_runtime_us = task->total_runtime;
	stats->num_slices = task->slice_count;
	stats->num_preemptions = task->preemption_count;
	stats->num_yields = task->yield_count;
	stats->last_run_us = 0;  // TODO: Track last run duration
	stats->avg_slice_us = task->slice_count > 0 ? 
	                      task->total_runtime / task->slice_count : 0;
	
	return 0;
}

void scheduler_yield(void)
{
	// Cooperative yield: task voluntarily gives up CPU
	// 1. Mark current task time slice ended voluntarily
	// 2. Update yield count
	// 3. Move task to end of ready queue for fairness
	
	k_mutex_lock(&sched_state.mutex, K_FOREVER);
	
	if (sched_state.current_task >= 0) {
		struct task_cb *task = get_task(sched_state.current_task);
		if (task && task->state == TASK_STATE_RUNNING) {
			task->yield_count++;
			task->state = TASK_STATE_READY;
			
			// Move to end of ready queue (after others of same priority)
			remove_from_ready_queue(sched_state.current_task);
			add_to_ready_queue(sched_state.current_task);
			
			LOG_DBG("Task '%s' yielded (count=%u)", task->name, task->yield_count);
		}
	}
	
	k_mutex_unlock(&sched_state.mutex);
}

void scheduler_block(const char *reason)
{
	k_mutex_lock(&sched_state.mutex, K_FOREVER);
	
	if (sched_state.current_task >= 0) {
		struct task_cb *task = get_task(sched_state.current_task);
		if (task) {
			task->state = TASK_STATE_BLOCKED;
			task->block_reason = reason;
			remove_from_ready_queue(sched_state.current_task);
			LOG_DBG("Task '%s' blocked: %s", task->name, reason ? reason : "unknown");
		}
	}
	
	k_mutex_unlock(&sched_state.mutex);
}

int scheduler_unblock(task_handle_t handle)
{
	k_mutex_lock(&sched_state.mutex, K_FOREVER);
	
	struct task_cb *task = get_task(handle);
	if (!task) {
		k_mutex_unlock(&sched_state.mutex);
		return -ENOENT;
	}
	
	if (task->state != TASK_STATE_BLOCKED) {
		k_mutex_unlock(&sched_state.mutex);
		return -EINVAL;
	}
	
	task->state = TASK_STATE_READY;
	task->block_reason = NULL;
	add_to_ready_queue(handle);
	
	k_mutex_unlock(&sched_state.mutex);
	
	LOG_DBG("Task '%s' unblocked", task->name);
	return 0;
}

void scheduler_tick(void)
{
	sched_state.tick_count++;
	
	// Check if current task exceeded its time slice
	if (sched_state.current_task >= 0) {
		struct task_cb *task = get_task(sched_state.current_task);
		if (!task) {
			return;
		}
		
		uint64_t current_time = k_uptime_get();
		uint64_t runtime = current_time - task->start_time;
		
		// Convert runtime from us to ms
		uint32_t runtime_ms = runtime / 1000;
		
		if (runtime_ms >= task->time_slice_ms) {
			// Time slice expired - preempt task
			k_mutex_lock(&sched_state.mutex, K_FOREVER);
			
			if (task->state == TASK_STATE_RUNNING) {
				task->preemption_count++;
				task->state = TASK_STATE_READY;
				
				// Update runtime statistics
				task->total_runtime += runtime;
				
				// Move to ready queue for rescheduling
				remove_from_ready_queue(sched_state.current_task);
				add_to_ready_queue(sched_state.current_task);
				
				LOG_DBG("Task '%s' preempted (slice=%ums, preempt_count=%u)",
				        task->name, task->time_slice_ms, task->preemption_count);
				
				// Clear current task to trigger reschedule
				sched_state.current_task = -1;
			}
			
			k_mutex_unlock(&sched_state.mutex);
		}
	}
	
	// Update last tick time
	sched_state.last_tick = k_uptime_get();
}

int scheduler_run(void)
{
	if (!sched_state.initialized) {
		return -ENODEV;
	}
	
	// Main scheduling loop iteration:
	// 1. Select next task based on priority and readiness
	// 2. Context switch to selected task
	// 3. Execute task entry function (WASM app execution)
	// 4. Update runtime statistics
	// 5. Handle task completion/blocking/yielding
	
	k_mutex_lock(&sched_state.mutex, K_FOREVER);
	
	// Select next task to run
	task_handle_t next = select_next_task();
	if (next < 0) {
		k_mutex_unlock(&sched_state.mutex);
		return 0;  // No tasks ready (idle state)
	}
	
	struct task_cb *task = get_task(next);
	if (!task) {
		k_mutex_unlock(&sched_state.mutex);
		LOG_ERR("Invalid task handle: %d", next);
		return -EINVAL;
	}
	
	// Context switch preparation
	task_handle_t prev = sched_state.current_task;
	sched_state.current_task = next;
	task->state = TASK_STATE_RUNNING;
	task->slice_count++;
	task->start_time = k_uptime_get();
	
	// Remove from ready queue while running
	remove_from_ready_queue(next);
	
	k_mutex_unlock(&sched_state.mutex);
	
	LOG_DBG("Context switch: task '%s' (priority=%d, slice=%u)",
	        task->name, task->priority, task->slice_count);
	
	// Execute task entry function
	// In real WASM implementation, this calls ocre_resume() or wasm_runtime_execute()
	if (task->entry) {
		task->entry(task->arg);
	}
	
	// Task execution completed (returned from entry or yielded/blocked)
	k_mutex_lock(&sched_state.mutex, K_FOREVER);
	
	// Calculate runtime for this execution slice
	uint64_t runtime = k_uptime_get() - task->start_time;
	task->total_runtime += runtime;
	
	// Handle task state after execution
	if (task->state == TASK_STATE_RUNNING) {
		// Task completed normally (entry function returned)
		task->state = TASK_STATE_TERMINATED;
		LOG_INF("Task '%s' terminated (runtime=%lluus, slices=%u)",
		        task->name, task->total_runtime, task->slice_count);
	} else if (task->state == TASK_STATE_READY) {
		// Task yielded or was preempted - add back to ready queue
		add_to_ready_queue(next);
	} else if (task->state == TASK_STATE_BLOCKED) {
		// Task blocked on I/O or event - stays out of ready queue
		LOG_DBG("Task '%s' blocked: %s", task->name, 
		        task->block_reason ? task->block_reason : "unknown");
	}
	
	// Clear current task reference
	sched_state.current_task = -1;
	
	k_mutex_unlock(&sched_state.mutex);
	
	return 1;
}

void scheduler_set_power_aware(bool enable)
{
	sched_state.power_aware = enable;
	LOG_INF("Power-aware scheduling %s", enable ? "enabled" : "disabled");
}

task_handle_t scheduler_current_task(void)
{
	return sched_state.current_task;
}

void scheduler_print_debug(void)
{
	LOG_INF("=== Scheduler Debug ===");
	LOG_INF("Ticks: %u", sched_state.tick_count);
	LOG_INF("Current task: %d", sched_state.current_task);
	LOG_INF("Ready queue (%d tasks):", sched_state.ready_count);
	
	for (int i = 0; i < sched_state.ready_count; i++) {
		struct task_cb *task = get_task(sched_state.ready_queue[i]);
		if (task) {
			LOG_INF("  [%d] %s (pri=%d)", i, task->name, task->priority);
		}
	}
	
	LOG_INF("All tasks:");
	for (int i = 0; i < SCHED_MAX_TASKS; i++) {
		if (sched_state.tasks[i].in_use) {
			struct task_cb *task = &sched_state.tasks[i];
			LOG_INF("  %s: state=%d, slices=%u, runtime=%llu us",
			        task->name, task->state, task->slice_count,
			        task->total_runtime);
		}
	}
}
