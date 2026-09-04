"""Validate and run the deterministic board-targeted AymOS Signal Lab."""

from __future__ import annotations

from collections import Counter
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import stat
import subprocess
import sys
from typing import Any
import zlib

from tools.aymos_lab.trace import TRAILER, TraceError, decode, read_bounded


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
EVIDENCE_ROOT = REPOSITORY_ROOT / "build/signal-lab/evidence"
EVIDENCE_MARKER = ".aymos-signal-lab-evidence"
IMPLEMENTATIONS = ("scalar", "m4")
FRAME_COUNT = 4
SAMPLE_COUNT = 128
TAP_COUNT = 16
VALID_OUTPUT_COUNT = 113
TOTAL_OUTPUT_COUNT = 452
WORK_UNITS = 7232
SEED = 0x1A2B3C4D
INPUT_CRC = 0x0CAF72FD
OUTPUT_CRC = 0xAFC277C1
FINAL_TICK = 21
TRACE_CAPACITY = 256
RENODE_HOST_TIMEOUT_SECONDS = "25"
RENODE_VIRTUAL_DURATION_SECONDS = "0.03"
OUTER_TIMEOUT_SECONDS = 45
MAX_EXECUTION_COMPRESSED_BYTES = 2 * 1024 * 1024
MAX_EXECUTION_BYTES = 12 * 1024 * 1024
MAX_EXECUTION_ENTRIES = 2_000_000
EXPECTED_ARCHITECTURE_FLAGS = "-mcpu=cortex-m4 -mthumb -mfloat-abi=soft"
EXPECTED_TOOLCHAIN_VERSION = "14.3.1"

RESULT_PATTERN = re.compile(
    rb"AYMOS SIGNAL PASS IMPL=(scalar|m4) "
    rb"FRAMES=([0-9]+) SAMPLES=([0-9]+) TAPS=([0-9]+) "
    rb"VALID=([0-9]+) OUTPUTS=([0-9]+) WORK=([0-9]+) "
    rb"SEED=0x([0-9A-F]{8}) INPUT_CRC=0x([0-9A-F]{8}) "
    rb"OUTPUT_CRC=0x([0-9A-F]{8})\r\n"
)

SemanticRecord = tuple[str, int | None, int | None, int, int, int, int]


