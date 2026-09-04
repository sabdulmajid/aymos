"""Strict bounded decoder for the AymOS trace transport, schema version 1."""

from __future__ import annotations

import argparse
import binascii
import json
import os
from pathlib import Path
import stat
import struct
import sys
from typing import Any

MAGIC = b"AYMT"
PREAMBLE = b"AYMOS READY\r\nTRACE BEGIN\r\n"
TRAILER = b"AYMOS TRACE DONE\r\n"
FRAME_VERSION = 1
SCHEMA_VERSION = 1
FRAME_RECORD = 1
FRAME_FOOTER = 2
HEADER = struct.Struct("<4sBBHHH")
RECORD = struct.Struct("<QIHBBHHIII")
FOOTER = struct.Struct("<IIIIIQ")
CRC = struct.Struct("<I")
MAX_INPUT_BYTES = 1024 * 1024
MAX_FRAMES = 4096
MAX_RESYNC_SCAN_BYTES = 4096
MAX_TASK_ID = 4
INVALID_TASK_ID = 0xFFFF

EVENT_NAMES = {
    1: "kernel_start",
    2: "task_create",
    3: "task_release",
    4: "task_select",
    5: "select_candidate",
    6: "task_start",
    7: "task_preempt",
    8: "task_yield",
    9: "task_sleep",
    10: "task_wake",
    11: "task_exit",
    12: "context_switch",
    13: "idle_start",
    14: "idle_stop",
    15: "deadline_met",
    16: "deadline_miss",
    17: "alloc",
    18: "free",
    19: "task_wait_period",
    20: "task_release_skipped",
    21: "owner_release",
    22: "trace_overflow",
}
SELECT_REASONS = {
    1: "only_ready",
    2: "deadline",
    3: "priority",
    4: "stable_tid",
    5: "idle",
    6: "excluded_fallback",
}


class TraceError(ValueError):
    """A byte stream does not satisfy the complete trace contract."""


def _fail(message: str, *, offset: int, frame: int | None = None,
          sequence: int | None = None, data: bytes | None = None) -> TraceError:
    where = f"offset {offset}"
    if frame is not None:
        where += f", frame {frame}"
    if sequence is not None:
        where += f", sequence {sequence}"
    if data is not None:
        end = min(len(data), offset + 1 + MAX_RESYNC_SCAN_BYTES)
        next_sync = data.find(MAGIC, offset + 1, end)
        if next_sync >= 0:
            message += f"; bounded diagnostic found next sync at offset {next_sync}"
        else:
            message += f"; no sync in next {end - offset - 1} bytes"
    return TraceError(f"{where}: {message}")


def _task_valid(task: int) -> bool:
    return task == INVALID_TASK_ID or 0 <= task <= MAX_TASK_ID


def _deadline(value0: int, value1: int) -> int:
    return value0 | (value1 << 32)


def read_bounded(path: Path) -> bytes:
    """Read one regular file without requesting more than the limit plus one."""
    with path.open("rb") as source:
        before = os.fstat(source.fileno())
        if not stat.S_ISREG(before.st_mode):
            raise TraceError(f"input is not a regular file: {path}")
        if before.st_size > MAX_INPUT_BYTES:
            raise TraceError(
                f"input is {before.st_size} bytes; limit is {MAX_INPUT_BYTES}")
        data = source.read(MAX_INPUT_BYTES + 1)
        after = os.fstat(source.fileno())
    if len(data) > MAX_INPUT_BYTES:
        raise TraceError(
            f"input exceeds {MAX_INPUT_BYTES} bytes while reading")
    if (before.st_dev, before.st_ino) != (after.st_dev, after.st_ino) or \
            before.st_size != after.st_size or len(data) != after.st_size:
        raise TraceError("input changed size while being read")
    return data


