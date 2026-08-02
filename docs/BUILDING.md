# Reproducible F401RE build and Renode boot

## Supported environment

The verified host path supports Linux x86_64. The bootstrap requires:

- `awk`;
- Bash;
- Git 2.25 or newer (sparse checkout and partial clone support);
- curl 7.61 or newer;
- GNU Make;
- a system `cc` with C11, AddressSanitizer, and UndefinedBehaviorSanitizer for
  native policy tests;
- `cmp`, `env`, `file`, `find`, `grep`, `od`, `readlink`, `sha256sum`, `sort`,
  `stat`, `tar`, `timeout`, `xargs`, and `xz`; and
- network access during `make setup`.

No root access, container daemon, global ARM compiler, global Renode, system
Python package, or global STM32 header is used. The Makefile rejects
command-line or environment definitions of
`ARM_GNU_DIR`, `CROSS_COMPILE`, `CC`, `OBJCOPY`, `OBJDUMP`, `READELF`, `SIZE`,
`NM`, `RENODE_DIR`, `RENODE`, `PYTHON_ENV_DIR`, and `PYTHON`; this prevents a
caller, including `make -e`, from bypassing the locked-tool integrity gate.
`BOARD` and `APP` remain ordinary documented build selectors. Arm's x86_64
toolchain is built on RHEL 8; hosts older than RHEL 8 or Ubuntu 20.04 may lack
compatible runtime libraries. Setup executes the tools and reports an
incompatibility rather than allowing a later, ambiguous error.

Only Linux x86_64 is claimed in this PR. Other host architectures must add and
validate their own locked tool archive instead of falling back to `PATH`.
Likewise, any future user-selectable tool path must first implement the same
selected-path and content-hash validation and record the selection in build
metadata. Arbitrary tool overrides are not a supported escape hatch.

## Install the locked inputs

```sh
make setup
```

Inputs are declared in `tools/setup/dependencies.lock`:

- Arm GNU Toolchain 14.3.rel1 for x86_64 `arm-none-eabi`;
- CMSIS Core from STM32CubeF4 v1.28.3;
- the STM32F4 CMSIS device commit referenced by that Cube release; and
- the STM32F4 HAL commit referenced by that Cube release;
- Renode 1.16.1's portable .NET x86_64 archive;
- CPython 3.12.13 from the locked `python-build-standalone` release; and
- exact wheels and hashes for Robot Framework 6.1, retryfailed 0.2.0, psutil
  5.9.8, PyYAML 6.0.3, and telnetlib3 2.0.8.

The Arm archive byte size and SHA-256 are checked before extraction. Every
consumed Arm executable (`gcc`, `as`, `ld`, `nm`, `objcopy`, `objdump`,
`readelf`, and `size`) is checked against a locked digest, and an installation
manifest ties them back to the verified archive. STM32 dependencies are checked
for exact HEAD and for modified/untracked files. Required sources, headers, and
licenses are validated. The Cube parent is blob-filtered and sparse: projects,
middleware, and excluded blobs are not downloaded.

Renode, CPython, and the Renode virtual environment have deterministic tree
manifests covering directory paths, regular-file paths and hashes, and symlink
paths and target strings. Validation rejects additions, removals, type changes,
target changes, and special nodes. Only regular `*.pyc` files directly under
`__pycache__` are excluded as interpreter-generated mutable cache state. All
locked-Python commands also unset `PYTHONHOME` and `PYTHONPATH`, set
`PYTHONNOUSERSITE=1`, and disable automatic bytecode writes.

Python wheels are installed offline with `--no-index`, `--only-binary`, and
`--require-hashes`. Renode upstream names psutil 5.9.3; AymOS intentionally
uses a locally tested 5.9.8 substitution because 5.9.3 has no CPython 3.12
wheel and would add an undeclared host C compiler input. This is not described
as satisfying an exact `==5.9.3` constraint.

Setup uses a repository-local concurrency lock. Interrupted temporary paths use
unique names and are cleaned on normal exit, SIGINT, and SIGTERM. A complete
`.part` is verified and promoted without asking curl to resume it; invalid or
oversized partial files are quarantined instead of causing a persistent HTTP
416 loop.

