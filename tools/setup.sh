#!/usr/bin/env bash

set -euo pipefail
export LC_ALL=C

readonly script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly repo_root="$(cd -- "${script_dir}/.." && pwd)"
readonly lock_file="${script_dir}/setup/dependencies.lock"

# The lock file is project-owned input and intentionally uses shell assignments.
# shellcheck source=tools/setup/dependencies.lock
source "${lock_file}"

readonly tools_dir="${repo_root}/.tools"
readonly deps_dir="${repo_root}/.deps"
readonly downloads_dir="${tools_dir}/downloads"
readonly toolchain_dir="${tools_dir}/${ARM_GNU_DIRECTORY}"
readonly toolchain_marker="${toolchain_dir}/.aymos-install-manifest"
readonly setup_lock_dir="${tools_dir}/setup.lock"
temporary_paths=()
setup_lock_owned=false

die() {
    printf 'setup: error: %s\n' "$*" >&2
    exit 1
}

note() {
    printf 'setup: %s\n' "$*"
}

cleanup_temporary_paths() {
    local path

    for path in "${temporary_paths[@]}"; do
        case "${path}" in
            "${tools_dir}"/.extract-*|"${deps_dir}"/.*.fetch-*)
                [[ ! -e "${path}" ]] || rm -rf -- "${path}"
                ;;
            *)
                printf 'setup: refusing to clean unexpected temporary path: %s\n' \
                    "${path}" >&2
                ;;
        esac
    done

    if [[ "${setup_lock_owned}" == true && -d "${setup_lock_dir}" ]]; then
        rm -f -- "${setup_lock_dir}/pid"
        rmdir -- "${setup_lock_dir}"
    fi
}

trap cleanup_temporary_paths EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

require_command() {
    command -v "$1" >/dev/null 2>&1 || die "required host command not found: $1"
}

version_at_least() {
    local actual="$1"
    local required_major="$2"
    local required_minor="$3"
    local major="${actual%%.*}"
    local rest="${actual#*.}"
    local minor="${rest%%.*}"

    [[ "${major}" =~ ^[0-9]+$ && "${minor}" =~ ^[0-9]+$ ]] || return 1
    ((major > required_major || (major == required_major && minor >= required_minor)))
}

sha256_file() {
    sha256sum "$1" | awk '{print $1}'
}

archive_matches() {
    local archive_path="$1"
    local expected="$2"
    local expected_size="$3"
    local actual
    local actual_size

    [[ -f "${archive_path}" ]] || return 1
    actual="$(sha256_file "${archive_path}")"
    actual_size="$(stat -c '%s' "${archive_path}")"
    [[ "${actual}" == "${expected}" && "${actual_size}" == "${expected_size}" ]]
}

quarantine_file() {
    local path="$1"
    local reason="$2"
    local quarantine="${path}.rejected-${reason}-$$"

    mv -- "${path}" "${quarantine}"
    note "quarantined invalid download as ${quarantine}"
}

prepare_toolchain_archive() {
    local archive_path="$1"
    local partial_path="$2"
    local size
    local digest

    if archive_matches "${archive_path}" "${ARM_GNU_SHA256}" "${ARM_GNU_SIZE}"; then
        return
    elif [[ -f "${archive_path}" ]]; then
        quarantine_file "${archive_path}" archive
    fi

    if [[ -f "${partial_path}" ]]; then
        size="$(stat -c '%s' "${partial_path}")"
        if ((size == ARM_GNU_SIZE)); then
            if archive_matches "${partial_path}" "${ARM_GNU_SHA256}" "${ARM_GNU_SIZE}"; then
                mv -- "${partial_path}" "${archive_path}"
                return
            fi
            quarantine_file "${partial_path}" checksum
        elif ((size > ARM_GNU_SIZE)); then
            quarantine_file "${partial_path}" oversized
        fi
    fi

    note "downloading Arm GNU Toolchain ${ARM_GNU_VERSION}"
    curl --fail --location --retry 3 --silent --show-error --continue-at - \
        --output "${partial_path}" "${ARM_GNU_URL}"
    if ! archive_matches "${partial_path}" "${ARM_GNU_SHA256}" "${ARM_GNU_SIZE}"; then
        size="$(stat -c '%s' "${partial_path}")"
        digest="$(sha256_file "${partial_path}")"
        quarantine_file "${partial_path}" downloaded
        die "downloaded toolchain mismatch: expected ${ARM_GNU_SIZE}/${ARM_GNU_SHA256}, got ${size}/${digest}"
    fi
    mv -- "${partial_path}" "${archive_path}"
}

