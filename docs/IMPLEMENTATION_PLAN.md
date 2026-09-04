# AymOS Real-Time Systems Lab implementation plan

Status: PRs 1 through 6 are merged. PR 7 is implemented on top of PR 6. Its
focused host checks and one real normal and overload run pass. Independent
review is approved. Committed-state evidence passes. Hosted CI remains the
publication gate.

This document defines the first trustworthy vertical slice of AymOS. It is a
campaign plan, not a claim that the described target behavior exists today.
Each pull request (PR) must update the README and this document when evidence
changes a design decision.

### Feasibility review record

Lead feasibility review completed before PR 1 implementation. The draft was
corrected to:

- pin Renode v1.16.1's portable .NET archive and integrity hash and account for
  `renode-test` Python requirements;
- reuse the STM32F4 model only through a reviewed F401RE-sized, 84 MHz,
  slice-accurate platform definition;
- remove the generic platform's unpinned remote SVD fetch, retain required
  peripheral tags, and test that emulator runs need no undeclared network after
  setup;
- use a pinned rootless CPython runtime for Renode/host tools rather than an
  undocumented system Python environment;
- distinguish reset's address-zero vector fetch from subsequent VTOR-based
  exception dispatch, set VTOR in `SystemInit` before HAL/SysTick, and align the
  complete F401 vector table to its required boundary;
- keep PR 2's SysTick/SVC smoke application-local (observed-flag handlers with
  thread-mode reporting), leaving SVC/PendSV/PSP task evidence to PR 3;
- require exact-one banner validation from raw UART bytes with inner Robot and
  outer host timeouts; and
- define naturally aligned 32-byte trace records, selection snapshots,
  drop-new overflow, an always-observable loss footer, and strict parser
  completeness checks.

These findings are resolved in the plan. PR 2 now has local implementation
evidence; later findings remain owned by their corresponding acceptance gates.

## Product objective and evidence standard

AymOS is intended to be a small, inspectable Cortex-M4 real-time systems lab.
The first release will execute deterministic workloads in Renode, emit enough
kernel state to explain EDF decisions, and render those events as a host-side
timeline. The firmware loaded by Renode will be the same STM32F401RE ELF built
for the NUCLEO-F401RE. There will be no host-side scheduler substituted for the
SVC, PendSV, PSP, and SysTick paths.

The target workflow is:

```sh
make setup
make firmware
make test
make run
make test-emulator
make timeline
```

`make setup` may require network access, but it must not require root access or
an undocumented system-wide ARM compiler or Renode installation. Every
downloaded executable and source dependency will be version-locked and
integrity-checked. Build and run metadata will record the resolved versions.

Passing in Renode demonstrates functional behavior in that emulator. It does
not establish physical interrupt latency, worst-case execution time, hardware
timing, or a hard real-time guarantee. Physical NUCLEO-F401RE support remains
unverified until a named hardware test has actually been performed and its
result recorded.

## Re-verified baseline architecture

At the start of the campaign the repository contained a compact,
single-address-space kernel and board support prototype:

- `stm-startup/startup_stm32f401retx.s` contains an STM32F401xE reset handler
  and vector table. Reset initializes `.data` and `.bss`, calls `SystemInit`,
  calls `__libc_init_array`, and enters `main`.
- `src/util.c`, `src/stm32f4xx_hal_msp.c`, and `headers/main.h` describe a
  NUCLEO-F401RE-like board: USART2 on PA2/PA3, the user LED on PA5, and the user
  button on PC13. The configured clock is 84 MHz and UART is 115200 8N1.
- `src/kernel.c` keeps 16 static TCB slots, reserves slot 0 for idle, scans
  READY tasks for the smallest `time_remaining`, and uses `priority` as an
  equal-deadline tie breaker.
- Task stacks are allocated from `src/memory.c` and initialized with a
  hand-built Cortex-M exception frame. `src/svc_handler.s` decodes SVC through
  a C handler and uses PendSV to save and restore R4-R11 on the PSP.
- `src/memory.c` implements an in-heap linked-list allocator with splitting,
  ownership metadata, next-fit state, and coalescing.
- Seven files under `src/tests/` are manually selected alternative firmware
  entry points, not an automated test suite.

The intended control flow is:

```text
Reset_Handler -> SystemInit -> main -> HAL/board initialization
                                  -> osKernelInit
                                  -> task creation
                                  -> osKernelStart -> SVC -> PendSV
                                  -> thread mode on PSP

SysTick -> release/time update -> request PendSV
PendSV  -> save old R4-R11 -> select/commit next task -> restore R4-R11
```

This flow is not functional on `main` at the start of the campaign. The
following findings were re-verified rather than inherited from the audit:

- The Makefile searches `Src/` and `Inc/`, while the repository uses `src/`
  and `headers/`. `make -n` therefore links with an empty object list.
- It references an absent `STM32F407VGTx_FLASH.ld` despite the F401RE startup
  file and board pinout.
- It does not include `stm-startup/`, CMSIS, or STM32 HAL sources. Those
  dependencies are not present in the checkout.
- The current host has neither `arm-none-eabi-gcc` nor `renode`; plain `make`
  fails at the first compiler invocation.
- `src/main.c` is an obsolete one-stack SVC experiment. It does not initialize
  the kernel or create a kernel-managed task.
- `src/stm32f4xx_it.c` refers to obsolete symbols (`kernel_running`,
  `current_TID`, `next_TID`, `g_system_time`, `svc_number`, and
  `getEarliestDeadlineTask`) rather than the names currently defined by the
  kernel.
- SysTick is assigned a lower urgency than PendSV. PendSV should be the
  lowest-urgency exception so it cannot interrupt kernel tick bookkeeping.
- Yield chooses a task before making the current task READY, and the switch
  helper does not consistently establish the selected task as RUNNING.
- The fabricated task frame has no task argument or return trampoline. Task
  exit deallocates the stack while it is still the active PSP stack.
- `deadline` and `time_remaining` currently conflate period, wake delay,
  release, deadline, and countdown semantics.
- The allocator hard-codes a 24-byte header, aligns to only four bytes, and has
  unsafe next-fit edge cases. Its heap and `_sbrk` both span from the image end
  toward the reserved MSP region.
- The Makefile requests the hard-float ABI, but PendSV does not preserve the
  high floating-point registers.
- README statements about protected memory, synchronization, comprehensive
  interrupt management, and a clean build are not supported by the code.

PRs 1 and 2 resolved the build/boot findings. PR 3 replaces the obsolete kernel,
handler, and standalone-SVC sources with `kernel/src/kernel.c` and
`arch/arm_cm4/context_switch.S`. Its separate `APP=lifecycle` image uses fixed,
eight-byte-aligned stack slots so lifecycle correctness can be established
without pretending the legacy allocator was already hardened. PR 4 replaced
the overloaded timing model with explicit EDF state. PR 5 replaces the legacy
allocator with the checked task-owned core described below while deliberately
retaining those fixed task stacks.

## Target repository structure

