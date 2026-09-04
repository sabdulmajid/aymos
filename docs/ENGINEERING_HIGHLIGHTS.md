# AymOS engineering highlights

This document points to the parts of AymOS that contain the most important
systems work. Each section explains the problem, the design, and the evidence
that checks the result.

## Cortex-M4 context switching

The context-switch path is intentionally short. The
[assembly handler](../arch/arm_cm4/context_switch.S) contains only the work
that must occur at the processor boundary.

- SVC reads the active exception-frame stack and passes the frame to C.
- PendSV saves registers `r4` through `r11` on the current PSP stack.
- C selects the next task and returns its saved PSP value.
- PendSV restores `r4` through `r11`, selects PSP for thread mode, and uses
  exception return value `0xFFFFFFFD`.
- Interrupts stay disabled while the scheduler changes the active context.

The processor creates the other half of the exception frame. Together, these
frames preserve the task registers and keep each task on its own stack. The
[lifecycle test](../apps/lifecycle/main.c) checks first dispatch, voluntary
yield, preemption, task return, exit, and continued execution.

Why this matters: the project performs a real Cortex-M context switch instead
of calling task functions in sequence from a host program.

## Explicit task lifecycle

The [kernel](../kernel/src/kernel.c) owns every task-state transition. Task
creation starts with `os_task_config_default()`, which gives the caller valid
defaults before the caller changes timing fields.

The initial stack frame places the task argument in `r0`. It also places a
trampoline in the return path. If a task function returns, the trampoline
calls `os_task_exit()`. The kernel changes the task to its exit state and
reclaims the stack only after another stack is active.

The public [kernel API](../kernel/include/aymos_kernel.h) also exposes sleep,
periodic wait, yield, task information, monotonic time, and memory statistics.

Why this matters: normal C task functions can return safely, and task slots can
be reused without freeing the stack that still executes the exit path.

## Deterministic EDF scheduling

The [scheduler](../kernel/src/scheduler.c) keeps release time, wake time,
period, relative deadline, absolute deadline, execution budget, and accounting
state in separate fields.

It selects work in this order:

1. Earliest absolute deadline.
2. Lowest numeric priority when deadlines are equal.
3. Lowest task ID as a stable final tie breaker.
4. Idle when no application task is ready.

A 64-bit monotonic order keeps decisions correct when the public 32-bit tick
value wraps. The scheduler records a deadline miss once for each active job.
It also counts a periodic release that arrives while the previous job is still
active.

The [native scheduler tests](../tests/native/test_scheduler.c) cover deadline
order, ties, sleep and wake, periodic releases, deadline misses, tick wrap,
idle selection, and task-slot reuse.

Why this matters: scheduler behavior is defined by explicit rules and remains
repeatable at boundary conditions.

## Structured kernel trace

The [trace schema](../kernel/include/aymos_trace.h) defines versioned 32-byte
records. Events cover task creation, release, selection, preemption, yield,
sleep, wake, exit, context switches, idle state, deadlines, and memory use.

The [trace ring](../kernel/src/trace.c) accepts fixed-size records without
formatted text. A record contains its tick, sequence number, task IDs, event
type, and three event values. Selection events include the candidate state
that explains an EDF decision.

The ring has a fixed capacity. It counts attempted, emitted, and dropped
records. It can emit an overflow event when space becomes available. The final
footer records these counts and the final tick.

Why this matters: the report explains what the kernel knew when it made a
decision. It does not have to infer missing scheduler state after the run.

## Task-owned allocator

The [allocator](../kernel/src/allocator.c) uses an explicit memory arena. It
does not share that arena with newlib. It provides:

- eight-byte alignment for metadata and payloads;
- next-fit allocation with split and exact-fit handling;
- adjacent free-block coalescing;
- heap-boundary and linked-block validation;
- allocation markers and owner checks;
- invalid-free and failed-allocation counters; and
- usage, high-watermark, and fragmentation statistics.

Kernel entry points disable interrupts while they update allocator state. Task
exit releases all allocations that belong to that task.

The [allocator tests](../tests/native/test_allocator.c) use address and
undefined-behavior checks for split, exact fit, exhaustion, invalid free,
ownership, coalescing, and fragmentation behavior.

Why this matters: task stacks and dynamic task memory use one checked ownership
model with observable failure data.

## Cortex-M4 fixed-point DSP

The [M4 FIR implementation](../dsp/src/fir_q15_m4.c) processes signed Q15 data
without allocation or floating-point state. It packs two 16-bit products into
each `SMLALD` operation and uses `SSAT` for the final signed 16-bit result.

The [scalar implementation](../dsp/src/fir_q15_scalar.c) is the readable
reference. Native tests compare both paths with an independent implementation
for tap counts from 1 through 64. The board-targeted workload then checks 452
outputs and a fixed output CRC-32 value of `0xAFC277C1`.

The code-generation check requires the packed instructions inside the exact M4
function bounds. The execution record confirms 3,616 packed MAC instructions
for 7,232 products.

Why this matters: AymOS runs useful integer processing and provides direct
evidence that the architecture-specific instructions executed.

## Reproducible build inputs

The [dependency lock](../tools/setup/dependencies.lock) records versions,
source revisions, archive sizes, and SHA-256 values for the ARM toolchain,
STM32 sources, Renode, Python, and Python packages.

The [setup tool](../tools/setup.sh) installs these inputs inside the repository.
It checks executable hashes, dependency state, file types, symbolic-link
targets, and complete directory manifests. Build commands reject an incomplete
or changed installation before compilation starts.

Firmware validation checks the ARM architecture, soft-float ABI, vector table,
entry point, flash and RAM ranges, load segments, heap boundaries, and stack
reservation. Each completed workload stores its command, tool versions, Git
commit, firmware, linker map, trace, result, and artifact hashes.

Why this matters: another developer can inspect both the program result and
the exact inputs that produced it.

## Safe report publication

The [report publisher](../tools/aymos_lab/performance_report.py) validates its
input schema, byte counts, hashes, and execution evidence before it creates a
report. It writes into a private marked directory and publishes the complete
directory with one rename.

File reads use directory descriptors and no-follow rules. Retention rechecks
the selected directory, marker, inode, device, and Linux mount ID before it
removes an old report.

Why this matters: a partial or replaced evidence directory cannot become a
trusted report, and cleanup stays inside the owned run directory.