tool_hash() {
    case "$1" in
        as) printf '%s\n' "${ARM_GNU_AS_SHA256}" ;;
        gcc) printf '%s\n' "${ARM_GNU_GCC_SHA256}" ;;
        ld) printf '%s\n' "${ARM_GNU_LD_SHA256}" ;;
        nm) printf '%s\n' "${ARM_GNU_NM_SHA256}" ;;
        objcopy) printf '%s\n' "${ARM_GNU_OBJCOPY_SHA256}" ;;
        objdump) printf '%s\n' "${ARM_GNU_OBJDUMP_SHA256}" ;;
        readelf) printf '%s\n' "${ARM_GNU_READELF_SHA256}" ;;
        size) printf '%s\n' "${ARM_GNU_SIZE_SHA256}" ;;
        *) die "no locked digest for ARM tool: $1" ;;
    esac
}

write_toolchain_marker() {
    local compiler="${toolchain_dir}/bin/arm-none-eabi-gcc"
    local compiler_digest
    local tool

    compiler_digest="$(sha256_file "${compiler}")"
    [[ "${compiler_digest}" == "${ARM_GNU_GCC_SHA256}" ]] ||
        die "installed compiler checksum does not match the locked archive"
    {
        printf 'marker_version=1\n'
        printf 'archive=%s\n' "${ARM_GNU_ARCHIVE}"
        printf 'archive_sha256=%s\n' "${ARM_GNU_SHA256}"
        printf 'archive_size=%s\n' "${ARM_GNU_SIZE}"
        for tool in as gcc ld nm objcopy objdump readelf size; do
            printf 'tool_sha256.arm-none-eabi-%s=%s\n' "${tool}" "$(tool_hash "${tool}")"
        done
    } > "${toolchain_marker}"
}

validate_toolchain_marker() {
    local compiler="${toolchain_dir}/bin/arm-none-eabi-gcc"
    local tool
    local expected

    [[ -f "${toolchain_marker}" ]] || die "toolchain install marker is missing"
    grep -Fxq 'marker_version=1' "${toolchain_marker}" ||
        die "toolchain marker version mismatch"
    grep -Fxq "archive=${ARM_GNU_ARCHIVE}" "${toolchain_marker}" ||
        die "toolchain marker archive mismatch"
    grep -Fxq "archive_sha256=${ARM_GNU_SHA256}" "${toolchain_marker}" ||
        die "toolchain marker checksum mismatch"
    grep -Fxq "archive_size=${ARM_GNU_SIZE}" "${toolchain_marker}" ||
        die "toolchain marker size mismatch"
    for tool in as gcc ld nm objcopy objdump readelf size; do
        expected="$(tool_hash "${tool}")"
        [[ -x "${toolchain_dir}/bin/arm-none-eabi-${tool}" ]] ||
            die "toolchain executable is missing: arm-none-eabi-${tool}"
        grep -Fxq "tool_sha256.arm-none-eabi-${tool}=${expected}" \
            "${toolchain_marker}" ||
            die "toolchain marker digest mismatch: arm-none-eabi-${tool}"
        [[ "$(sha256_file "${toolchain_dir}/bin/arm-none-eabi-${tool}")" == "${expected}" ]] ||
            die "toolchain executable was modified after setup: arm-none-eabi-${tool}"
    done
    [[ -f "${toolchain_dir}/license.txt" ]] || die "toolchain license is missing"
    [[ -f "${toolchain_dir}/14.3.rel1-x86_64-arm-none-eabi-manifest.txt" ]] ||
        die "toolchain upstream manifest is missing"
}