The repository will move toward this layout incrementally. PRs should avoid
large moves unrelated to their acceptance criteria and preserve useful Git
history.

```text
apps/
  boot/                    minimal board smoke application
  lifecycle/               task lifecycle integration scenario
  deadline_lab/            deterministic product demo
arch/
  arm_cm4/                 startup, SVC/PendSV, exception helpers
bsp/
  nucleo_f401re/           clock, GPIO, UART, linker script, HAL configuration
include/aymos/              public kernel API
kernel/
  allocator/               educational bounded allocator
  scheduler/               portable scheduler policy and timing state
  trace/                   fixed-size trace records and ring
  *.c                      task lifecycle and kernel coordination
platform/
  renode/                  F401RE platform description and launch scripts
tests/
  native/                  host scheduler, allocator, parser, and schema tests
  renode/                  black-box ARM firmware tests
tools/
  aymos_lab/               trace collection, parser, summaries, timeline CLI
  setup/                   dependency lock and setup helpers
docs/
  IMPLEMENTATION_PLAN.md
  TRACE_FORMAT.md
third_party/
  README.md                provenance and licenses, not mutable build output
build/                     ignored build products
.deps/                     ignored pinned source dependencies
.tools/                    ignored pinned executable tools
runs/                      ignored generated runs except curated examples
```

Applications are link-time selections, not runtime-loaded programs. The build
will accept an explicit name, for example `make firmware APP=boot`, and will
fail on unknown names. The default application will be documented in `make
help` and the README. Each application contributes an explicit source manifest;
wildcarding every C file under a tree is prohibited because several existing
test files define `main`.

## Dependency and toolchain strategy

### Locking and local installation

A machine-readable lock file under `tools/setup/` will record, for every
dependency:

- upstream URL;
- release/tag and immutable commit where applicable;
- archive filename;
- SHA-256 for downloaded archives;
- license location;
- supported host architecture.

`make setup` will invoke a small, auditable shell helper that:

1. validates basic host tools (`bash`, `make`, `git`, `curl`, `tar`, `xz`, and
   a SHA-256 utility);
2. downloads into a temporary file;
3. verifies SHA-256 before extraction;
4. installs under `.tools/` or `.deps/` without root;
5. verifies the resolved version/commit after installation; and
6. is idempotent and safe to resume.

The Makefile invokes tools by an explicit project-local path and does not
silently fall back to a same-named executable on `PATH`. PR 1 rejects
command-line and environment overrides of the toolchain directory, cross
prefix, compiler, and binary utilities, including attempts made through
`make -e`. A future non-x86_64 or advanced-user override is permitted only
after it gains the same selected-path and content-hash validation as the
canonical toolchain, with the selection recorded in metadata; until then it
must fail explicitly.

Download and extracted directories will be ignored by Git. Dependency source
will not be edited in place. Project-owned and third-party compilation units
will use separate warning variables so warnings in our code stay errors while
unavoidable vendor diagnostics remain visible and documented.

### Pinned components

The initial conservative choices are:

- Arm GNU Toolchain 14.3.rel1, x86_64 Linux, `arm-none-eabi`. The exact official
  archive URL and SHA-256 must be validated against Arm's release material in
  PR 1 before the lock is accepted.
- STM32CubeF4 v1.28.3 as the compatibility baseline. To avoid downloading the
  entire firmware bundle, setup will fetch only the official component repos
  at the commits referenced by the peeled v1.28.3 commit
  `94cae6e83f00e276a11957e7833c01ac3d0bd7af`:
  - `stm32f4xx_hal_driver` at
    `b6f0ed3829f3829eb358a2e7417d80bba1a42db7`;
  - `cmsis_device_f4` at
    `3c77349ce04c8af401454cc51f85ea9a50e34fc1`;
  - CMSIS Core headers from the matching Cube release or an explicitly pinned
    CMSIS release after include-level compatibility is verified.
- Renode is pinned to v1.16.1's portable .NET Linux asset:
  `https://github.com/renode/renode/releases/download/v1.16.1/renode-1.16.1.linux-portable-dotnet.tar.gz`,
  SHA-256
  `00e113cdbd0f5354cf2f64bbe3f5a070d8958409542fca66e45ac97d982938c0`.
  PR 2 verifies that this asset executes on the supported host. `renode-test`
  packages are installed with exact pins/hashes into the project's local
  virtual environment rather than resolved from the user's Python environment.
- Python is a project-local runtime, not a system prerequisite beyond setup.
  PR 2 pins CPython 3.12.13's x86_64 Linux install-only archive from Astral's
  `python-build-standalone` 20260718 release by URL, size, and SHA-256. Both
  `renode-test` and host validation use a virtual environment created by that
  interpreter with exact, hash-locked requirements. `python3`, `venv`, and
  `pip` from `PATH` are not fallback behavior.

Official upstreams are the Arm GNU Toolchain release repository,
<https://github.com/STMicroelectronics/STM32CubeF4>, its referenced component
repositories, and <https://github.com/renode/renode>. Dependency updates must
be isolated changes with regenerated hashes and full build/emulator testing.

### Floating-point ABI

Firmware will initially use Thumb-2 and the software floating-point calling
convention (`-mcpu=cortex-m4 -mthumb -mfloat-abi=soft`). Application and all
linked libraries must agree. Hardware floating point is deferred until the
lazy-stacking behavior and S16-S31 preservation have architecture-level tests.

## PR 1 build design

PR 1 is deliberately a board/build foundation, not a scheduler repair.

- Establish `nucleo_f401re` as the only supported board and
  `STM32F401xE` as the device define.
- Replace source wildcards with named project, board, startup, and HAL lists.
  HAL files will come only from the locked dependency locations.
- Add a reviewed F401RE linker script with 512 KiB flash at `0x08000000` and
  96 KiB SRAM at `0x20000000`. The vector table will be retained at the flash
  origin with `0x200` alignment for the complete device table, and the map will
  export explicit vector, image, custom-heap, and MSP-stack boundaries.
- Use the correct F401xE startup/vector assembly from the locked CMSIS device
  package, or retain the existing file only after a byte/handler review against
  that source. Do not compile two vector tables.
- Set `SCB->VTOR` explicitly to the linker-exported flash vector address in
  `SystemInit`, before `HAL_Init` enables SysTick. Runtime code must use linker
  symbols or CMSIS constants instead of reading a presumed vector alias at
  address zero. Reset itself still requires STM32's address-zero flash boot
  alias (or equivalent emulator MSP/PC initialization); firmware cannot remove
  that architectural reset requirement by writing VTOR later.
- Give the AymOS allocator a linker-reserved, eight-byte-aligned region with
  start/end symbols. Newlib dynamic allocation will be explicitly disabled for
  the first slice (an `_sbrk` failure is preferable to overlapping heaps).
  Project code in PR 1 will not call `malloc`. A future separate newlib heap
  would require a non-overlapping linker region and a demonstrated need.
- Replace the obsolete SVC experiment with an `apps/boot` entry point that
  initializes the HAL, 84 MHz clock, GPIO, and USART2, emits a small boot line,
  and idles. It will not claim that task switching works before PR 3.
