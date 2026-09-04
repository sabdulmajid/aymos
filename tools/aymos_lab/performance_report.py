"""Validate DSP evidence and publish a standalone comparison report."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import fcntl
import gzip
import hashlib
import html
import io
import json
import os
from pathlib import Path
import re
import secrets
import stat
import sys
import tempfile
from typing import Any
import zlib

from tools.aymos_lab import signal_lab
from tools.aymos_lab import trace as trace_tool


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_EVIDENCE_ROOT = REPOSITORY_ROOT / "build/signal-lab/evidence"
DEFAULT_RUNS_ROOT = REPOSITORY_ROOT / "runs"
EVIDENCE_MARKER = ".aymos-signal-lab-evidence"
REPORT_MARKER = ".aymos-signal-report"
REPORT_MARKER_COMPLETE = "schema=1\nstate=complete\n"
REPORT_MARKER_ACTIVE = "schema=1\nstate=active\n"
RETAIN_REPORT_RUNS = 8
SUMMARY_SCHEMA_VERSION = 1
REPORT_SCHEMA_VERSION = 1
RUN_PATTERN = re.compile(
    r"[0-9]{8}T[0-9]{12}Z-[0-9a-f]{8}-signal"
)
SHA256_PATTERN = re.compile(r"[0-9a-f]{64}")
DIRECTORY_FLAGS = (
    os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
)
FILE_FLAGS = os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC

ARTIFACT_LIMITS = {
    "build-metadata.txt": 16 * 1024,
    "command.txt": 16 * 1024,
    "emulator.log": 256 * 1024,
    "execution-summary.json": 32 * 1024,
    "execution.bin.gz": signal_lab.MAX_EXECUTION_COMPRESSED_BYTES,
    "firmware.elf": 2 * 1024 * 1024,
    "firmware.map": 1024 * 1024,
    "metadata.txt": 32 * 1024,
    "result.json": 16 * 1024,
    "trace.bin": 16 * 1024,
    "trace.json": 256 * 1024,
    "uart-validation.log": 256 * 1024,
    "uart.bin": 64 * 1024,
    "uart.txt": 16 * 1024,
}
ARTIFACT_NAMES = tuple(sorted(ARTIFACT_LIMITS))
SUMMARY_LIMIT = 256 * 1024

TOP_KEYS = {
    "board", "random_seed", "result_equality", "runs", "schema_version",
    "structured_trace_equality", "timing_claims", "trace_schema_version",
}
RUN_KEYS = {
    "artifacts", "build_configuration", "execution", "execution_limits",
    "git_commit", "implementation", "record_count", "renode_version",
    "repository_clean", "result", "toolchain_version",
}
BUILD_KEYS = {"app", "board", "float_abi", "signal_impl"}
LIMIT_KEYS = {
    "compressed_trace_bytes", "execution_entries", "host_timeout_seconds",
    "outer_timeout_seconds", "uncompressed_trace_bytes",
    "virtual_duration_seconds",
}
RESULT_KEYS = {
    "frames", "implementation", "input_crc32", "output_crc32",
    "samples_per_frame", "seed", "status", "taps", "total_outputs",
    "valid_outputs_per_frame", "work_units",
}
EXECUTION_KEYS = {
    "claim", "compressed_bytes", "dynamic_instructions", "entry_count",
    "format", "function_address", "function_call_count",
    "function_instruction_hits", "function_size_bytes", "schema_version",
    "selected_function", "target", "timing_claims", "uncompressed_bytes",
}
TASKS = {
    0: ("idle", "#64748b"),
    1: ("sampler", "#22d3ee"),
    2: ("processor", "#a78bfa"),
    3: ("verifier", "#34d399"),
}


class PerformanceReportError(ValueError):
    """DSP evidence cannot produce a trusted report."""


def _strict_equal(actual: Any, expected: Any, label: str) -> None:
    if type(actual) is not type(expected):
        raise PerformanceReportError(
            f"{label} type mismatch: got {type(actual).__name__}, "
            f"expected {type(expected).__name__}"
        )
    if isinstance(actual, dict):
        if set(actual) != set(expected):
            raise PerformanceReportError(
                f"{label} keys mismatch: got {set(actual)!r}, "
                f"expected {set(expected)!r}"
            )
        for key in actual:
            _strict_equal(actual[key], expected[key], f"{label}.{key}")
        return
    if isinstance(actual, (list, tuple)):
        if len(actual) != len(expected):
            raise PerformanceReportError(
                f"{label} length mismatch: got {len(actual)}, "
                f"expected {len(expected)}"
            )
        for index, (actual_item, expected_item) in enumerate(
                zip(actual, expected)):
            _strict_equal(actual_item, expected_item, f"{label}[{index}]")
        return
    if actual != expected:
        raise PerformanceReportError(
            f"{label} mismatch: got {actual!r}, expected {expected!r}"
        )


def _expect(actual: Any, expected: Any, label: str) -> None:
    _strict_equal(actual, expected, label)


def _exact_keys(value: Any, keys: set[str], label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise PerformanceReportError(f"{label} must be an object")
    _expect(set(value), keys, f"{label} keys")
    return value


def _json_bytes(data: bytes, label: str) -> dict[str, Any]:
    def object_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise PerformanceReportError(
                    f"duplicate JSON key in {label}: {key!r}"
                )
            result[key] = value
        return result

    def reject_constant(value: str) -> Any:
        raise PerformanceReportError(
            f"non-finite JSON number in {label}: {value}"
        )

    try:
        value = json.loads(
            data.decode("utf-8"), object_pairs_hook=object_pairs,
            parse_constant=reject_constant,
        )
    except (UnicodeError, json.JSONDecodeError) as error:
        raise PerformanceReportError(f"invalid {label}: {error}") from error
    if not isinstance(value, dict):
        raise PerformanceReportError(f"{label} must be a JSON object")
    return value


def _metadata_bytes(data: bytes, label: str) -> dict[str, str]:
    try:
        lines = data.decode("utf-8").splitlines()
    except UnicodeError as error:
        raise PerformanceReportError(f"invalid UTF-8 in {label}") from error
    result: dict[str, str] = {}
    for line in lines:
        if "=" not in line:
            raise PerformanceReportError(f"invalid line in {label}: {line!r}")
        key, value = line.split("=", 1)
        if not key or key in result:
            raise PerformanceReportError(f"invalid key in {label}: {key!r}")
        result[key] = value
    return result


def _open_directory(path: Path, label: str) -> int:
    try:
        descriptor = os.open(path, DIRECTORY_FLAGS)
    except OSError as error:
        raise PerformanceReportError(f"cannot open {label}: {error}") from error
    if not stat.S_ISDIR(os.fstat(descriptor).st_mode):
        os.close(descriptor)
        raise PerformanceReportError(f"{label} is not a directory")
    return descriptor


def _read_regular_at(
    directory: int, name: str, limit: int, label: str,
) -> bytes:
    if not name or "/" in name or name in (".", ".."):
        raise PerformanceReportError(f"unsafe file name for {label}")
    try:
        descriptor = os.open(name, FILE_FLAGS, dir_fd=directory)
    except OSError as error:
        raise PerformanceReportError(f"cannot open {label}: {error}") from error
    try:
        before = os.fstat(descriptor)
        if not stat.S_ISREG(before.st_mode):
            raise PerformanceReportError(f"{label} is not a regular file")
        if before.st_size > limit:
            raise PerformanceReportError(
                f"{label} is {before.st_size} bytes; limit is {limit}"
            )
        output = bytearray()
        while len(output) <= limit:
            block = os.read(descriptor, min(65536, limit + 1 - len(output)))
            if not block:
                break
            output.extend(block)
        after = os.fstat(descriptor)
    finally:
        os.close(descriptor)
    data = bytes(output)
    if len(data) > limit:
        raise PerformanceReportError(f"{label} exceeds {limit} bytes")
    before_identity = (
        before.st_dev, before.st_ino, before.st_size,
        before.st_mtime_ns, before.st_ctime_ns,
    )
    after_identity = (
        after.st_dev, after.st_ino, after.st_size,
        after.st_mtime_ns, after.st_ctime_ns,
    )
    if before_identity != after_identity or len(data) != before.st_size:
        raise PerformanceReportError(f"{label} changed while it was read")
    return data


def _read_regular(path: Path, limit: int, label: str) -> bytes:
    directory = _open_directory(path.parent, f"{label} parent")
    try:
        return _read_regular_at(directory, path.name, limit, label)
    finally:
        os.close(directory)


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _validate_artifact_manifest(value: Any, label: str) -> dict[str, Any]:
    manifest = _exact_keys(value, set(ARTIFACT_NAMES), label)
    for name, entry_value in manifest.items():
        entry = _exact_keys(entry_value, {"bytes", "sha256"}, f"{label}.{name}")
        size = entry["bytes"]
        digest = entry["sha256"]
        if not isinstance(size, int) or isinstance(size, bool) or size < 0:
            raise PerformanceReportError(f"{label}.{name}.bytes is invalid")
        if size > ARTIFACT_LIMITS[name]:
            raise PerformanceReportError(f"{label}.{name} exceeds its byte limit")
        if not isinstance(digest, str) or SHA256_PATTERN.fullmatch(digest) is None:
            raise PerformanceReportError(f"{label}.{name}.sha256 is invalid")
    return manifest


def _validate_result(value: Any, implementation: str) -> dict[str, Any]:
    result = _exact_keys(value, RESULT_KEYS, f"{implementation} result")
    expected = {
        "frames": signal_lab.FRAME_COUNT,
        "implementation": implementation,
        "input_crc32": signal_lab.INPUT_CRC,
        "output_crc32": signal_lab.OUTPUT_CRC,
        "samples_per_frame": signal_lab.SAMPLE_COUNT,
        "seed": signal_lab.SEED,
        "status": "PASS",
        "taps": signal_lab.TAP_COUNT,
        "total_outputs": signal_lab.TOTAL_OUTPUT_COUNT,
        "valid_outputs_per_frame": signal_lab.VALID_OUTPUT_COUNT,
        "work_units": signal_lab.WORK_UNITS,
    }
    _expect(result, expected, f"{implementation} fixed result")
    return result


def _validate_execution(value: Any, implementation: str) -> dict[str, Any]:
    execution = _exact_keys(value, EXECUTION_KEYS, f"{implementation} execution")
    expected_common = {
        "claim": "functional executed-instruction evidence; not a timing measurement",
        "format": "ReTrace-v4-PCAndOpcode",
        "function_call_count": signal_lab.FRAME_COUNT,
        "schema_version": 1,
        "target": "thumb cortex-m4",
        "timing_claims": "none",
    }
    for key, expected in expected_common.items():
        _expect(execution[key], expected, f"{implementation} execution {key}")
    for key in (
        "compressed_bytes", "entry_count", "function_instruction_hits",
        "function_size_bytes", "uncompressed_bytes",
    ):
        if (not isinstance(execution[key], int) or isinstance(execution[key], bool)
                or execution[key] <= 0):
            raise PerformanceReportError(f"{implementation} execution {key} is invalid")
    expected_name = (
        "aymos_fir_q15_scalar" if implementation == "scalar"
        else "aymos_fir_q15_m4"
    )
    _expect(execution["selected_function"], expected_name,
            f"{implementation} selected function")
    address = execution["function_address"]
    if not isinstance(address, str) or re.fullmatch(r"0x[0-9a-f]{8}", address) is None:
        raise PerformanceReportError(f"{implementation} function address is invalid")
    dynamic = execution["dynamic_instructions"]
    expected_dynamic = (
        {"smlalbb": signal_lab.WORK_UNITS} if implementation == "scalar"
        else {"smlald": signal_lab.WORK_UNITS // 2,
              "ssat": signal_lab.TOTAL_OUTPUT_COUNT}
    )
    dynamic = _exact_keys(dynamic, set(expected_dynamic),
                          f"{implementation} dynamic instructions")
    for mnemonic, expected_count in expected_dynamic.items():
        entry = _exact_keys(
            dynamic[mnemonic], {"address", "opcode_hex", "executed_count"},
            f"{implementation} {mnemonic}",
        )
        _expect(entry["executed_count"], expected_count,
                f"{implementation} {mnemonic} count")
        if (not isinstance(entry["address"], str)
                or re.fullmatch(r"0x[0-9a-f]{8}", entry["address"]) is None):
            raise PerformanceReportError(f"{implementation} {mnemonic} address is invalid")
        if (not isinstance(entry["opcode_hex"], str)
                or re.fullmatch(r"[0-9a-f]{4}|[0-9a-f]{8}", entry["opcode_hex"]) is None):
            raise PerformanceReportError(f"{implementation} {mnemonic} opcode is invalid")
    return execution


def _validate_run_summary(value: Any, implementation: str) -> dict[str, Any]:
    run = _exact_keys(value, RUN_KEYS, f"{implementation} run")
    _expect(run["implementation"], implementation, f"{implementation} identity")
    if (not isinstance(run["git_commit"], str)
            or re.fullmatch(r"[0-9a-f]{40}", run["git_commit"]) is None):
        raise PerformanceReportError(f"{implementation} git commit is invalid")
    _expect(run["repository_clean"], True, f"{implementation} source state")
    _expect(run["record_count"], signal_lab.EXPECTED_RECORD_COUNT,
            f"{implementation} record count")
    _expect(run["renode_version"], "1.16.1", f"{implementation} Renode")
    _expect(run["toolchain_version"], signal_lab.EXPECTED_TOOLCHAIN_VERSION,
            f"{implementation} toolchain")
    build = _exact_keys(run["build_configuration"], BUILD_KEYS,
                        f"{implementation} build configuration")
    _expect(build, {
        "app": "signal_lab", "board": "nucleo_f401re",
        "float_abi": "soft", "signal_impl": implementation,
    }, f"{implementation} build configuration")
    limits = _exact_keys(run["execution_limits"], LIMIT_KEYS,
                         f"{implementation} limits")
    _expect(limits, {
        "compressed_trace_bytes": signal_lab.MAX_EXECUTION_COMPRESSED_BYTES,
        "execution_entries": signal_lab.MAX_EXECUTION_ENTRIES,
        "host_timeout_seconds": int(signal_lab.RENODE_HOST_TIMEOUT_SECONDS),
        "outer_timeout_seconds": signal_lab.OUTER_TIMEOUT_SECONDS,
        "uncompressed_trace_bytes": signal_lab.MAX_EXECUTION_BYTES,
        "virtual_duration_seconds": signal_lab.RENODE_VIRTUAL_DURATION_SECONDS,
    }, f"{implementation} limits")
    run["result"] = _validate_result(run["result"], implementation)
    run["execution"] = _validate_execution(run["execution"], implementation)
    run["artifacts"] = _validate_artifact_manifest(
        run["artifacts"], f"{implementation} artifacts"
    )
    return run


def _validate_evidence_root(root: int) -> None:
    expected = {EVIDENCE_MARKER, "summary.json", *signal_lab.IMPLEMENTATIONS}
    actual = set(os.listdir(root))
    _expect(actual, expected, "evidence root entries")
    marker = _read_regular_at(root, EVIDENCE_MARKER, 64, "evidence marker")
    _expect(marker, b"schema=1\n", "evidence marker")


def _artifact_bytes(
    root: int, implementation: str, manifest: dict[str, Any],
) -> dict[str, bytes]:
    try:
        implementation_root = os.open(
            implementation, DIRECTORY_FLAGS, dir_fd=root
        )
    except OSError as error:
        raise PerformanceReportError(
            f"cannot open {implementation} evidence: {error}"
        ) from error
    try:
        _expect(set(os.listdir(implementation_root)), set(ARTIFACT_NAMES),
                f"{implementation} evidence entries")
        result: dict[str, bytes] = {}
        for name in ARTIFACT_NAMES:
            data = _read_regular_at(
                implementation_root, name, ARTIFACT_LIMITS[name],
                f"{implementation}/{name}",
            )
            _expect(len(data), manifest[name]["bytes"],
                    f"{implementation}/{name} byte size")
            _expect(_sha256_bytes(data), manifest[name]["sha256"],
                    f"{implementation}/{name} SHA-256")
            result[name] = data
        return result
    finally:
        os.close(implementation_root)


def _require_metadata(
    metadata: dict[str, str], expected: dict[str, str], label: str,
) -> None:
    for key, value in expected.items():
        if key not in metadata:
            raise PerformanceReportError(f"{label} is missing {key}")
        _expect(metadata[key], value, f"{label} {key}")


def _validate_execution_blob(
    data: bytes, execution: dict[str, Any], implementation: str,
) -> None:
    output = bytearray()
    try:
        with gzip.GzipFile(fileobj=io.BytesIO(data), mode="rb") as source:
            while True:
                block = source.read(65536)
                if not block:
                    break
                output.extend(block)
                if len(output) > signal_lab.MAX_EXECUTION_BYTES:
                    raise PerformanceReportError(
                        f"{implementation} execution trace exceeds its limit"
                    )
    except (EOFError, gzip.BadGzipFile, zlib.error) as error:
        raise PerformanceReportError(
            f"{implementation} execution trace is invalid gzip: {error}"
        ) from error
    _expect(len(output), execution["uncompressed_bytes"],
            f"{implementation} uncompressed execution size")
    try:
        entry_count, _, _ = signal_lab._parse_execution_entries(
            bytes(output), {}, (0, 0)
        )
    except signal_lab.SignalLabError as error:
        raise PerformanceReportError(
            f"{implementation} execution trace is malformed: {error}"
        ) from error
    _expect(entry_count, execution["entry_count"],
            f"{implementation} execution entry count")


def _reanalyze_execution(
    files: dict[str, bytes], implementation: str, expected: dict[str, Any],
) -> dict[str, Any]:
    objdump = (
        REPOSITORY_ROOT /
        ".tools/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-objdump"
    )
    if objdump.is_symlink() or not objdump.is_file():
        raise PerformanceReportError(f"missing pinned objdump: {objdump}")
    with tempfile.TemporaryDirectory(prefix="aymos-report-") as temporary:
        root = Path(temporary)
        elf = root / "firmware.elf"
        execution_trace = root / "execution.bin.gz"
        elf.write_bytes(files["firmware.elf"])
        execution_trace.write_bytes(files["execution.bin.gz"])
        try:
            actual = signal_lab.analyze_execution_trace(
                execution_trace, elf, implementation, objdump
            )
        except (OSError, signal_lab.SignalLabError) as error:
            raise PerformanceReportError(
                f"{implementation} execution recheck failed: {error}"
            ) from error
    _expect(actual, expected, f"{implementation} execution recheck")
    return actual


def _validate_uart_evidence(
    files: dict[str, bytes], implementation: str,
    expected_trace: dict[str, Any], expected_result: dict[str, Any],
) -> None:
    uart = files["uart.bin"]
    if uart.count(trace_tool.TRAILER) != 1:
        raise PerformanceReportError(
            f"{implementation} UART must contain one trace trailer"
        )
    trace_end = uart.index(trace_tool.TRAILER) + len(trace_tool.TRAILER)
    try:
        decoded, raw_records = trace_tool.decode(uart[:trace_end])
        result = signal_lab.parse_result(uart[trace_end:], implementation)
        signal_lab.validate_schedule(decoded)
    except (trace_tool.TraceError, signal_lab.SignalLabError) as error:
        raise PerformanceReportError(
            f"{implementation} UART contract failed: {error}"
        ) from error
    _expect(decoded, expected_trace, f"{implementation} decoded UART trace")
    _expect(raw_records, files["trace.bin"],
            f"{implementation} raw UART records")
    _expect(result, expected_result, f"{implementation} UART result")
    expected_status = (
        f"AYMOS DSP PASS IMPL={implementation} "
        f"RECORDS={signal_lab.EXPECTED_RECORD_COUNT} "
        f"OUTPUT_CRC=0x{signal_lab.OUTPUT_CRC:08X}\n"
    ).encode("ascii")
    _expect(files["uart.txt"], expected_status,
            f"{implementation} UART status")
    _expect(files["uart-validation.log"], b"",
            f"{implementation} UART validation log")


def _parse_summary(data: bytes) -> tuple[dict[str, Any], dict[str, dict[str, Any]]]:
    summary_data = data
    summary = _exact_keys(_json_bytes(summary_data, "DSP summary"),
                          TOP_KEYS, "DSP summary")
    _expect(summary["schema_version"], SUMMARY_SCHEMA_VERSION, "summary schema")
    _expect(summary["board"], "nucleo_f401re", "summary board")
    _expect(summary["random_seed"], "0x1A2B3C4D", "summary seed")
    _expect(summary["result_equality"], True, "result equality claim")
    _expect(summary["structured_trace_equality"], True, "trace equality claim")
    _expect(summary["trace_schema_version"], 1, "trace schema")
    _expect(summary["timing_claims"], "none_emulator_functional_test_only",
            "summary timing boundary")
    if not isinstance(summary["runs"], list) or len(summary["runs"]) != 2:
        raise PerformanceReportError("summary must contain two runs")
    by_impl: dict[str, dict[str, Any]] = {}
    for value in summary["runs"]:
        if not isinstance(value, dict):
            raise PerformanceReportError("summary run must be an object")
        implementation = value.get("implementation")
        if implementation not in signal_lab.IMPLEMENTATIONS or implementation in by_impl:
            raise PerformanceReportError("summary has invalid implementation membership")
        by_impl[implementation] = _validate_run_summary(value, implementation)
    _expect(set(by_impl), set(signal_lab.IMPLEMENTATIONS), "summary implementations")
    return summary, by_impl


def _load_evidence_descriptor(root_descriptor: int) -> dict[str, Any]:
    _validate_evidence_root(root_descriptor)
    summary_data = _read_regular_at(
        root_descriptor, "summary.json", SUMMARY_LIMIT,
        "DSP summary",
    )
    summary, by_impl = _parse_summary(summary_data)
    artifacts = {
        implementation: _artifact_bytes(
            root_descriptor, implementation,
            by_impl[implementation]["artifacts"],
        )
        for implementation in signal_lab.IMPLEMENTATIONS
    }
    for implementation in signal_lab.IMPLEMENTATIONS:
        run = by_impl[implementation]
        files = artifacts[implementation]
        stored_result = _json_bytes(files["result.json"],
                                    f"{implementation} result.json")
        _expect(stored_result, run["result"], f"{implementation} stored result")
        stored_execution = _json_bytes(
            files["execution-summary.json"],
            f"{implementation} execution-summary.json",
        )
        _expect(stored_execution, run["execution"],
                f"{implementation} stored execution summary")
        trace = _json_bytes(files["trace.json"], f"{implementation} trace.json")
        try:
            signal_lab.validate_schedule(trace)
        except signal_lab.SignalLabError as error:
            raise PerformanceReportError(
                f"{implementation} trace contract failed: {error}"
            ) from error
        _validate_uart_evidence(files, implementation, trace, run["result"])
        run_metadata = _metadata_bytes(files["metadata.txt"],
                                       f"{implementation} metadata")
        build_metadata = _metadata_bytes(files["build-metadata.txt"],
                                         f"{implementation} build metadata")
        _require_metadata(run_metadata, {
            "schema": "1", "board": "nucleo_f401re", "app": "signal_lab",
            "signal_impl": implementation, "git_commit": run["git_commit"],
            "repository_clean": "true", "firmware_sha256":
                run["artifacts"]["firmware.elf"]["sha256"],
            "architecture_flags": signal_lab.EXPECTED_ARCHITECTURE_FLAGS,
            "float_abi": "soft", "renode_version": run["renode_version"],
            "trace_schema_version": "1", "execution_trace_format":
                "ReTrace-v4-PCAndOpcode", "host_timeout_seconds":
                signal_lab.RENODE_HOST_TIMEOUT_SECONDS,
            "virtual_duration_seconds": signal_lab.RENODE_VIRTUAL_DURATION_SECONDS,
            "timing_claims": "none_emulator_functional_test_only",
        }, f"{implementation} run metadata")
        _require_metadata(build_metadata, {
            "project": "aymos", "board": "nucleo_f401re", "app": "signal_lab",
            "git_commit": run["git_commit"], "repository_clean": "true",
            "dependencies_verified": "true", "dependencies_clean": "true",
            "compiler": signal_lab.EXPECTED_TOOLCHAIN_VERSION,
            "architecture_flags": signal_lab.EXPECTED_ARCHITECTURE_FLAGS,
            "float_abi": "soft", "dsp_optimization": "-O2",
            "renode": run["renode_version"], "signal_impl": implementation,
            "trace_schema_version": "1",
        }, f"{implementation} build metadata")
        _expect(run["execution"]["compressed_bytes"],
                len(files["execution.bin.gz"]),
                f"{implementation} compressed execution size")
        _validate_execution_blob(
            files["execution.bin.gz"], run["execution"], implementation
        )
        _reanalyze_execution(files, implementation, run["execution"])

    scalar = by_impl["scalar"]
    m4 = by_impl["m4"]
    _expect(scalar["git_commit"], m4["git_commit"], "firmware source commit")
    scalar_result = dict(scalar["result"])
    m4_result = dict(m4["result"])
    scalar_result.pop("implementation")
    m4_result.pop("implementation")
    _expect(scalar_result, m4_result, "fixed firmware result equality")
    _expect(artifacts["scalar"]["trace.bin"], artifacts["m4"]["trace.bin"],
            "binary structured trace equality")
    _expect(artifacts["scalar"]["trace.json"], artifacts["m4"]["trace.json"],
            "JSON structured trace equality")
    trace = _json_bytes(artifacts["scalar"]["trace.json"], "common trace")
    return {
        "source_summary": summary,
        "runs": by_impl,
        "artifacts": artifacts,
        "trace": trace,
        "git_commit": scalar["git_commit"],
    }


def load_evidence(root: Path = DEFAULT_EVIDENCE_ROOT) -> dict[str, Any]:
    """Read and fully validate one completed two-image DSP gate."""
    root_descriptor = _open_directory(root, "DSP evidence root")
    try:
        return _load_evidence_descriptor(root_descriptor)
    finally:
        os.close(root_descriptor)


def _schedule(trace: dict[str, Any]) -> dict[str, Any]:
    records = trace["records"]
    switches = [record for record in records if record["event"] == "context_switch"]
    intervals: list[dict[str, Any]] = []
    for index, record in enumerate(switches):
        start = record["tick"]
        end = (switches[index + 1]["tick"] if index + 1 < len(switches)
               else trace["footer"]["final_tick"])
        incoming = record["related"]
        if incoming not in TASKS or end < start:
            raise PerformanceReportError("common trace has an invalid switch interval")
        if end > start:
            intervals.append({"task": incoming, "start": start, "end": end})
    releases = [
        {"task": record["task"], "tick": record["tick"],
         "absolute_deadline": record["value0"], "job": record["value2"]}
        for record in records if record["event"] == "task_release"
    ]
    deadline_met = [
        {"task": record["task"], "tick": record["tick"],
         "absolute_deadline": record["value0"], "job": record["value2"]}
        for record in records if record["event"] == "deadline_met"
    ]
    return {
        "final_tick": trace["footer"]["final_tick"],
        "record_count": trace["record_count"],
        "intervals": intervals,
        "releases": releases,
        "deadlines_met": deadline_met,
    }


def build_report_model(evidence: dict[str, Any]) -> dict[str, Any]:
    scalar = evidence["runs"]["scalar"]
    m4 = evidence["runs"]["m4"]
    result = dict(scalar["result"])
    result.pop("implementation")
    return {
        "schema_version": REPORT_SCHEMA_VERSION,
        "title": "AymOS DSP workload",
        "board": "nucleo_f401re",
        "source_commit": evidence["git_commit"],
        "trace_schema_version": 1,
        "result": result,
        "instruction_evidence": {
            "scalar": {
                "function": scalar["execution"]["selected_function"],
                "single_lane_smlalbb": scalar["execution"]
                    ["dynamic_instructions"]["smlalbb"]["executed_count"],
                "function_instruction_hits": scalar["execution"]
                    ["function_instruction_hits"],
            },
            "m4": {
                "function": m4["execution"]["selected_function"],
                "packed_smlald": m4["execution"]
                    ["dynamic_instructions"]["smlald"]["executed_count"],
                "products_covered": signal_lab.WORK_UNITS,
                "ssat": m4["execution"]["dynamic_instructions"]
                    ["ssat"]["executed_count"],
                "function_instruction_hits": m4["execution"]
                    ["function_instruction_hits"],
            },
        },
        "schedule": _schedule(evidence["trace"]),
        "toolchain_version": scalar["toolchain_version"],
        "renode_version": scalar["renode_version"],
        "execution_limits": scalar["execution_limits"],
        "measurement_boundary": (
            "Functional emulator instruction and scheduling evidence only. "
            "No speed, cycle, latency, WCET, or physical performance claim."
        ),
        "insight": (
            "The packed loop executes one SMLALD for each pair of products. "
            "The scalar compiler uses one SMLALBB for each product. The packed "
            "MAC count is half the single-lane MAC count. The M4 function has "
            "more total executed instruction records in this first path, so "
            "this result does not establish whole-function efficiency."
        ),
    }


def _svg_timeline(model: dict[str, Any]) -> str:
    schedule = model["schedule"]
    left, width, row_height, top = 150, 870, 48, 44
    final_tick = schedule["final_tick"]
    domain = max(
        final_tick,
        *(release["absolute_deadline"] for release in schedule["releases"]),
    )
    height = top + len(TASKS) * row_height + 34

    def x(tick: int) -> float:
        return left + width * tick / domain

    values = [
        f'<svg viewBox="0 0 1060 {height}" role="img" '
        'aria-label="Common DSP workload task-state timeline">',
        f'<rect width="1060" height="{height}" rx="12" fill="#0b1323"/>',
    ]
    ticks = list(range(0, domain + 1, 3))
    if ticks[-1] != domain:
        ticks.append(domain)
    for tick in ticks:
        px = x(tick)
        values.append(
            f'<path d="M{px:.1f} 30V{height - 18}" stroke="#253149"/>'
            f'<text x="{px:.1f}" y="21" fill="#8291aa" text-anchor="middle" '
            f'font-size="10">{tick}</text>'
        )
    for task_id, (name, _) in TASKS.items():
        y = top + task_id * row_height
        values.append(
            f'<text x="18" y="{y + 19}" fill="#dce7f5" font-size="13" '
            f'font-weight="600">{name}</text>'
            f'<path d="M{left} {y + 15}H{left + width}" stroke="#33415d"/>'
        )
    for interval in schedule["intervals"]:
        task_id = interval["task"]
        name, color = TASKS[task_id]
        start, end = x(interval["start"]), x(interval["end"])
        values.append(
            f'<rect x="{start:.1f}" y="{top + task_id * row_height + 3}" '
            f'width="{max(2.0, end - start):.1f}" height="24" rx="5" '
            f'fill="{color}"><title>{name}: scheduled RUNNING state from tick '
            f'{interval["start"]} to {interval["end"]}</title></rect>'
        )
    for release in schedule["releases"]:
        task_id = release["task"]
        y = top + task_id * row_height
        release_x = x(release["tick"])
        deadline_x = x(release["absolute_deadline"])
        values.append(
            f'<circle cx="{release_x:.1f}" cy="{y + 2}" r="4" fill="#f8fafc">'
            f'<title>{TASKS[task_id][0]} release at tick {release["tick"]}</title>'
            f'</circle><path d="M{deadline_x:.1f} {y - 5}V{y + 31}" '
            f'stroke="#f8fafc" stroke-dasharray="3 3"><title>'
            f'{TASKS[task_id][0]} deadline at tick '
            f'{release["absolute_deadline"]}</title></path>'
        )
    values.append("</svg>")
    return "".join(values)


def render_html(model: dict[str, Any]) -> str:
    result = model["result"]
    scalar = model["instruction_evidence"]["scalar"]
    m4 = model["instruction_evidence"]["m4"]
    timeline = _svg_timeline(model)
    return f'''<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>AymOS DSP workload</title><style>
:root{{--bg:#070d18;--panel:#101827;--line:#26344c;--ink:#edf5ff;--muted:#9aabc1;--cyan:#67e8f9;--violet:#c4b5fd}}
*{{box-sizing:border-box}}body{{margin:0;background:var(--bg);color:var(--ink);font:15px/1.55 ui-sans-serif,system-ui,sans-serif}}
main{{max-width:1180px;margin:auto;padding:44px 24px 72px}}h1{{font-size:38px;margin:0}}h2{{margin-top:34px}}p{{color:var(--muted)}}
.cards{{display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:14px;margin:28px 0}}.card,.panel{{background:var(--panel);border:1px solid var(--line);border-radius:16px;padding:18px}}
.label{{color:var(--cyan);font-weight:700;letter-spacing:.04em}}.value{{font-size:28px;font-weight:750;margin:8px 0}}.sub{{color:var(--muted)}}
.plot{{overflow:auto}}.plot svg{{min-width:900px;width:100%}}table{{width:100%;border-collapse:collapse}}th,td{{text-align:left;padding:11px;border-bottom:1px solid var(--line)}}th{{color:var(--cyan)}}code{{color:#bae6fd}}
</style></head><body><main><h1>AymOS DSP workload</h1>
<p>Two exact NUCLEO-F401RE firmware images run the same deterministic workload through the AymOS kernel.</p>
<div class="cards"><div class="card"><div class="label">FIXED RESULT</div><div class="value">{result['total_outputs']} outputs</div><div class="sub">Output CRC-32 <code>0x{result['output_crc32']:08X}</code><br>Both fixed result contracts pass. The structured traces are byte-identical.</div></div>
<div class="card"><div class="label">SCALAR C</div><div class="value">{scalar['single_lane_smlalbb']:,} SMLALBB</div><div class="sub">One executed single-lane MAC per product.<br>{scalar['function_instruction_hits']:,} total FIR-body instruction records.</div></div>
<div class="card"><div class="label" style="color:var(--violet)">PACKED M4</div><div class="value">{m4['packed_smlald']:,} SMLALD</div><div class="sub">{m4['products_covered']:,} products and {m4['ssat']:,} SSAT.<br>{m4['function_instruction_hits']:,} total FIR-body instruction records.</div></div></div>
<h2>Scheduled task state</h2><p>Circles mark releases. Dashed lines mark absolute deadlines. Bars show scheduled RUNNING state. They include task wait time and are not CPU-active time.</p><div class="panel plot">{timeline}</div>
<h2>What the evidence means</h2><p>{html.escape(model['insight'])}</p>
<h2>Provenance</h2><table><tr><th>Source commit</th><td><code>{html.escape(model['source_commit'])}</code></td></tr><tr><th>Target</th><td>NUCLEO-F401RE, Cortex-M4, soft-float</td></tr><tr><th>Toolchain</th><td>Arm GNU {html.escape(model['toolchain_version'])}; DSP translation units use <code>-O2</code></td></tr><tr><th>Renode</th><td>{html.escape(model['renode_version'])}</td></tr><tr><th>Trace</th><td>schema {model['trace_schema_version']}; {model['schedule']['record_count']} exact records</td></tr></table>
<h2>Measurement boundary</h2><p>{html.escape(model['measurement_boundary'])}</p>
</main></body></html>\n'''


def workload_document(model: dict[str, Any]) -> dict[str, Any]:
    result = model["result"]
    return {
        "schema_version": 1,
        "name": "signal_lab",
        "board": model["board"],
        "seed": f"0x{result['seed']:08X}",
        "frames": result["frames"],
        "samples_per_frame": result["samples_per_frame"],
        "taps": result["taps"],
        "valid_outputs_per_frame": result["valid_outputs_per_frame"],
        "task_releases": {"sampler": [0, 6, 12, 18],
                          "processor": [1, 7, 13, 19],
                          "verifier": [2, 8, 14, 20]},
        "implementations": ["scalar", "m4"],
    }


def _json_text(value: Any) -> str:
    return json.dumps(value, indent=2, sort_keys=True) + "\n"


def _open_runs_root(root: Path) -> tuple[Path, int]:
    repository = REPOSITORY_ROOT.resolve(strict=True)
    requested = Path(os.path.abspath(root))
    expected = repository / "runs"
    if requested != expected:
        raise PerformanceReportError("runs root must be the repository runs directory")
    repository_descriptor = _open_directory(repository, "repository root")
    try:
        try:
            os.mkdir("runs", 0o755, dir_fd=repository_descriptor)
        except FileExistsError:
            pass
        try:
            root_descriptor = os.open(
                "runs", DIRECTORY_FLAGS, dir_fd=repository_descriptor
            )
        except OSError as error:
            raise PerformanceReportError(
                f"cannot open repository runs directory: {error}"
            ) from error
    finally:
        os.close(repository_descriptor)
    return expected, root_descriptor


def _write_all(descriptor: int, data: bytes) -> None:
    offset = 0
    while offset < len(data):
        written = os.write(descriptor, data[offset:])
        if written <= 0:
            raise PerformanceReportError("could not complete report file write")
        offset += written


def _write_bytes_at(directory: int, name: str, data: bytes) -> None:
    if not name or "/" in name or name in (".", ".."):
        raise PerformanceReportError(f"unsafe output file name: {name!r}")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC
    descriptor = os.open(name, flags, 0o644, dir_fd=directory)
    try:
        if not stat.S_ISREG(os.fstat(descriptor).st_mode):
            raise PerformanceReportError(f"output is not a regular file: {name}")
        _write_all(descriptor, data)
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _replace_bytes_at(directory: int, name: str, data: bytes) -> None:
    flags = os.O_WRONLY | os.O_TRUNC | os.O_NOFOLLOW | os.O_CLOEXEC
    descriptor = os.open(name, flags, dir_fd=directory)
    try:
        if not stat.S_ISREG(os.fstat(descriptor).st_mode):
            raise PerformanceReportError(f"output is not a regular file: {name}")
        _write_all(descriptor, data)
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _directory_identity(descriptor: int) -> tuple[int, int]:
    state = os.fstat(descriptor)
    if not stat.S_ISDIR(state.st_mode):
        raise PerformanceReportError("owned report entry is not a directory")
    return state.st_dev, state.st_ino


def _mount_id(descriptor: int) -> int:
    path = Path(f"/proc/self/fdinfo/{descriptor}")
    try:
        data = path.read_text(encoding="ascii")
    except (OSError, UnicodeError) as error:
        raise PerformanceReportError(
            "cannot read Linux mount identity for report retention"
        ) from error
    matches = re.findall(r"^mnt_id:\s*([0-9]+)\s*$", data, re.MULTILINE)
    if len(matches) != 1:
        raise PerformanceReportError(
            "Linux mount identity is missing or malformed"
        )
    return int(matches[0])


def _owned_report_identity(
    root: int, name: str, allowed_markers: tuple[bytes, ...],
) -> tuple[int, int]:
    try:
        descriptor = os.open(name, DIRECTORY_FLAGS, dir_fd=root)
    except OSError as error:
        raise PerformanceReportError(
            f"cannot open owned report directory {name}: {error}"
        ) from error
    try:
        identity = _directory_identity(descriptor)
        marker = _read_regular_at(
            descriptor, REPORT_MARKER, 64, f"report marker {name}"
        )
        if marker not in allowed_markers:
            raise PerformanceReportError(f"report marker is not owned: {name}")
        return identity
    finally:
        os.close(descriptor)


def _name_exists(root: int, name: str) -> bool:
    try:
        os.stat(name, dir_fd=root, follow_symlinks=False)
    except FileNotFoundError:
        return False
    return True


def _restore_quarantine(root: int, quarantine: str, original: str) -> None:
    if not _name_exists(root, original) and _name_exists(root, quarantine):
        os.rename(
            quarantine, original, src_dir_fd=root, dst_dir_fd=root
        )


def _remove_tree_contents(
    directory: int, root_device: int, root_mount_id: int,
) -> None:
    for name in os.listdir(directory):
        state = os.stat(name, dir_fd=directory, follow_symlinks=False)
        if stat.S_ISDIR(state.st_mode):
            child = os.open(name, DIRECTORY_FLAGS, dir_fd=directory)
            try:
                opened = os.fstat(child)
                if (opened.st_dev, opened.st_ino) != (state.st_dev, state.st_ino):
                    raise PerformanceReportError(
                        f"report child changed before removal: {name}"
                    )
                if opened.st_dev != root_device:
                    raise PerformanceReportError(
                        f"report child crosses a device boundary: {name}"
                    )
                if _mount_id(child) != root_mount_id:
                    raise PerformanceReportError(
                        f"report child crosses a mount boundary: {name}"
                    )
                _remove_tree_contents(child, root_device, root_mount_id)
                current = os.stat(
                    name, dir_fd=directory, follow_symlinks=False
                )
                if (current.st_dev, current.st_ino) != (
                        opened.st_dev, opened.st_ino):
                    raise PerformanceReportError(
                        f"report child changed during removal: {name}"
                    )
                os.rmdir(name, dir_fd=directory)
            finally:
                os.close(child)
        else:
            os.unlink(name, dir_fd=directory)


def _validate_tree_boundaries(
    directory: int, root_device: int, root_mount_id: int,
) -> None:
    for name in os.listdir(directory):
        state = os.stat(name, dir_fd=directory, follow_symlinks=False)
        if not stat.S_ISDIR(state.st_mode):
            continue
        child = os.open(name, DIRECTORY_FLAGS, dir_fd=directory)
        try:
            opened = os.fstat(child)
            if (opened.st_dev, opened.st_ino) != (state.st_dev, state.st_ino):
                raise PerformanceReportError(
                    f"report child changed before boundary check: {name}"
                )
            if opened.st_dev != root_device:
                raise PerformanceReportError(
                    f"report child crosses a device boundary: {name}"
                )
            if _mount_id(child) != root_mount_id:
                raise PerformanceReportError(
                    f"report child crosses a mount boundary: {name}"
                )
            _validate_tree_boundaries(child, root_device, root_mount_id)
        finally:
            os.close(child)


def _quarantine_and_remove(
    root: int, name: str, identity: tuple[int, int],
    allowed_markers: tuple[bytes, ...],
) -> None:
    quarantine = f".signal-report-prune-{os.getpid()}-{secrets.token_hex(4)}"
    os.rename(name, quarantine, src_dir_fd=root, dst_dir_fd=root)
    descriptor: int | None = None
    try:
        descriptor = os.open(quarantine, DIRECTORY_FLAGS, dir_fd=root)
        quarantine_identity = _directory_identity(descriptor)
        marker = _read_regular_at(
            descriptor, REPORT_MARKER, 64,
            f"report marker {quarantine}",
        )
        if marker not in allowed_markers or quarantine_identity != identity:
            raise PerformanceReportError(
                f"report directory changed before removal: {name}"
            )
        root_mount_id = _mount_id(descriptor)
        _validate_tree_boundaries(
            descriptor, quarantine_identity[0], root_mount_id
        )
        _remove_tree_contents(
            descriptor, quarantine_identity[0], root_mount_id
        )
        current = os.stat(quarantine, dir_fd=root, follow_symlinks=False)
        if (current.st_dev, current.st_ino) != quarantine_identity:
            raise PerformanceReportError(
                f"report directory changed during removal: {name}"
            )
        os.rmdir(quarantine, dir_fd=root)
    except BaseException as error:
        _restore_quarantine(root, quarantine, name)
        if isinstance(error, PerformanceReportError):
            raise
        raise PerformanceReportError(
            f"report directory changed before removal: {name}: {error}"
        ) from error
    finally:
        if descriptor is not None:
            os.close(descriptor)


def _completed_report_candidates(
    root: int,
) -> list[tuple[str, tuple[int, int]]]:
    candidates: list[tuple[str, tuple[int, int]]] = []
    for name in os.listdir(root):
        if RUN_PATTERN.fullmatch(name) is None:
            continue
        try:
            identity = _owned_report_identity(
                root, name, (REPORT_MARKER_COMPLETE.encode("ascii"),)
            )
        except PerformanceReportError:
            continue
        candidates.append((name, identity))
    candidates.sort(key=lambda item: item[0], reverse=True)
    return candidates


def _prune_report_runs_at(root: int, current_name: str) -> None:
    if RUN_PATTERN.fullmatch(current_name) is None:
        raise PerformanceReportError("current report name is invalid")
    current_identity = _owned_report_identity(
        root, current_name, (REPORT_MARKER_COMPLETE.encode("ascii"),)
    )
    candidates = _completed_report_candidates(root)
    others = [item for item in candidates if item[0] != current_name]
    for expired_name, identity in others[RETAIN_REPORT_RUNS - 1:]:
        _quarantine_and_remove(
            root, expired_name, identity,
            (REPORT_MARKER_COMPLETE.encode("ascii"),),
        )
    if _owned_report_identity(
            root, current_name,
            (REPORT_MARKER_COMPLETE.encode("ascii"),)) != current_identity:
        raise PerformanceReportError("current report changed during retention")


def _prune_report_runs(root: Path, current: Path) -> None:
    expected, descriptor = _open_runs_root(root)
    try:
        if current.parent != expected:
            raise PerformanceReportError("current report is outside the runs root")
        _prune_report_runs_at(descriptor, current.name)
    finally:
        os.close(descriptor)


def _open_repository_relative_directory(
    path: Path, label: str,
) -> tuple[Path, int]:
    repository = REPOSITORY_ROOT.resolve(strict=True)
    requested = Path(os.path.abspath(path))
    try:
        relative = requested.relative_to(repository)
    except ValueError as error:
        raise PerformanceReportError(
            f"{label} must be inside the repository"
        ) from error
    if not relative.parts:
        raise PerformanceReportError(f"{label} may not be the repository root")
    descriptor = _open_directory(repository, "repository root")
    try:
        for component in relative.parts:
            if component in ("", ".", "..") or "/" in component:
                raise PerformanceReportError(
                    f"{label} has an unsafe path component"
                )
            child = os.open(
                component, DIRECTORY_FLAGS, dir_fd=descriptor
            )
            os.close(descriptor)
            descriptor = child
    except BaseException as error:
        os.close(descriptor)
        if isinstance(error, PerformanceReportError):
            raise
        raise PerformanceReportError(f"cannot open {label}: {error}") from error
    return relative, descriptor


def publish_report(
    evidence_root: Path = DEFAULT_EVIDENCE_ROOT,
    runs_root: Path = DEFAULT_RUNS_ROOT,
) -> Path:
    evidence_name, evidence_descriptor = _open_repository_relative_directory(
        evidence_root, "evidence root"
    )
    try:
        evidence = _load_evidence_descriptor(evidence_descriptor)
    finally:
        os.close(evidence_descriptor)
    model = build_report_model(evidence)
    root, root_descriptor = _open_runs_root(runs_root)
    try:
        lock_flags = (
            os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC
            | os.O_NONBLOCK
        )
        lock_descriptor = os.open(
            ".aymos-signal-report.lock", lock_flags, 0o600,
            dir_fd=root_descriptor,
        )
        if not stat.S_ISREG(os.fstat(lock_descriptor).st_mode):
            os.close(lock_descriptor)
            raise PerformanceReportError("report lock is not a regular file")
        with os.fdopen(lock_descriptor, "w", encoding="ascii") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            run_id = (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
                      + f"-{model['source_commit'][:8]}-signal")
            if RUN_PATTERN.fullmatch(run_id) is None:
                raise PerformanceReportError("generated report ID is invalid")
            if _name_exists(root_descriptor, run_id):
                raise PerformanceReportError(
                    f"report destination exists: {root / run_id}"
                )
            stage_name = (
                f".signal-report-stage-{os.getpid()}-{secrets.token_hex(4)}"
            )
            os.mkdir(stage_name, 0o700, dir_fd=root_descriptor)
            stage_identity: tuple[int, int] | None = None
            stage_descriptor: int | None = None
            try:
                stage_descriptor = os.open(
                    stage_name, DIRECTORY_FLAGS, dir_fd=root_descriptor
                )
                stage_identity = _directory_identity(stage_descriptor)
                _write_bytes_at(
                    stage_descriptor, REPORT_MARKER,
                    REPORT_MARKER_ACTIVE.encode("ascii"),
                )
                output_files: dict[str, bytes] = {}
                for implementation in signal_lab.IMPLEMENTATIONS:
                    os.mkdir(implementation, 0o755, dir_fd=stage_descriptor)
                    implementation_descriptor = os.open(
                        implementation, DIRECTORY_FLAGS,
                        dir_fd=stage_descriptor,
                    )
                    try:
                        for name in ARTIFACT_NAMES:
                            data = evidence["artifacts"][implementation][name]
                            _write_bytes_at(implementation_descriptor, name, data)
                            output_files[f"{implementation}/{name}"] = data
                        os.fsync(implementation_descriptor)
                    finally:
                        os.close(implementation_descriptor)
                output_files.update({
                    "workload.json": _json_text(
                        workload_document(model)).encode("utf-8"),
                    "summary.json": _json_text(model).encode("utf-8"),
                    "comparison.html": render_html(model).encode("utf-8"),
                })
                for name in (
                    "workload.json", "summary.json", "comparison.html",
                ):
                    _write_bytes_at(stage_descriptor, name, output_files[name])
                metadata = {
                    "schema_version": REPORT_SCHEMA_VERSION,
                    "run_id": run_id,
                    "created_utc": datetime.now(timezone.utc).isoformat(),
                    "source_evidence": evidence_name.as_posix(),
                    "source_commit": model["source_commit"],
                    "board": model["board"],
                    "toolchain_version": model["toolchain_version"],
                    "renode_version": model["renode_version"],
                    "build_configuration": {
                        "app": "signal_lab",
                        "implementations": ["scalar", "m4"],
                        "architecture_flags":
                            signal_lab.EXPECTED_ARCHITECTURE_FLAGS,
                        "float_abi": "soft", "dsp_optimization": "-O2",
                    },
                    "random_seed": f"0x{model['result']['seed']:08X}",
                    "trace_schema_version": model["trace_schema_version"],
                    "execution_limits": model["execution_limits"],
                    "emulator_arguments": {
                        implementation: _metadata_bytes(
                            evidence["artifacts"][implementation]["metadata.txt"],
                            f"{implementation} metadata",
                        )["emulator_arguments"]
                        for implementation in signal_lab.IMPLEMENTATIONS
                    },
                    "measurement_boundary": model["measurement_boundary"],
                    "files": {
                        name: {
                            "bytes": len(data), "sha256": _sha256_bytes(data),
                        }
                        for name, data in sorted(output_files.items())
                    },
                }
                _write_bytes_at(
                    stage_descriptor, "metadata.json",
                    _json_text(metadata).encode("utf-8"),
                )
                _replace_bytes_at(
                    stage_descriptor, REPORT_MARKER,
                    REPORT_MARKER_COMPLETE.encode("ascii"),
                )
                os.fsync(stage_descriptor)
                os.close(stage_descriptor)
                stage_descriptor = None
                os.rename(
                    stage_name, run_id, src_dir_fd=root_descriptor,
                    dst_dir_fd=root_descriptor,
                )
                os.fsync(root_descriptor)
                _prune_report_runs_at(root_descriptor, run_id)
                os.fsync(root_descriptor)
            except BaseException:
                if stage_descriptor is not None:
                    os.close(stage_descriptor)
                if (stage_identity is not None
                        and _name_exists(root_descriptor, stage_name)):
                    try:
                        _quarantine_and_remove(
                            root_descriptor, stage_name, stage_identity,
                            (REPORT_MARKER_ACTIVE.encode("ascii"),
                             REPORT_MARKER_COMPLETE.encode("ascii")),
                        )
                    except PerformanceReportError:
                        pass
                raise
    finally:
        os.close(root_descriptor)
    return root / run_id


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=Path, default=DEFAULT_EVIDENCE_ROOT)
    parser.add_argument("--runs", type=Path, default=DEFAULT_RUNS_ROOT)
    arguments = parser.parse_args(argv)
    try:
        print(publish_report(arguments.evidence, arguments.runs))
    except PerformanceReportError as error:
        print(f"performance report failed: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
