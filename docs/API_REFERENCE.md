# AymOS API Reference

This document describes the public functions provided by AymOS. Each section lists the function prototype, its parameters, the return value and any notes about its usage.

> **Status:** The task APIs below are linked and tested by `APP=lifecycle` and
> `APP=edf`. The allocator section remains an inventory of unlinked legacy code.

## Task Management

### `int os_kernel_init(void)`
Initialize the task table, fixed stack slots, idle task, stack-alignment control,
and system-exception priorities. Returns `1` on success and `0` if already
initialized or running.

### `int os_task_create(const os_task_config_t *config, os_task_id_t *created_id)`
Create a task before kernel start. The entry, argument, eight-byte-multiple stack
size, and nested `timing` configuration are explicit. Stack sizes range from
256 through 1024 bytes. Returns `1` on success and `0` for invalid input or no
slot. Runtime creation is not yet supported.

### `os_task_config_t os_task_config_default(os_task_entry_t entry, void *argument)`
Return a complete deterministic configuration: one-shot, immediate release,
one-tick relative deadline, no period, no enforced budget, priority 128, and a
1024-byte stack. Callers copy this value and override public configuration
fields; they never initialize internal TCB/scheduler state.

One-shot tasks require `period_ticks == 0`. Periodic tasks require a nonzero
period and constrained deadline (`relative_deadline_ticks <= period_ticks`).
Relative deadlines are nonzero. Initial release delay, period, deadline,
execution budget, and sleep intervals may not exceed `INT32_MAX`; initial delay
plus relative deadline must also fit that half-range. Lower numeric priority
wins only after equal deadlines.

### `void os_kernel_start(void)`
Start scheduling through SVC and PendSV and enter privileged thread mode on PSP.
This function does not return; an invalid start produces a UART panic.

### `void os_yield(void)`
Voluntarily offer the processor to another ready task through SVC. If no other
user task is ready, the calling task resumes.

### `int os_sleep(uint32_t ticks)`
Suspend the calling task's current job for 1 through `INT32_MAX` ticks.
Returns `0` without issuing SVC for zero or a value outside that half-range;
returns `1` after a valid sleep completes. Sleep preserves the active job,
release, absolute deadline, and accounting. Wake occurs at the full 64-bit
`wake_order`; `wake_tick` is its wrapping low-word display.

### `int os_wait_next_period(void)`
Complete the current periodic job and wait for its next scheduled release.
Returns `0` without SVC for a non-periodic/invalid caller and `1` after the next
job is dispatched. Cadence is based on scheduled release plus period, not
completion time. One job may be active. A release reached before completion
increments `missed_release_count`, advances cadence, and does not queue an
overlapping job.

### `os_task_id_t os_current_task(void)`
Return the current task identifier, or `OS_TASK_ID_INVALID` before dispatch.

### `int os_task_info(os_task_id_t id, os_task_info_t *info)`
Copy the task's state/kind, release/wake ticks, period, relative/absolute
deadlines, configured budget, execution/completion/miss/release/job counters,
priority, and active-job flag under a short interrupt critical section. The
`*_order` fields are full 64-bit monotonic values; the corresponding `*_tick`
fields are their low 32 bits for display. Returns `1` on success and `0` for an
invalid or dormant task.

`os_task_info`, `os_tick_count`, and `os_monotonic_tick_count` are thread-mode
task-inspection APIs. Their PRIMASK-protected snapshots prevent task/SysTick
interleaving, but are not a general inspection contract for arbitrary
higher-priority ISR callers. General ISR-safe inspection APIs are deferred.

### `void os_task_exit(void)`
Terminate the calling user task through SVC. The task-entry trampoline invokes
this automatically when an entry function returns. It never returns to the old
PSP; PendSV reclaims the stack slot while executing on MSP.

### Lifecycle inspection helpers

`os_tick_count`, `os_monotonic_tick_count`, `os_last_reclaimed_task`,
`os_reclaim_count`, and `os_thread_uses_psp` provide bounded diagnostic
evidence. The first returns the low 32 bits; the second snapshots the full
scheduling time. They are not the structured tracing API planned for PR 6.

### Scheduling and time semantics

`OS_TASK_WAITING_RELEASE` has no active job; `OS_TASK_SLEEPING` suspends an
active job; `OS_TASK_EXITING` is the reclamation transition; and
`OS_TASK_DORMANT` is an unconfigured/reclaimed slot. One-shot return ends its
only job. Periodic entries stay on the same stack across
`os_wait_next_period` and may eventually return to terminate.

Relative configuration remains half-range bounded, but runtime comparisons do
not use signed 32-bit deltas. The kernel advances one 64-bit monotonic time and
stores full release, wake, and absolute-deadline keys. Due checks use `>=`; EDF
uses strict `<` on the full deadline key, then lower numeric priority and lower
task ID. This remains asymmetric and deterministic when an active job is more
than `2^31` ticks overdue or the visible low word wraps. An unfinished job is
missed at equality with its full absolute deadline and latched once. Idle wins
only when no user job is eligible. Exhausting `UINT64_MAX` is a fail-stop kernel
panic rather than wrapping the ordering domain. `execution_budget_ticks` is
observable metadata, not an enforced limit or admission-control result.

## Memory Management

### `int k_mem_init(void)`
Initialise the heap. Called by `osKernelInit` during start-up. Returns `RTX_OK` on success.

### `void* k_mem_alloc(unsigned int size)`
Allocate a block of memory from the heap. The returned address is word aligned. Returns `NULL` if allocation fails.

### `int k_mem_dealloc(void* ptr)`
Free a block previously allocated with `k_mem_alloc`. Returns `RTX_OK` on success or `RTX_ERR` on invalid input.

### `int k_mem_count_extfrag(unsigned int size)`
Return the number of free blocks that are too small to satisfy an allocation of `size` bytes.
