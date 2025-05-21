# AymOS API Reference

This document describes the public functions provided by AymOS. Each section lists the function prototype, its parameters, the return value and any notes about its usage.

## Task Management

### `void osKernelInit(void)`
Initialise the kernel. Must be called before any other OS function. Returns nothing.

### `int osCreateTask(TCB* task)`
Create a task with default deadline. The `task` structure must contain a pointer to the task function, stack size and initial priority. Returns `RTX_OK` on success or `RTX_ERR` on failure.

### `int osCreateDeadlineTask(int deadline, TCB* task)`
Create a task with an explicit deadline in milliseconds. The deadline sets the period for periodic tasks. Returns `RTX_OK` on success or `RTX_ERR` on error.

### `int osKernelStart(void)`
Start executing tasks. This switches the processor to the first ready task. Returns `RTX_OK` if the kernel started correctly.

### `void osYield(void)`
Yield the processor voluntarily. The scheduler selects the next ready task according to the deadline scheduler.

### `void osSleep(int timeInMs)`
Suspend the calling task for the given amount of time. The task automatically becomes ready again after the interval expires.

### `void osPeriodYield(void)`
Yield until the next period of the task, calculated from its deadline.

### `int osSetDeadline(int deadline, task_t TID)`
Update the deadline of an existing task. Returns `RTX_OK` on success.

### `int osSetPriority(uint8_t priority, task_t TID)`
Change the priority of a task. Lower numeric values represent higher priority. Returns `RTX_OK` on success.

### `task_t osGetTID(void)`
Return the identifier of the currently running task. Returns `TID_NULL` if the kernel is not running.

### `int osTaskInfo(task_t TID, TCB* task_copy)`
Fill `task_copy` with information about a task. Returns `RTX_OK` if successful.

### `int osTaskExit(void)`
Terminate the calling task and release its resources.

## Memory Management

### `int k_mem_init(void)`
Initialise the heap. Called by `osKernelInit` during start-up. Returns `RTX_OK` on success.

### `void* k_mem_alloc(unsigned int size)`
Allocate a block of memory from the heap. The returned address is word aligned. Returns `NULL` if allocation fails.

### `int k_mem_dealloc(void* ptr)`
Free a block previously allocated with `k_mem_alloc`. Returns `RTX_OK` on success or `RTX_ERR` on invalid input.

### `int k_mem_count_extfrag(unsigned int size)`
Return the number of free blocks that are too small to satisfy an allocation of `size` bytes.

