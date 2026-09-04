#!/usr/bin/env python3
"""Validate and materialize a complete PR 6 UART trace."""

from __future__ import annotations

from collections import Counter
import json
from pathlib import Path
import sys
from typing import Any

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY_ROOT))

from tools.aymos_lab.trace import TraceError, decode, read_bounded


class TraceWorkloadError(TraceError):
    """A valid schema-1 trace is not the deterministic PR 6 workload."""


EXPECTED_EVENT_COUNTS = Counter({
    "kernel_start": 1,
    "task_create": 5,
    "task_release": 5,
    "task_select": 18,
    "select_candidate": 27,
    "task_start": 5,
    "task_preempt": 4,
    "task_yield": 1,
    "task_sleep": 2,
    "task_wake": 2,
    "task_exit": 5,
    "context_switch": 13,
    "idle_start": 4,
    "idle_stop": 3,
    "deadline_met": 4,
    "deadline_miss": 1,
    "alloc": 1,
    "free": 1,
})

EXPECTED_PROJECTION = (
    ("task_create", 1, None, 0),
    ("task_release", 1, None, 0),
    ("task_create", 2, None, 0),
    ("task_create", 3, None, 0),
    ("task_release", 3, None, 0),
    ("kernel_start", None, None, 0),
    ("task_select", 1, None, 0),
    ("select_candidate", 1, 1, 0),
    ("select_candidate", 3, 1, 0),
    ("context_switch", None, 1, 0),
    ("task_start", 1, None, 0),
    ("alloc", 1, None, 0),
    ("free", 1, None, 0),
    ("task_create", 4, 1, 0),
    ("task_release", 4, None, 0),
    ("task_select", 4, 1, 0),
    ("select_candidate", 1, 4, 0),
    ("select_candidate", 3, 4, 0),
    ("select_candidate", 4, 4, 0),
    ("task_release", 2, None, 1),
    ("task_select", 2, 1, 1),
    ("select_candidate", 1, 2, 1),
    ("select_candidate", 2, 2, 1),
    ("select_candidate", 3, 2, 1),
    ("select_candidate", 4, 2, 1),
    ("task_select", 2, 1, 1),
    ("select_candidate", 1, 2, 1),
    ("select_candidate", 2, 2, 1),
    ("select_candidate", 3, 2, 1),
    ("select_candidate", 4, 2, 1),
    ("task_preempt", 1, 2, 1),
    ("context_switch", 1, 2, 1),
    ("task_start", 2, None, 1),
    ("task_sleep", 2, None, 1),
    ("task_select", 4, 2, 1),
    ("select_candidate", 1, 4, 1),
    ("select_candidate", 3, 4, 1),
    ("select_candidate", 4, 4, 1),
    ("context_switch", 2, 4, 1),
    ("task_start", 4, None, 1),
    ("deadline_met", 4, None, 1),
    ("task_exit", 4, None, 1),
    ("task_select", 1, 4, 1),
    ("select_candidate", 1, 1, 1),
    ("select_candidate", 3, 1, 1),
    ("context_switch", 4, 1, 1),
    ("task_create", 4, 1, 1),
    ("task_yield", 1, None, 1),
    ("task_select", 3, 1, 1),
    ("select_candidate", 1, 3, 1),
    ("select_candidate", 3, 3, 1),
    ("context_switch", 1, 3, 1),
    ("task_start", 3, None, 1),
    ("deadline_met", 3, None, 1),
    ("task_exit", 3, None, 1),
    ("task_select", 1, 3, 1),
    ("select_candidate", 1, 1, 1),
    ("context_switch", 3, 1, 1),
    ("task_sleep", 1, None, 1),
    ("task_select", 0, 1, 1),
    ("idle_start", 0, 1, 1),
    ("context_switch", 1, 0, 1),
    ("task_wake", 2, None, 3),
    ("task_select", 2, 0, 3),
    ("select_candidate", 2, 2, 3),
    ("task_select", 2, 0, 3),
    ("select_candidate", 2, 2, 3),
    ("idle_stop", 0, 2, 3),
    ("task_preempt", 0, 2, 3),
    ("context_switch", 0, 2, 3),
    ("deadline_met", 2, None, 3),
    ("task_exit", 2, None, 3),
    ("task_select", 0, 2, 3),
    ("idle_start", 0, 2, 3),
    ("context_switch", 2, 0, 3),
    ("task_release", 4, None, 4),
    ("task_select", 4, 0, 4),
    ("select_candidate", 4, 4, 4),
    ("task_select", 4, 0, 4),
    ("select_candidate", 4, 4, 4),
    ("idle_stop", 0, 4, 4),
    ("task_preempt", 0, 4, 4),
    ("context_switch", 0, 4, 4),
    ("task_start", 4, None, 4),
    ("deadline_met", 4, None, 4),
    ("task_exit", 4, None, 4),
    ("task_select", 0, 4, 4),
    ("idle_start", 0, 4, 4),
    ("context_switch", 4, 0, 4),
    ("task_wake", 1, None, 8),
    ("task_select", 1, 0, 8),
    ("select_candidate", 1, 1, 8),
    ("task_select", 1, 0, 8),
    ("select_candidate", 1, 1, 8),
    ("idle_stop", 0, 1, 8),
    ("task_preempt", 0, 1, 8),
    ("context_switch", 0, 1, 8),
    ("deadline_miss", 1, None, 15),
    ("task_exit", 1, None, 15),
    ("task_select", 0, 1, 15),
    ("idle_start", 0, 1, 15),
    ("context_switch", 1, 0, 15),
)


