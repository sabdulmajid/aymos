#!/usr/bin/env bash

set -euo pipefail
export LC_ALL=C

readonly script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly repo_root="$(cd -- "${script_dir}/../.." && pwd)"
readonly renode_dir="${repo_root}/.tools/renode-1.16.1-dotnet-x86_64"
readonly python_venv="${repo_root}/.tools/python-venv-renode-1.16.1"
readonly robot_suite="${AYMOS_ROBOT_SUITE:-tests/renode/boot.robot}"

case "${robot_suite}" in
    tests/renode/boot.robot|tests/renode/lifecycle.robot|tests/renode/edf.robot|tests/renode/allocator.robot) ;;
    *)
        printf 'renode-test: unsupported Robot suite: %s\n' \
            "${robot_suite}" >&2
        exit 2
        ;;
esac

if [[ "${AYMOS_NETWORK_ISOLATED:-0}" == 1 ]]; then
    [[ -x "${AYMOS_IP_COMMAND:-}" ]] || {
        printf '%s\n' 'renode-test: network isolation requires an explicit ip command' >&2
        exit 2
    }
    "${AYMOS_IP_COMMAND}" link set lo up
fi

mkdir -p -- "${AYMOS_ROBOT_RESULTS}" "${AYMOS_TEST_HOME}" \
    "${AYMOS_TEST_XDG_CONFIG_HOME}"

cd -- "${repo_root}"
exec env -u PYTHONHOME -u PYTHONPATH \
    PATH="${python_venv}/bin:${PATH}" \
    PYTHONDONTWRITEBYTECODE=1 \
    PYTHONNOUSERSITE=1 \
    RENODE_CI_MODE=YES \
    HOME="${AYMOS_TEST_HOME}" \
    XDG_CONFIG_HOME="${AYMOS_TEST_XDG_CONFIG_HOME}" \
    "${renode_dir}/renode-test" \
    --stop-on-error \
    --save-logs always \
    --keep-renode-output \
    --test-timeout 10s \
    --cleanup-timeout 3 \
    --kill-timeout 2 \
    -r "${AYMOS_ROBOT_RESULTS}" \
    "${robot_suite}"
