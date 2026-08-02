# AymOS Functionality Overview

This document explains the main modules of AymOS and how they work together. It supplements the information in the README.

> **Status:** `APP=lifecycle` and `APP=edf` link and test the kernel below.
> `src/memory.c` remains experimental and is not claimed as complete.

## Kernel

The kernel (`kernel/src/kernel.c`) coordinates Cortex-M lifecycle state with the
portable policy in `kernel/src/scheduler.c`.
`arch/arm_cm4/context_switch.S` supplies SVC and PendSV. Key functions include:

- `os_kernel_init` – initializes task state, the idle task, stack alignment, and
  exception priorities.
- `os_task_create` – configures a pre-start task in an eight-byte-aligned fixed
  stack slot.
- `os_kernel_start` – enters the first task through SVC and PendSV.
- `os_yield`, `os_sleep`, and `os_wait_next_period` – request transitions
  through SVC.
- `os_task_exit` – is reached by the entry trampoline when a task returns.

PendSV saves/restores R4-R11 on PSP while its C state-commit helper runs on MSP.
An exiting stack slot is reclaimed only in that handler context. The lifecycle
Renode test independently exercises voluntary and SysTick-caused switches and
assembly register probes verify R4-R11 preservation.

The scheduling record separates releases, wake, period, relative/absolute
deadline, execution accounting, completion, deadline misses, unavailable
releases, and job sequence. One 64-bit monotonic domain qualifies all release,
wake, and absolute-deadline values; 32-bit ticks are low-word display values.
EDF orders READY active jobs by strict full deadline, priority, and stable task
ID, including across wrap and for long-overdue jobs. Native ASan/UBSan tests
compile the same policy source. `APP=edf` proves periodic release/preemption in
real ARM firmware; it is not a host scheduler.

## Memory Management

AymOS provides a small allocator in `src/memory.c`. Blocks are stored in a linked list located directly in the heap. The allocator tracks which task owns each block and merges adjacent free blocks to reduce fragmentation.

Important API functions:

- `k_mem_init` – sets up the heap based on the linker symbols for the stack and image end.
- `k_mem_alloc` – allocates aligned memory, splitting blocks when needed.
- `k_mem_dealloc` – frees a block and merges neighboring free regions.
- `k_mem_count_extfrag` – counts how many free blocks are too small for a requested size.

## Startup and HAL

The active build uses the pinned CMSIS-device vendor
`startup_stm32f401xe.s` and `system_stm32f4xx.c` sources together with project
board support. The vendor startup assembly establishes the vector table and
initial stack pointer; the system source and board support initialize the
Cortex-M4 and required peripherals. The legacy `stm-startup/` and `src/`
startup/system files are not linked into the active firmware.

## Tests

Several small test programs under `src/tests` demonstrate the kernel and memory system:

- `create_task_test.c` – creates tasks and monitors state transitions.
- `allocation_timing_test.c` – measures memory allocation performance.
- `periodic_test.c` – exercises periodic task behaviour.

These files are manual firmware experiments with separate `main` functions;
they are not selected by the current build and are not an automated test suite.
The current slice includes automated native scheduler tests plus Renode boot,
lifecycle, and deterministic EDF tests. Automated allocator coverage remains
deferred to PR 5; the manual experiments above remain unlinked.

## Next Steps

For the verified workflow, use `make setup`, `make firmware`, `make test`,
`make test-emulator`, `make test-lifecycle`, and `make test-edf`, then read
`docs/BUILDING.md`. `make run-lifecycle` and `make run-edf` print exact
guest-generated streams.
