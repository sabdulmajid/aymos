# AymOS Functionality Overview

This document explains the main modules of AymOS and how they work together. It supplements the information in the README.

> **Status:** `APP=lifecycle` links and tests the narrow lifecycle kernel
> described below. EDF timing semantics and `src/memory.c` remain experimental
> and are not claimed as complete.

## Kernel

The kernel (`kernel/src/kernel.c`) owns task lifecycle state and a small ready
selection rule. `arch/arm_cm4/context_switch.S` supplies the SVC veneer and
PendSV save/restore path. Key functions include:

- `os_kernel_init` – initializes task state, the idle task, stack alignment, and
  exception priorities.
- `os_task_create` – configures a pre-start task in an eight-byte-aligned fixed
  stack slot.
- `os_kernel_start` – enters the first task through SVC and PendSV.
- `os_yield` and `os_sleep` – request lifecycle transitions through SVC.
- `os_task_exit` – is reached by the entry trampoline when a task returns.

PendSV saves/restores R4-R11 on PSP while its C state-commit helper runs on MSP.
An exiting stack slot is reclaimed only in that handler context. The lifecycle
Renode test independently exercises voluntary and SysTick-caused switches and
assembly register probes verify R4-R11 preservation. The current deadline and
priority comparison is not the explicit EDF timing model promised by PR 4.

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
The current slice adds automated host and Renode boot tests. Later PRs replace
these manual kernel experiments with scheduler, lifecycle, and allocator tests.

## Next Steps

For the verified workflow, use `make setup`, `make firmware`, `make test`,
`make test-emulator`, and `make test-lifecycle`, then read
`docs/BUILDING.md`. Use `make run-lifecycle` to print the exact guest lifecycle
stream.