- PR 1's SysTick handler is HAL-only: it advances the HAL tick without entering
  the stale kernel timing path. PR 3 replaces that temporary boundary with the
  corrected kernel tick/preemption integration after lifecycle invariants exist.
- Build the project-owned BSP and boot application with strict warnings. Keep
  vendor compiler options and diagnostics visibly separate.
- Produce `aymos.elf`, `aymos.bin`, `aymos.map`, a saved size report, and
  optional disassembly under a board/application-specific build directory.
- Add build validation using `file`, `readelf`, `objdump`/`nm`, `size`, and map
  checks. Validate ARM EABI, Cortex-M4 attributes, soft-float ABI, the reset
  vector/entry point, and all allocated sections against the F401RE flash/RAM
  ranges.
- Keep `make flash` explicit and non-default. It may build the binary and print
  the required probe tool while remaining marked unverified when no board is
  available.

Kernel compilation may be temporarily separate from the boot application's
link if stale interrupt code prevents a safe image. If so, PR 1 must state that
fact prominently; PR 3 must link and exercise the real kernel. Prefer compiling
the kernel when small non-behavioral compatibility fixes suffice, but do not
smuggle lifecycle redesign into PR 1 merely to make the link green.

## Renode integration approach

PR 2 derives a repository-owned minimal definition from Renode's pinned generic
STM32F4 model. It does not inherit the generic definition at runtime, because
that file contains an unpinned remote `ApplySVD` fetch and many peripherals
outside this slice. The local definition uses flash size `0x80000`, SRAM size
`0x18000`, and SysTick frequency 84 MHz. It maps:

- 512 KiB flash at `0x08000000`;
- 96 KiB SRAM at `0x20000000`;
- an ARM Cortex-M4 CPU and NVIC;
- SysTick through the NVIC/CPU model;
- RCC/clock behavior sufficient for HAL initialization;
- USART2 at its STM32F4 address with its IRQ connection;
- GPIO blocks required by the boot application or later demo.

This is a slice-accurate model for the peripherals and memory used by AymOS,
not an exact model of every STM32F401RE register or silicon behavior.
Unsupported peripherals and unverified register behavior will be named rather
than implied.

The harness will load the exact board ELF, expose USART2 as a host-testable
stream, run without a GUI, and enforce a wall-clock timeout. After `make setup`,
the emulator paths must work with network access denied; no model, SVD, Python,
or test dependency may be fetched at run time. `make run` is the
interactive/headless demonstration path. `make test-emulator` invokes a Robot
test with a UART2 terminal tester timeout and wraps the entire Renode process in
a second host-side timeout. It counts the exact banner bytes in the raw UART
capture and requires exactly one `AYMOS READY`. After that banner, the guest
invokes minimal application-local smoke handlers: SysTick and SVC each set an
observed flag; neither schedules, switches context, or writes UART. Thread mode
observes both flags and emits the smoke result. The test retains raw
UART separately from `emulator.log` on every failure.

Reset occurs before firmware can write VTOR: the Cortex-M reset sequence must
still obtain initial MSP and reset PC from address zero. PR 2 must either model
the STM32 flash boot alias at zero or explicitly configure equivalent emulator
initial MSP/PC behavior and test it. Independently, `SystemInit` sets
`SCB->VTOR` to the linker-exported flash vector base before `HAL_Init` enables
SysTick, so all later exceptions use the linked table. ELF checks verify its
initial MSP/reset PC and that the complete F401 table is aligned to `0x200`.
Existing kernel code that reads address zero after reset will be converted to a
linker/vector symbol before it is exercised.

If HAL clock initialization hangs because the chosen RCC model does not model a
polled hardware-ready transition, the investigation must first compare accesses
with Renode's existing STM32F4 models. Any emulator-specific firmware bypass
must be narrow, compile-time visible in build metadata, and must not alter the
kernel scheduling path. A single register-level clock setup shared by emulator
and board is preferable to divergent board code.

PR 2 is a hard gate. No lifecycle integration is considered complete until the
F401RE-targeted ELF boots reliably through this harness.

## Testing layers

The campaign uses complementary layers; no single layer substitutes for the
others.

### 1. Dependency and build validation

- Verify hashes, source commits, and reported tool versions.
- Re-run setup to prove idempotence.
- Build from a checkout with project-local caches removed.
- Fail on project-owned compiler warnings.
- Inspect ELF identity, EABI attributes, symbols, vector placement, section
  VMAs/LMAs, flash/RAM bounds, heap/stack separation, map file, and size.
- Build all supported application selections, not just the default.

### 2. Native policy/unit tests

Portable scheduler policy, tick comparisons, allocator core logic, trace
schema, trace parser, and timeline interval construction will be built for the
host. Architecture dependencies must be behind narrow interfaces rather than
stubbed throughout the kernel. Tests will use assertions and deterministic
fixtures. Allocator tests will use AddressSanitizer and UndefinedBehaviorSanitizer
where their host adaptation permits it.

Native tests prove policy and memory algorithms; they do not prove ARM
exception entry, stack frames, privilege state, or actual preemption.

### 3. ARM/Renode integration tests

Black-box tests will boot the board ELF and assert UART or structured trace
events for:

- reset and boot;
- SVC dispatch and first PSP task;
- voluntary yield and PendSV;
- SysTick-driven preemption;
- task return/exit and deferred stack reclamation;
- deterministic EDF releases and choices;
- repeated allocation/task reuse;
- trace framing, overflow behavior, and end-to-end Deadline Lab runs.

Every emulator command has an explicit timeout and preserves diagnostics on
failure. Tests compare semantic event sequences, not unstable host timestamps.

### 4. Host-tool contract and end-to-end tests

- Validate trace headers, record sizes, enum ranges, sequence continuity,
  checksums/framing, and truncated/corrupt input behavior.
- Use golden traces only for parser/renderer unit tests; end-to-end acceptance
  traces must come from the executed Cortex-M4 firmware.
- Assert summary metrics and timeline intervals for normal and overload modes.
- Run the one-command workflow into an isolated, bounded run directory and
  validate its manifest and artifacts.

### 5. Physical hardware tests

`make flash` and a serial smoke procedure will be documented. Until run on a
named NUCLEO-F401RE, its result is explicitly “not validated.” Renode event
ordering must not be presented as measured board timing.

## Timing model direction

PR 4 will give each schedulable job explicit state rather than decrementing one
overloaded field. The public configuration will have deterministic defaults and
will distinguish at least:

- task kind: one-shot or periodic;
- phase/initial release tick;
- period for periodic tasks;
- relative deadline;
- absolute deadline for the current job;
- next release/wake tick;
- release/job sequence;
- completion/accounting state;
- deadline-miss count;
- sleeping versus waiting-for-release versus dormant state.