`.tools/` and `.deps/` are ignored. They are immutable build inputs: setup
refuses to reset or overwrite local dependency changes.

`tools/setup.sh --check` is the offline/read-only validation mode. The Makefile
uses it as an ordering gate for every object, so even `make -j` cannot compile
before dependency/tool integrity and cleanliness pass.

## Build and validate

```sh
make firmware
```

This is equivalent to:

```sh
make firmware BOARD=nucleo_f401re APP=boot
```

The verified lifecycle image is selected explicitly:

```sh
make firmware BOARD=nucleo_f401re APP=lifecycle
```

It uses the same startup, linker map, board support, locked dependencies, and
ELF validator. Its artifacts are under `build/nucleo_f401re/lifecycle/`.

The deterministic EDF image uses the same kernel and policy:

```sh
make firmware BOARD=nucleo_f401re APP=edf
```

Its artifacts are under `build/nucleo_f401re/edf/`.

The Makefile invokes the compiler by its absolute project-local path. Project
code uses `-Wall -Wextra -Werror` plus additional diagnostics. Vendor code uses
visible `-Wall -Wextra` warnings without converting upstream warnings into
project errors. Source lists are explicit and application selection fails for
unknown names.

The output directory is `build/nucleo_f401re/boot/`. The build produces:

- `aymos.elf`: debug-enabled ARM executable;
- `aymos.bin`: flashable raw image;
- `aymos.map`: linker map and cross-reference;
- `size.txt`: Berkeley-format size report;
- `build-metadata.txt`: board, app, Git state, compiler, flags, dependency
  revisions, and successful integrity/clean-state fields; and
- inspection artifacts from `file`, `readelf`, `nm`, `objdump`, and the raw
  vector section.

Validation checks:

- ELF32 little-endian ARM, EABI5, ARMv7E-M/Thumb-2, soft-float ABI;
- absence of hard-float procedure-call attributes;
- loadable/allocated sections contained in 512 KiB flash or 96 KiB SRAM;
- the complete vector table at `0x08000000`, aligned to `0x200`;
- actual initial vector words equal to the linked MSP top and Thumb
  `Reset_Handler`;
- every PT_LOAD has `MemSiz >= FileSiz`, fits its VMA in F401RE flash/SRAM,
  keeps every non-empty file payload LMA in flash, and includes a non-empty
  segment whose VMA and LMA both start at the flash origin;
- ELF entry equals `Reset_Handler | 1`;
- focused disassembly confirms that reset calls `SystemInit` before `main`;
- `Reset_Handler` resides in flash;
- eight-byte-aligned custom heap boundaries;
- an empty newlib heap that cannot overlap the AymOS heap;
- a 4 KiB MSP reservation ending at `0x20018000`; and
- corresponding heap/stack symbols in the map.

The validator retains `reset-handler-disassembly.txt` and
`system-init-disassembly.txt`. The reset call order is checked automatically.
The PR 1 review manually inspects the SystemInit artifact for the VTOR store;
the validator does not claim to semantically prove arbitrary disassembly.

Run validation again without recompiling:

```sh
make validate
```

Generate source/assembly disassembly:

```sh
make disassembly
```

## Clean reproduction

Build products can be removed and regenerated without reinstalling tools:

```sh
make clean
make firmware
```

To exercise dependency bootstrap from scratch, move or delete only the explicit
ignored `.tools/` and `.deps/` directories, then run:

```sh
make setup
make setup
make firmware
make test
make run
make test-emulator
```

The second setup invocation proves idempotence. Do not point cleanup commands at
the repository root or a home directory.

## Memory ownership and allocator boundary

The linker reserves all free SRAM between `.bss` and the 4 KiB MSP region as
the AymOS heap and exports `__aymos_heap_start__`/`__aymos_heap_end__`. Those
boundaries are eight-byte aligned. The newlib heap start and end are identical,
and the retained `_sbrk` implementation always returns `ENOMEM`.

The boot app does not need dynamic memory. Kernel apps initialize the hardened
allocator over this exact interval, validate its runtime boundaries, and never
delegate it to newlib. The portable core is `kernel/src/allocator.c`; the
obsolete `k_mem` implementation was removed in PR 5.

## Headless Renode workflow

Run the host-side model and UART validator tests:

```sh
make test
```

