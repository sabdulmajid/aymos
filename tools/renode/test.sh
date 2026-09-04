#!/usr/bin/env bash

set -euo pipefail
export LC_ALL=C

readonly script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly repo_root="$(cd -- "${script_dir}/../.." && pwd)"
readonly python="${repo_root}/.tools/python-venv-renode-1.16.1/bin/python3"
readonly renode="${repo_root}/.tools/renode-1.16.1-dotnet-x86_64/renode"
readonly app="${AYMOS_APP:-boot}"
case "${app}" in
    boot)
        robot_suite="tests/renode/boot.robot"
        uart_validator="${script_dir}/verify_uart.py"
        ;;
    lifecycle)
        robot_suite="tests/renode/lifecycle.robot"
        uart_validator="${script_dir}/verify_lifecycle_uart.py"
        ;;
    edf)
        robot_suite="tests/renode/edf.robot"
        uart_validator="${script_dir}/verify_edf_uart.py"
        ;;
    allocator)
        robot_suite="tests/renode/allocator.robot"
        uart_validator="${script_dir}/verify_allocator_uart.py"
        ;;
    *)
        printf 'renode-test: unsupported AYMOS_APP: %s\n' "${app}" >&2
        exit 2
        ;;
esac
readonly robot_suite uart_validator
readonly elf="${repo_root}/build/nucleo_f401re/${app}/aymos.elf"
readonly boot_script="${repo_root}/platform/renode/boot.resc"
readonly platform="${repo_root}/platform/renode/nucleo_f401re.repl"
readonly repeat="${RENODE_REPEAT:-1}"
readonly host_timeout="${AYMOS_RENODE_HOST_TIMEOUT:-30}"
readonly test_id="$(date -u +%Y%m%dT%H%M%SZ)-$$"
readonly output_root="${AYMOS_RENODE_TEST_OUTPUT_DIR:-${repo_root}/build/renode/test/${test_id}}"
offline=false

usage() {
    printf '%s\n' 'usage: tools/renode/test.sh [--offline]' >&2
    exit 2
}