install_toolchain() {
    local archive_path="${downloads_dir}/${ARM_GNU_ARCHIVE}"
    local partial_path="${archive_path}.part"
    local extract_parent

    prepare_toolchain_archive "${archive_path}" "${partial_path}"

    if [[ -x "${toolchain_dir}/bin/arm-none-eabi-gcc" ]]; then
        [[ -f "${toolchain_marker}" ]] ||
            die "refusing pre-existing toolchain without an AymOS install marker: ${toolchain_dir}"
        validate_toolchain_marker
        note "Arm GNU Toolchain already installed"
        return
    fi

    extract_parent="${tools_dir}/.extract-${ARM_GNU_DIRECTORY}-$$"
    temporary_paths+=("${extract_parent}")
    mkdir -p -- "${extract_parent}"
    note "extracting Arm GNU Toolchain ${ARM_GNU_VERSION}"
    tar -xJf "${archive_path}" -C "${extract_parent}"
    [[ -x "${extract_parent}/${ARM_GNU_DIRECTORY}/bin/arm-none-eabi-gcc" ]] ||
        die "toolchain archive did not contain the expected compiler"
    [[ ! -e "${toolchain_dir}" ]] ||
        die "incomplete toolchain directory exists: ${toolchain_dir}"
    mv -- "${extract_parent}/${ARM_GNU_DIRECTORY}" "${toolchain_dir}"
    rmdir -- "${extract_parent}"
    write_toolchain_marker
}

validate_git_dependency() {
    local destination="$1"
    local expected="$2"
    local name="$3"
    local actual
    local status

    [[ -d "${destination}/.git" ]] || die "${name} Git metadata is missing"
    actual="$(git -C "${destination}" rev-parse HEAD)"
    [[ "${actual}" == "${expected}" ]] ||
        die "${name} is at ${actual}, expected ${expected}"
    status="$(GIT_OPTIONAL_LOCKS=0 git -C "${destination}" status \
        --porcelain=v1 --untracked-files=all)"
    [[ -z "${status}" ]] ||
        die "${name} contains local modifications or untracked files; refusing to overwrite them"
}

install_git_dependency() {
    local name="$1"
    local url="$2"
    local commit="$3"
    local destination="${deps_dir}/${name}"
    local temporary="${deps_dir}/.${name}.fetch-$$"
    local actual

    temporary_paths+=("${temporary}")

    if [[ -d "${destination}/.git" ]]; then
        validate_git_dependency "${destination}" "${commit}" "${name}"
        note "${name} already installed at ${commit}"
        return
    fi

    [[ ! -e "${destination}" ]] ||
        die "non-git dependency path exists: ${destination}"
    mkdir -p -- "${temporary}"
    git -C "${temporary}" init --quiet
    git -C "${temporary}" remote add origin "${url}"
    note "fetching ${name} at ${commit}"
    git -C "${temporary}" fetch --quiet --depth 1 origin "${commit}"
    git -C "${temporary}" -c advice.detachedHead=false checkout --quiet --detach FETCH_HEAD
    actual="$(git -C "${temporary}" rev-parse HEAD)"
    [[ "${actual}" == "${commit}" ]] ||
        die "${name} resolved to ${actual}, expected ${commit}"
    mv -- "${temporary}" "${destination}"
}