Boot the exact `build/nucleo_f401re/boot/aymos.elf` for 0.2 seconds of Renode
virtual time and print the validated raw USART2 stream:

```sh
make run
```

Run the Robot integration test:

```sh
make test-emulator
```

Robot independently checks that address zero aliases the flash vector, the
initial MSP word is `0x20018000`, execution reaches both UART lines, and VTOR is
`0x08000000` after startup. Its UART wait is five seconds, its Robot case limit
is ten seconds, and the entire host process is bounded by GNU `timeout` with a
TERM/KILL sequence. The final oracle parses raw UART bytes and requires exactly
one of each line, in order:

```text
AYMOS READY\r\n
AYMOS SMOKE SYSTICK=1 SVC=1\r\n
```

Every invocation gets a new directory under `build/renode/run/` or
`build/renode/test/`. It retains command and metadata files, ELF/platform
hashes, raw and decoded UART, emulator logs, and Robot XML/HTML outputs. On
failure the scripts return nonzero, print the retained directory, and show the
tails of available logs plus a raw UART hex dump.

Ten independent boots exercise reset/startup repeatability:

```sh
RENODE_REPEAT=10 make test-emulator
```

After setup, the checked runtime inputs contain no URLs. Where the host permits
unprivileged user/network namespaces, the actual Robot/Renode process can also
be run with all non-loopback networking removed:

```sh
make test-emulator-offline
```

This optional evidence command needs host `unshare` and `ip`; it restores only
loopback because Robot communicates with Renode through a bounded localhost
server. The namespace command is itself under the outer timeout.

The repository model intentionally covers only this slice: flash and its reset
alias, SRAM, Cortex-M4/NVIC/SysTick, flash control, RCC/PWR, RTC/EXTI needed by
the model, GPIOA, USART2, and the user LED. Renode currently reports visible
warnings for unmodeled flash cache-enable bits and one RCC reserved bit during
HAL clock initialization. Those warnings are retained in `emulator.log`; the
firmware nevertheless completes HAL initialization and both interrupt smokes.
The model does not represent every F401RE peripheral or register.

The CI workflow runs setup, firmware validation, host tests, and three fresh
Renode boots plus three lifecycle and three EDF scenarios on Ubuntu 24.04, then
retains build/emulator artifacts even on failure. PR 2's hosted boot workflow
passed; each stacked PR must rerun its own hosted check.

## Cortex-M4 lifecycle workflow

Build and print the exact guest-generated lifecycle stream:

```sh
make run-lifecycle
```

Assert the same stream through the bounded Robot harness:

```sh
make test-lifecycle
RENODE_REPEAT=10 make test-lifecycle
```

For a network-isolated run, select the lifecycle app on the existing offline
target:

```sh
make APP=lifecycle test-emulator-offline
```

The lifecycle firmware fabricates an eight-byte-aligned basic exception frame,
places the caller's argument in stacked R0, starts at an entry trampoline, and
returns through `os_task_exit`. SVC validates the stacked PC/opcode and MSP/PSP
origin. PendSV saves and restores R4-R11, commits the single-RUNNING-task state
under PRIMASK while using MSP, and returns with `EXC_RETURN=0xFFFFFFFD` under the
soft-float ABI. SysTick has higher urgency than PendSV; PendSV is lowest.

The exact UART contract distinguishes voluntary yields from a preemption that
can only occur after a sleeping task is awakened in SysTick. Assembly probes
verify R4-R11 across both switch types. After each task returns, another thread
observes that the old fixed stack slot was reclaimed, and the idle task proves
continued PSP execution. Panic output, missing/extra/duplicate/reordered bytes,
or a timeout fail the test and retain diagnostics.

This is functional Cortex-M4 model evidence. The fixed stack pool remains a
narrow mechanism separate from the PR 5 dynamic heap. Sleep accepts
only 1 through `INT32_MAX` ticks; zero/larger intervals are rejected before SVC
and exercised by the lifecycle workload.

## Native EDF policy and ARM workload