def _details(event: int, task: int, related: int, values: tuple[int, int, int]) -> dict[str, Any]:
    value0, value1, value2 = values
    if event in (3, 5, 6, 10, 15, 16):
        details: dict[str, Any] = {
            "absolute_deadline": _deadline(value0, value1),
            "job_sequence": value2,
        }
        if event == 5:
            details["priority"] = value2 & 0xFF
            details["state"] = (value2 >> 8) & 0xFF
            details["excluded"] = bool(value2 & (1 << 16))
            details["incumbent"] = bool(value2 & (1 << 17))
            details["eligible"] = bool(value2 & (1 << 18))
            details.pop("job_sequence")
        return details
    if event == 4:
        return {
            "ready_mask": value0,
            "reason": SELECT_REASONS.get(value1 & 0xFF,
                                         f"unknown:{value1 & 0xFF}"),
            "purpose": {1: "dispatch", 2: "systick_probe",
                        3: "runtime_create_probe"}.get((value1 >> 8) & 0xFF,
                                                       "unknown"),
            "excluded": None if ((value1 >> 16) & 0xFF) == 0xFF
                        else ((value1 >> 16) & 0xFF),
            "eligible_count": (value1 >> 24) & 0xFF,
            "candidate_count": value2,
        }
    if event == 2:
        return {"kind": value0, "priority": value1,
                "initial_state": value2}
    if event == 9:
        return {"wake_tick": _deadline(value0, value1),
                "sleep_ticks": value2}
    if event == 19:
        return {"next_release_tick": _deadline(value0, value1)}
    if event == 20:
        return {"release_tick": _deadline(value0, value1),
                "missed_release_count": value2}
    if event == 21:
        return {"released_blocks": value0, "owner": value1,
                "success": bool(value2)}
    if event == 12:
        return {"cause": {1: "start", 2: "yield", 3: "sleep", 4: "exit",
                          5: "wait_period", 6: "systick_preempt",
                          7: "runtime_create",
                          8: "coalesced_preempt"}.get(value0, "unknown"),
                "outgoing_state": value1}
    if event == 7:
        return {"source": {1: "systick", 2: "runtime_create",
                           3: "coalesced"}.get(value0, "unknown")}
    if event == 17:
        return {"requested_bytes": value0, "pointer_offset": value1,
                "success": bool(value2)}
    if event == 18:
        return {"pointer_offset": value0, "subtype": "explicit",
                "success": bool(value2)}
    if event == 22:
        return {"dropped": value0, "attempted": value1,
                "emitted": value2}
    return {"value0": value0, "value1": value1, "value2": value2,
            "task": task, "related": related}