install_cube_cmsis_core() {
    local destination="${deps_dir}/stm32cube_f4_core"
    local temporary="${deps_dir}/.stm32cube_f4_core.fetch-$$"
    local actual
    local tag_object
    local peeled

    temporary_paths+=("${temporary}")

    if [[ -d "${destination}/.git" ]]; then
        validate_git_dependency "${destination}" "${STM32_CUBE_COMMIT}" \
            "STM32CubeF4 CMSIS Core"
        note "STM32CubeF4 CMSIS Core already installed at ${STM32_CUBE_COMMIT}"
        return
    fi

    [[ ! -e "${destination}" ]] ||
        die "non-git dependency path exists: ${destination}"
    mkdir -p -- "${temporary}"
    git -C "${temporary}" init --quiet
    git -C "${temporary}" remote add origin "${STM32_CUBE_URL}"
    git -C "${temporary}" config remote.origin.promisor true
    git -C "${temporary}" config remote.origin.partialclonefilter blob:none
    git -C "${temporary}" sparse-checkout init --no-cone
    git -C "${temporary}" sparse-checkout set \
        /Drivers/CMSIS/Core/Include/ /Drivers/CMSIS/LICENSE.txt
    note "fetching STM32CubeF4 ${STM32_CUBE_TAG} CMSIS Core at ${STM32_CUBE_COMMIT}"
    git -C "${temporary}" fetch --quiet --depth 1 --filter=blob:none origin \
        "refs/tags/${STM32_CUBE_TAG}:refs/tags/${STM32_CUBE_TAG}"
    tag_object="$(git -C "${temporary}" rev-parse "refs/tags/${STM32_CUBE_TAG}")"
    peeled="$(git -C "${temporary}" rev-parse "refs/tags/${STM32_CUBE_TAG}^{commit}")"
    [[ "${tag_object}" == "${STM32_CUBE_TAG_OBJECT}" ]] ||
        die "STM32CubeF4 tag object is ${tag_object}, expected ${STM32_CUBE_TAG_OBJECT}"
    [[ "${peeled}" == "${STM32_CUBE_COMMIT}" ]] ||
        die "STM32CubeF4 tag peels to ${peeled}, expected ${STM32_CUBE_COMMIT}"
    git -C "${temporary}" -c advice.detachedHead=false checkout --quiet --detach \
        "${STM32_CUBE_COMMIT}"
    actual="$(git -C "${temporary}" rev-parse HEAD)"
    [[ "${actual}" == "${STM32_CUBE_COMMIT}" ]] ||
        die "STM32CubeF4 resolved to ${actual}, expected ${STM32_CUBE_COMMIT}"
    mv -- "${temporary}" "${destination}"
}

validate_installation() {
    local compiler="${toolchain_dir}/bin/arm-none-eabi-gcc"
    local gcc_version

    [[ -x "${compiler}" ]] || die "compiler installation is incomplete"
    validate_toolchain_marker
    gcc_version="$(${compiler} -dumpfullversion)"
    [[ "${gcc_version}" == 14.3.1 ]] ||
        die "unexpected GCC version ${gcc_version}; expected 14.3.1"

    validate_git_dependency "${deps_dir}/stm32cube_f4_core" "${STM32_CUBE_COMMIT}" \
        "STM32CubeF4 CMSIS Core"
    [[ "$(git -C "${deps_dir}/stm32cube_f4_core" config --get remote.origin.partialclonefilter)" == \
        "blob:none" ]] || die "STM32CubeF4 checkout is not blob-filtered"
    grep -Fxq '/Drivers/CMSIS/Core/Include/' \
        "${deps_dir}/stm32cube_f4_core/.git/info/sparse-checkout" ||
        die "STM32CubeF4 checkout does not contain the locked sparse Core path"
    validate_git_dependency "${deps_dir}/cmsis_device_f4" "${STM32_DEVICE_COMMIT}" \
        "CMSIS device"
    validate_git_dependency "${deps_dir}/stm32f4xx_hal_driver" "${STM32_HAL_COMMIT}" \
        "STM32 HAL"
    [[ -f "${deps_dir}/stm32cube_f4_core/Drivers/CMSIS/Core/Include/core_cm4.h" ]] ||
        die "CMSIS Core header is missing"
    [[ -f "${deps_dir}/stm32cube_f4_core/Drivers/CMSIS/Core/Include/cmsis_gcc.h" ]] ||
        die "CMSIS GCC header is missing"
    [[ -f "${deps_dir}/stm32cube_f4_core/Drivers/CMSIS/LICENSE.txt" ]] ||
        die "CMSIS Core license is missing"
    [[ -f "${deps_dir}/cmsis_device_f4/Include/stm32f401xe.h" ]] ||
        die "STM32F401 device header is missing"
    [[ -f "${deps_dir}/cmsis_device_f4/Source/Templates/gcc/startup_stm32f401xe.s" ]] ||
        die "STM32F401 startup assembly is missing"
    [[ -f "${deps_dir}/cmsis_device_f4/Source/Templates/system_stm32f4xx.c" ]] ||
        die "STM32F4 system source is missing"
    [[ -f "${deps_dir}/cmsis_device_f4/LICENSE.md" ]] ||
        die "CMSIS device license is missing"
    [[ -f "${deps_dir}/stm32f4xx_hal_driver/Inc/stm32f4xx_hal.h" ]] ||
        die "STM32 HAL header is missing"
    [[ -f "${deps_dir}/stm32f4xx_hal_driver/Src/stm32f4xx_hal.c" ]] ||
        die "STM32 HAL source is missing"
    [[ -f "${deps_dir}/stm32f4xx_hal_driver/Src/stm32f4xx_hal_uart.c" ]] ||
        die "STM32 UART HAL source is missing"
    [[ -f "${deps_dir}/stm32f4xx_hal_driver/LICENSE.md" ]] ||
        die "STM32 HAL license is missing"

    local consumed
    for consumed in \
        Inc/stm32f4xx_hal.h \
        Inc/stm32f4xx_hal_cortex.h \
        Inc/stm32f4xx_hal_dma.h \
        Inc/stm32f4xx_hal_flash.h \
        Inc/stm32f4xx_hal_gpio.h \
        Inc/stm32f4xx_hal_pwr.h \
        Inc/stm32f4xx_hal_rcc.h \
        Inc/stm32f4xx_hal_uart.h \
        Src/stm32f4xx_hal.c \
        Src/stm32f4xx_hal_cortex.c \
        Src/stm32f4xx_hal_flash.c \
        Src/stm32f4xx_hal_gpio.c \
        Src/stm32f4xx_hal_rcc.c \
        Src/stm32f4xx_hal_uart.c; do
        [[ -f "${deps_dir}/stm32f4xx_hal_driver/${consumed}" ]] ||
            die "build-consumed STM32 HAL file is missing: ${consumed}"
    done

    note "validated Arm GCC ${gcc_version} and all locked STM32 dependencies"
}

