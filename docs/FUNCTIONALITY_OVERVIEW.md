# AymOS Functionality Overview

This document explains the main modules of AymOS and how they work together. It supplements the information in the README.

> **Status:** The kernel and allocator below describe legacy experimental
> source. PR 1 builds only the F401RE boot application and board support. It
> does not link or validate task switching, EDF timing, or the allocator.

## Kernel

The kernel (`src/kernel.c`) is responsible for task management and scheduling. It maintains an array of Task Control Blocks (TCBs) representing each task's stack, priority, and state. Tasks are scheduled using an Earliest Deadline First (EDF) policy. Key functions include:

- `osKernelInit` – sets up the kernel data structures and the idle task.
- `osCreateTask` / `osCreateDeadlineTask` – allocate a stack, initialize the TCB, and add the task to the scheduler.
- `osKernelStart` – starts executing tasks.
- `osYield`, `osSleep`, and `osPeriodYield` – cause context switches through supervisor calls.
- `osTaskExit` – remove a task and free its resources.

Context switching is performed in `src/svc_handler.s` using ARM SVC and PendSV exceptions.

## Memory Management

AymOS provides a small allocator in `src/memory.c`. Blocks are stored in a linked list located directly in the heap. The allocator tracks which task owns each block and merges adjacent free blocks to reduce fragmentation.

Important API functions:

- `k_mem_init` – sets up the heap based on the linker symbols for the stack and image end.
- `k_mem_alloc` – allocates aligned memory, splitting blocks when needed.
- `k_mem_dealloc` – frees a block and merges neighboring free regions.
- `k_mem_count_extfrag` – counts how many free blocks are too small for a requested size.

## Startup and HAL

The `stm-startup` directory and files like `src/system_stm32f4xx.c` configure the ARM Cortex‑M hardware. The startup assembly establishes the vector table and initial stack pointer, while the system file sets up clocks and peripherals.

## Tests

Several small test programs under `src/tests` demonstrate the kernel and memory system:

- `create_task_test.c` – creates tasks and monitors state transitions.
- `allocation_timing_test.c` – measures memory allocation performance.
- `periodic_test.c` – exercises periodic task behaviour.

These files are manual firmware experiments with separate `main` functions;
they are not selected by the current build and are not an automated test suite.
The staged campaign will replace them with native and Renode tests.

## Next Steps

For the verified build workflow, use `make setup && make firmware` and read
`docs/BUILDING.md`. Study the legacy exception handlers as prototype code, not
as a currently verified context-switch implementation.