if (($# > 1)); then
    usage
elif (($# == 1)); then
    [[ "$1" == --offline ]] || usage
    offline=true
fi

[[ "${repeat}" =~ ^[0-9]+$ ]] && ((repeat >= 1 && repeat <= 100)) || {
    printf 'renode-test: invalid RENODE_REPEAT: %s\n' "${repeat}" >&2
    exit 2
}
[[ "${host_timeout}" =~ ^[0-9]+$ ]] && ((host_timeout >= 5 && host_timeout <= 300)) || {
    printf 'renode-test: invalid AYMOS_RENODE_HOST_TIMEOUT: %s\n' \
        "${host_timeout}" >&2
    exit 2
}
[[ "${repo_root}" != *[$'\n\r\t ']* ]] || {
    printf '%s\n' 'renode-test: the repository path may not contain whitespace' >&2
    exit 2
}
for required in "${python}" "${renode}" "${elf}" "${boot_script}" \
    "${platform}" "${uart_validator}" "${repo_root}/${robot_suite}"; do
    [[ -f "${required}" ]] || {
        printf 'renode-test: missing required input: %s\n' "${required}" >&2
        exit 2
    }
done

"${script_dir}/check_platform.sh"

unshare_command=""
ip_command=""
if [[ "${offline}" == true ]]; then
    unshare_command="$(command -v unshare || true)"
    ip_command="$(command -v ip || true)"
    [[ -x "${unshare_command}" && -x "${ip_command}" ]] || {
        printf '%s\n' \
            'renode-test: --offline requires host unshare and ip commands' >&2
        exit 2
    }
fi

mkdir -p -- "$(dirname -- "${output_root}")"
mkdir -- "${output_root}"

overall_status=0
for ((attempt = 1; attempt <= repeat; attempt++)); do
    attempt_dir="${output_root}/attempt-${attempt}"
    mkdir -- "${attempt_dir}"
    mkdir -- "${attempt_dir}/home" "${attempt_dir}/xdg"
    uart_capture="${attempt_dir}/uart.bin"
    robot_results="${attempt_dir}/robot"
    test_log="${attempt_dir}/test.log"
    renode_build="$(
        timeout --signal=TERM --kill-after=2s 10s \
            env HOME="${attempt_dir}/home" \
            XDG_CONFIG_HOME="${attempt_dir}/xdg" \
            "${renode}" --version
    )" || {
        printf 'renode-test: Renode version command failed or timed out; artifacts: %s\n' \
            "${attempt_dir}" >&2
        exit 1
    }
    renode_build="$(printf '%s' "${renode_build}" | tr '\n' ' ' | sed 's/[[:space:]]*$//')"

    {
        printf 'schema=1\n'
        printf 'board=nucleo_f401re\n'
        printf 'app=%s\n' "${app}"
        printf 'attempt=%s\n' "${attempt}"
        printf 'repeat=%s\n' "${repeat}"
        printf 'network_isolated=%s\n' "${offline}"
        printf 'git_commit=%s\n' "$(git -C "${repo_root}" rev-parse HEAD)"
        printf 'repository_clean=%s\n' \
            "$(test -z "$(git -C "${repo_root}" status --porcelain)" && printf true || printf false)"
        printf 'firmware_sha256=%s\n' "$(sha256sum "${elf}" | awk '{print $1}')"
        printf 'platform_sha256=%s\n' "$(sha256sum "${platform}" | awk '{print $1}')"
        printf 'renode_version=1.16.1\n'
        printf 'renode_build=%s\n' "${renode_build}"
        printf 'command_file=command.txt\n'
        printf 'robot_suite=%s\n' "${robot_suite}"
        printf 'host_timeout_seconds=%s\n' "${host_timeout}"
        printf 'timing_claims=none_emulator_functional_test_only\n'
    } > "${attempt_dir}/metadata.txt"

    runner_environment=(
        "AYMOS_FIRMWARE_ELF=${elf}"
        "AYMOS_BOOT_SCRIPT=${boot_script}"
        "AYMOS_PLATFORM=${platform}"
        "AYMOS_UART_CAPTURE=${uart_capture}"
        "AYMOS_ROBOT_RESULTS=${robot_results}"
        "AYMOS_TEST_HOME=${attempt_dir}/home"
        "AYMOS_TEST_XDG_CONFIG_HOME=${attempt_dir}/xdg"
        "AYMOS_ROBOT_SUITE=${robot_suite}"
    )
    runner=(env "${runner_environment[@]}" "${script_dir}/robot_once.sh")
    if [[ "${offline}" == true ]]; then
        runner=(
            "${unshare_command}" --user --map-root-user --net
            env "AYMOS_NETWORK_ISOLATED=1" "AYMOS_IP_COMMAND=${ip_command}"
            "${runner_environment[@]}" "${script_dir}/robot_once.sh"
        )
    fi

    printf '%q ' timeout --signal=TERM --kill-after=5s "${host_timeout}s" \
        "${runner[@]}" > "${attempt_dir}/command.txt"
    printf '> %q 2>&1\n' "${test_log}" >> "${attempt_dir}/command.txt"

    set +e
    timeout --signal=TERM --kill-after=5s "${host_timeout}s" \
        "${runner[@]}" > "${test_log}" 2>&1
    status=$?
    set -e

    renode_stdout="$(find "${robot_results}/logs" -maxdepth 1 \
        -type f -name '*.renode_stdout.log' -print -quit 2>/dev/null || true)"
    if [[ -n "${renode_stdout}" ]]; then
        cp -- "${renode_stdout}" "${attempt_dir}/emulator.log"
    fi

    if ((status == 0)); then
        set +e
        env -u PYTHONHOME -u PYTHONPATH \
            PYTHONDONTWRITEBYTECODE=1 PYTHONNOUSERSITE=1 \
            "${python}" "${uart_validator}" "${uart_capture}" \
            > "${attempt_dir}/uart.txt" 2> "${attempt_dir}/uart-validation.log"
        status=$?
        set -e
    fi

    if ((status != 0)); then
        overall_status="${status}"
        printf 'renode-test: attempt %d failed with status %d; artifacts: %s\n' \
            "${attempt}" "${status}" "${attempt_dir}" >&2
        printf '%s\n' '--- test.log (last 200 lines) ---' >&2
        tail -n 200 "${test_log}" >&2 || true
        if [[ -f "${attempt_dir}/emulator.log" ]]; then
            printf '%s\n' '--- emulator.log (last 200 lines) ---' >&2
            tail -n 200 "${attempt_dir}/emulator.log" >&2 || true
        fi
        if [[ -f "${attempt_dir}/uart-validation.log" ]]; then
            printf '%s\n' '--- uart-validation.log ---' >&2
            cat "${attempt_dir}/uart-validation.log" >&2 || true
        fi
        if [[ -f "${uart_capture}" ]]; then
            printf '%s\n' '--- uart.bin (hex/ASCII) ---' >&2
            od -An -tx1c "${uart_capture}" >&2 || true
        fi
        break
    fi

    printf 'renode-test: attempt %d/%d passed\n' "${attempt}" "${repeat}"
done

printf 'renode-test: artifacts: %s\n' "${output_root}"
exit "${overall_status}"
