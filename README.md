# AymOS Real-Time Systems Lab

AymOS is becoming a small, inspectable Cortex-M4 real-time systems lab. The
current verified slice builds board-targeted images reproducibly, boots the
exact ELF headlessly in pinned Renode, and runs a deterministic Cortex-M4 task
lifecycle through SVC, PendSV, PSP, and SysTick. The same kernel now applies an
explicit, wrap-safe EDF timing policy to a deterministic periodic workload.
Task-owned dynamic memory now uses the same hardened educational allocator in
native sanitizer tests and real Cortex-M4 firmware. A bounded structured trace
explains the same guest kernel's decisions without UART in producer paths.
The Deadline Lab now turns those parts into one visible workload and a
standalone scheduling report.

## Deadline Lab

```sh
make setup
make demo
```

`make demo` builds and runs normal and overload firmware. Both images use the
real Cortex-M4 kernel path. The command checks each structured trace and writes
standalone HTML reports under `runs/<run-id>/`.

The normal workload completes eight jobs with no deadline miss. The overload
workload changes only the load task from one to eight execution ticks. It
records the first miss at tick 12. The timeline shows releases, execution,
preemptions, idle intervals, absolute deadlines, misses, and the emitted EDF
selection reason.

| Verified run | Trace records | Running task ticks | Idle ticks | Misses |
| --- | ---: | ---: | ---: | ---: |
| normal | 92 | 10 | 11 | 0 |
| overload | 84 | 17 | 4 | 1 |

These values come from one complete Renode run of each mode. Run `make demo`
to regenerate them. See [Deadline Lab](docs/DEADLINE_LAB.md) for the workload,
artifact contract, and measurement boundary.

## Signal Lab (implemented, experimental)

```sh
make test-native-dsp
make check-dsp-codegen
make test-signal-lab
```

The native test checks the scalar and portable paired Q15 FIR functions. The
object-code check requires `smlald` and `ssat` in the Cortex-M4 function. The
Signal Lab runs separate scalar and M4 images through the real kernel. Each
image processes the same four signal frames. The test checks the exact task
schedule, result checksums, firmware identity, and executed instruction trace.
The M4 run must execute 3,616 `smlald` instructions and 452 `ssat`
instructions. Arm GCC also selects 7,232 single-lane `smlalbb` instructions
from the portable scalar C loop. The evidence proves functional execution. It
does not measure hardware speed. The one-tick task intervals include `WFI`
wait time and do not measure CPU-active time. See
[Cortex-M4 performance lab](docs/PERFORMANCE_LAB.md) for the numerical contract,
workload, artifacts, and evidence limits.

The canonical target is the STM32F401RE on a NUCLEO-F401RE board:

- ARM Cortex-M4 / Thumb-2;
- 512 KiB flash at `0x08000000`;
- 96 KiB SRAM at `0x20000000`;
- USART2 on PA2/PA3 at 115200 8N1;
- user LED on PA5; and
- software floating-point ABI until floating-point context preservation is
  implemented and tested.

## Implemented and tested

- Rootless, project-local Arm GNU Toolchain 14.3.rel1 setup on Linux x86_64.
- Immutable STM32CubeF4 v1.28.3-compatible CMSIS Core, F401 device, and HAL
  sources with commit and cleanliness validation.
- Explicit source manifests; no source-directory wildcard discovery.
- F401xE startup/vector assembly and a reviewed F401RE linker memory map.
- `SystemInit` sets VTOR to flash before `HAL_Init` enables SysTick.
- HAL-only SysTick, 84 MHz clock setup, GPIO, and polling USART2 boot output.
- A linker-owned, eight-byte-aligned AymOS heap region, an empty newlib heap,
  and a separate 4 KiB MSP reservation.
- ELF, binary, map, size, build metadata, attributes, symbols, vector bytes, and
  memory-range validation.
- Project-local Renode 1.16.1, CPython 3.12.13, and hash-locked Robot test
  dependencies; emulator tests do not use global Python packages.
- A repository-owned, slice-accurate F401RE model with the reset flash alias,
  Cortex-M4/NVIC/SysTick, 512 KiB flash, 96 KiB SRAM, RCC/PWR, GPIOA, and
  USART2.
- A bounded headless boot that emits exactly one `AYMOS READY`, observes an
  application-local SysTick and SVC smoke handler, and emits the smoke result
  from thread mode.
