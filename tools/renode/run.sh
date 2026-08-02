#!/usr/bin/env bash

set -euo pipefail
export LC_ALL=C

readonly script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly repo_root="$(cd -- "${script_dir}/../.." && pwd)"
readonly renode="${repo_root}/.tools/renode-1.16.1-dotnet-x86_64/renode"
readonly python="${repo_root}/.tools/python-venv-renode-1.16.1/bin/python3"
readonly app="${AYMOS_APP:-boot}"
case "${app}" in
    boot)
        uart_validator="${script_dir}/verify_uart.py"
        default_virtual_duration="0.2"
        ;;
    lifecycle)
        uart_validator="${script_dir}/verify_lifecycle_uart.py"
        default_virtual_duration="0.5"
        ;;
    edf)
        uart_validator="${script_dir}/verify_edf_uart.py"
        default_virtual_duration="0.5"
        ;;
    allocator)
        uart_validator="${script_dir}/verify_allocator_uart.py"
        default_virtual_duration="0.5"
        ;;
    *)
        printf 'renode-run: unsupported AYMOS_APP: %s\n' "${app}" >&2
        exit 2
        ;;
esac
readonly uart_validator default_virtual_duration
readonly elf="${repo_root}/build/nucleo_f401re/${app}/aymos.elf"
readonly boot_script="${repo_root}/platform/renode/boot.resc"
readonly platform="${repo_root}/platform/renode/nucleo_f401re.repl"
readonly host_timeout="${AYMOS_RENODE_HOST_TIMEOUT:-30}"
readonly virtual_duration="${AYMOS_RENODE_VIRTUAL_DURATION:-${default_virtual_duration}}"
readonly run_id="$(date -u +%Y%m%dT%H%M%SZ)-$$"
readonly output_dir="${AYMOS_RENODE_OUTPUT_DIR:-${repo_root}/build/renode/run/${run_id}}"
readonly uart_capture="${output_dir}/uart.bin"
readonly emulator_log="${output_dir}/emulator.log"
readonly monitor_command="\$bin=@${elf}; \$platform=@${platform}; include @${boot_script}; sysbus.usart2 CreateFileBackend @${uart_capture}; emulation RunFor \"${virtual_duration}\"; quit"

validate_inputs() {
    [[ "${host_timeout}" =~ ^[0-9]+$ ]] && ((host_timeout >= 5 && host_timeout <= 300)) || {
        printf 'renode-run: invalid AYMOS_RENODE_HOST_TIMEOUT: %s\n' \
            "${host_timeout}" >&2
        exit 2
    }
    [[ "${virtual_duration}" =~ ^[0-9]+([.][0-9]+)?$ ]] || {
        printf 'renode-run: invalid AYMOS_RENODE_VIRTUAL_DURATION: %s\n' \
            "${virtual_duration}" >&2
        exit 2
    }
    for required in "${renode}" "${python}" "${elf}" "${boot_script}" \
        "${platform}" "${uart_validator}"; do
        [[ -f "${required}" ]] || {
            printf 'renode-run: missing required input: %s\n' "${required}" >&2
            exit 2
        }
    done
    [[ "${repo_root}" != *[$'\n\r\t ']* ]] || {
        printf '%s\n' \
            'renode-run: the repository path may not contain whitespace' >&2
        exit 2
    }
}

show_diagnostics() {
    local status=$?

    trap - EXIT
    if ((status != 0)); then
        printf 'renode-run: failed with status %d; artifacts: %s\n' \
            "${status}" "${output_dir}" >&2
        if [[ -f "${emulator_log}" ]]; then
            printf '%s\n' '--- emulator.log (last 200 lines) ---' >&2
            tail -n 200 "${emulator_log}" >&2
        fi
        if [[ -f "${output_dir}/uart-validation.log" ]]; then
            printf '%s\n' '--- uart-validation.log ---' >&2
            cat "${output_dir}/uart-validation.log" >&2
        fi
        if [[ -f "${uart_capture}" ]]; then
            printf '%s\n' '--- uart.bin (hex/ASCII) ---' >&2
            od -An -tx1c "${uart_capture}" >&2
        fi
    fi
    exit "${status}"
}

validate_inputs
mkdir -p -- "$(dirname -- "${output_dir}")"
mkdir -- "${output_dir}"
mkdir -- "${output_dir}/home" "${output_dir}/xdg"
trap show_diagnostics EXIT

renode_build="$(
    timeout --signal=TERM --kill-after=2s 10s \
        env HOME="${output_dir}/home" XDG_CONFIG_HOME="${output_dir}/xdg" \
        "${renode}" --version
)" || {
    printf '%s\n' 'renode-run: Renode version command failed or timed out' >&2
    exit 1
}
renode_build="$(printf '%s' "${renode_build}" | tr '\n' ' ' | sed 's/[[:space:]]*$//')"

{
    printf 'schema=1\n'
    printf 'board=nucleo_f401re\n'
    printf 'app=%s\n' "${app}"
    printf 'git_commit=%s\n' "$(git -C "${repo_root}" rev-parse HEAD)"
    printf 'repository_clean=%s\n' \
        "$(test -z "$(git -C "${repo_root}" status --porcelain)" && printf true || printf false)"
    printf 'firmware=%s\n' "${elf}"
    printf 'firmware_sha256=%s\n' "$(sha256sum "${elf}" | awk '{print $1}')"
    printf 'platform=%s\n' "${platform}"
    printf 'platform_sha256=%s\n' "$(sha256sum "${platform}" | awk '{print $1}')"
    printf 'renode_version=1.16.1\n'
    printf 'renode_build=%s\n' "${renode_build}"
    printf 'command_file=command.txt\n'
    printf 'emulator_arguments=--console --disable-gui --plain -e <monitor_command>\n'
    printf 'host_timeout_seconds=%s\n' "${host_timeout}"
    printf 'virtual_duration_seconds=%s\n' "${virtual_duration}"
    printf 'timing_claims=none_emulator_functional_test_only\n'
} > "${output_dir}/metadata.txt"

printf '%q ' \
    timeout --signal=TERM --kill-after=5s "${host_timeout}s" \
    env "HOME=${output_dir}/home" "XDG_CONFIG_HOME=${output_dir}/xdg" \
    "${renode}" --console --disable-gui --plain \
    -e "${monitor_command}" \
    > "${output_dir}/command.txt"
printf '> %q 2>&1\n' "${emulator_log}" >> "${output_dir}/command.txt"

timeout --signal=TERM --kill-after=5s "${host_timeout}s" \
    env HOME="${output_dir}/home" XDG_CONFIG_HOME="${output_dir}/xdg" \
    "${renode}" --console --disable-gui --plain \
    -e "${monitor_command}" \
    > "${emulator_log}" 2>&1

if grep -Eq "There was an error executing command|Error E[0-9]+:" \
    "${emulator_log}"; then
    printf '%s\n' 'renode-run: Renode reported a monitor command error' >&2
    exit 1
fi

set +e
env -u PYTHONHOME -u PYTHONPATH \
    PYTHONDONTWRITEBYTECODE=1 PYTHONNOUSERSITE=1 \
    "${python}" "${uart_validator}" "${uart_capture}" \
    > "${output_dir}/uart.txt" 2> "${output_dir}/uart-validation.log"
status=$?
set -e
((status == 0)) || exit "${status}"
cat "${output_dir}/uart.txt"

trap - EXIT
printf 'renode-run: artifacts: %s\n' "${output_dir}"