def decode(data: bytes) -> tuple[dict[str, Any], bytes]:
    if len(data) > MAX_INPUT_BYTES:
        raise TraceError(f"input is {len(data)} bytes; limit is {MAX_INPUT_BYTES}")
    if data.startswith(PREAMBLE):
        offset = len(PREAMBLE)
        require_trailer = True
    elif data.startswith(MAGIC):
        offset = 0
        require_trailer = False
    else:
        raise _fail("missing exact trace preamble or frame sync", offset=0,
                    data=data)

    records: list[dict[str, Any]] = []
    footer: dict[str, int] | None = None
    expected_sequence = 0
    previous_tick = 0
    frame_index = 0
    footer_end = 0
    raw_records = bytearray()

    while offset < len(data):
        if require_trailer and data.startswith(TRAILER, offset):
            break
        if frame_index >= MAX_FRAMES:
            raise _fail(f"frame count exceeds {MAX_FRAMES}", offset=offset,
                        frame=frame_index)
        if len(data) - offset < HEADER.size:
            raise _fail("truncated frame header", offset=offset,
                        frame=frame_index)
        magic, framing, frame_type, schema, length, flags = HEADER.unpack_from(data, offset)
        if magic != MAGIC:
            raise _fail("invalid frame sync or trailing garbage", offset=offset,
                        frame=frame_index, data=data)
        if framing != FRAME_VERSION:
            raise _fail(f"unknown framing version {framing}", offset=offset,
                        frame=frame_index)
        if schema != SCHEMA_VERSION:
            raise _fail(f"unknown schema version {schema}", offset=offset,
                        frame=frame_index)
        if frame_type not in (FRAME_RECORD, FRAME_FOOTER):
            raise _fail(f"unknown frame type {frame_type}", offset=offset,
                        frame=frame_index)
        expected_length = RECORD.size if frame_type == FRAME_RECORD else FOOTER.size
        if length != expected_length:
            raise _fail(f"invalid payload length {length}; expected {expected_length}",
                        offset=offset, frame=frame_index)
        if flags != 0:
            raise _fail(f"unknown frame flags 0x{flags:x}", offset=offset,
                        frame=frame_index)
        frame_size = HEADER.size + length + CRC.size
        if frame_size > len(data) - offset:
            raise _fail("truncated frame payload or CRC", offset=offset,
                        frame=frame_index)
        payload_start = offset + HEADER.size
        payload_end = payload_start + length
        expected_crc = CRC.unpack_from(data, payload_end)[0]
        actual_crc = binascii.crc32(data[offset + 4:payload_end]) & 0xFFFFFFFF
        if actual_crc != expected_crc:
            raise _fail(f"CRC32 mismatch: got 0x{expected_crc:08x}, expected 0x{actual_crc:08x}",
                        offset=offset, frame=frame_index, data=data)

        if frame_type == FRAME_FOOTER:
            if footer is not None:
                raise _fail("duplicate footer", offset=offset,
                            frame=frame_index)
            (attempted, emitted, dropped, final_sequence, final_flags,
             final_tick) = FOOTER.unpack_from(data, payload_start)
            footer = {"attempted": attempted, "emitted": emitted,
                      "dropped": dropped, "final_sequence": final_sequence,
                      "flags": final_flags, "final_tick": final_tick}
            footer_end = offset + frame_size
            offset = footer_end
            frame_index += 1
            break

        if footer is not None:
            raise _fail("record appears after footer", offset=offset,
                        frame=frame_index)
        (tick, sequence, record_schema, event, record_flags, task, related,
         value0, value1, value2) = RECORD.unpack_from(data, payload_start)
        if record_schema != SCHEMA_VERSION:
            raise _fail(f"record schema {record_schema} does not match frame",
                        offset=offset, frame=frame_index, sequence=sequence)
        if event not in EVENT_NAMES:
            raise _fail(f"unknown event {event}", offset=offset,
                        frame=frame_index, sequence=sequence)
        if record_flags != 0:
            raise _fail(f"unknown record flags 0x{record_flags:x}", offset=offset,
                        frame=frame_index, sequence=sequence)
        if not _task_valid(task) or not _task_valid(related):
            raise _fail(f"impossible task IDs task={task} related={related}",
                        offset=offset, frame=frame_index, sequence=sequence)
        if sequence != expected_sequence:
            raise _fail(f"sequence gap: expected {expected_sequence}, got {sequence}",
                        offset=offset, frame=frame_index, sequence=sequence)
        if records and tick < previous_tick:
            raise _fail(f"nonmonotonic tick {tick} after {previous_tick}",
                        offset=offset, frame=frame_index, sequence=sequence)
        record = {
            "tick": tick, "sequence": sequence,
            "event": EVENT_NAMES[event], "event_id": event,
            "task": None if task == INVALID_TASK_ID else task,
            "related": None if related == INVALID_TASK_ID else related,
            "value0": value0, "value1": value1, "value2": value2,
            "details": _details(event, task, related, (value0, value1, value2)),
            "raw_hex": data[payload_start:payload_end].hex(),
        }
        records.append(record)
        raw_records.extend(data[payload_start:payload_end])
        expected_sequence += 1
        previous_tick = tick
        offset += frame_size
        frame_index += 1

    if footer is None:
        raise _fail("missing terminal footer", offset=offset, frame=frame_index)
    if footer["flags"] != 0:
        raise _fail(f"footer reports unsupported/incomplete flags 0x{footer['flags']:x}",
                    offset=footer_end - FOOTER.size - CRC.size,
                    frame=frame_index - 1)
    if footer["emitted"] != len(records):
        raise _fail(f"footer emitted={footer['emitted']} but decoded {len(records)} records",
                    offset=footer_end - FOOTER.size - CRC.size,
                    frame=frame_index - 1)
    if footer["attempted"] != footer["emitted"] + footer["dropped"]:
        raise _fail("footer attempted != emitted + dropped",
                    offset=footer_end - FOOTER.size - CRC.size,
                    frame=frame_index - 1)
    expected_final = 0xFFFFFFFF if footer["attempted"] == 0 else footer["attempted"] - 1
    if footer["final_sequence"] != expected_final:
        raise _fail(f"footer final sequence {footer['final_sequence']} != {expected_final}",
                    offset=footer_end - FOOTER.size - CRC.size,
                    frame=frame_index - 1)
    if footer["dropped"] != 0:
        raise _fail(f"complete run dropped {footer['dropped']} records",
                    offset=footer_end - FOOTER.size - CRC.size,
                    frame=frame_index - 1)
    if records and footer["final_tick"] < records[-1]["tick"]:
        raise _fail("footer final tick precedes the last record",
                    offset=footer_end - FOOTER.size - CRC.size,
                    frame=frame_index - 1)
    if require_trailer:
        if data[offset:] != TRAILER:
            raise _fail("missing exact terminal line or trailing garbage",
                        offset=offset, frame=frame_index)
    elif offset != len(data):
        raise _fail("trailing garbage after footer", offset=offset,
                    frame=frame_index)

    _validate_selection_snapshots(records)
    _validate_event_roles(records)
    _validate_event_payloads(records)
    if any(record["event"] == "trace_overflow" for record in records):
        raise TraceError(
            "complete zero-loss trace contains a trace_overflow record")
    result = {
        "schema_version": SCHEMA_VERSION,
        "framing_version": FRAME_VERSION,
        "record_size": RECORD.size,
        "record_count": len(records),
        "footer": footer,
        "records": records,
    }
    return result, bytes(raw_records)