Configured relative delays/deadlines remain below half the `uint32_t` range,
but runtime ordering does not compare wrapped 32-bit values. One 64-bit
monotonic kernel time qualifies release, wake, and absolute-deadline keys. Due
checks use ordinary `>=` and EDF uses strict `<` on those keys. The familiar
32-bit tick values are low-word display fields only. Exhausting `UINT64_MAX` is
a fail-stop kernel error, not an epoch wrap. Execution demand in the Deadline
Lab will be deterministic guest-visible work/ticks, not host wall time.

EDF selection will compare eligible jobs by absolute deadline, then explicit
static priority, then stable TID as the final deterministic tie breaker. Idle is
selected only when there is no eligible user job. Deadline misses are counted
and traced; they are not silently converted into releases.

## Trace architecture

PR 6 will implement a bounded, versioned trace designed for low perturbation:

1. Kernel sites construct naturally aligned, 32-byte binary event records. A
   record contains only guest tick and monotonically increasing sequence as its
   ordering/time coordinates, plus schema version/event ID, primary and related
   task IDs, and event-specific values. Host wall time and emulator virtual time
   are not inserted into kernel records.
2. Producers append records to a statically allocated single-core ring in a
   short interrupt-safe critical section. No producer calls `printf`, waits for
   UART, or performs allocation.
3. A low-priority controlled flush path removes committed records and frames
   them for USART2. Framing includes enough length/version/integrity information
   for the host to reject corrupt or partial records and resynchronize.
4. On a full ring, the producer uses drop-new: the attempted record is
   discarded, existing committed history is retained, and a loss counter is
   incremented. The run terminator/footer carries the final loss count through
   a reserved transport path so loss remains observable even if the ring never
   regains space. A `TRACE_OVERFLOW` record may additionally summarize loss
   when normal space becomes available, but it is not the only evidence.
5. The host saves raw UART bytes separately from Renode's diagnostic
   `emulator.log`, decodes validated canonical records, and retains parser
   diagnostics and schema version.

At minimum, the schema will represent kernel start; task creation, release,
selection, first start, preemption, yield, sleep, wake, exit; context switches;
idle enter/leave; deadline met/missed; allocation/free; and trace overflow.
Selection events carry the READY-set bit mask, relevant candidate deadlines,
selected task, and an explicit tie reason (deadline, priority, stable TID, or
idle fallback). If the fixed payload cannot contain every deadline, a defined
selection snapshot sequence will carry the full state before the decision. The
host must not invent kernel state that was never recorded.

Schema changes require a version bump, C layout/alignment static assertions,
host decoder tests, malformed-input tests, and documentation in
`docs/TRACE_FORMAT.md`. The parser rejects unknown schema versions, truncated
frames, sequence gaps, missing terminal footers, and any reported overflow for
a run that claims to be complete.
Repeated deterministic runs must be semantically equivalent after excluding
explicitly documented transport-only fields.

## Pull-request dependency graph and merge order

The initial campaign is intentionally gated and mostly stacked because later
acceptance tests exercise foundations introduced earlier:

```text
main
  |
  +-- PR 1 reproducible F401RE build
        |
        +-- PR 2 Renode boot harness
              |
              +-- PR 3 lifecycle/context correctness
                    |
                    +-- PR 4 explicit timing and EDF
                          |
                          +-- PR 5 allocator hardening
                                |
                                +-- PR 6 structured tracing
                                      |
                                      +-- PR 7 Deadline Lab/timeline
```

Each draft PR description must name its exact base and head. During the
campaign, PR N is based on PR N-1's head. The required merge order is 1 through
7. If the owner merges an earlier PR, descendants will be rebased onto the new
`main` without force-updating another agent's active worktree. PR 5 is
conceptually independent of PR 4 but remains ordered after it to keep one
auditable integration line and because PR 6 needs both.

Every substantial PR is implemented in its own branch/worktree, reviewed by an
agent that did not implement it, corrected in that same branch, and fully
retested. No PR is merged by the campaign agents.

## Per-PR scope and acceptance criteria

### PR 1: reproducible F401RE firmware build

Base: `main`.

Scope is the dependency lock/setup, explicit build, F401RE linker/startup/BSP,
non-overlapping heap ownership, conservative ABI, minimal boot application,
artifact validation, application selection, and accurate build documentation.
Scheduler semantics are a non-goal.

Acceptance evidence:

- From a documented clean Linux x86_64 environment, `make setup` followed by
  `make firmware` succeeds without a global ARM compiler or root.
- Setup is idempotent and rejects hash/version mismatches.
- The build has no project-owned warnings and does not conceal vendor warnings.
- ELF, binary, map, and size report are produced at documented paths.
- `file`/`readelf` show a 32-bit little-endian ARM EABI Cortex-M4 image using the
  chosen soft-float ABI.
- Vector/reset entry and loadable flash sections lie within
  `0x08000000..0x0807ffff`; RAM sections and linker reservations lie within
  `0x20000000..0x20017fff` without overlap.
- The vector table and correct F401xE startup are present, and no F407 linker
  artifact is used. The complete table is `0x200` aligned. Generated
  disassembly is manually inspected to confirm that `SystemInit` sets VTOR to
  its linked flash address before HAL SysTick is enabled.
- The map proves that custom heap and MSP stack are disjoint and newlib cannot
  allocate across the custom heap.
- `APP=boot` is documented and unknown app names fail.
- `make clean && make firmware` reproduces the image from the pinned local
  dependencies. A clean-dependency setup/build is also exercised in review or
  CI.
- A different reviewer resolves findings on dependency integrity, linker
  ranges, startup/vector selection, ABI consistency, warning policy, and docs.

### PR 2: Renode boot harness and emulator CI

Base: PR 1 head.

Scope is a pinned local Renode, minimal F401RE platform, headless runner,
USART2 capture, stable `AYMOS READY` banner, timeout/failure diagnostics,
emulator test, and CI-equivalent command. Kernel simulation is a non-goal.

Acceptance evidence:

- `make run` loads the exact F401RE ELF and visibly reaches the banner.
- `make test-emulator` exits successfully only after the raw UART capture
  contains exactly one stable banner and thread mode reports the post-banner
  application-local SysTick-flag and SVC-flag smoke results. The Robot
  terminal tester and outer host process each have an explicit timeout.
- A missing banner, guest fault, or timeout produces nonzero status and retains
  command, Renode log, and raw UART output as separate artifacts.
- At least ten clean repeated boots pass to expose startup races.
- Metadata identifies Renode version, ELF hash, application, and arguments.
- The local model is derived from generic `stm32f4.repl` with reviewed F401
  sizing/frequency and documents that it is slice-accurate rather than exact
  silicon; it does not inherit any remote runtime input.
- The local model has no unpinned `ApplySVD` URL, retains required model tags,
  and runs with network disabled after setup.
- Renode tests use the pinned project-local CPython and hash-locked virtual
  environment; they never fall back to system Python packages.
- Startup sets VTOR to the linked flash vector base and does not rely on an
  address-zero alias after reset. Reset fetch itself is supplied by a tested
  flash alias or equivalent explicit emulator initial MSP/PC configuration.
