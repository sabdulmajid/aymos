# AymOS Real-Time Systems Lab

AymOS is becoming a small, inspectable Cortex-M4 real-time systems lab. The
current verified slice builds a board-targeted boot image reproducibly and
boots that exact ELF headlessly in pinned Renode. The EDF kernel source is
retained for the next architecture/lifecycle PR, but is not linked into the
boot application and is not yet claimed to execute safely.

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
  evidence, retained failure artifacts, and a pinned CI workflow definition
  whose hosted run passed on Ubuntu 24.04.

## Build

The supported host is Linux x86_64. Required bootstrap commands are
`awk`, `bash`, Git 2.25+, curl 7.61+, `make`, `cmp`, `env`, `file`, `find`,
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
```

`make setup` downloads locked Arm, Renode, CPython, Python-wheel, and STM32
inputs under `.tools/` and `.deps/`. Archives and wheels are checked for exact
byte size and SHA-256 before use. It is safe to rerun and rejects modified
dependency contents or unexpected files rather than overwriting them.

Every build first runs `tools/setup.sh --check`, an offline/read-only integrity
gate. Compilation cannot start until all consumed tool hashes, dependency
commits, clean states, files, and licenses pass.

The only current selection is explicit:

```sh
make firmware BOARD=nucleo_f401re APP=boot
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

## Implemented but experimental

The legacy source tree contains:

- a 16-slot task table with an EDF-like scan and priority tie breaker;
- Cortex-M SVC/PendSV assembly intended to save and restore R4-R11 on PSP; and
- a linked-list allocator with splitting, task-owner metadata, and coalescing.

These components are educational prototypes. They currently have known task
state, frame, timing-model, interrupt, alignment, and allocator issues recorded
in [the implementation plan](docs/IMPLEMENTATION_PLAN.md). Allocator ownership
metadata is not hardware memory protection or task isolation.

The API inventory in [docs/API_REFERENCE.md](docs/API_REFERENCE.md) describes
the legacy source surface, not a verified runtime contract.

## Planned campaign

The reviewed sequence is:

1. reproducible F401RE build (implemented and tested);
2. Renode boot harness and emulator tests (implemented and tested locally);
3. task lifecycle and Cortex-M context-switch correctness;
4. explicit timing semantics and deterministic EDF tests;
5. allocator hardening;
6. bounded structured kernel tracing; and
7. the Deadline Lab workload and standalone scheduling timeline.

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