def _validate_selection_snapshots(records: list[dict[str, Any]]) -> None:
    index = 0
    while index < len(records):
        record = records[index]
        if record["event"] != "task_select":
            if record["event"] == "select_candidate":
                raise TraceError(f"sequence {record['sequence']}: orphan selection candidate")
            index += 1
            continue
        count = record["details"]["candidate_count"]
        if count > MAX_TASK_ID:
            raise TraceError(f"sequence {record['sequence']}: candidate count {count} exceeds {MAX_TASK_ID}")
        candidates = records[index + 1:index + 1 + count]
        if len(candidates) != count or any(item["event"] != "select_candidate" for item in candidates):
            raise TraceError(f"sequence {record['sequence']}: incomplete contiguous candidate snapshot")
        selected = record["task"]
        ready_mask = record["details"]["ready_mask"]
        if ready_mask & ~0x1E:
            raise TraceError(f"sequence {record['sequence']}: READY mask has reserved bits")
        seen: set[int] = set()
        for candidate in candidates:
            tid = candidate["task"]
            if tid is None or tid == 0 or tid in seen:
                raise TraceError(f"sequence {candidate['sequence']}: candidate is absent/duplicate")
            if candidate["related"] != selected or candidate["tick"] != record["tick"]:
                raise TraceError(f"sequence {candidate['sequence']}: candidate snapshot linkage mismatch")
            seen.add(tid)
        ready_candidates = {item["task"] for item in candidates
                            if item["details"]["state"] == 2}
        expected = {tid for tid in range(1, MAX_TASK_ID + 1)
                    if ready_mask & (1 << tid)}
        if ready_candidates != expected:
            raise TraceError(f"sequence {record['sequence']}: READY mask and candidate set differ")
        eligible = [item for item in candidates if item["details"]["eligible"]]
        if len(eligible) != record["details"]["eligible_count"]:
            raise TraceError(f"sequence {record['sequence']}: eligible count differs")
        excluded = record["details"]["excluded"]
        for item in candidates:
            if item["details"]["excluded"] != (item["task"] == excluded):
                raise TraceError(f"sequence {item['sequence']}: excluded marker differs")
        purpose = record["details"]["purpose"]
        if purpose == "unknown":
            raise TraceError(f"sequence {record['sequence']}: unknown selection purpose")
        incumbent = record["related"]
        incumbent_marked = [item for item in candidates
                            if item["details"]["incumbent"]]
        if purpose == "dispatch":
            if any(item["details"]["state"] != 2 for item in candidates):
                raise TraceError(f"sequence {record['sequence']}: dispatch candidate is not READY")
            expected_marked = ([] if incumbent not in seen else [incumbent])
            if [item["task"] for item in incumbent_marked] != expected_marked:
                raise TraceError(f"sequence {record['sequence']}: dispatch incumbent marker differs")
        else:
            if excluded is not None or incumbent is None or selected == incumbent:
                raise TraceError(
                    f"sequence {record['sequence']}: impossible probe selection")
            if incumbent == 0:
                if incumbent_marked or any(item["details"]["state"] != 2
                                           for item in candidates):
                    raise TraceError(
                        f"sequence {record['sequence']}: idle probe metadata differs")
            elif len(candidates) < 2 or len(incumbent_marked) != 1 or \
                    incumbent_marked[0]["task"] != incumbent or \
                    incumbent_marked[0]["details"]["state"] != 3 or \
                    any(item["details"]["state"] != 2
                        for item in candidates if item["task"] != incumbent):
                raise TraceError(
                    f"sequence {record['sequence']}: probe incumbent/state metadata differs")
        for item in candidates:
            if item["details"]["eligible"] != (item["task"] != excluded):
                raise TraceError(f"sequence {item['sequence']}: eligibility marker differs")
        reason = record["details"]["reason"]
        if selected == 0:
            if (candidates or eligible or ready_mask != 0 or
                    record["details"]["eligible_count"] != 0 or
                    excluded is not None or reason != "idle" or
                    purpose != "dispatch"):
                raise TraceError(f"sequence {record['sequence']}: invalid idle selection snapshot")
        else:
            if selected not in seen:
                raise TraceError(f"sequence {record['sequence']}: selected task absent from candidates")
            selectable = eligible
            if not selectable and selected == excluded:
                selectable = [item for item in candidates
                              if item["task"] == selected]
            ordered = sorted(selectable, key=lambda item: (
                item["details"]["absolute_deadline"],
                item["details"]["priority"], item["task"]))
            if not ordered or ordered[0]["task"] != selected:
                raise TraceError(f"sequence {record['sequence']}: selected task contradicts candidate keys")
            if len(ordered) == 1:
                expected_reason = ("excluded_fallback"
                                   if not eligible and selected == excluded
                                   else "only_ready")
            elif ordered[0]["details"]["absolute_deadline"] != ordered[1]["details"]["absolute_deadline"]:
                expected_reason = "deadline"
            elif ordered[0]["details"]["priority"] != ordered[1]["details"]["priority"]:
                expected_reason = "priority"
            else:
                expected_reason = "stable_tid"
            if reason != expected_reason:
                raise TraceError(f"sequence {record['sequence']}: tie reason {reason} != {expected_reason}")
        index += 1 + count


