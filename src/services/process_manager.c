/**
 * @file process_manager.c
 * @brief AkiraOS Process Manager Implementation
 */

#include "process_manager.h"
#include <string.h>

static akira_process_t processes[MAX_PROCESSES];
static int process_count = 0;
static uint32_t next_pid = 1;

int process_manager_launch(const akira_process_t *process)
{
	if (!process || process_count >= MAX_PROCESSES)
		return -1;
	processes[process_count] = *process;
	processes[process_count].pid = next_pid++;
	processes[process_count].running = true;
	process_count++;
	return processes[process_count - 1].pid;
}

int process_manager_stop(uint32_t pid)
{
	for (int i = 0; i < process_count; ++i)
	{
		if (processes[i].pid == pid)
		{
			processes[i].running = false;
			return 0;
		}
	}
	return -1;
}

int process_manager_status(uint32_t pid)
{
	for (int i = 0; i < process_count; ++i)
	{
		if (processes[i].pid == pid)
		{
			return processes[i].running ? 1 : 0;
		}
	}
	return -1;
}

int process_manager_list(akira_process_t *out_list, int max_count)
{
	if (!out_list || max_count <= 0)
		return -1;
	int count = (process_count < max_count) ? process_count : max_count;
	memcpy(out_list, processes, sizeof(akira_process_t) * count);
	return count;
}