def _expect(actual: Any, expected: Any, label: str) -> None:
    if actual != expected:
        raise TraceWorkloadError(
            f"trace workload {label} mismatch: got {actual!r}, "
            f"expected {expected!r}")


def _of(records: list[dict[str, Any]], event: str) -> list[dict[str, Any]]:
    return [record for record in records if record["event"] == event]


def validate_ordered_projection(records: list[dict[str, Any]]) -> None:
    projection = tuple((record["event"], record["task"], record["related"],
                        record["tick"]) for record in records)
    _expect(projection, EXPECTED_PROJECTION, "ordered semantic projection")


def validate_workload(decoded: dict[str, Any]) -> None:
    """Assert the exact semantic contract of ``APP=trace`` schema 1."""
    records = decoded.get("records")
    footer = decoded.get("footer")
    if not isinstance(records, list) or not isinstance(footer, dict):
        raise TraceWorkloadError("trace workload result has no records/footer")

    _expect(Counter(record["event"] for record in records),
            EXPECTED_EVENT_COUNTS, "event counts")
    validate_ordered_projection(records)
    _expect(decoded.get("record_count"), 102, "record count")
    _expect((footer.get("attempted"), footer.get("emitted"),
             footer.get("dropped"), footer.get("final_sequence"),
             footer.get("flags"), footer.get("final_tick")),
            (102, 102, 0, 101, 0, 15), "footer")

    kernels = _of(records, "kernel_start")
    _expect([(item["sequence"], item["tick"], item["task"],
              item["related"], item["value0"], item["value1"],
              item["value2"]) for item in kernels],
            [(5, 0, None, None, 0, 5, 0)], "kernel start")
    _expect([(item["task"], item["related"], item["tick"])
             for item in _of(records, "task_create")],
            [(1, None, 0), (2, None, 0), (3, None, 0), (4, 1, 0),
             (4, 1, 1)],
            "task creation")
    _expect([(item["task"], item["tick"],
              item["details"]["absolute_deadline"],
              item["details"]["job_sequence"])
             for item in _of(records, "task_release")],
            [(1, 0, 15, 1), (3, 0, 200, 1), (4, 0, 5, 1),
             (2, 1, 4, 1), (4, 4, 24, 1)], "task releases")

    _expect([
        (item["tick"], item["task"], item["related"],
         item["details"]["purpose"], item["details"]["reason"],
         item["details"]["ready_mask"],
         item["details"]["candidate_count"],
         item["details"]["excluded"])
        for item in _of(records, "task_select")
    ], [
        (0, 1, None, "dispatch", "deadline", 10, 2, None),
        (0, 4, 1, "runtime_create_probe", "deadline", 24, 3, None),
        (1, 2, 1, "systick_probe", "deadline", 28, 4, None),
        (1, 2, 1, "dispatch", "deadline", 30, 4, None),
        (1, 4, 2, "dispatch", "deadline", 26, 3, None),
        (1, 1, 4, "dispatch", "deadline", 10, 2, None),
        (1, 3, 1, "dispatch", "only_ready", 10, 2, 1),
        (1, 1, 3, "dispatch", "only_ready", 2, 1, None),
        (1, 0, 1, "dispatch", "idle", 0, 0, None),
        (3, 2, 0, "systick_probe", "only_ready", 4, 1, None),
        (3, 2, 0, "dispatch", "only_ready", 4, 1, None),
        (3, 0, 2, "dispatch", "idle", 0, 0, None),
        (4, 4, 0, "systick_probe", "only_ready", 16, 1, None),
        (4, 4, 0, "dispatch", "only_ready", 16, 1, None),
        (4, 0, 4, "dispatch", "idle", 0, 0, None),
        (8, 1, 0, "systick_probe", "only_ready", 2, 1, None),
        (8, 1, 0, "dispatch", "only_ready", 2, 1, None),
        (15, 0, 1, "dispatch", "idle", 0, 0, None),
    ], "scheduler selections")

    _expect([(item["tick"], item["task"], item["related"],
              item["value0"])
             for item in _of(records, "task_preempt")],
            [(1, 1, 2, 3), (3, 0, 2, 1), (4, 0, 4, 1),
             (8, 0, 1, 1)],
            "preemption sources")
    _expect([(item["tick"], item["task"], item["related"],
              item["details"]["cause"])
             for item in _of(records, "context_switch")],
            [(0, None, 1, "start"),
             (1, 1, 2, "coalesced_preempt"),
             (1, 2, 4, "sleep"),
             (1, 4, 1, "exit"),
             (1, 1, 3, "yield"),
             (1, 3, 1, "exit"),
             (1, 1, 0, "sleep"),
             (3, 0, 2, "systick_preempt"),
             (3, 2, 0, "exit"),
             (4, 0, 4, "systick_preempt"),
             (4, 4, 0, "exit"),
             (8, 0, 1, "systick_preempt"),
             (15, 1, 0, "exit")], "context switches")
    _expect([(item["task"], item["tick"])
             for item in _of(records, "task_start")],
            [(1, 0), (2, 1), (4, 1), (3, 1), (4, 4)], "task starts")
    _expect([(item["task"], item["tick"], item["value1"])
             for item in _of(records, "task_exit")],
            [(4, 1, 0), (3, 1, 0), (2, 3, 0), (4, 4, 0),
             (1, 15, 1)],
            "task exits")
    _expect([(item["task"], item["tick"])
             for item in _of(records, "task_yield")],
            [(1, 1)], "voluntary yield")
    _expect([(item["task"], item["tick"],
              item["details"]["wake_tick"],
              item["details"]["sleep_ticks"])
             for item in _of(records, "task_sleep")],
            [(2, 1, 3, 2), (1, 1, 8, 7)], "sleep requests")
    _expect([(item["task"], item["tick"],
              item["details"]["absolute_deadline"])
             for item in _of(records, "task_wake")],
            [(2, 3, 4), (1, 8, 15)], "wakeups")

    _expect([(item["task"], item["tick"],
              item["details"]["absolute_deadline"])
             for item in _of(records, "deadline_met")],
            [(4, 1, 5), (3, 1, 200), (2, 3, 4), (4, 4, 24)],
            "met deadlines")
    _expect([(item["task"], item["tick"],
              item["details"]["absolute_deadline"])
             for item in _of(records, "deadline_miss")],
            [(1, 15, 15)], "deadline miss")
    _expect([(item["task"], item["tick"], item["value0"],
              item["value1"], item["value2"])
             for item in _of(records, "alloc")],
            [(1, 0, 24, 24, 1)], "allocation")
    _expect([(item["task"], item["tick"], item["value0"],
              item["value1"], item["value2"])
             for item in _of(records, "free")],
            [(1, 0, 24, 1, 1)], "deallocation")
    _expect([(item["sequence"], item["tick"], item["task"],
              item["related"])
             for item in _of(records, "idle_start")],
            [(60, 1, 0, 1), (73, 3, 0, 2), (87, 4, 0, 4),
             (100, 15, 0, 1)], "idle intervals")
    _expect([(item["sequence"], item["tick"], item["task"],
              item["related"])
             for item in _of(records, "idle_stop")],
            [(67, 3, 0, 2), (80, 4, 0, 4), (94, 8, 0, 1)],
            "idle resumes")
    _expect((records[-1]["sequence"], records[-1]["event"],
             records[-1]["task"], records[-1]["related"]),
            (101, "context_switch", 1, 0), "terminal context switch")


def validate(data: bytes) -> tuple[dict, bytes]:
    decoded, raw_records = decode(data)
    validate_workload(decoded)
    return decoded, raw_records


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("usage: verify_trace_uart.py UART_CAPTURE", file=sys.stderr)
        return 2
    capture = Path(argv[1])
    try:
        decoded, raw_records = validate(read_bounded(capture))
        (capture.parent / "trace.bin").write_bytes(raw_records)
        (capture.parent / "trace.json").write_text(
            json.dumps(decoded, sort_keys=True, indent=2) + "\n",
            encoding="utf-8")
    except (OSError, TraceError) as error:
        print(f"trace-uart: {error}", file=sys.stderr)
        return 1
    print(f"AYMOS TRACE PASS RECORDS={decoded['record_count']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
