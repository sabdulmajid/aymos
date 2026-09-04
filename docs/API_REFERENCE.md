# AymOS API Reference

This document describes the public functions provided by AymOS. Each section lists the function prototype, its parameters, the return value and any notes about its usage.

> **Status:** The snake-case task APIs below are linked and tested by
> `APP=lifecycle`. Their scope is deliberately limited to the PR 3 lifecycle
> contract. The allocator section remains an inventory of unlinked legacy code.

## Task Management

### `int os_kernel_init(void)`
Initialize the task table, fixed stack slots, idle task, stack-alignment control,
and system-exception priorities. Returns `1` on success and `0` if already
initialized or running.

### `int os_task_create(const os_task_config_t *config, os_task_id_t *created_id)`
Create a task before kernel start. The entry, argument, eight-byte-multiple stack
size, nonzero `deadline_ticks`, and priority are explicit. PR 3 accepts stack
sizes from 256 through 1024 bytes and assigns one fixed slot per task. Returns
`1` on success and `0` for invalid input or no slot. Runtime creation is not yet
supported.

### `void os_kernel_start(void)`
Start scheduling through SVC and PendSV and enter privileged thread mode on PSP.
This function does not return; an invalid start produces a UART panic.

### `void os_yield(void)`
Voluntarily offer the processor to another ready task through SVC. If no other
user task is ready, the calling task resumes.

### `int os_sleep(uint32_t ticks)`
Put the calling task to sleep for an interval from 1 through `INT32_MAX` ticks.
Returns `0` without issuing SVC for zero or a value outside that half-range;
returns `1` after a valid sleep completes. Within this deliberately bounded PR
3 contract, SysTick uses signed-delta comparison safely across counter wrap and
a newly awakened task can request PendSV preemption. PR 4 owns the final timing
model and its full boundary test matrix.

### `os_task_id_t os_current_task(void)`
Return the current task identifier, or `OS_TASK_ID_INVALID` before dispatch.

### `int os_task_info(os_task_id_t id, os_task_info_t *info)`
Copy the task's ID, lifecycle state, current deadline field, and priority under a
short interrupt critical section. Returns `1` on success and `0` for invalid or
dormant tasks.

### `void os_task_exit(void)`
Terminate the calling user task through SVC. The task-entry trampoline invokes
this automatically when an entry function returns. It never returns to the old
PSP; PendSV reclaims the stack slot while executing on MSP.

### Lifecycle inspection helpers

`os_tick_count`, `os_last_reclaimed_task`, `os_reclaim_count`, and
`os_thread_uses_psp` provide bounded diagnostic evidence for the lifecycle
workload. They are not the structured tracing API planned for PR 6.

## Memory Management

### `int k_mem_init(void)`
Initialise the heap. Called by `osKernelInit` during start-up. Returns `RTX_OK` on success.

### `void* k_mem_alloc(unsigned int size)`
Allocate a block of memory from the heap. The returned address is word aligned. Returns `NULL` if allocation fails.

### `int k_mem_dealloc(void* ptr)`
Free a block previously allocated with `k_mem_alloc`. Returns `RTX_OK` on success or `RTX_ERR` on invalid input.

### `int k_mem_count_extfrag(unsigned int size)`
Return the number of free blocks that are too small to satisfy an allocation of `size` bytes.