- `make flash` remains honest about absent physical-board validation.
- Independent review verifies the memory/peripheral model and ensures no
  scheduler behavior is faked in host code.

Gate: do not accept PR 3 integration until this boot is reliable.

Local PR 2 evidence (2026-08-01): `make setup`, its offline integrity check,
`make firmware`, `make test`, `make run`, `make test-emulator`, and
`make test-emulator-offline` passed. Ten fresh Robot boots also passed with
`RENODE_REPEAT=10 make test-emulator`. The raw capture was 42 bytes and
contained the two required lines exactly once. Robot checked the address-zero
flash alias, `0x20018000` initial MSP word, and `0x08000000` VTOR. Each actual
emulator process was bounded by an outer TERM/KILL timeout. These local results
were followed by independent approval. Hosted GitHub Actions run 30721900895
also passed on the committed PR 2 head.

### PR 3: task lifecycle and context-switch correctness

Base: PR 2 head.

Scope is state-transition centralization, initial frame/trampoline/argument,
safe return and deferred reclamation, current/next state ordering, obsolete ISR
symbol reconciliation, PSP/MSP and EXC_RETURN correctness, PendSV/SysTick/SVC
priorities, diagnostics, and a lifecycle firmware scenario. Timing API redesign
is a non-goal.

Acceptance evidence:

- At all kernel-observable stable points, exactly one valid task is RUNNING
  while active; every switched-out task has the correct non-RUNNING state.
- A Renode scenario proves initial SVC dispatch, voluntary yield,
  SysTick-triggered preemption, argument delivery, task return through the
  trampoline, safe exit, deferred stack reclaim, and continued user/idle work.
- Task stack allocation and fabricated frames guarantee eight-byte alignment
  at exception boundaries in this PR; this requirement is not deferred to the
  broader allocator hardening in PR 5.
- The PendSV path preserves R4-R11 and uses the verified basic-frame
  `EXC_RETURN` for thread mode/PSP under the soft-float ABI.
- SVC decoding uses the stack selected by `EXC_RETURN`, validates the SVC site,
  and handles unknown calls diagnostically.
- PendSV is the lowest-urgency exception and cannot interrupt SysTick kernel
  bookkeeping.
- Impossible state assertions/fault output fail the emulator test rather than
  hang invisibly.
- An independent Cortex-M specialist signs off specifically on exception-frame
  word order, xPSR Thumb bit, PC/LR/trampoline, R0 argument, PSP/MSP transition,
  EXC_RETURN, SVC instruction decoding, register preservation, priorities,
  alignment, and soft-float assumptions.

Gate: do not expand timing semantics until the lifecycle scenario passes.

Local PR 3 implementation evidence (2026-08-01): the lifecycle ELF passes the
F401RE/soft-float/vector/memory validator.
`make run-lifecycle` and `make test-lifecycle` execute the real ARM image and
accept only the exact guest UART sequence proving first dispatch, voluntary
yields, a sleeping-task wake followed by SysTick-driven preemption, task
arguments, PSP thread mode, trampoline returns, handler-side reclamation, user
continuation, and idle continuation. Assembly probes validate R4-R11 across
both voluntary and preemptive switches. Ten repeated lifecycle boots, the
network-isolated lifecycle test, the original boot path, forced timeout/process
cleanup, and input-validation negatives passed. Independent architecture review
approved the frame, SVC, PSP/MSP, EXC_RETURN, register, priority, state, reclaim,
ABI, failure, test, documentation, and scope contracts after one blocking sleep
half-range finding was fixed and retested. PR 4 is based exactly on the
committed PR 3 head `3283e1cf31ae339e8ffd377e65dce9ab0e8305f3`.

### PR 4: explicit timing model and EDF correctness

Base: PR 3 head.

Scope is the explicit task/job timing model, public configuration API with
defaults, wrap-safe comparisons, deterministic EDF/ties/slot reuse,
miss/release accounting, native policy tests, and deterministic two-task ARM
workload.

Acceptance evidence:

- Native tests cover earlier absolute deadline, priority tie, stable final tie,
  sleeping exclusion, exact wake tick, miss recording, periodic release and
  next deadline, wraparound, idle selection, one-shot termination, and reused
  task slots.
- Public callers never initialize internal TCB fields directly; rejected
  configurations have explicit errors.
- Misses remain observable and are not erased by release updates.
- The same predefined two-task workload runs repeatedly in Renode and an
  automated assertion observes the identical semantic selection sequence.
- Native and ARM results agree on policy fixtures without using a host
  scheduling simulation as the emulator result.
- Independent review checks monotonic-time assumptions, simultaneous events,
  boundary ticks, state transitions, deterministic ties, ISR interaction, and
  API documentation.

Gate: the deterministic scheduler sequence must pass before trace explanations
are trusted.

Local PR 4 implementation evidence (2026-08-02): architecture-neutral
`kernel/src/scheduler.c` is linked unchanged into ARM firmware and a strict
native ASan/UBSan test. The native suite covers every fixture above, invalid
half-range/constrained-deadline configurations, active-release accounting,
miss persistence, full-wrap boundaries, and the exact ARM fixture. It also
constructs reachable time-zero task sets whose later periodic deadline is
exactly `2^31` and more than `2^31` ticks after an active overdue deadline,
proving strict/asymmetric/stable EDF ordering. Every native invocation rebuilds
the executable and records the resolved compiler/real path/hash/version, flags,
input hashes, and binary hash. The public default/config API is separate from
kernel-owned scheduler state.

The implemented periodic model is constrained-deadline and single-active-job.
A release boundary reached while its predecessor is active increments an
observable missed-release counter and advances scheduled cadence; it is neither
overlapped, silently queued, nor regenerated from completion time. An unfinished
job is missed at equality with its absolute deadline, once per job. Sleep keeps
the active job/deadline; completion waits on the established cadence.

`APP=edf` shares fixture constants with native tests and exercises two real
SysTick release/preemption decisions plus `os_wait_next_period` through
SVC/PendSV/PSP. The exact semantic UART oracle, boot regression, lifecycle
regression, repeat run, and network-isolated run are PR 4 publication gates.
These are functional emulator results, not physical timing, latency, WCET, or
hard-real-time evidence. Two independent re-reviews approved the final timing,
overflow, interrupt-context, provenance, test, and documentation contracts.

### PR 5: allocator hardening

Base: PR 4 head.

Scope is `sizeof` metadata, eight-byte alignment, safe next-fit behavior,
pointer/header/allocation validation, double-free rejection, critical sections,
ownership policy, task-exit cleanup decision, heap bounds, statistics, and
native/Renode tests. A general libc allocator is a non-goal.

Acceptance evidence:

- Native deterministic tests cover split, exact fit, front/middle/end
  coalescing, exhaustion/recovery, null and invalid free, interior pointer,
  double free, ownership violation, alignment, boundary rejection,
  fragmentation, and statistics.
- Host-adapted tests pass with AddressSanitizer and UndefinedBehaviorSanitizer;
  any test that cannot use them documents why.