- Host UART-contract tests, a Robot boot test, network-isolated emulator
  evidence, retained failure artifacts, and a pinned CI workflow. The PR 2
  hosted workflow passed on Ubuntu 24.04.
- A real, soft-float Cortex-M4 kernel path with validated SVC decoding, a small
  PendSV R4-R11 save/restore routine, privileged thread mode on PSP, and
  explicit SVC/SysTick/PendSV priority ordering.
- Eight-byte-aligned fixed task-stack slots, an initial hardware/software
  exception frame with the task argument in R0, a task-entry trampoline, and
  handler-mode stack reclamation after task return.
- An exact lifecycle oracle that proves first dispatch, two voluntary yields,
  SysTick-driven preemption, R4-R11 preservation, two safe exits/reclaims, and
  continued idle execution in the ARM firmware.
- One architecture-neutral C scheduling policy linked unchanged into the ARM
  kernel and native ASan/UBSan tests; there is no host reimplementation of the
  emulator result.
- Explicit one-shot/periodic task configuration, delayed and periodic releases,
  sleep/wake state, absolute deadlines, execution accounting, deadline misses,
  missed active releases, and deterministic EDF ties.
- An exact two-task ARM EDF oracle that proves two tick-driven periodic
  releases/preemptions and `os_wait_next_period` through SVC/PendSV.
- A bounded eight-byte-aligned next-fit allocator with checked in-arena
  metadata, exact-owner frees, coalescing, usage/fragmentation statistics, and
  an empty, non-overlapping newlib heap.
- Thread-mode runtime task creation and deferred PendSV cleanup of all
  allocations owned by an exiting task, while the existing fixed stack slots
  remain unchanged.
- Native allocator tests under ASan/UBSan plus an exact ARM workload that
  repeatedly reuses one task slot, exercises a 1024-byte allocation, checks
  payload canaries, and proves owner cleanup and interrupt-mask restoration.
- A versioned 32-byte event schema, 256-record static drop-new ring, atomic EDF
  selection snapshots, CRC-framed UART transport, and terminal loss footer.
- A strict bounded Python decoder producing canonical JSON and raw records,
  plus an application-specific semantic oracle for a finite real Cortex-M4
  workload whose repeated traces are identical.
- A four-task Deadline Lab with normal and overload modes, exact trace checks,
  run provenance, JSON summaries, and standalone HTML/SVG timelines.
- A three-task Signal Lab that runs scalar and M4 Q15 FIR images, requires the
  same fixed input/output CRC contract, and proves the selected instructions
  execute.

## Build

The supported host is Linux x86_64. Required bootstrap commands are
`awk`, `bash`, Git 2.25+, curl 7.61+, `make`, `cc`, `cmp`, `env`, `file`, `find`,
`grep`, `od`, `readlink`, `sha256sum`, `sort`, `stat`, `tar`, `timeout`,
`xargs`, and `xz`.
Root access, a global ARM compiler, a global Renode, and a system Python
environment are not used. The optional network-isolation check also needs host
`unshare` and `ip`.

```sh
make setup
make firmware
make test
make run
make test-emulator
make test-lifecycle
make test-edf
make test-allocator
make test-trace
make demo
```

`make setup` downloads locked Arm, Renode, CPython, Python-wheel, and STM32
inputs under `.tools/` and `.deps/`. Archives and wheels are checked for exact
byte size and SHA-256 before use. It is safe to rerun and rejects modified
dependency contents or unexpected files rather than overwriting them.

Every build first runs `tools/setup.sh --check`, an offline/read-only integrity
gate. Compilation cannot start until all consumed tool hashes, dependency
commits, clean states, files, and licenses pass.

Application selection is explicit:

```sh
make firmware BOARD=nucleo_f401re APP=boot
make firmware BOARD=nucleo_f401re APP=lifecycle
make firmware BOARD=nucleo_f401re APP=edf
make firmware BOARD=nucleo_f401re APP=allocator
make firmware BOARD=nucleo_f401re APP=trace
make firmware BOARD=nucleo_f401re APP=deadline_lab WORKLOAD_MODE=normal
make firmware BOARD=nucleo_f401re APP=deadline_lab WORKLOAD_MODE=overload
```

Unknown board/application names fail instead of silently changing the image.
See [docs/BUILDING.md](docs/BUILDING.md) for dependency provenance, artifact
paths, validation details, and clean-build instructions.

Key outputs are under `build/nucleo_f401re/boot/`:

