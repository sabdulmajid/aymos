# Third-party dependencies

Third-party source and executable packages are not vendored into this
repository. `make setup` installs the immutable inputs declared in
`tools/setup/dependencies.lock` under ignored `.deps/` and `.tools/`
directories. The build never searches global include or toolchain paths.

The PR 1 dependency set is:

| Component | Locked source | License retained after setup |
|---|---|---|
| Arm GNU Toolchain | Arm 14.3.rel1 x86_64 `arm-none-eabi` official archive | Toolchain `share/doc` and manifest files |
| CMSIS Core (M) | `STM32CubeF4` v1.28.3, sparse checkout of `Drivers/CMSIS/Core/Include` | `Drivers/CMSIS/LICENSE.txt` (Apache-2.0) |
| STM32F4 CMSIS device | `cmsis_device_f4` commit referenced by CubeF4 v1.28.3 | Upstream repository license (Apache-2.0) |
| STM32F4 HAL | `stm32f4xx_hal_driver` commit referenced by CubeF4 v1.28.3 | Upstream repository license (BSD-3-Clause) |

Only CMSIS Core headers and the two named STM32 driver repositories are
fetched. STM32Cube projects, middleware, examples, and click-through/restricted
components are not downloaded or compiled. The Cube checkout is both sparse and
blob-filtered, so excluded project/middleware blobs are not transferred.

PR 1 setup supports Linux x86_64 with Git 2.25 or newer and curl 7.61 or
newer. It also validates `bash`, `file`, `grep`, `make`, `sha256sum`, `stat`,
`tar`, and `xz`. Arm builds this toolchain on RHEL 8; hosts older than RHEL 8 or
Ubuntu 20.04 may lack required runtime libraries and are not claimed as
supported. Setup executes the compiler and fails clearly if its runtime is not
compatible.

The authoritative URLs, commits, archive byte size, and archive SHA-256 are in
the lock file. Updating a dependency requires an isolated lock change plus a
clean setup, build, ELF validation, and license review.
