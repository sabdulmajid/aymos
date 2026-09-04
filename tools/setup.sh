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
readonly renode_dir="${tools_dir}/${RENODE_DIRECTORY}"
readonly python_dir="${tools_dir}/${PYTHON_DIRECTORY}"
readonly python_venv_dir="${tools_dir}/${PYTHON_VENV_DIRECTORY}"
readonly wheels_dir="${downloads_dir}/python-wheels"
readonly python_requirements="${script_dir}/setup/renode-requirements.lock"
readonly tree_manifest_name=".aymos-tree-manifest"
readonly legacy_tree_file_list_name=".aymos-file-list-sha256"
readonly setup_lock_dir="${tools_dir}/setup.lock"
readonly -a locked_python_env=(
    env -u PYTHONHOME -u PYTHONPATH
    PYTHONDONTWRITEBYTECODE=1 PYTHONNOUSERSITE=1
)
temporary_paths=()
setup_lock_owned=false
python_venv_owned=false

die() {
    printf 'setup: error: %s\n' "$*" >&2
    exit 1
}

note() {
    printf 'setup: %s\n' "$*"
}

renode_version_output() {
    timeout --signal=TERM --kill-after=2s 10s "$1" --version
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

    if [[ "${python_venv_owned}" == true && -d "${python_venv_dir}" ]]; then
        rm -rf -- "${python_venv_dir}"
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

prepare_locked_download() {
    local label="$1"
    local url="$2"
    local expected="$3"
    local expected_size="$4"
    local archive_path="$5"
    local partial_path="${archive_path}.part"
    local size
    local digest

    if archive_matches "${archive_path}" "${expected}" "${expected_size}"; then
        return
    elif [[ -f "${archive_path}" ]]; then
        quarantine_file "${archive_path}" archive
    fi

    if [[ -f "${partial_path}" ]]; then
        size="$(stat -c '%s' "${partial_path}")"
        if ((size == expected_size)); then
            if archive_matches "${partial_path}" "${expected}" "${expected_size}"; then
                mv -- "${partial_path}" "${archive_path}"
                return
            fi
            quarantine_file "${partial_path}" checksum
        elif ((size > expected_size)); then
            quarantine_file "${partial_path}" oversized
        fi
    fi

    note "downloading ${label}"
    curl --fail --location --retry 3 --silent --show-error --continue-at - \
        --output "${partial_path}" "${url}"
    if ! archive_matches "${partial_path}" "${expected}" "${expected_size}"; then
        size="$(stat -c '%s' "${partial_path}")"
        digest="$(sha256_file "${partial_path}")"
        quarantine_file "${partial_path}" downloaded
        die "download mismatch for ${label}: expected ${expected_size}/${expected}, got ${size}/${digest}"
    fi
    mv -- "${partial_path}" "${archive_path}"
}

emit_tree_manifest() {
    local directory="$1"
    local special_node_present

    (
        cd -- "${directory}"
        special_node_present="$(
            find . ! -type d ! -type f ! -type l -printf x -quit
        )" || return 1
        if [[ -n "${special_node_present}" ]]; then
            printf 'setup: unsupported special filesystem node in dependency: %s\n' \
                "${directory}" >&2
            return 1
        fi

        printf 'AYMOS_TREE_V3\0DIRECTORIES\0'
        find . -type d -print0 | sort -z || return 1

        printf 'FILES\0'
        # Bytecode is interpreter-generated mutable cache state. Because the
        # -type f filter comes first, only regular *.pyc files directly below
        # __pycache__ are ignored; matching symlinks remain in the next section.
        # Hash regular files in ARG_MAX-sized batches rather than forking once
        # per file. sha256sum -z keeps arbitrary path bytes unambiguous.
        find . -type f ! -path "./${tree_manifest_name}" \
            ! \( -path '*/__pycache__/*.pyc' \
                ! -path '*/__pycache__/*/*.pyc' \) -print0 | sort -z | \
            xargs -0 -r sha256sum -z -- || return 1

        printf 'SYMLINKS\0'
        # NUL cannot occur in a path or symlink target. The record prefix and
        # separator make the path/target association explicit; paths are also
        # present directly in every sorted record.
        find . -type l -printf 'L\034%p\034%l\0' | sort -z
    )
}