- Critical-section behavior is safe from task/SysTick interleaving and does not
  leave interrupts disabled on an error path.
- Linker/runtime assertions agree on heap start/end and every returned stack is
  eight-byte aligned.
- Custom and newlib heaps cannot overlap.
- The ownership and task-exit policy is documented and tested: fixed stack
  reclaim remains deferred, and PendSV automatically releases every remaining
  non-stack block owned by the exiting task before its ID is reused.
- A repeated Renode create/return/exit scenario reuses task slots and memory
  without corruption, leaks beyond the documented policy, or changing allocator
  invariants.
- Independent review targets integer overflow, pointer provenance, corrupted
  metadata, interrupt safety, stale owner IDs after slot reuse, fragmentation
  metrics, and sanitizer test fidelity.

Gate: memory-stress demonstrations remain excluded until these tests pass.

Local PR 5 implementation evidence (2026-08-02): the legacy `k_mem` source and
manual allocator tests were retired in favor of one `sizeof`-based portable
core linked unchanged into native and ARM builds. The arena is normalized to
eight-byte boundaries and partitioned by contiguous checked metadata. Next-fit
starts at the first block, continues from the block after allocation, preserves
its cursor on ordinary free, and retargets only when coalescing removes the
cursor. Invalid metadata fails closed; invalid, interior, double, boundary, and
foreign-owner frees are rejected.

Kernel APIs assign allocations to the current RUNNING user task and preserve
the incoming PRIMASK on all success/error paths. Runtime `os_task_create` is
thread-mode only and atomically publishes a complete fixed-stack task; an
immediate outranking job pends PendSV without prematurely changing the caller's
RUNNING state. On exit, PendSV keeps the fixed PSP stack intact while running a
one-pass owner mark plus one-pass coalesce on MSP, then resets the slot. The
linear interrupt-masked cleanup latency is documented; fixed task stacks remain
outside the heap.

The native sanitizer suite passes 548 checks. `APP=allocator` builds and
validates for ARMv7E-M soft-float and emits its exact Renode oracle: eight
immediate runtime preemptions reuse task slot 2, payload canaries survive
foreign/free/reclaim operations, and final state is zero allocations and one
coalesced free block. Repeat, offline, and earlier-PR regression gates passed.
Independent adversarial review approved the implementation after its one
ownership-contract finding was corrected; committed-state and hosted-CI
evidence remain the publication gate.

### PR 6: structured kernel tracing

Base: PR 5 head.

Scope is the versioned fixed record schema, bounded ring, nonblocking producers,
overflow behavior, controlled UART framing/flush, host decoder, schema docs,
and deterministic semantic comparison. Timeline rendering is a non-goal.

Acceptance evidence:

- Every required event type is documented, versioned, emitted at a defined
  state boundary, and covered by C/host schema tests.
- Record size/layout has compile-time assertions and endian/transport behavior
  is explicit.
- No PendSV, SysTick, allocator critical section, or trace producer performs
  formatted/blocking UART I/O or allocates memory.
- Ring-full drop-new behavior cannot deadlock or corrupt memory; the terminal
  footer makes its final loss count observable even if normal records never
  resume.
- The parser rejects bad version, length, checksum, enum, sequence, truncation,
  impossible IDs, sequence gaps, missing footer, and any overflow in a claimed
  complete run with useful diagnostics.
- Records are naturally aligned 32-byte structures and use guest tick plus
  sequence as their only ordering/time coordinates.
- Selection snapshots expose READY mask, relevant absolute deadlines, winner,
  and explicit tie reason.
- A complete deterministic Renode run decodes successfully and two repeated
  runs have semantically equivalent records.
- Selection records expose the deadlines and tie-break information actually
  used by the kernel.
- Independent review checks producer/consumer races, nested interrupts,
  commit visibility, overflow recursion, transport resynchronization,
  perturbation, parser resource bounds, and schema completeness.

Gate: timeline work cannot start until a complete validated guest trace exists.

Local PR 6 implementation evidence (2026-08-02): schema 1 is a compile-time
asserted, naturally aligned 32-byte little-endian record. The portable ring has
256 static records, bounded five-record atomic batches, release/acquire fences,
drop-new history preservation, attempt sequences, saturating loss counters, and
an explicit open-to-closed terminal snapshot. Native ASan/UBSan tests pass 103
checks, including all 22 schema event IDs, wrap, insufficient-space whole-batch
drops, nonrecursive overflow recovery before later attempts, close-before-drain,
and post-close rejection. ARM
disassembly contains `dmb ish` before publication and before consumer copy.

Selection summary/candidate batches emit user READY mask, purpose, incumbent,
yield exclusion, eligibility, full 64-bit deadlines, priority, winner, and the
exact deadline/priority/stable-ID or fallback reason. SysTick and runtime-create
probes are separate from actual PendSV dispatch snapshots. No trace producer
allocates, formats, or transmits UART. Final idle closes producers under one
saved/restored PRIMASK, drains in thread mode, CRC-frames records, and transmits
a direct 28-byte footer with attempted/emitted/dropped/final-sequence/flags and
final guest tick.

The locked-Python tests pass 23 groups: 18 generic malformed/schema/semantic
groups and five exact-projection mutation groups. The generic decoder accepts
coherent event IDs 1 through 21, validates event 22 while rejecting it as
inconsistent with a claimed complete zero-loss run, and rejects loss, gaps,
corruption, invalid event/task/payload/selection state, bounds, footer errors,
and trailing bytes. Its shared file reader rejects nonregular and oversized
inputs without unbounded reads. It also detects a size change to the opened
file during the read.
Diagnostic resynchronization scans at most 4096 bytes and never makes a damaged
stream acceptable.

A fresh real Cortex-M4 Renode run passed the exact 102-record all-event
projection with footer tick 15 and no drops. The workload proves a masked
direct runtime-create request coalesced with a pending SysTick request, with
PendSV reporting the actual selected task rather than the earlier probe target;
task-slot reuse; repeated idle-to-user preemption; yield; two sleep/wake pairs;
allocation/free; four met deadlines; one miss; safe exit/reclaim; and terminal
idle. The formal review corrections passed the 103-check native trace suite,
all 23 host trace tests, a ten-run byte/JSON-equivalence gate, a
network-isolated trace run, and all inherited native/host and three-run
boot/lifecycle/EDF/allocator regressions. Independent re-review,
committed-state evidence, and hosted CI passed before merge. The 8192-byte ring
reduces available dynamic heap, and all traces remain functional emulator
evidence without timing claims.

### PR 7: Deadline Lab workload and host timeline

Base: PR 6 head.

Scope is a deterministic fast sampler, controller/processor, slow telemetry,
idle task, configurable load generator, normal and overload configurations,
one-command run capture, summaries, standalone HTML timeline, bounded artifact
layout, and end-to-end documentation. A browser service or broad experiment
framework is a non-goal.

Acceptance evidence:

- A single documented command generates a complete normal run and a complete
  overload run from guest firmware execution.
- Normal mode meets all configured deadlines; overload mode produces a
  reproducible first miss.
