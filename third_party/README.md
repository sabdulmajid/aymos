# Third-party dependencies

Third-party source and executable packages are not vendored into this
repository. `make setup` installs the immutable inputs declared in
`tools/setup/dependencies.lock` under ignored `.deps/` and `.tools/`
directories. The build never searches global include or toolchain paths.

The current dependency set is:

| Component | Locked source | License retained after setup |
|---|---|---|
| Arm GNU Toolchain | Arm 14.3.rel1 x86_64 `arm-none-eabi` official archive | Toolchain `share/doc` and manifest files |
| CMSIS Core (M) | `STM32CubeF4` v1.28.3, sparse checkout of `Drivers/CMSIS/Core/Include` | `Drivers/CMSIS/LICENSE.txt` (Apache-2.0) |
| STM32F4 CMSIS device | `cmsis_device_f4` commit referenced by CubeF4 v1.28.3 | Upstream repository license (Apache-2.0) |
| STM32F4 HAL | `stm32f4xx_hal_driver` commit referenced by CubeF4 v1.28.3 | Upstream repository license (BSD-3-Clause) |
| Renode | 1.16.1 portable .NET x86_64 release archive | `licenses/renode-license` and bundled component licenses |
| CPython | 3.12.13 `python-build-standalone` install-only archive, release 20260718 | `lib/python3.12/LICENSE.txt` |
| Robot test packages | Exact hash-locked wheels listed below | Wheel `.dist-info` license/metadata files |

The Renode Python environment contains Robot Framework 6.1,
robotframework-retryfailed 0.2.0, psutil 5.9.8, PyYAML 6.0.3, and telnetlib3
2.0.8. These are the complete runtime set installed with `--no-index` and
`--require-hashes`. Renode upstream names psutil 5.9.3; AymOS intentionally
substitutes locally tested 5.9.8 because 5.9.3 has no CPython 3.12 wheel and
would make a host C compiler an undocumented input. This is not claimed to
satisfy an exact `==5.9.3` constraint.

Only CMSIS Core headers and the two named STM32 driver repositories are
fetched. STM32Cube projects, middleware, examples, and click-through/restricted
components are not downloaded or compiled. The Cube checkout is both sparse and
blob-filtered, so excluded project/middleware blobs are not transferred.

Setup supports Linux x86_64 with Git 2.25 or newer and curl 7.61 or newer. It
validates the bootstrap utilities listed in `docs/BUILDING.md`, including the
commands used for deterministic tree manifests and bounded Renode version
checks. Arm builds this toolchain on RHEL 8; hosts older than RHEL 8 or Ubuntu
20.04 may lack required runtime libraries and are not claimed as supported.
Setup executes the compiler and fails clearly if its runtime is not compatible.

The authoritative URLs, commits, archive/wheel byte sizes, and SHA-256 values
are in `tools/setup/dependencies.lock`; the exact Python installation set is in
`tools/setup/renode-requirements.lock`. Updating a dependency requires an
isolated lock change plus clean setup, build, host tests, emulator tests, and
license review.
