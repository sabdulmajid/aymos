# Reproducible F401RE build

## Supported environment

PR 1 supports Linux x86_64. The bootstrap requires:

- `awk`;
- Bash;
- Git 2.25 or newer (sparse checkout and partial clone support);
- curl 7.61 or newer;
- GNU Make;
- `file`, `grep`, `od`, `sha256sum`, `stat`, `tar`, and `xz`; and
- network access during `make setup`.

No root access, container daemon, global ARM compiler, or global STM32 headers
are used. The Makefile rejects command-line or environment definitions of
`ARM_GNU_DIR`, `CROSS_COMPILE`, `CC`, `OBJCOPY`, `OBJDUMP`, `READELF`, `SIZE`,
and `NM`; this prevents a caller, including `make -e`, from bypassing the
locked-tool integrity gate. `BOARD` and `APP` remain ordinary documented build
selectors. Arm's x86_64 toolchain is built on RHEL 8; hosts older than RHEL 8 or
Ubuntu 20.04 may lack compatible runtime libraries. Setup executes the compiler
and reports that failure rather than allowing a later, ambiguous build error.

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
- the STM32F4 HAL commit referenced by that Cube release.

The Arm archive byte size and SHA-256 are checked before extraction. Every
consumed Arm executable (`gcc`, `as`, `ld`, `nm`, `objcopy`, `objdump`,
`readelf`, and `size`) is checked against a locked digest, and an installation
manifest ties them back to the verified archive. STM32 dependencies are checked
for exact HEAD and for modified/untracked files. Required sources, headers, and
licenses are validated. The Cube parent is blob-filtered and sparse: projects,
middleware, and excluded blobs are not downloaded.

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
```

The second setup invocation proves idempotence. Do not point cleanup commands at
the repository root or a home directory.

## Memory ownership in PR 1

The linker reserves all free SRAM between `.bss` and the 4 KiB MSP region as
the AymOS heap and exports `__aymos_heap_start__`/`__aymos_heap_end__`. Those
boundaries are eight-byte aligned. The newlib heap start and end are identical,
and the retained `_sbrk` implementation always returns `ENOMEM`.

The legacy AymOS allocator is not linked into the boot app. Its later hardening
will consume the already-reserved linker region. PR 1 therefore establishes
non-overlapping ownership without claiming that the allocator itself is fixed.

## Application and hardware status

Startup `SystemInit` configures VTOR before `APP=boot` initializes HAL, an
84 MHz HSI/PLL clock, PA5, and USART2. The app then emits
`AYMOS BOOT F401RE` and waits for interrupts. SysTick only advances the HAL
tick; it does not call legacy scheduler code.

Renode boot/testing is PR 2. Physical flashing is not validated. `make flash`
builds the binary, prints that limitation, and exits nonzero rather than running
an undocumented global programmer.
