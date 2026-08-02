#!/usr/bin/env bash

set -euo pipefail
export LC_ALL=C

readonly script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly repo_root="$(cd -- "${script_dir}/../.." && pwd)"

cd -- "${repo_root}"

readonly runtime_inputs=(
    platform/renode/nucleo_f401re.repl
    platform/renode/boot.resc
    tests/renode/boot.robot
    tests/renode/lifecycle.robot
    tests/renode/edf.robot
    tests/renode/allocator.robot
)

for runtime_input in "${runtime_inputs[@]}"; do
    [[ -f "${runtime_input}" ]] || {
        printf 'renode-platform: missing runtime input: %s\n' "${runtime_input}" >&2
        exit 1
    }
done

if grep -Eni '(https?://|ftp://|@https?:)' "${runtime_inputs[@]}"; then
    printf '%s\n' \
        'renode-platform: network URL found in an emulator runtime input' >&2
    exit 1
fi

printf '%s\n' 'renode-platform: runtime inputs contain no network URLs'
