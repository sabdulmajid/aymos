# AymOS

AymOS is a compact real-time operating system for the ARM Cortex-M4. It
targets the STM32F401RE microcontroller on the NUCLEO-F401RE board.

AymOS runs tasks that have release times and deadlines. Its scheduler selects
the ready task with the earliest deadline. The firmware records each important
kernel event, so a developer can see which task ran, when it ran, and why the
kernel selected it.

A task is a C function with its own stack and execution state. The operating
system decides which task can use the processor. An absolute deadline is the
tick when one released job must be complete.

## The problem

An embedded controller often has to sample inputs, calculate a result, and
send status data on one processor. Each operation must finish at the correct
time. A late operation can make the full system incorrect, even when every
calculation gives the correct value.

These failures are difficult to inspect. Task switches occur inside processor
exception handlers, and normal log messages do not show enough scheduler
state. AymOS makes this execution path visible and repeatable.

## The solution

AymOS combines a small Cortex-M4 kernel with structured execution evidence:

- A preemptive Earliest Deadline First (EDF) scheduler selects ready tasks.
- A fixed priority resolves a tie between equal deadlines.
- Tasks can start, yield, sleep, wake, return, and exit.
- SVC starts task execution, SysTick updates time, and PendSV switches tasks.
- Each task uses an aligned Process Stack Pointer (PSP) stack.
- Exception handlers use the Main Stack Pointer (MSP).
- A task-owned allocator supplies aligned memory and reports heap usage and
  fragmentation.
- A bounded binary trace records task, scheduler, deadline, idle, and memory
  events.
- Host tools validate the trace and create a standalone scheduling timeline.
- A fixed-point FIR workload demonstrates useful Cortex-M4 DSP instructions.
- Project-local, pinned tools make the build independent of an installed ARM
  compiler or Python environment.

The implementation stays small on purpose. A developer can follow the path
from the public C API to the scheduler and then to the context-switch assembly.

## Why use AymOS

AymOS is a complete path for a small, deadline-driven embedded application. A
developer writes ordinary C task functions, assigns their timing requirements,
and lets the kernel control processor time. The same run produces a report
that explains the result.

A typical use case is a device that samples a sensor, updates a controller,
and sends telemetry at different rates. AymOS keeps these operations separate,
runs the most urgent ready work first, and records enough state to explain a
late result.

| Development need | AymOS value |
| --- | --- |
| Run several timed operations on one microcontroller | Independent tasks with periods, deadlines, priorities, and stacks |
| Understand an unexpected task order | Scheduler events include the selected task, candidates, deadlines, and reason |
| Reproduce a result on another Linux computer | Pinned tools, checked dependencies, fixed workloads, and recorded commands |
| Test a useful processor-specific workload | A Q15 FIR filter with portable and Cortex-M4 implementations |
| Extend the kernel with confidence | Native policy tests and ARM firmware checks cover the same core behavior |

To explore the system, change a task function or its `os_task_config_t`
settings, run the demonstration again, and compare the new timeline with the
previous run. You can change release times, periods, deadlines, priorities,
and work demand without changing the scheduler. The
[scheduling guide](docs/SCHEDULING_DEMO.md) gives the exact entry points and
commands.

## Quick start

Use a Linux x86-64 computer with Git, GNU Make, curl, and a C compiler. Then
run one command:

```sh
make quickstart
```

The first run obtains and checks the pinned ARM toolchain, STM32 sources,
Renode, and Python environment. It then builds two NUCLEO-F401RE firmware
images, runs them, checks their structured traces, and creates an HTML report.

Open the printed `runs/<run-id>/index.html` file in a browser. The report does
not need a server or a network connection.

## What the demonstration does

The firmware runs four application tasks and one idle task:

| Task | Purpose | Release pattern | Deadline | Normal work |
| --- | --- | --- | ---: | ---: |
| Sampler | Acquire a periodic input | Every 6 ticks | 3 ticks | 1 tick |
| Controller | Process the input | Every 10 ticks | 8 ticks | 2 ticks |
| Telemetry | Prepare status data | Every 18 ticks | 14 ticks | 1 tick |
| Load | Apply controlled CPU demand | Once | 12 ticks | 1 tick |
| Idle | Wait when no task is ready | As needed | None | As needed |

The command runs the same workload in two modes. Normal mode gives the load
task one work tick. Overload mode gives it eight work ticks. No other task
configuration changes.

The deterministic result is:

| Metric | Normal | Overload |
| --- | ---: | ---: |
| Released jobs | 8 | 8 |
| Context switches | 13 | 11 |
| Preemptions | 4 | 2 |
| Idle ticks | 11 | 4 |
| Deadline misses | 0 | 1 |