- Automated tests assert releases, selections, running intervals, preemptions,
  idle intervals, absolute deadline markers, and the first miss with its
  preceding emitted scheduling evidence.
- Timeline explanations cite emitted state rather than reconstructing hidden
  policy.
- Each run contains `metadata.json`, `workload.yaml`, `firmware.elf`,
  `firmware.map`, `trace.bin`, `trace.json`, `summary.json`, `timeline.html`, and
  `emulator.log`.
- Metadata contains Git commit and dirty state, ELF hash, toolchain and Renode
  versions, build configuration, workload/hash, emulator arguments, schema
  version, and random seed or an explicit “none.”
- Run IDs are collision-safe but reproducibility comparisons use content
  metadata, not directory names. Retention is bounded and never deletes outside
  the explicit `runs/` target.
- HTML is standalone, opens locally, and renders without a server. Host
  dependencies are locked in a project virtual environment.
- Independent review checks trace-to-interval correctness, simultaneous event
  ordering, empty/corrupt input, escaping, artifact provenance, deterministic
  overload, timeout cleanup, and scope.

Local PR 7 implementation evidence (2026-09-04): `make demo` built and
validated separate soft-float F401RE normal and overload ELF files. It ran both
through the existing pinned Renode harness and strict schema-1 decoder. Normal
mode produced 92 of 256 possible records, 11 idle ticks, and no miss. Overload
mode produced 84 records, four idle ticks, and its only miss for the load task
at its absolute deadline on tick 12. Both runs closed without trace loss at
tick 21. Fifteen focused host model/report tests pass. Independent review is
complete and approved.

## Review and PR quality protocol

Before implementation, the lead assigns non-overlapping file ownership and a
dedicated worktree/branch. The implementer records exact commands and raw result
artifacts. The lead reviews every diff. A different agent performs adversarial
review against correctness, architecture assumptions, undefined behavior,
interrupt safety, alignment, failures, tests, documentation, and scope.

Architecture-sensitive PRs explicitly review exception frames, EXC_RETURN,
PSP/MSP transitions, eight-byte alignment, SVC decoding, PendSV/SysTick
priorities, register preservation, and floating-point ABI. A documentation-only
response cannot close a behavioral defect. Each finding is fixed, rejected with
evidence, or left open and the PR marked blocked. All affected build, native,
and Renode tests are rerun after changes.

Every draft PR description includes:

- problem and scope;
- design decisions;
- files/modules changed;
- exact test and demonstration commands;
- test results and retained artifacts;
- known limitations and non-goals;
- exact base/head and dependencies;
- reviewer findings and resolutions;
- generated timeline/screenshots when useful; and
- one judgment: ready for final review, blocked, or needs another pass.

A green compile alone is never sufficient. Campaign agents never merge or mark
an unresolved PR complete.

## Explicitly deferred features

The first campaign does not implement:

- a general-purpose shell;
- message queues, semaphores, mutexes, or event flags;
- networking, filesystems, USB, or dynamic executable loading;
- multiple architecture ports, SMP, or MPU-backed process isolation;
- POSIX compatibility;
- AI scheduling;
- large experiment sweeps or commit-to-commit benchmark dashboards;
- a full browser application; or
- hard real-time performance claims.

Hardware FPU context support is also deferred while the firmware uses the
soft-float ABI. A later diagnostic UART command interface may be proposed after
the vertical slice, but is not part of these PRs.

## Documentation policy

The README will separate “implemented and tested,” “implemented but
experimental,” “planned,” and “hardware-only/not validated.” It will not call
allocator metadata memory protection, claim synchronization primitives or
comprehensive interrupt management, claim clean-checkout builds before PR 1,
or equate emulator behavior with hardware timing. Malformed encoding in
architecture names will be corrected when encountered. Each PR changes claims
only after its own acceptance evidence exists.

## Known risks and unresolved decisions

| Risk or decision | Current position | Resolution gate |
|---|---|---|
| Renode F401RE model completeness | Resolved for the boot slice: local model uses `0x80000` flash, `0x18000` SRAM, 84 MHz SysTick, and only required peripherals. HAL boots with visible model warnings. This remains slice-accurate, not exact silicon. | Expand only when a later tested workload needs another peripheral. |
| Renode generic model fetches an unpinned SVD | Resolved for PR 2: the local runtime model has no URLs; static scan and the actual Robot boot passed inside a network namespace after setup. | Keep URL scan and offline evidence in later emulator PRs. |
| Reset versus VTOR | Reset still needs vectors at address zero before `SystemInit`; later exceptions use the `0x200`-aligned flash table through VTOR. | PR 1 ELF/VTOR checks and PR 2 boot-alias or explicit MSP/PC test. |
| Exact Arm archive URL/hash | Resolved in PR 1: official 14.3.rel1 x86_64 `arm-none-eabi` archive size/hash and consumed tool hashes are locked. | Reverify on any toolchain update. |
| CMSIS Core source | Resolved in PR 1: compatible Core headers are fetched sparsely from the pinned CubeF4 v1.28.3 commit. | Reverify on a Cube/CMSIS update. |
| Existing versus vendor startup | Resolved in PR 1: the pinned CMSIS-device F401xE startup is the only linked vector table. | Preserve the single-vector invariant. |
| HAL versus register-level board setup | HAL is retained initially because the repository already uses it; only required modules are compiled. | PR 2 RCC/UART experiment. |
| Kernel excluded from PR 1 boot link | Acceptable only if clearly reported and PR 3 links it; prefer inclusion after small compatibility fixes. | PR 1 feasibility review. |
| Newlib heap | Dynamic newlib allocation is disabled initially so the custom heap is the sole dynamic allocator. | Map and `_sbrk` tests in PR 1; revisit only with demonstrated need. |
| `printf` in boot path | Avoid dynamic/formatted output where a fixed UART write suffices. | Link/map/runtime check in PRs 1-2. |
| Exception-frame correctness | Existing assembly is not trusted merely because it links. | Independent architecture review and Renode lifecycle test in PR 3. |
| Tick wraparound contract | Relative durations remain below half the 32-bit range; runtime due/EDF order uses full 64-bit monotonic keys and fails before 64-bit exhaustion. | Native low-word wrap, exact-half, greater-than-half overdue, strict-order, and exhaustion-boundary tests in PR 4. |
| Allocator ownership on task exit | Resolved in PR 5 implementation: fixed stack reclaim remains deferred; every non-stack allocation is task-ID owned and auto-released by a bounded mark/coalesce sweep in PendSV before slot reuse. This prevents outstanding blocks from being inherited, but tags are not generation-aware stale-pointer detection; pointers retained after free/exit are invalid. This bookkeeping is not isolation. | Preserve native owner tests, ARM slot-reuse evidence, stale-pointer documentation, and linear interrupt-masked cleanup latency. |
| Trace record/wire encoding | Resolved in PR 6 implementation: naturally aligned 32-byte schema-1 records; `AYMT` framing version 1; CRC32; 28-byte terminal footer; strict bounded decoder; raw records and canonical JSON artifacts. | Preserve schema tests and require a version change for incompatible layouts. |
| Trace perturbation | Events change guest execution cost even without blocking output. Treat results as explanatory ordering, not timing proof. | Record overflow/loss and document in PR 6. |
| Deterministic execution demand | Use deterministic guest work/release units; never host wall-clock loops. | Repeated semantic traces in PRs 4 and 7. |
| Timeline renderer footprint | Resolved in PR 7: the report is dependency-free standalone HTML with inline SVG. | Preserve escaping and deterministic host report tests. |
| Python bootstrap reproducibility | Install a pinned CPython 3.12 `python-build-standalone` archive locally, then use hash-locked virtual environments; no system-Python fallback. | Exact runtime asset/hash and clean offline-after-setup test in PR 2. |
| Physical board behavior | No board is currently available in the stated environment. | Remains explicitly unverified until a recorded hardware run. |