check_host() {
    [[ "$(uname -s)" == Linux ]] || die "PR 1 setup currently supports Linux only"
    [[ "$(uname -m)" == x86_64 ]] || die "PR 1 setup currently supports Linux x86_64 only"

    require_command awk
    require_command curl
    require_command file
    require_command grep
    require_command git
    require_command make
    require_command od
    require_command sha256sum
    require_command stat
    require_command tar
    require_command xz

    local git_version
    local curl_version
    git_version="$(git version | awk '{print $3}')"
    curl_version="$(curl --version | awk 'NR == 1 {print $2}')"
    version_at_least "${git_version}" 2 25 ||
        die "Git ${git_version} is too old; Git 2.25 or newer is required"
    version_at_least "${curl_version}" 7 61 ||
        die "curl ${curl_version} is too old; curl 7.61 or newer is required"
}

main() {
    local mode="${1:-install}"
    [[ $# -le 1 ]] || die "usage: setup.sh [--check]"
    [[ "${mode}" == install || "${mode}" == --check ]] ||
        die "usage: setup.sh [--check]"

    check_host

    if [[ "${mode}" == --check ]]; then
        [[ ! -e "${setup_lock_dir}" ]] ||
            die "dependency setup is currently locked; retry after it completes"
        validate_installation
        note "offline dependency integrity check passed"
        return
    fi

    mkdir -p -- "${downloads_dir}" "${deps_dir}"
    if ! mkdir -- "${setup_lock_dir}" 2>/dev/null; then
        local lock_pid=""
        [[ -f "${setup_lock_dir}/pid" ]] ||
            die "setup lock has no PID; fail-safe retry required: ${setup_lock_dir}"
        lock_pid="$(<"${setup_lock_dir}/pid")"
        [[ "${lock_pid}" =~ ^[0-9]+$ ]] ||
            die "setup lock has an invalid PID; inspect without deleting user data: ${setup_lock_dir}"
        if kill -0 "${lock_pid}" 2>/dev/null; then
            die "another setup is running as PID ${lock_pid}"
        fi
        mv -- "${setup_lock_dir}" "${setup_lock_dir}.stale-${lock_pid}-$$"
        mkdir -- "${setup_lock_dir}"
        note "replaced a stale setup lock"
    fi
    setup_lock_owned=true
    printf '%s\n' "$$" > "${setup_lock_dir}/pid"
    install_toolchain
    install_cube_cmsis_core
    install_git_dependency cmsis_device_f4 "${STM32_DEVICE_URL}" "${STM32_DEVICE_COMMIT}"
    install_git_dependency stm32f4xx_hal_driver "${STM32_HAL_URL}" "${STM32_HAL_COMMIT}"
    validate_installation
}

main "$@"