In overload mode, the load task reaches its absolute deadline at tick 12
before it completes. The timeline shows its release, each running interval,
the sampler preemption, the EDF candidate data, and the first deadline miss.
The host report uses events from the Cortex-M4 firmware. It does not create a
replacement schedule.

## Useful Cortex-M4 computation

The repository also contains an allocation-free, 16-tap Q15 finite impulse
response (FIR) filter. Q15 represents a signed fraction in a 16-bit integer.
This format is common when an embedded target must process sampled data without
floating-point state.

The workload processes four frames of 128 samples. It compares a scalar C
implementation with a Cortex-M4 implementation that uses packed `SMLALD`
multiply-accumulate instructions and `SSAT` saturation instructions. Both
implementations produce 452 outputs with the same CRC-32 value,
`0xAFC277C1`. The execution record confirms 7,232 scalar MAC instructions or
3,616 packed dual-lane MAC instructions for the same 7,232 products.

This gives the project a practical computation, a portable reference, and an
inspectable architecture-specific implementation. The instruction report is
functional evidence. Board measurement tools can use the same firmware hash
when they add cycle and latency data.

## How the kernel runs a task

ARM Cortex-M processors separate normal task execution from exception-handler
execution. AymOS uses that separation as follows:

1. Reset code prepares memory and calls `main()` on MSP.
2. The application creates task control blocks and aligned task stacks.
3. `osKernelStart()` requests an SVC exception. SVC restores the first task
   frame and changes thread execution to PSP.
4. SysTick advances the kernel tick. It releases periodic or sleeping tasks
   when their wake tick arrives.
5. EDF compares the absolute deadlines of all ready tasks. It records the
   candidates and the selection reason.
6. When another task must run, the kernel requests PendSV. PendSV saves the
   current task registers and restores the next task registers.
7. Processor exception return restores the hardware frame and resumes the
   selected task on its own PSP stack.
8. If a task function returns, a trampoline calls `osTaskExit()`. The kernel
   reclaims the old stack only after execution has moved to another stack.
9. The idle task executes when no application task is ready.

This path exercises the Cortex-M4 vector table, SVC, PendSV, SysTick, PSP,
MSP, exception return, and the EDF scheduler in the board-targeted firmware.

## Engineering highlights

The repository contains several focused systems-engineering examples:

- [Cortex-M4 context switching](arch/arm_cm4/context_switch.S) preserves the
  task context through SVC and PendSV with separate MSP and PSP stacks.
- [EDF scheduling](kernel/src/scheduler.c) uses explicit timing fields, a
  64-bit order across 32-bit tick wrap, deterministic ties, and an idle
  fallback.
- [Task lifecycle management](kernel/src/kernel.c) handles initial exception
  frames, task arguments, returned task functions, and deferred stack reclaim.
- [Structured tracing](kernel/src/trace.c) writes fixed-size events to a
  bounded ring without formatted output in interrupt-sensitive paths.
- [Allocator hardening](kernel/src/allocator.c) provides eight-byte alignment,
  ownership checks, coalescing, boundary validation, and fragmentation data.
- [Cortex-M4 DSP](dsp/src/fir_q15_m4.c) uses packed integer operations while a
  scalar implementation supplies a clear correctness reference.
- [Reproducible setup](tools/setup.sh) checks tool archives, executable hashes,
  dependency revisions, and project-local Python packages.

See [Engineering highlights](docs/ENGINEERING_HIGHLIGHTS.md) for the design
choices, source paths, and tests behind each item.

## Evidence and quality checks

Each completed run keeps the exact firmware ELF and linker map, build
configuration, tool versions, command line, UART capture, binary trace,
decoded events, summary metrics, HTML timeline, and emulator log. Metadata
binds the result to its Git commit and records SHA-256 hashes for the artifacts.

Automated checks cover:

- firmware format, vector table, memory ranges, stack alignment, and soft-float
  ABI;
- task dispatch, yield, preemption, sleep, wake, return, exit, and stack
  reclamation;
- EDF order, priority ties, deadline accounting, periodic releases, idle
  selection, and tick wraparound;
- allocator alignment, invalid frees, coalescing, exhaustion, ownership, and
  fragmentation statistics;
- trace framing, event order, bounds, corruption, and overflow behavior; and
- FIR correctness, scalar and packed equivalence, and Cortex-M4 instruction
  selection.

## Documentation

- [Build, test, and command reference](docs/BUILDING.md)
- [Scheduling demonstration](docs/SCHEDULING_DEMO.md)
- [Fixed-point DSP workload](docs/DSP_WORKLOAD.md)
- [Kernel functionality](docs/FUNCTIONALITY_OVERVIEW.md)
- [Engineering highlights](docs/ENGINEERING_HIGHLIGHTS.md)
- [Trace format](docs/TRACE_FORMAT.md)
- [Implementation decisions](docs/IMPLEMENTATION_PLAN.md)