def _validate_event_roles(records: list[dict[str, Any]]) -> None:
    user = {1, 2, 3, 4}
    for record in records:
        event = record["event"]
        task = record["task"]
        related = record["related"]
        sequence = record["sequence"]
        valid = True
        if event == "kernel_start":
            valid = task is None and related is None
        elif event == "task_create":
            valid = task in user and (related is None or related in user)
        elif event == "task_release":
            valid = task in user and related is None
        elif event == "task_select":
            valid = task in (user | {0}) and (related is None or related in user | {0})
        elif event == "select_candidate":
            valid = task in user and related in user | {0}
        elif event in {"task_start", "task_yield", "task_sleep", "task_wake",
                       "task_exit", "deadline_met", "deadline_miss", "alloc",
                       "free", "task_wait_period"}:
            valid = task in user and related is None
        elif event in {"task_release_skipped", "owner_release"}:
            valid = task in user and related is None
        elif event == "task_preempt":
            valid = task in user | {0} and related in user
        elif event == "context_switch":
            valid = (task is None or task in user | {0}) and related in user | {0}
        elif event == "idle_start":
            valid = task == 0 and (related is None or related in user)
        elif event == "idle_stop":
            valid = task == 0 and related in user
        elif event == "trace_overflow":
            valid = task is None and related is None
        if not valid:
            raise TraceError(f"sequence {sequence}: impossible task roles for {event}")