`make test` includes `make test-native`. The explicit bootstrap compiler is
system `cc`; its resolved path and full `--version` output are retained in
`build/native/compiler.txt`. Every `make test-native` rebuilds through temporary
files and records the resolved/real compiler path, compiler SHA-256,
full version, exact flags, every project input hash, and resulting binary hash.
The recipe verifies that final hash before success, so a cached executable cannot
be paired with new provenance. Strict native tests use ASan/UBSan and link the
same `kernel/src/scheduler.c` as ARM firmware—not a host reimplementation.

```sh
make run-edf
make test-edf
RENODE_REPEAT=10 make test-edf
make APP=edf test-emulator-offline
```

Task 2 releases at tick 0/deadline 50. Periodic task 1 releases at tick
5/deadline 15, preempts task 2, completes via `os_wait_next_period`, then
releases on its fixed cadence at tick 20/deadline 30 and preempts again. The
same fixture constants drive native and Cortex-M tests. Robot and the raw UART
parser reject missing, duplicate, extra, or reordered semantic lines.

## Native allocator and ARM ownership workload

`make test-native` also rebuilds `kernel/src/allocator.c` with strict host
warnings, AddressSanitizer, and UndefinedBehaviorSanitizer. Its separate
compiler/input/binary provenance is retained in
`build/native/compiler-allocator.txt`. Deterministic tests cover aligned and
misaligned arenas, split/exact-fit, next-fit continuation/wrap, coalescing,
exhaustion/recovery, a 1024-byte stack-sized block, invalid/interior/boundary/
double/foreign-owner frees, owner sweep, fragmentation/statistics, payload
canaries, and fail-closed metadata corruption.

Run the real ARM task-owned memory scenario with:

```sh
make run-allocator
make test-allocator
RENODE_REPEAT=10 make test-allocator
make APP=allocator test-emulator-offline
```

Task 1 allocates a persistent canary buffer, then creates an immediately
higher-ranked task 2 eight times at runtime. Each new task preempts through the
normal pending PendSV path, allocates aligned buffers including 1024 bytes,
rejects a foreign-owner free, an overflowing request, and a double free,
verifies PRIMASK restoration and payload canaries, then returns. PendSV runs on
MSP, releases the exiting task's two outstanding buffers, and makes slot 2
reusable. The manager verifies the heap and counters after every round; idle
requires zero allocated blocks and one fully coalesced free block. The exact raw
UART validator rejects any missing, extra, duplicated, or reordered result.

This is allocator/lifecycle correctness evidence in Renode, not a hardware
latency or worst-case exit-sweep measurement. Owner cleanup masks interrupts
for a linear mark-and-coalesce pass, so keeping the educational heap small and
freeing long-lived allocations explicitly are part of the current contract.

Periodic scheduling is constrained-deadline and single-active-job. At a cadence
boundary, an unfinished active job receives a deadline miss at equality when
applicable; the unavailable release is separately counted in
`missed_release_count`, and cadence advances. It is neither overlapped nor
recreated from completion time. Execution counters saturate at `UINT32_MAX`;
configured execution budget is observable but not enforced.

All runtime release, wake, miss, and EDF decisions use a 64-bit monotonic key.
The 32-bit tick values shown in UART and task info are low-word views only.
Native fixtures cross `UINT32_MAX`, compare deadlines separated by exactly
`2^31`, and retain strict order for jobs overdue by more than `2^31` ticks.
The kernel panics before the monotonic counter could wrap at `UINT64_MAX`.

## Application and hardware status

Startup `SystemInit` configures VTOR before `APP=boot` initializes HAL, an
84 MHz HSI/PLL clock, PA5, and USART2. The app emits `AYMOS READY`, waits until
the application-local SysTick flag is observed, invokes SVC, checks its
application-local flag, emits the smoke result from thread mode, and then
waits for interrupts. Neither handler prints, schedules, switches context, nor
uses PSP/PendSV. Select `APP=lifecycle` for the separately tested real kernel
path; keeping the smoke app independent protects the PR 2 boot regression.
Select `APP=edf` for explicit periodic timing/EDF behavior.
Select `APP=allocator` for runtime task creation, owner cleanup, and allocator
invariant evidence.

Physical flashing is not validated. `make flash` builds the binary, prints that
limitation, and exits nonzero rather than running an undocumented global
programmer. Renode execution is functional model evidence only; its virtual or
host time does not prove hardware timing, interrupt latency, WCET, or hard
real-time behavior.