write_tree_manifest() {
    local directory="$1"
    local manifest="${directory}/${tree_manifest_name}"

    # Remove the superseded regular-file-only marker when migrating a local
    # development install. Clean installs never contain it.
    rm -f -- "${directory}/${legacy_tree_file_list_name}"
    emit_tree_manifest "${directory}" > "${manifest}"
}

validate_tree_manifest() {
    local directory="$1"
    local label="$2"
    local manifest="${directory}/${tree_manifest_name}"

    [[ -f "${manifest}" && ! -L "${manifest}" ]] ||
        die "${label} install manifest is missing or is not a regular file"
    emit_tree_manifest "${directory}" | cmp --silent "${manifest}" - ||
        die "${label} filesystem tree was modified after setup"
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

install_renode() {
    local archive_path="${downloads_dir}/${RENODE_ARCHIVE}"
    local extract_parent
    local version_output

    prepare_locked_download "Renode ${RENODE_VERSION}" "${RENODE_URL}" \
        "${RENODE_SHA256}" "${RENODE_SIZE}" "${archive_path}"

    if [[ -x "${renode_dir}/renode" ]]; then
        validate_tree_manifest "${renode_dir}" "Renode"
        version_output="$(renode_version_output "${renode_dir}/renode")" ||
            die "installed Renode version command failed or timed out"
        [[ "${version_output}" == *"Renode v${RENODE_VERSION}."* ]] ||
            die "unexpected installed Renode version"
        note "Renode ${RENODE_VERSION} already installed"
        return
    fi

    [[ ! -e "${renode_dir}" ]] ||
        die "incomplete Renode directory exists: ${renode_dir}"
    extract_parent="${tools_dir}/.extract-${RENODE_DIRECTORY}-$$"
    temporary_paths+=("${extract_parent}")
    mkdir -p -- "${extract_parent}"
    note "extracting Renode ${RENODE_VERSION}"
    tar -xzf "${archive_path}" -C "${extract_parent}"
    [[ -x "${extract_parent}/${RENODE_ARCHIVE_DIRECTORY}/renode" ]] ||
        die "Renode archive did not contain the expected executable"
    [[ -x "${extract_parent}/${RENODE_ARCHIVE_DIRECTORY}/renode-test" ]] ||
        die "Renode archive did not contain renode-test"
    [[ -f "${extract_parent}/${RENODE_ARCHIVE_DIRECTORY}/licenses/renode-license" ]] ||
        die "Renode archive did not contain its license"
    mv -- "${extract_parent}/${RENODE_ARCHIVE_DIRECTORY}" "${renode_dir}"
    rmdir -- "${extract_parent}"
    {
        printf 'archive=%s\n' "${RENODE_ARCHIVE}"
        printf 'archive_sha256=%s\n' "${RENODE_SHA256}"
        printf 'archive_size=%s\n' "${RENODE_SIZE}"
    } > "${renode_dir}/.aymos-install-source"
    write_tree_manifest "${renode_dir}"
}

install_python() {
    local archive_path="${downloads_dir}/${PYTHON_ARCHIVE}"
    local extract_parent
    local actual_version

    prepare_locked_download "CPython ${PYTHON_VERSION}" "${PYTHON_URL}" \
        "${PYTHON_SHA256}" "${PYTHON_SIZE}" "${archive_path}"

    if [[ -x "${python_dir}/bin/python3" ]]; then
        validate_tree_manifest "${python_dir}" "CPython"
        actual_version="$("${locked_python_env[@]}" \
            "${python_dir}/bin/python3" \
            -c 'import platform; print(platform.python_version())')"
        [[ "${actual_version}" == "${PYTHON_VERSION}" ]] ||
            die "installed Python is ${actual_version}, expected ${PYTHON_VERSION}"
        note "CPython ${PYTHON_VERSION} already installed"
        return
    fi

    [[ ! -e "${python_dir}" ]] ||
        die "incomplete Python directory exists: ${python_dir}"
    extract_parent="${tools_dir}/.extract-${PYTHON_DIRECTORY}-$$"
    temporary_paths+=("${extract_parent}")
    mkdir -p -- "${extract_parent}"
    note "extracting CPython ${PYTHON_VERSION}"
    tar -xzf "${archive_path}" -C "${extract_parent}"
    [[ -x "${extract_parent}/python/bin/python3" ]] ||
        die "Python archive did not contain the expected interpreter"
    [[ -f "${extract_parent}/python/lib/python3.12/LICENSE.txt" ]] ||
        die "Python archive did not contain its license"
    mv -- "${extract_parent}/python" "${python_dir}"
    rmdir -- "${extract_parent}"
    {
        printf 'archive=%s\n' "${PYTHON_ARCHIVE}"
        printf 'archive_sha256=%s\n' "${PYTHON_SHA256}"
        printf 'archive_size=%s\n' "${PYTHON_SIZE}"
    } > "${python_dir}/.aymos-install-source"
    write_tree_manifest "${python_dir}"
}

install_python_wheels() {
    local count="${#RENODE_PYTHON_WHEEL_NAMES[@]}"
    local index

    ((count == ${#RENODE_PYTHON_WHEEL_URLS[@]} &&
       count == ${#RENODE_PYTHON_WHEEL_SHA256S[@]} &&
       count == ${#RENODE_PYTHON_WHEEL_SIZES[@]})) ||
        die "Python wheel lock arrays have different lengths"
    mkdir -p -- "${wheels_dir}"
    for ((index = 0; index < count; index++)); do
        prepare_locked_download \
            "Python wheel ${RENODE_PYTHON_WHEEL_NAMES[index]}" \
            "${RENODE_PYTHON_WHEEL_URLS[index]}" \
            "${RENODE_PYTHON_WHEEL_SHA256S[index]}" \
            "${RENODE_PYTHON_WHEEL_SIZES[index]}" \
            "${wheels_dir}/${RENODE_PYTHON_WHEEL_NAMES[index]}"
    done
}

validate_python_environment() {
    local actual_version
    local resolved_python

    [[ -x "${python_venv_dir}/bin/python3" ]] ||
        die "project Python virtual environment is incomplete"
    validate_tree_manifest "${python_venv_dir}" "Python virtual environment"
    resolved_python="$(readlink -f "${python_venv_dir}/bin/python3")"
    [[ "${resolved_python}" == "$(readlink -f "${python_dir}/bin/python3")" ]] ||
        die "virtual environment does not use the locked Python interpreter"
    actual_version="$("${locked_python_env[@]}" \
        "${python_venv_dir}/bin/python3" -c \
        'import platform; print(platform.python_version())')"
    [[ "${actual_version}" == "${PYTHON_VERSION}" ]] ||
        die "virtual environment uses Python ${actual_version}, expected ${PYTHON_VERSION}"
    "${locked_python_env[@]}" "${python_venv_dir}/bin/python3" -c \
        'from importlib.metadata import version
import psutil, RetryFailed, robot, telnetlib3, yaml
assert robot.__version__ == "6.1"
assert psutil.__version__ == "5.9.8"
assert yaml.__version__ == "6.0.3"
assert telnetlib3.__version__ == "2.0.8"
assert version("robotframework-retryfailed") == "0.2.0"' ||
        die "locked Renode Python packages failed their import/version check"
}

install_python_environment() {
    if [[ -d "${python_venv_dir}" ]]; then
        validate_python_environment
        note "Renode Python environment already installed"
        return
    fi

    [[ ! -e "${python_venv_dir}" ]] ||
        die "non-directory Python environment path exists: ${python_venv_dir}"
    note "creating the Renode Python environment"
    python_venv_owned=true
    "${locked_python_env[@]}" "${python_dir}/bin/python3" -m venv \
        "${python_venv_dir}"
    "${locked_python_env[@]}" PIP_DISABLE_PIP_VERSION_CHECK=1 \
        "${python_venv_dir}/bin/python3" -m pip install \
        --no-index --only-binary=:all: --require-hashes \
        --find-links "${wheels_dir}" --requirement "${python_requirements}"
    {
        printf 'python_archive_sha256=%s\n' "${PYTHON_SHA256}"
        printf 'requirements_sha256=%s\n' "$(sha256_file "${python_requirements}")"
    } > "${python_venv_dir}/.aymos-install-source"
    write_tree_manifest "${python_venv_dir}"
    validate_python_environment
    python_venv_owned=false
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
    local actual_version
    local count
    local index
    local renode_version

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

    [[ -x "${renode_dir}/renode" && -x "${renode_dir}/renode-test" ]] ||
        die "Renode installation is incomplete"
    validate_tree_manifest "${renode_dir}" "Renode"
    grep -Fxq "archive=${RENODE_ARCHIVE}" "${renode_dir}/.aymos-install-source" ||
        die "Renode source manifest archive mismatch"
    grep -Fxq "archive_sha256=${RENODE_SHA256}" \
        "${renode_dir}/.aymos-install-source" ||
        die "Renode source manifest checksum mismatch"
    renode_version="$(renode_version_output "${renode_dir}/renode")" ||
        die "installed Renode version command failed or timed out"
    [[ "${renode_version}" == *"Renode v${RENODE_VERSION}."* ]] ||
        die "unexpected Renode version output"

    [[ -x "${python_dir}/bin/python3" ]] ||
        die "locked Python installation is incomplete"
    validate_tree_manifest "${python_dir}" "CPython"
    grep -Fxq "archive=${PYTHON_ARCHIVE}" "${python_dir}/.aymos-install-source" ||
        die "Python source manifest archive mismatch"
    grep -Fxq "archive_sha256=${PYTHON_SHA256}" \
        "${python_dir}/.aymos-install-source" ||
        die "Python source manifest checksum mismatch"
    actual_version="$("${locked_python_env[@]}" \
        "${python_dir}/bin/python3" \
        -c 'import platform; print(platform.python_version())')"
    [[ "${actual_version}" == "${PYTHON_VERSION}" ]] ||
        die "installed Python is ${actual_version}, expected ${PYTHON_VERSION}"

    count="${#RENODE_PYTHON_WHEEL_NAMES[@]}"
    for ((index = 0; index < count; index++)); do
        archive_matches "${wheels_dir}/${RENODE_PYTHON_WHEEL_NAMES[index]}" \
            "${RENODE_PYTHON_WHEEL_SHA256S[index]}" \
            "${RENODE_PYTHON_WHEEL_SIZES[index]}" ||
            die "locked Python wheel is missing or modified: ${RENODE_PYTHON_WHEEL_NAMES[index]}"
    done
    validate_python_environment

    note "validated Arm GCC ${gcc_version}, STM32 dependencies, Renode ${RENODE_VERSION}, and Python ${PYTHON_VERSION}"
}

check_host() {
    [[ "$(uname -s)" == Linux ]] || die "setup currently supports Linux only"
    [[ "$(uname -m)" == x86_64 ]] || die "setup currently supports Linux x86_64 only"

    require_command awk
    require_command curl
    require_command cmp
    require_command env
    require_command file
    require_command find
    require_command grep
    require_command git
    require_command make
    require_command od
    require_command readlink
    require_command sha256sum
    require_command sort
    require_command stat
    require_command tar
    require_command timeout
    require_command xargs
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
    install_renode
    install_python
    install_python_wheels
    install_python_environment
    validate_installation
}

main "$@"
