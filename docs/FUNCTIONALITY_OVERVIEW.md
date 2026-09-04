# AymOS Functionality Overview

This document explains the main modules of AymOS and how they work together. It supplements the information in the README.

> **Status:** `APP=lifecycle`, `APP=edf`, and `APP=allocator` link and test the
> kernel below. The allocator core is also compiled unchanged into native
> ASan/UBSan tests.

## Kernel

The kernel (`kernel/src/kernel.c`) coordinates Cortex-M lifecycle state with the
portable policy in `kernel/src/scheduler.c`.
`arch/arm_cm4/context_switch.S` supplies SVC and PendSV. Key functions include:

- `os_kernel_init` – initializes task state, the idle task, stack alignment, and
  exception priorities.
- `os_task_create` – configures a pre-start or thread-mode runtime task in an
  eight-byte-aligned fixed stack slot.
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

`kernel/src/allocator.c` is a small portable next-fit allocator over the exact
linker-exported AymOS heap. Every payload is eight-byte aligned. Metadata uses
`sizeof`, partitions the arena contiguously, and is checked for bounds,
alignment, markers, state, size, and links before an operation follows it.
Allocation splits only when the remainder can hold metadata plus an aligned
payload; free coalesces adjacent blocks.

Kernel wrappers tag blocks with the running task ID and mask interrupts around
metadata operations. Foreign-owner, invalid, interior, and double frees fail.
Task return is completed in PendSV on MSP: a bounded owner sweep frees remaining
buffers before the task ID becomes reusable. Fixed task stacks remain separate
from the dynamic heap. This prevents outstanding blocks from being inherited by
the reused slot, but task-ID tags do not detect stale pointers by task
generation; pointers retained after free or task exit are invalid and must not
be reused. Statistics expose aligned usage, free/largest blocks, fragment
counts, a high-water mark, and saturating operation/error counts. Newlib cannot
contend for the region because its linker interval is empty and `_sbrk` always
fails. Ownership tags are bookkeeping, not hardware-enforced isolation.

## Startup and HAL

The active build uses the pinned CMSIS-device vendor
`startup_stm32f401xe.s` and `system_stm32f4xx.c` sources together with project
board support. The vendor startup assembly establishes the vector table and
initial stack pointer; the system source and board support initialize the
Cortex-M4 and required peripherals. The legacy `stm-startup/` and `src/`
startup/system files are not linked into the active firmware.

## Tests

Several old, unselected programs under `src/tests` demonstrate early kernel experiments:

- `create_task_test.c` – creates tasks and monitors state transitions.
- `periodic_test.c` – exercises periodic task behaviour.

These files are manual firmware experiments with separate `main` functions;
they are not selected by the current build and are not an automated test suite.
The current slice includes automated native scheduler and allocator tests plus
Renode boot, lifecycle, deterministic EDF, and allocator/task-reuse tests. The
obsolete `k_mem` implementation and its three manual allocator programs were
retired in PR 5 so the repository does not present two allocator contracts.

## Next Steps

For the verified workflow, use `make setup`, `make firmware`, `make test`,
`make test-emulator`, `make test-lifecycle`, `make test-edf`, and
`make test-allocator`, then read
`docs/BUILDING.md`. `make run-lifecycle` and `make run-edf` print exact
guest-generated streams; `make run-allocator` prints the allocation stress
contract.