def _validate_event_payloads(records: list[dict[str, Any]]) -> None:
    for record in records:
        event = record["event"]
        sequence = record["sequence"]
        value0, value1, value2 = (record["value0"], record["value1"],
                                  record["value2"])
        valid = True
        if event == "kernel_start":
            valid = value0 == 0 and value1 == 5 and value2 == 0
        elif event == "task_create":
            valid = value0 in (0, 1) and value1 <= 0xFF and value2 in (1, 2)
        elif event in {"task_release", "task_start", "task_wake",
                       "deadline_met", "deadline_miss"}:
            valid = value2 > 0
        elif event == "task_preempt":
            valid = value0 in (1, 2, 3) and value1 == 0 and value2 == 0
        elif event in {"task_yield", "idle_start", "idle_stop"}:
            valid = value0 == 0 and value1 == 0 and value2 == 0
        elif event == "task_sleep":
            valid = value2 > 0 and _deadline(value0, value1) >= record["tick"]
        elif event == "task_exit":
            valid = value0 > 0 and value2 == 0
        elif event == "context_switch":
            valid = value0 in range(1, 9) and value1 in range(0, 6) and value2 == 0
        elif event == "alloc":
            valid = value0 > 0 and (
                (value2 == 0 and value1 == 0xFFFFFFFF) or
                (value2 == 1 and value1 != 0xFFFFFFFF))
        elif event == "free":
            valid = (value1 == 1 and value2 in (0, 1) and
                     (value2 == 0 or value0 != 0xFFFFFFFF))
        elif event == "task_wait_period":
            valid = value2 == 0 and _deadline(value0, value1) > record["tick"]
        elif event == "task_release_skipped":
            valid = value2 > 0
        elif event == "owner_release":
            valid = value0 > 0 and value1 == record["task"] and value2 == 1
        elif event == "trace_overflow":
            valid = value0 > 0 and value1 >= value2
        if not valid:
            raise TraceError(f"sequence {sequence}: invalid payload for {event}")

        if event == "task_select":
            reason = value1 & 0xFF
            purpose = (value1 >> 8) & 0xFF
            excluded = (value1 >> 16) & 0xFF
            if (reason not in SELECT_REASONS or purpose not in (1, 2, 3) or
                    excluded not in (1, 2, 3, 4, 0xFF)):
                raise TraceError(f"sequence {sequence}: invalid selection summary metadata")
        elif event == "select_candidate":
            state = (value2 >> 8) & 0xFF
            if value2 & ~0x7FFFF or state not in (2, 3):
                raise TraceError(f"sequence {sequence}: invalid candidate metadata")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="decode a complete AymOS trace")
    parser.add_argument("input", type=Path)
    parser.add_argument("--json", dest="json_path", type=Path, required=True)
    parser.add_argument("--trace-bin", type=Path)
    args = parser.parse_args(argv)
    try:
        result, raw_records = decode(read_bounded(args.input))
        args.json_path.write_text(json.dumps(result, sort_keys=True, indent=2) + "\n",
                                  encoding="utf-8")
        if args.trace_bin is not None:
            args.trace_bin.write_bytes(raw_records)
    except (OSError, TraceError) as error:
        print(f"trace-decode: {error}", file=sys.stderr)
        return 1
    print(f"trace-decode: {result['record_count']} records, schema {SCHEMA_VERSION}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