```text
aymos.elf
aymos.bin
aymos.map
size.txt
build-metadata.txt
file.txt
readelf-header.txt
readelf-attributes.txt
readelf-sections.txt
readelf-program-headers.txt
symbols.txt
vector-table.bin
```

Useful commands:

```sh
make validate
make disassembly
make test-emulator-offline
RENODE_REPEAT=10 make test-emulator
make run-lifecycle
RENODE_REPEAT=10 make test-lifecycle
make run-edf
RENODE_REPEAT=10 make test-edf
make run-allocator
RENODE_REPEAT=10 make test-allocator
make run-trace
RENODE_REPEAT=10 make test-trace
make demo
make clean
make help
```

`make run` prints the two validated UART lines and retains its command,
metadata, raw UART, and emulator log under `build/renode/run/`. Robot test runs
are retained under `build/renode/test/`, including XML/HTML results and failure
diagnostics. The raw UART validator is the final acceptance oracle: both lines
must appear exactly once and in order.

`make flash` deliberately stops after building and prints an unvalidated
hardware notice. It does not guess which probe/programmer the user has.

## Implemented but deliberately limited

Tasks may be created before kernel start or by a running user task. Runtime
creation is bounded by the four user slots and runs its slot/frame publication
under PRIMASK; an immediately ready task that outranks the caller requests
PendSV. Each task still receives one fixed stack slot. Periodic tasks use constrained deadlines
(`relative_deadline_ticks <= period_ticks`) and permit one active job per task.
If a cadence release arrives before completion, `missed_release_count` is
incremented and cadence advances; no overlapping job is created. Execution
budget is reported/accounted but not enforced. Intervals are limited to
`INT32_MAX` ticks, while release/wake/deadline ordering uses a single 64-bit
monotonic kernel time. Public 32-bit tick fields are the low word for familiar
guest display; EDF never compares those wrapped values.

`os_memory_alloc` is intentionally a small kernel allocator, not a libc
replacement. Calls are limited to the currently running non-idle task in
thread mode. Every block is tagged with that task ID; exact-base frees by any
other task are rejected. PendSV releases remaining task-owned blocks only
after execution has left the exiting task's PSP. Cleanup prevents outstanding
blocks from remaining allocated to, or being inherited by, a reused task slot,
but adds bounded interrupt-masked exit latency proportional to the number of
heap blocks. Task-ID tags do not provide generation-aware stale-pointer
detection: a pointer retained after explicit free or task exit is invalid and
must not be reused. Fixed task stacks are deliberately not moved onto this heap
in PR 5. Ownership metadata is defensive bookkeeping, not hardware memory
protection or task isolation.

Tracing is enabled for `APP=trace`, `APP=deadline_lab`, and `APP=signal_lab`.
Earlier application UART contracts remain unchanged. The 8192-byte static ring
reduces the
remaining dynamic heap by the same amount. Producers perform bounded record
writes under PRIMASK; terminal idle thread mode closes and drains the ring
before polling UART. `uart.bin`
preserves the wire, `trace.bin` contains concatenated 32-byte records, and
`trace.json` is the canonical decode. The trace run/test commands require the
exact 102-record task, selection, committed/coalesced preemption, switch,
deadline, memory, idle-resumption, slot-reuse, and terminal-idle contract;
repeat equality is an additional check, not the only oracle. See
[docs/TRACE_FORMAT.md](docs/TRACE_FORMAT.md).

## Implementation campaign

The reviewed sequence is:

1. reproducible F401RE build (implemented and tested);
2. Renode boot harness and emulator tests (implemented and tested locally);
3. task lifecycle and Cortex-M context-switch correctness (implemented and
   tested);
4. explicit timing semantics and deterministic EDF tests (implemented and
   tested locally);
5. allocator hardening (implemented and tested locally);
6. bounded structured kernel tracing (implemented and tested locally); and
7. Deadline Lab workload and standalone scheduling timeline (implemented and
   tested locally).

The complete gates, review requirements, and deferred scope are in
[docs/IMPLEMENTATION_PLAN.md](docs/IMPLEMENTATION_PLAN.md).

Message queues, synchronization primitives, networking, filesystems, USB,
process isolation, POSIX compatibility, and a general shell are not implemented
and are explicitly outside this campaign.

## Hardware and timing status

No physical NUCLEO-F401RE was available for this slice. Board flashing and
physical serial output therefore remain unvalidated. The Renode results prove
functional boot and interrupt/UART behavior in this model; they do not prove
physical interrupt latency, worst-case execution time, clock accuracy, or hard
real-time guarantees.