def _expected_semantic_projection() -> tuple[SemanticRecord, ...]:
    records: list[SemanticRecord] = []

    def add(
        event: str, task: int | None, related: int | None, tick: int,
        value0: int = 0, value1: int = 0, value2: int = 0,
    ) -> None:
        records.append((event, task, related, tick, value0, value1, value2))

    def deadline(
        event: str, task: int, tick: int, absolute: int, job: int,
    ) -> None:
        add(event, task, None, tick, absolute, 0, job)

    def select_ready(
        tick: int, current: int | None, selected: int, absolute: int,
        priority: int, purpose: int,
    ) -> None:
        summary = 1 | (purpose << 8) | (0xFF << 16) | (1 << 24)
        candidate = priority | (2 << 8) | (1 << 18)
        add("task_select", selected, current, tick,
            1 << selected, summary, 1)
        add("select_candidate", selected, selected, tick,
            absolute, 0, candidate)

    def select_idle(tick: int, current: int) -> None:
        summary = 5 | (1 << 8) | (0xFF << 16)
        add("task_select", 0, current, tick, 0, summary, 0)

    def switch(
        tick: int, outgoing: int | None, incoming: int, cause: int,
        outgoing_state: int,
    ) -> None:
        add("context_switch", outgoing, incoming, tick,
            cause, outgoing_state, 0)

    add("task_create", 1, None, 0, 1, 0, 2)
    deadline("task_release", 1, 0, 3, 1)
    add("task_create", 2, None, 0, 1, 1, 1)
    add("task_create", 3, None, 0, 1, 2, 1)
    add("kernel_start", None, None, 0, 0, 5, 0)
    select_ready(0, None, 1, 3, 0, 1)
    switch(0, None, 1, 1, 0)
    deadline("task_start", 1, 0, 3, 1)

    deadline("task_release", 2, 1, 4, 1)
    deadline("deadline_met", 1, 1, 3, 1)
    add("task_wait_period", 1, None, 1, 6)
    select_ready(1, 1, 2, 4, 1, 1)
    switch(1, 1, 2, 5, 1)
    deadline("task_start", 2, 1, 4, 1)

    deadline("task_release", 3, 2, 5, 1)
    deadline("deadline_met", 2, 2, 4, 1)
    add("task_wait_period", 2, None, 2, 7)
    select_ready(2, 2, 3, 5, 2, 1)
    switch(2, 2, 3, 5, 1)
    deadline("task_start", 3, 2, 5, 1)

    deadline("deadline_met", 3, 3, 5, 1)
    add("task_wait_period", 3, None, 3, 8)
    select_idle(3, 3)
    add("idle_start", 0, 3, 3)
    switch(3, 3, 0, 5, 1)

    for base in (6, 12, 18):
        job = (base // 6) + 1
        deadline("task_release", 1, base, base + 3, job)
        select_ready(base, 0, 1, base + 3, 0, 2)
        select_ready(base, 0, 1, base + 3, 0, 1)
        add("idle_stop", 0, 1, base)
        add("task_preempt", 0, 1, base, 1)
        switch(base, 0, 1, 6, 2)

        deadline("task_release", 2, base + 1, base + 4, job)
        deadline("deadline_met", 1, base + 1, base + 3, job)
        if base == 18:
            add("task_exit", 1, None, base + 1, 4, 0, 0)
        else:
            add("task_wait_period", 1, None, base + 1, base + 6)
        select_ready(base + 1, 1, 2, base + 4, 1, 1)
        switch(base + 1, 1, 2, 4 if base == 18 else 5,
               5 if base == 18 else 1)

        deadline("task_release", 3, base + 2, base + 5, job)
        deadline("deadline_met", 2, base + 2, base + 4, job)
        if base == 18:
            add("task_exit", 2, None, base + 2, 4, 0, 0)
        else:
            add("task_wait_period", 2, None, base + 2, base + 7)
        select_ready(base + 2, 2, 3, base + 5, 2, 1)
        switch(base + 2, 2, 3, 4 if base == 18 else 5,
               5 if base == 18 else 1)

        deadline("deadline_met", 3, base + 3, base + 5, job)
        if base == 18:
            add("task_exit", 3, None, base + 3, 4, 0, 0)
        else:
            add("task_wait_period", 3, None, base + 3, base + 8)
        select_idle(base + 3, 3)
        add("idle_start", 0, 3, base + 3)
        switch(base + 3, 3, 0, 4 if base == 18 else 5,
               5 if base == 18 else 1)
    return tuple(records)


EXPECTED_SEMANTIC_PROJECTION = _expected_semantic_projection()
EXPECTED_RECORD_COUNT = len(EXPECTED_SEMANTIC_PROJECTION)


class SignalLabError(TraceError):
    """Signal Lab evidence does not satisfy its deterministic contract."""


def _expect(actual: Any, expected: Any, label: str) -> None:
    if actual != expected:
        raise SignalLabError(
            f"Signal Lab {label} mismatch: got {actual!r}, expected {expected!r}"
        )


def parse_result(suffix: bytes, implementation: str) -> dict[str, Any]:
    if implementation not in IMPLEMENTATIONS:
        raise SignalLabError(f"unsupported Signal Lab implementation: {implementation}")
    match = RESULT_PATTERN.fullmatch(suffix)
    if match is None:
        raise SignalLabError("result suffix does not match the exact ASCII contract")
    result = {
        "implementation": match.group(1).decode("ascii"),
        "frames": int(match.group(2)),
        "samples_per_frame": int(match.group(3)),
        "taps": int(match.group(4)),
        "valid_outputs_per_frame": int(match.group(5)),
        "total_outputs": int(match.group(6)),
        "work_units": int(match.group(7)),
        "seed": int(match.group(8), 16),
        "input_crc32": int(match.group(9), 16),
        "output_crc32": int(match.group(10), 16),
        "status": "PASS",
    }
    expected = {
        "implementation": implementation,
        "frames": FRAME_COUNT,
        "samples_per_frame": SAMPLE_COUNT,
        "taps": TAP_COUNT,
        "valid_outputs_per_frame": VALID_OUTPUT_COUNT,
        "total_outputs": TOTAL_OUTPUT_COUNT,
        "work_units": WORK_UNITS,
        "seed": SEED,
        "input_crc32": INPUT_CRC,
        "output_crc32": OUTPUT_CRC,
        "status": "PASS",
    }
    _expect(result, expected, "result")
    return result


def semantic_projection(decoded: dict[str, Any]) -> tuple[SemanticRecord, ...]:
    records = decoded.get("records")
    if not isinstance(records, list):
        raise SignalLabError("decoded trace has no record list")
    projection: list[SemanticRecord] = []
    for record in records:
        event = record.get("event")
        tick = record.get("tick")
        task = record.get("task")
        related = record.get("related")
        values = (record.get("value0"), record.get("value1"),
                  record.get("value2"))
        if not isinstance(event, str) or not isinstance(tick, int):
            raise SignalLabError("trace record has an invalid event or tick")
        if task is not None and not isinstance(task, int):
            raise SignalLabError("trace record has an invalid task")
        if related is not None and not isinstance(related, int):
            raise SignalLabError("trace record has an invalid related task")
        if any(not isinstance(value, int) for value in values):
            raise SignalLabError("trace record has an invalid semantic value")
        value0, value1, value2 = values
        projection.append(
            (event, task, related, tick, value0, value1, value2)
        )
    return tuple(projection)


def validate_schedule(decoded: dict[str, Any]) -> None:
    footer = decoded.get("footer")
    if not isinstance(footer, dict):
        raise SignalLabError("decoded trace has no footer")
    _expect(
        {key: footer.get(key) for key in (
            "attempted", "emitted", "dropped", "final_sequence", "flags",
            "final_tick",
        )},
        {
            "attempted": EXPECTED_RECORD_COUNT,
            "emitted": EXPECTED_RECORD_COUNT,
            "dropped": 0,
            "final_sequence": EXPECTED_RECORD_COUNT - 1,
            "flags": 0,
            "final_tick": FINAL_TICK,
        },
        "footer",
    )
    record_count = decoded.get("record_count")
    _expect(record_count, EXPECTED_RECORD_COUNT, "record count")
    if EXPECTED_RECORD_COUNT > TRACE_CAPACITY:
        raise SignalLabError("expected trace exceeds the static ring capacity")
    _expect(semantic_projection(decoded), EXPECTED_SEMANTIC_PROJECTION,
            "semantic record projection")


def validate_capture(path: Path, implementation: str) -> tuple[dict[str, Any], bytes, dict[str, Any]]:
    data = read_bounded(path)
    if data.count(TRAILER) != 1:
        raise SignalLabError("UART capture must contain one trace trailer")
    trace_end = data.index(TRAILER) + len(TRAILER)
    decoded, raw_records = decode(data[:trace_end])
    result = parse_result(data[trace_end:], implementation)
    validate_schedule(decoded)
    return decoded, raw_records, result


def _read_execution_trace(path: Path) -> tuple[bytes, int]:
    with path.open("rb") as raw:
        before = os.fstat(raw.fileno())
        if not stat.S_ISREG(before.st_mode):
            raise SignalLabError(f"execution trace is not a regular file: {path}")
        if before.st_size > MAX_EXECUTION_COMPRESSED_BYTES:
            raise SignalLabError(
                f"compressed execution trace is {before.st_size} bytes; "
                f"limit is {MAX_EXECUTION_COMPRESSED_BYTES}"
            )
        output = bytearray()
        try:
            with gzip.GzipFile(fileobj=raw, mode="rb") as source:
                while True:
                    block = source.read(65536)
                    if not block:
                        break
                    output.extend(block)
                    if len(output) > MAX_EXECUTION_BYTES:
                        raise SignalLabError(
                            f"execution trace exceeds {MAX_EXECUTION_BYTES} bytes"
                        )
        except (EOFError, gzip.BadGzipFile, zlib.error) as error:
            raise SignalLabError(f"invalid gzip execution trace: {error}") from error
        after = os.fstat(raw.fileno())
    if (before.st_dev, before.st_ino, before.st_size) != (
        after.st_dev, after.st_ino, after.st_size
    ):
        raise SignalLabError("execution trace changed while it was read")
    return bytes(output), before.st_size


def _tool_output(command: list[str]) -> str:
    result = subprocess.run(
        command, cwd=REPOSITORY_ROOT, check=False,
        capture_output=True, text=True, timeout=10,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise SignalLabError(f"tool failed ({' '.join(command)}): {detail}")
    return result.stdout


def _function_symbol(objdump: Path, elf: Path, name: str) -> tuple[int, int] | None:
    symbols = _tool_output([str(objdump), "--syms", str(elf)])
    matches: list[tuple[int, int]] = []
    for line in symbols.splitlines():
        fields = line.split()
        if len(fields) >= 6 and fields[-1] == name and "F" in fields[1:-2]:
            matches.append((int(fields[0], 16), int(fields[-2], 16)))
    if len(matches) > 1:
        raise SignalLabError(f"ELF contains multiple {name} function symbols")
    return matches[0] if matches else None


def _function_instructions(
    objdump: Path, elf: Path, name: str, start: int, size: int
) -> list[dict[str, Any]]:
    output = _tool_output(
        [str(objdump), "--disassemble=" + name, str(elf)]
    )
    instructions: list[dict[str, Any]] = []
    for line in output.splitlines():
        fields = line.split("\t")
        if len(fields) < 3 or not fields[0].strip().endswith(":"):
            continue
        address_text = fields[0].strip()[:-1]
        raw_words = fields[1].strip().split()
        mnemonic_parts = fields[2].strip().split()
        if not re.fullmatch(r"[0-9a-fA-F]+", address_text) or not raw_words or not mnemonic_parts:
            continue
        if any(re.fullmatch(r"[0-9a-fA-F]{4}", word) is None for word in raw_words):
            continue
        address = int(address_text, 16)
        if address < start or address >= start + size:
            raise SignalLabError(f"{name} disassembly escaped its symbol range")
        joined = "".join(raw_words)
        opcode = int(joined, 16).to_bytes(len(raw_words) * 2, "little")
        instructions.append({
            "address": address,
            "opcode": opcode,
            "mnemonic": mnemonic_parts[0].lower(),
            "line": line.strip(),
        })
    if not instructions or instructions[0]["address"] != start:
        raise SignalLabError(f"could not decode exact {name} function")
    return instructions


def _parse_execution_entries(
    data: bytes, watches: dict[int, bytes], function_range: tuple[int, int]
) -> tuple[int, Counter[int], int]:
    if not data.startswith(b"ReTrace\x04"):
        raise SignalLabError("execution trace is not ReTrace version 4")
    if len(data) < 12:
        raise SignalLabError("execution trace header is truncated")
    pc_width = data[8]
    has_opcodes = data[9]
    uses_multiple = data[10]
    name_length = data[11]
    offset = 12 + name_length
    if offset > len(data):
        raise SignalLabError("execution trace target name is truncated")
    target = data[12:offset]
    if (pc_width, has_opcodes, uses_multiple, target) != (
        4, 1, 0, b"thumb cortex-m4"
    ):
        raise SignalLabError(
            "execution trace target must be single-ISA thumb cortex-m4 PCAndOpcode"
        )

    entry_count = 0
    watch_hits: Counter[int] = Counter()
    function_hits = 0
    function_start, function_size = function_range
    while offset < len(data):
        if entry_count >= MAX_EXECUTION_ENTRIES:
            raise SignalLabError(
                f"execution entry count exceeds {MAX_EXECUTION_ENTRIES}"
            )
        if len(data) - offset < 6:
            raise SignalLabError("execution trace entry is truncated")
        pc = int.from_bytes(data[offset:offset + 4], "little")
        opcode_length = data[offset + 4]
        offset += 5
        if opcode_length not in (2, 4) or len(data) - offset < opcode_length + 1:
            raise SignalLabError("execution trace has an invalid Thumb opcode length")
        opcode = data[offset:offset + opcode_length]
        offset += opcode_length
        additional_type = data[offset]
        offset += 1
        if additional_type != 0:
            raise SignalLabError("execution trace has unexpected additional data")
        expected_opcode = watches.get(pc)
        if expected_opcode is not None:
            if opcode != expected_opcode:
                raise SignalLabError(
                    f"executed opcode mismatch at 0x{pc:08x}: "
                    f"got {opcode.hex()}, expected {expected_opcode.hex()}"
                )
            watch_hits[pc] += 1
        if function_start <= pc < function_start + function_size:
            function_hits += 1
        entry_count += 1
    return entry_count, watch_hits, function_hits


def analyze_execution_trace(
    path: Path, elf: Path, implementation: str, objdump: Path
) -> dict[str, Any]:
    selected = "aymos_fir_q15_m4" if implementation == "m4" else "aymos_fir_q15_scalar"
    absent = "aymos_fir_q15_scalar" if implementation == "m4" else "aymos_fir_q15_m4"
    symbol = _function_symbol(objdump, elf, selected)
    if symbol is None:
        raise SignalLabError(f"ELF does not contain selected function {selected}")
    if _function_symbol(objdump, elf, absent) is not None:
        raise SignalLabError(f"ELF unexpectedly contains unselected function {absent}")
    start, size = symbol
    instructions = _function_instructions(objdump, elf, selected, start, size)
    forbidden = [
        item["mnemonic"] for item in instructions
        if item["mnemonic"].startswith("v") or "memcpy" in item["line"].lower()
    ]
    if forbidden:
        raise SignalLabError(f"selected FIR contains forbidden instructions: {forbidden}")

    required: dict[str, dict[str, Any]] = {}
    if implementation == "m4":
        for mnemonic, expected_count in (
            ("smlald", WORK_UNITS // 2),
            ("ssat", TOTAL_OUTPUT_COUNT),
        ):
            matches = [item for item in instructions if item["mnemonic"] == mnemonic]
            if len(matches) != 1:
                raise SignalLabError(
                    f"{selected} must contain one {mnemonic}; found {len(matches)}"
                )
            required[mnemonic] = {
                "address": matches[0]["address"],
                "opcode": matches[0]["opcode"],
                "expected_count": expected_count,
            }
    else:
        if any(item["mnemonic"] in ("smlald", "ssat") for item in instructions):
            raise SignalLabError(
                "scalar FIR contains a packed SMLALD/SSAT instruction"
            )
        matches = [
            item for item in instructions if item["mnemonic"] == "smlalbb"
        ]
        if len(matches) != 1:
            raise SignalLabError(
                f"{selected} must contain one smlalbb; found {len(matches)}"
            )
        required["smlalbb"] = {
            "address": matches[0]["address"],
            "opcode": matches[0]["opcode"],
            "expected_count": WORK_UNITS,
        }

    watches = {start: instructions[0]["opcode"]}
    watches.update({item["address"]: item["opcode"] for item in required.values()})
    data, compressed_size = _read_execution_trace(path)
    entry_count, hits, function_hits = _parse_execution_entries(
        data, watches, symbol
    )
    _expect(hits[start], FRAME_COUNT, f"{selected} call count")
    dynamic: dict[str, Any] = {}
    for mnemonic, item in required.items():
        count = hits[item["address"]]
        _expect(count, item["expected_count"], f"executed {mnemonic} count")
        dynamic[mnemonic] = {
            "address": f"0x{item['address']:08x}",
            "opcode_hex": item["opcode"].hex(),
            "executed_count": count,
        }
    return {
        "schema_version": 1,
        "claim": "functional executed-instruction evidence; not a timing measurement",
        "format": "ReTrace-v4-PCAndOpcode",
        "target": "thumb cortex-m4",
        "compressed_bytes": compressed_size,
        "uncompressed_bytes": len(data),
        "entry_count": entry_count,
        "selected_function": selected,
        "function_address": f"0x{start:08x}",
        "function_size_bytes": size,
        "function_call_count": hits[start],
        "function_instruction_hits": function_hits,
        "dynamic_instructions": dynamic,
        "timing_claims": "none",
    }


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(65536), b""):
            digest.update(block)
    return digest.hexdigest()


def _read_metadata(path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except UnicodeError as error:
        raise SignalLabError(f"metadata is not UTF-8: {path}") from error
    for line in lines:
        if "=" not in line:
            raise SignalLabError(f"invalid metadata line in {path}: {line!r}")
        key, value = line.split("=", 1)
        if not key or key in result:
            raise SignalLabError(f"invalid or duplicate metadata key in {path}: {key!r}")
        result[key] = value
    return result


def _expect_metadata(
    actual: dict[str, str], expected: dict[str, str], label: str,
) -> None:
    for key, value in expected.items():
        if key not in actual:
            raise SignalLabError(f"Signal Lab {label} is missing {key}")
        _expect(actual[key], value, f"{label} {key}")


def _read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise SignalLabError(f"invalid JSON evidence in {path}: {error}") from error
    if not isinstance(value, dict):
        raise SignalLabError(f"JSON evidence is not an object: {path}")
    return value


def _write_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _artifact_manifest(root: Path, names: tuple[str, ...]) -> dict[str, Any]:
    manifest: dict[str, Any] = {}
    for name in names:
        path = root / name
        if not path.is_file() or path.is_symlink():
            raise SignalLabError(f"missing regular evidence artifact: {path}")
        manifest[name] = {
            "bytes": path.stat().st_size,
            "sha256": _sha256(path),
        }
    return manifest


def signal_environment(
    implementation: str, output_dir: Path, firmware_elf: Path | None = None,
) -> dict[str, str]:
    if implementation not in IMPLEMENTATIONS:
        raise SignalLabError(f"unsupported Signal Lab implementation: {implementation}")
    environment = dict(os.environ)
    environment.update({
        "AYMOS_APP": "signal_lab",
        "AYMOS_SIGNAL_IMPL": implementation,
        "AYMOS_RENODE_HOST_TIMEOUT": RENODE_HOST_TIMEOUT_SECONDS,
        "AYMOS_RENODE_VIRTUAL_DURATION": RENODE_VIRTUAL_DURATION_SECONDS,
        "AYMOS_RENODE_OUTPUT_DIR": str(output_dir),
    })
    if firmware_elf is not None:
        environment["AYMOS_FIRMWARE_ELF"] = str(firmware_elf)
    return environment


def _run_process_group(command: list[str], environment: dict[str, str]) -> None:
    process = subprocess.Popen(
        command, cwd=REPOSITORY_ROOT, env=environment, start_new_session=True
    )
    try:
        status = process.wait(timeout=OUTER_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
        raise
    if status != 0:
        raise subprocess.CalledProcessError(status, command)


def _copy_stable_file(source: Path, destination: Path) -> dict[str, Any]:
    before = source.lstat()
    if not stat.S_ISREG(before.st_mode) or source.is_symlink():
        raise SignalLabError(f"build input is not a regular file: {source}")
    before_hash = _sha256(source)
    shutil.copy2(source, destination)
    after = source.lstat()
    after_hash = _sha256(source)
    identity = (
        before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns,
        before.st_ctime_ns,
    )
    after_identity = (
        after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns,
        after.st_ctime_ns,
    )
    if identity != after_identity or before_hash != after_hash:
        raise SignalLabError(f"build input changed while copied: {source}")
    _expect(_sha256(destination), before_hash, f"snapshot hash for {source.name}")
    return {
        "source": source,
        "identity": identity,
        "sha256": before_hash,
    }


def _snapshot_build_inputs(
    root: Path, implementation: str,
) -> tuple[Path, dict[str, dict[str, Any]]]:
    firmware_dir = (
        REPOSITORY_ROOT / "build/nucleo_f401re/signal_lab" / implementation
    )
    sources = {
        "firmware.elf": firmware_dir / "aymos.elf",
        "firmware.map": firmware_dir / "aymos.map",
        "build-metadata.txt": firmware_dir / "build-metadata.txt",
    }
    snapshot_dir = root / f".{implementation}-inputs"
    snapshot_dir.mkdir()
    snapshots = {
        name: _copy_stable_file(source, snapshot_dir / name)
        for name, source in sources.items()
    }
    return snapshot_dir, snapshots


def _assert_build_inputs_unchanged(
    snapshots: dict[str, dict[str, Any]],
) -> None:
    for name, snapshot in snapshots.items():
        source = snapshot["source"]
        current = source.lstat()
        identity = (
            current.st_dev, current.st_ino, current.st_size,
            current.st_mtime_ns, current.st_ctime_ns,
        )
        if (not stat.S_ISREG(current.st_mode) or source.is_symlink()
                or identity != snapshot["identity"]
                or _sha256(source) != snapshot["sha256"]):
            raise SignalLabError(
                f"live {name} changed after the evidence snapshot"
            )


def _validate_marked_evidence(path: Path, label: str) -> None:
    expected_parent = (REPOSITORY_ROOT / "build/signal-lab").resolve()
    if path.is_symlink() or path.parent.resolve() != expected_parent:
        raise SignalLabError(f"unsafe {label} path: {path}")
    if not path.is_dir():
        raise SignalLabError(f"{label} is not a directory: {path}")
    marker = path / EVIDENCE_MARKER
    if not marker.is_file() or marker.is_symlink():
        raise SignalLabError(f"{label} has no regular marker: {path}")


def _remove_marked_evidence(path: Path, label: str) -> None:
    _validate_marked_evidence(path, label)
    shutil.rmtree(path)


def _recover_evidence_backup(destination: Path) -> None:
    parent = REPOSITORY_ROOT / "build/signal-lab"
    backup = parent / ".evidence-backup"
    if not backup.exists() and not backup.is_symlink():
        return
    _validate_marked_evidence(backup, "evidence backup")
    if destination.exists() or destination.is_symlink():
        _validate_marked_evidence(destination, "published evidence")
        _remove_marked_evidence(backup, "evidence backup")
    else:
        os.replace(backup, destination)


def _replace_evidence(stage: Path, destination: Path) -> None:
    parent = REPOSITORY_ROOT / "build/signal-lab"
    backup = parent / ".evidence-backup"
    _validate_marked_evidence(stage, "evidence stage")
    _recover_evidence_backup(destination)
    if destination.is_symlink():
        raise SignalLabError(f"unsafe published evidence path: {destination}")
    had_previous = destination.exists()
    if had_previous:
        _validate_marked_evidence(destination, "published evidence")
        os.replace(destination, backup)
    try:
        os.replace(stage, destination)
        _validate_marked_evidence(destination, "published evidence")
    except BaseException:
        if (destination.exists() and not stage.exists()
                and not destination.is_symlink()):
            os.replace(destination, stage)
        if had_previous and not destination.exists() and backup.exists():
            os.replace(backup, destination)
        raise
    if had_previous:
        _remove_marked_evidence(backup, "evidence backup")


def _current_git_commit() -> str:
    commit = _tool_output(["git", "rev-parse", "HEAD"]).strip()
    if re.fullmatch(r"[0-9a-f]{40}", commit) is None:
        raise SignalLabError(f"unexpected git commit identifier: {commit!r}")
    return commit


def _run_implementation(
    root: Path, implementation: str, objdump: Path, expected_commit: str,
) -> dict[str, Any]:
    output_dir = root / implementation
    _expect(_current_git_commit(), expected_commit, "git commit before snapshot")
    snapshot_dir, snapshots = _snapshot_build_inputs(root, implementation)
    elf = snapshot_dir / "firmware.elf"
    firmware_map = snapshot_dir / "firmware.map"
    build_metadata = snapshot_dir / "build-metadata.txt"
    build_values = _read_metadata(build_metadata)
    _expect_metadata(build_values, {
        "project": "aymos",
        "board": "nucleo_f401re",
        "app": "signal_lab",
        "git_commit": expected_commit,
        "dependencies_verified": "true",
        "dependencies_clean": "true",
        "compiler": EXPECTED_TOOLCHAIN_VERSION,
        "architecture_flags": EXPECTED_ARCHITECTURE_FLAGS,
        "float_abi": "soft",
        "dsp_optimization": "-O2",
        "signal_impl": implementation,
        "trace_schema_version": "1",
        "trace_framing_version": "1",
        "trace_record_size": "32",
        "trace_footer_size": "28",
        "trace_ring_records": str(TRACE_CAPACITY),
    }, "build metadata")

    _run_process_group(
        [str(REPOSITORY_ROOT / "tools/renode/run.sh")],
        signal_environment(implementation, output_dir, elf),
    )
    for directory in (output_dir / "home", output_dir / "xdg"):
        shutil.rmtree(directory)

    metadata = _read_metadata(output_dir / "metadata.txt")
    _expect_metadata(metadata, {
        "schema": "1",
        "board": "nucleo_f401re",
        "app": "signal_lab",
        "workload_mode": "none",
        "signal_impl": implementation,
        "git_commit": expected_commit,
        "firmware": str(elf),
        "firmware_sha256": snapshots["firmware.elf"]["sha256"],
        "architecture_flags": EXPECTED_ARCHITECTURE_FLAGS,
        "float_abi": "soft",
        "renode_version": "1.16.1",
        "python_version": "3.12.13",
        "trace_schema_version": "1",
        "trace_framing_version": "1",
        "trace_record_size": "32",
        "trace_footer_size": "28",
        "trace_ring_records": str(TRACE_CAPACITY),
        "execution_trace_format": "ReTrace-v4-PCAndOpcode",
        "execution_trace_compression": "gzip",
        "execution_trace_synchronous": "true",
        "host_timeout_seconds": RENODE_HOST_TIMEOUT_SECONDS,
        "virtual_duration_seconds": RENODE_VIRTUAL_DURATION_SECONDS,
        "timing_claims": "none_emulator_functional_test_only",
    }, "run metadata")
    if metadata.get("repository_clean") not in ("true", "false"):
        raise SignalLabError("run metadata repository_clean is invalid")
    _expect(metadata["git_commit"], build_values["git_commit"],
            "run/build git commit")

    execution = analyze_execution_trace(
        output_dir / "execution.bin.gz", elf, implementation, objdump
    )
    _assert_build_inputs_unchanged(snapshots)
    _expect(_current_git_commit(), expected_commit, "git commit after run")
    _write_json(output_dir / "execution-summary.json", execution)
    os.replace(elf, output_dir / "firmware.elf")
    os.replace(firmware_map, output_dir / "firmware.map")
    os.replace(build_metadata, output_dir / "build-metadata.txt")
    snapshot_dir.rmdir()
    decoded, _, uart_result = validate_capture(
        output_dir / "uart.bin", implementation
    )
    result = _read_json(output_dir / "result.json")
    trace = _read_json(output_dir / "trace.json")
    _expect(result, uart_result, "stored result JSON")
    _expect(trace, decoded, "stored trace JSON")
    _expect(trace.get("record_count"), EXPECTED_RECORD_COUNT,
            "stored trace record count")
    artifacts = _artifact_manifest(output_dir, (
        "build-metadata.txt",
        "command.txt",
        "emulator.log",
        "execution-summary.json",
        "execution.bin.gz",
        "firmware.elf",
        "firmware.map",
        "metadata.txt",
        "result.json",
        "trace.bin",
        "trace.json",
        "uart-validation.log",
        "uart.bin",
        "uart.txt",
    ))
    return {
        "implementation": implementation,
        "git_commit": metadata["git_commit"],
        "repository_clean": metadata["repository_clean"] == "true",
        "renode_version": metadata["renode_version"],
        "toolchain_version": build_values["compiler"],
        "build_configuration": {
            "app": "signal_lab",
            "board": "nucleo_f401re",
            "float_abi": "soft",
            "signal_impl": implementation,
        },
        "execution_limits": {
            "compressed_trace_bytes": MAX_EXECUTION_COMPRESSED_BYTES,
            "execution_entries": MAX_EXECUTION_ENTRIES,
            "host_timeout_seconds": int(RENODE_HOST_TIMEOUT_SECONDS),
            "outer_timeout_seconds": OUTER_TIMEOUT_SECONDS,
            "uncompressed_trace_bytes": MAX_EXECUTION_BYTES,
            "virtual_duration_seconds": RENODE_VIRTUAL_DURATION_SECONDS,
        },
        "artifacts": artifacts,
        "record_count": trace.get("record_count"),
        "result": result,
        "execution": execution,
    }


def run_test() -> Path:
    base = REPOSITORY_ROOT / "build/signal-lab"
    if base.is_symlink():
        raise SignalLabError(f"build/signal-lab must not be a symlink: {base}")
    base.mkdir(parents=True, exist_ok=True)
    lock = base / ".evidence.lock"
    try:
        lock.mkdir()
    except FileExistsError as error:
        raise SignalLabError("another Signal Lab evidence run is active") from error
    stage = base / f".evidence-stage-{os.getpid()}"
    try:
        _recover_evidence_backup(EVIDENCE_ROOT)
        stage.mkdir()
        (stage / EVIDENCE_MARKER).write_text("schema=1\n", encoding="ascii")
        expected_commit = _current_git_commit()
        objdump = (
            REPOSITORY_ROOT /
            ".tools/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-objdump"
        )
        runs = [
            _run_implementation(stage, implementation, objdump,
                                expected_commit)
            for implementation in IMPLEMENTATIONS
        ]
        _expect(tuple(run["git_commit"] for run in runs),
                (expected_commit, expected_commit), "variant git commits")
        scalar_result = dict(runs[0]["result"])
        m4_result = dict(runs[1]["result"])
        scalar_result.pop("implementation")
        m4_result.pop("implementation")
        _expect(m4_result, scalar_result, "scalar/M4 result equality")
        _expect(
            runs[1]["artifacts"]["trace.bin"]["sha256"],
            runs[0]["artifacts"]["trace.bin"]["sha256"],
            "scalar/M4 structured trace equality",
        )
        summary = {
            "schema_version": 1,
            "board": "nucleo_f401re",
            "trace_schema_version": 1,
            "random_seed": f"0x{SEED:08X}",
            "timing_claims": "none_emulator_functional_test_only",
            "runs": runs,
            "result_equality": True,
            "structured_trace_equality": True,
        }
        _write_json(stage / "summary.json", summary)
        _replace_evidence(stage, EVIDENCE_ROOT)
    except BaseException:
        if stage.exists() and not stage.is_symlink():
            _remove_marked_evidence(stage, "failed evidence stage")
        raise
    finally:
        lock.rmdir()
    print(f"AYMOS SIGNAL LAB PASS artifacts={EVIDENCE_ROOT}")
    return EVIDENCE_ROOT


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("test")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        if args.command == "test":
            run_test()
    except (OSError, SignalLabError, subprocess.SubprocessError) as error:
        print(f"signal-lab: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