When a gate fails, dependent work stops. The exact failure and artifacts are
recorded, a focused investigation repairs the owning PR, affected tests rerun,
and work resumes only after the gate passes. Host tooling must never hide or
reinterpret a kernel failure to obtain a passing demonstration.

## Cortex-M4 performance lab campaign

This campaign adds one useful DSP path. It keeps the existing kernel and
real-time lab stable. Each pull request has a separate evidence gate.

### Performance PR 1: deterministic Q15 FIR base

Status: implemented; review findings resolved; independent re-review approved.

- Add allocation-free scalar, portable paired, and Cortex-M4 paired FIR code.
- Define one exact numerical contract for 1 through 64 taps.
- Test invalid input, coefficient order, truncation, saturation, odd taps, and
  deterministic equivalence under native sanitizers.
- Inspect the optimized Cortex-M4 function and require `smlald` and `ssat`.
- Keep the kernel, scheduler, trace, and applications unchanged.

Acceptance gate:

```sh
make test-native-dsp
make check-dsp-codegen
```

The object-code report is static evidence. It does not make a hardware timing
claim.

First review found two implementation issues. The ARM objects did not depend on
their build configuration, and a failed check could leave an old passing
report. A content-stable configuration stamp now records the compiler, flags,
dependency lock, and CMSIS header. Every ARM DSP object depends on this stamp.
The check removes an old report before validation and replaces a new report
atomically. A mutation check confirms that a configuration change rebuilds the
objects and that a forced checker failure leaves no report.

Re-review found that an object compilation error occurs before the checker
recipe. The public check target now has no prerequisites. It removes the prior
report before it invokes a private target for setup, configuration, object
compilation, and validation. An invalid compiler-flag check confirms that a
compilation failure leaves no passing JSON artifact.

Review also found an implementation-defined unsigned-to-signed conversion in
the native corpus. The test now converts explicit two's-complement bits with
only representable signed casts. Documentation now states that PR 1 executes
only the scalar and portable paired functions. PR 2 owns M4 result equivalence.

### Performance PR 2: board-targeted DSP workload

Status: implemented; independent review approved. It depends on Performance PR
1. The two-variant gate must run again from the committed head before
publication.

- Add a three-task deterministic signal-processing workload to the existing
  lab. Process four 128-sample frames with a 16-tap FIR.
- Build separate scalar and Cortex-M4 DSP configurations from controlled source
  lists and compile the DSP translation units at `-O2`.
- Run each exact board-targeted firmware image once through the existing SVC,
  PendSV, PSP, SysTick, EDF, and schema-1 trace paths.
- Emit the fixed seed, input and output CRC-32 values, work units, and selected
  implementation after the structured trace.
- Require the exact 103-record schedule and exact scalar/M4 firmware result
  equality.
- Record a bounded synchronous `PCAndOpcode` execution trace. Correlate exact
  opcode bytes and addresses with each selected ELF. Require four selected FIR
  calls and, for M4, 3,616 executed `smlald` and 452 executed `ssat`
  instructions.
- Retain the exact firmware, map, run metadata, traces, results, logs, commands,
  and artifact hashes under `build/signal-lab/evidence/`.

Acceptance gate:

```sh
make test-host-signal-lab
make test-signal-lab
```

This pull request does not use a host scheduler or a host FIR result as a
substitute for firmware execution. It stops if the firmware results differ.
The execution trace is functional evidence. It is not a cycle or speed
measurement.

A bounded capability check against pinned Renode 1.16.1 confirmed synchronous
`PCAndOpcode` ReTrace version 4 output for `thumb cortex-m4`. Renode does not
provide a reliable address-range filter for this path. The test therefore
captures the short complete run, compresses it, and enforces 2 MiB compressed,
12 MiB uncompressed, and 2,000,000-entry limits.

The independent review found a SysTick lost-wake window between the execution
count check and `WFI`. The task now holds PRIMASK across the check and DSB/WFI,
then restores the caller mask and executes ISB. Review also found that the first
oracle did not check all event payloads. One exact semantic projection now
checks all 103 records, including creation state, deadline/job values, next
release, switch cause/state, exit counters, and selection metadata.

The review required stronger provenance and publication failure handling. The
collector now snapshots the ELF, map, and build metadata before each run. It
binds the run hash to that snapshot and rejects commit, configuration, ABI, or
live build-artifact changes. Marked same-file-system backup/install/rollback
logic protects the prior complete evidence. Controlled gzip, JSON, missing
metadata, symlink, and timeout failures have focused tests.

The review also clarified the instruction claim. Portable scalar C compiles to
one single-lane `smlalbb` site and must execute it 7,232 times. The explicit M4
paired loop must execute `smlald` 3,616 times and `ssat` 452 times. Neither is a
timing claim. Finally, `DSP_FIRMWARE_CFLAGS` is an internal override, and a
mutation test proves a command-line `-O0` cannot replace the effective
Cortex-M4 soft-float `-O2` flags.

After these changes, 17 focused host tests pass. Both scalar and M4 firmware
images compile and pass ELF, vector, memory-map, and soft-float validation.
`git diff --check`, Python syntax, and shell syntax checks pass. The independent
review approved the frozen diff. The final `make test-signal-lab` run belongs
to the committed-head publication step so its retained hashes identify the
published source.

### Performance PR 3: comparison report and project presentation

Status: planned. It depends on Performance PR 2.

- Generate one standalone scalar-versus-DSP comparison report.
- Keep each run in a bounded layout with firmware, configuration, trace,
  checksum, code-generation evidence, and tool metadata.
- Add one representative SVG and JSON result to the README.
- Modernize CI for the focused native, object-code, and firmware Signal Lab
  gates. Do not add repeated long runs.
- Rewrite the README in ASD-STE100-style direct English. Separate verified,
  experimental, planned, and hardware-only behavior.

The report will explain numerical results and observed firmware scheduling. It
will not turn emulator ordering into a hardware timing claim. DWT cycle counts
and physical-board measurement move to a later milestone. GPU support,
hard-float context switching, and a general benchmark framework remain deferred
because they do not improve this DSP slice.
