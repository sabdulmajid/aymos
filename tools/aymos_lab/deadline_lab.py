"""Run and report the deterministic Cortex-M4 scheduling workload."""

from __future__ import annotations

from collections import Counter
from datetime import datetime, timezone
import argparse
import hashlib
import html
import json
import os
from pathlib import Path
import re
import secrets
import signal
import shutil
import subprocess
import sys
from typing import Any

from tools.aymos_lab.trace import TraceError, decode, read_bounded


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
RUNS_ROOT = REPOSITORY_ROOT / "runs"
RUN_MARKER = ".aymos-deadline-lab-run"
RUN_ID_PATTERN = re.compile(r"^\d{8}T\d{6}Z-\d+-[0-9a-f]{8}$")
RETAIN_RUNS = 8
TRACE_CAPACITY = 256
FINAL_TICK = 21
DEMO_HOST_TIMEOUT_SECONDS = "20"
DEMO_VIRTUAL_DURATION_SECONDS = "0.5"
DEMO_OUTER_TIMEOUT_SECONDS = 45

TASKS = {
    0: {"name": "idle", "color": "#94a3b8"},
    1: {"name": "sampler", "color": "#38bdf8"},
    2: {"name": "controller", "color": "#818cf8"},
    3: {"name": "telemetry", "color": "#2dd4bf"},
    4: {"name": "load", "color": "#f59e0b"},
}

BASE_WORKLOAD = {
    "sampler": {
        "task_id": 1, "kind": "periodic", "initial_release": 0,
        "period": 6, "relative_deadline": 3, "execution_ticks": 1,
        "jobs": 3, "priority": 0,
    },
    "controller": {
        "task_id": 2, "kind": "periodic", "initial_release": 1,
        "period": 10, "relative_deadline": 8, "execution_ticks": 2,
        "jobs": 2, "priority": 1,
    },
    "telemetry": {
        "task_id": 3, "kind": "periodic", "initial_release": 2,
        "period": 18, "relative_deadline": 14, "execution_ticks": 1,
        "jobs": 2, "priority": 2,
    },
    "load": {
        "task_id": 4, "kind": "one_shot", "initial_release": 0,
        "period": 0, "relative_deadline": 12, "execution_ticks": 1,
        "jobs": 1, "priority": 3,
    },
}

EXPECTED_RELEASES = {
    1: [(0, 3, 1), (6, 9, 2), (12, 15, 3)],
    2: [(1, 9, 1), (11, 19, 2)],
    3: [(2, 16, 1), (20, 34, 2)],
    4: [(0, 12, 1)],
}

EXPECTED_INTERVALS = {
    "normal": (
        (1, 0, 1), (2, 1, 3), (4, 3, 4), (3, 4, 5), (0, 5, 6),
        (1, 6, 7), (0, 7, 11), (2, 11, 12), (1, 12, 13),
        (2, 13, 14), (0, 14, 20), (3, 20, 21),
    ),
    "overload": (
        (1, 0, 1), (2, 1, 3), (4, 3, 6), (1, 6, 7), (4, 7, 12),
        (1, 12, 13), (3, 13, 14), (2, 14, 16), (0, 16, 20),
        (3, 20, 21),
    ),
}

# tick, selected, incumbent, purpose, reason, READY mask, excluded,
# candidates(task, deadline, priority, state, excluded, incumbent, eligible)
EXPECTED_SELECTIONS = {
    "normal": (
        (0, 1, None, "dispatch", "deadline", 18, None,
         ((1, 3, 0, 2, False, False, True),
          (4, 12, 3, 2, False, False, True))),
        (1, 2, 1, "dispatch", "deadline", 20, None,
         ((2, 9, 1, 2, False, False, True),
          (4, 12, 3, 2, False, False, True))),
        (3, 4, 2, "dispatch", "deadline", 24, None,
         ((3, 16, 2, 2, False, False, True),
          (4, 12, 3, 2, False, False, True))),
        (4, 3, 4, "dispatch", "only_ready", 8, None,
         ((3, 16, 2, 2, False, False, True),)),
        (5, 0, 3, "dispatch", "idle", 0, None, ()),
        (6, 1, 0, "systick_probe", "only_ready", 2, None,
         ((1, 9, 0, 2, False, False, True),)),
        (6, 1, 0, "dispatch", "only_ready", 2, None,
         ((1, 9, 0, 2, False, False, True),)),
        (7, 0, 1, "dispatch", "idle", 0, None, ()),
        (11, 2, 0, "systick_probe", "only_ready", 4, None,
         ((2, 19, 1, 2, False, False, True),)),
        (11, 2, 0, "dispatch", "only_ready", 4, None,
         ((2, 19, 1, 2, False, False, True),)),
        (12, 1, 2, "systick_probe", "deadline", 2, None,
         ((1, 15, 0, 2, False, False, True),
          (2, 19, 1, 3, False, True, True))),
        (12, 1, 2, "dispatch", "deadline", 6, None,
         ((1, 15, 0, 2, False, False, True),
          (2, 19, 1, 2, False, True, True))),
        (13, 2, 1, "dispatch", "only_ready", 4, None,
         ((2, 19, 1, 2, False, False, True),)),
        (14, 0, 2, "dispatch", "idle", 0, None, ()),
        (20, 3, 0, "systick_probe", "only_ready", 8, None,
         ((3, 34, 2, 2, False, False, True),)),
        (20, 3, 0, "dispatch", "only_ready", 8, None,
         ((3, 34, 2, 2, False, False, True),)),
        (21, 0, 3, "dispatch", "idle", 0, None, ()),
    ),
    "overload": (
        (0, 1, None, "dispatch", "deadline", 18, None,
         ((1, 3, 0, 2, False, False, True),
          (4, 12, 3, 2, False, False, True))),
        (1, 2, 1, "dispatch", "deadline", 20, None,
         ((2, 9, 1, 2, False, False, True),
          (4, 12, 3, 2, False, False, True))),
        (3, 4, 2, "dispatch", "deadline", 24, None,
         ((3, 16, 2, 2, False, False, True),
          (4, 12, 3, 2, False, False, True))),
        (6, 1, 4, "systick_probe", "deadline", 10, None,
         ((1, 9, 0, 2, False, False, True),
          (3, 16, 2, 2, False, False, True),
          (4, 12, 3, 3, False, True, True))),
        (6, 1, 4, "dispatch", "deadline", 26, None,
         ((1, 9, 0, 2, False, False, True),
          (3, 16, 2, 2, False, False, True),
          (4, 12, 3, 2, False, True, True))),
        (7, 4, 1, "dispatch", "deadline", 24, None,
         ((3, 16, 2, 2, False, False, True),
          (4, 12, 3, 2, False, False, True))),
        (12, 1, 4, "dispatch", "deadline", 14, None,
         ((1, 15, 0, 2, False, False, True),
          (2, 19, 1, 2, False, False, True),
          (3, 16, 2, 2, False, False, True))),
        (13, 3, 1, "dispatch", "deadline", 12, None,
         ((2, 19, 1, 2, False, False, True),
          (3, 16, 2, 2, False, False, True))),
        (14, 2, 3, "dispatch", "only_ready", 4, None,
         ((2, 19, 1, 2, False, False, True),)),
        (16, 0, 2, "dispatch", "idle", 0, None, ()),
        (20, 3, 0, "systick_probe", "only_ready", 8, None,
         ((3, 34, 2, 2, False, False, True),)),
        (20, 3, 0, "dispatch", "only_ready", 8, None,
         ((3, 34, 2, 2, False, False, True),)),
        (21, 0, 3, "dispatch", "idle", 0, None, ()),
    ),
}


class DeadlineLabError(TraceError):
    """The trace does not satisfy the scheduling workload contract."""


def workload(mode: str) -> dict[str, dict[str, int | str]]:
    """Return a detached workload definition for one supported mode."""
    if mode not in ("normal", "overload"):
        raise DeadlineLabError(f"unknown scheduling mode: {mode}")
    result = {name: dict(values) for name, values in BASE_WORKLOAD.items()}
    result["load"]["execution_ticks"] = 8 if mode == "overload" else 1
    return result


def _records(decoded: dict[str, Any], event: str) -> list[dict[str, Any]]:
    records = decoded.get("records")
    if not isinstance(records, list):
        raise DeadlineLabError("decoded trace has no record list")
    return [record for record in records if record.get("event") == event]


def _expect(actual: Any, expected: Any, label: str) -> None:
    if actual != expected:
        raise DeadlineLabError(
            f"Scheduling workload {label} mismatch: got {actual!r}, "
            f"expected {expected!r}")


def running_intervals(decoded: dict[str, Any]) -> list[dict[str, int]]:
    """Convert committed switches into positive-width running intervals."""
    switches = _records(decoded, "context_switch")
    footer = decoded.get("footer")
    if not isinstance(footer, dict) or not isinstance(
            footer.get("final_tick"), int):
        raise DeadlineLabError("decoded trace has no final tick")
    intervals: list[dict[str, int]] = []
    for index, switch in enumerate(switches):
        task = switch.get("related")
        start = switch.get("tick")
        end = (switches[index + 1].get("tick")
               if index + 1 < len(switches) else footer["final_tick"])
        if not isinstance(task, int) or not isinstance(start, int) or not isinstance(end, int):
            raise DeadlineLabError("context switch has an invalid task or tick")
        if end < start:
            raise DeadlineLabError("context switch ticks are not monotonic")
        if index > 0 and switch.get("task") != switches[index - 1].get("related"):
            raise DeadlineLabError("context switch chain is not continuous")
        if end > start:
            intervals.append({"task": task, "start": start, "end": end})
    return intervals


def explain_selection(record: dict[str, Any],
                      candidates: list[dict[str, Any]]) -> str:
    """Explain one choice using only its emitted selection snapshot."""
    selected = record.get("task")
    details = record.get("details", {})
    reason = details.get("reason", "unknown")
    selected_name = TASKS.get(selected, {"name": f"task {selected}"})["name"]
    if selected == 0:
        return "The kernel selected idle because no user task was ready."

    candidate_text = []
    for candidate in candidates:
        task_id = candidate.get("task")
        candidate_details = candidate.get("details", {})
        name = TASKS.get(task_id, {"name": f"task {task_id}"})["name"]
        candidate_text.append(
            f"{name} (deadline {candidate_details.get('absolute_deadline')}, "
            f"priority {candidate_details.get('priority')})")
    choices = ", ".join(candidate_text)
    reason_text = {
        "only_ready": "it was the only eligible task",
        "deadline": "it had the earliest absolute deadline",
        "priority": "its priority broke an equal-deadline tie",
        "stable_tid": "its stable task ID broke the remaining tie",
        "excluded_fallback": "the voluntary-yield exclusion had no alternative",
    }.get(reason, f"the trace reported reason {reason}")
    suffix = f" Candidates: {choices}." if choices else ""
    return f"The kernel selected {selected_name} because {reason_text}.{suffix}"


def selection_decisions(decoded: dict[str, Any]) -> list[dict[str, Any]]:
    records = decoded["records"]
    decisions: list[dict[str, Any]] = []
    for index, record in enumerate(records):
        if record["event"] != "task_select":
            continue
        count = record["details"]["candidate_count"]
        candidates = records[index + 1:index + 1 + count]
        decisions.append({
            "tick": record["tick"],
            "sequence": record["sequence"],
            "selected": record["task"],
            "incumbent": record["related"],
            "purpose": record["details"]["purpose"],
            "reason": record["details"]["reason"],
            "explanation": explain_selection(record, candidates),
        })
    return decisions


def selection_projection(decoded: dict[str, Any]) -> tuple[Any, ...]:
    """Return every emitted selection field used by the workload oracle."""
    records = decoded["records"]
    projection: list[tuple[Any, ...]] = []
    for index, record in enumerate(records):
        if record["event"] != "task_select":
            continue
        details = record["details"]
        candidates = records[index + 1:index + 1 +
                             details["candidate_count"]]
        candidate_projection = tuple((
            candidate["task"],
            candidate["details"]["absolute_deadline"],
            candidate["details"]["priority"],
            candidate["details"]["state"],
            candidate["details"]["excluded"],
            candidate["details"]["incumbent"],
            candidate["details"]["eligible"],
        ) for candidate in candidates)
        projection.append((
            record["tick"], record["task"], record["related"],
            details["purpose"], details["reason"], details["ready_mask"],
            details["excluded"], candidate_projection,
        ))
    return tuple(projection)


def validate_schedule_projection(decoded: dict[str, Any], mode: str) -> None:
    """Require the exact committed schedule and its emitted EDF snapshots."""
    workload(mode)
    intervals = tuple((item["task"], item["start"], item["end"])
                      for item in running_intervals(decoded))
    _expect(intervals, EXPECTED_INTERVALS[mode], "running interval projection")
    _expect(selection_projection(decoded), EXPECTED_SELECTIONS[mode],
            "selection projection")
    dispatches = [item for item in decoded["records"]
                  if item["event"] == "task_select" and
                  item["details"]["purpose"] == "dispatch"]
    switches = _records(decoded, "context_switch")
    _expect(len(dispatches), len(switches), "dispatch/switch count")
    for dispatch, switch in zip(dispatches, switches, strict=True):
        _expect((dispatch["tick"], dispatch["task"], dispatch["related"]),
                (switch["tick"], switch["related"], switch["task"]),
                "dispatch/switch correlation")
        if dispatch["sequence"] >= switch["sequence"]:
            raise DeadlineLabError(
                "Scheduler dispatch does not precede its committed switch")


def validate_workload(decoded: dict[str, Any], mode: str) -> None:
    """Validate the finite guest workload without duplicating its scheduler."""
    definition = workload(mode)
    footer = decoded.get("footer")
    if not isinstance(footer, dict):
        raise DeadlineLabError("decoded trace has no footer")
    record_count = decoded.get("record_count")
    if not isinstance(record_count, int) or record_count >= TRACE_CAPACITY:
        raise DeadlineLabError(
            f"trace uses {record_count!r} records; capacity is {TRACE_CAPACITY}")
    _expect(footer.get("final_tick"), FINAL_TICK, "final tick")
    _expect(footer.get("dropped"), 0, "dropped record count")

    _expect([(item["task"], item["details"]["kind"],
              item["details"]["priority"])
             for item in _records(decoded, "task_create")],
            [(1, 1, 0), (2, 1, 1), (3, 1, 2), (4, 0, 3)],
            "task creation")
    releases: dict[int, list[tuple[int, int, int]]] = {
        task_id: [] for task_id in range(1, 5)
    }
    for item in _records(decoded, "task_release"):
        releases[item["task"]].append((
            item["tick"], item["details"]["absolute_deadline"],
            item["details"]["job_sequence"]))
    _expect(releases, EXPECTED_RELEASES, "release schedule")

    _expect(Counter(item["task"] for item in _records(decoded, "task_start")),
            Counter({1: 1, 2: 1, 3: 1, 4: 1}), "first starts")
    _expect(Counter(item["task"] for item in
                    _records(decoded, "task_wait_period")),
            Counter({1: 2, 2: 1, 3: 1}), "period waits")
    _expect(Counter(item["task"] for item in _records(decoded, "task_exit")),
            Counter({1: 1, 2: 1, 3: 1, 4: 1}), "task exits")
    _expect(_records(decoded, "task_release_skipped"), [],
            "skipped releases")

    misses = [(item["task"], item["tick"],
               item["details"]["absolute_deadline"])
              for item in _records(decoded, "deadline_miss")]
    expected_misses = [] if mode == "normal" else [(4, 12, 12)]
    _expect(misses, expected_misses, "deadline misses")
    expected_met = (Counter({1: 3, 2: 2, 3: 2, 4: 1})
                    if mode == "normal" else
                    Counter({1: 3, 2: 2, 3: 2}))
    _expect(Counter(item["task"] for item in
                    _records(decoded, "deadline_met")),
            expected_met, "met deadlines")

    switches = _records(decoded, "context_switch")
    if not switches:
        raise DeadlineLabError("trace has no committed context switch")
    _expect((switches[0]["task"], switches[0]["related"],
             switches[0]["tick"]), (None, 1, 0), "first dispatch")
    _expect((switches[-1]["related"], switches[-1]["tick"]),
            (0, FINAL_TICK), "terminal idle switch")

    intervals = running_intervals(decoded)
    validate_schedule_projection(decoded, mode)
    run_ticks = Counter()
    for interval in intervals:
        run_ticks[interval["task"]] += interval["end"] - interval["start"]
    expected_ticks = {
        spec["task_id"]: spec["execution_ticks"] * spec["jobs"]
        for spec in definition.values()
    }
    for task_id, ticks in expected_ticks.items():
        _expect(run_ticks[task_id], ticks,
                f"{TASKS[task_id]['name']} running ticks")
    _expect(run_ticks[0], 11 if mode == "normal" else 4, "idle ticks")
    preemptions = [(item["task"], item["related"], item["tick"])
                   for item in _records(decoded, "task_preempt")]
    expected_preemptions = (
        [(0, 1, 6), (0, 2, 11), (2, 1, 12), (0, 3, 20)]
        if mode == "normal" else [(4, 1, 6), (0, 3, 20)])
    _expect(preemptions, expected_preemptions, "committed preemptions")

    if mode == "overload":
        miss_sequence = _records(decoded, "deadline_miss")[0]["sequence"]
        prior = [decision for decision in selection_decisions(decoded)
                 if decision["selected"] == 4 and
                 decision["sequence"] < miss_sequence]
        if not prior or prior[-1]["reason"] not in ("deadline", "only_ready"):
            raise DeadlineLabError(
                "overload miss has no prior emitted load selection evidence")


def build_summary(decoded: dict[str, Any], mode: str) -> dict[str, Any]:
    validate_workload(decoded, mode)
    intervals = running_intervals(decoded)
    decisions = selection_decisions(decoded)
    misses = _records(decoded, "deadline_miss")
    task_rows: dict[str, dict[str, int | str]] = {}
    for task_id, task in TASKS.items():
        task_intervals = [item for item in intervals if item["task"] == task_id]
        task_rows[str(task_id)] = {
            "name": task["name"],
            "released_jobs": sum(
                item["task"] == task_id
                for item in _records(decoded, "task_release")),
            "running_ticks": sum(
                item["end"] - item["start"] for item in task_intervals),
            "preemptions": sum(
                item["task"] == task_id
                for item in _records(decoded, "task_preempt")),
            "deadlines_met": sum(
                item["task"] == task_id
                for item in _records(decoded, "deadline_met")),
            "deadline_misses": sum(item["task"] == task_id for item in misses),
        }

    first_miss: dict[str, Any] | None = None
    if misses:
        miss = misses[0]
        prior = [decision for decision in decisions
                 if decision["selected"] == miss["task"] and
                 decision["sequence"] < miss["sequence"]]
        first_miss = {
            "task": miss["task"],
            "task_name": TASKS[miss["task"]]["name"],
            "tick": miss["tick"],
            "absolute_deadline": miss["details"]["absolute_deadline"],
            "preceding_selection": prior[-1] if prior else None,
        }
    return {
        "schema_version": 1,
        "mode": mode,
        "result": "all_deadlines_met" if not misses else "deadline_missed",
        "final_tick": decoded["footer"]["final_tick"],
        "trace_records": decoded["record_count"],
        "trace_capacity_records": TRACE_CAPACITY,
        "context_switches": len(_records(decoded, "context_switch")),
        "preemptions": len(_records(decoded, "task_preempt")),
        "idle_ticks": sum(item["end"] - item["start"] for item in intervals
                          if item["task"] == 0),
        "deadline_misses": len(misses),
        "first_deadline_miss": first_miss,
        "tasks": task_rows,
        "selection_decisions": decisions,
    }


def render_timeline(decoded: dict[str, Any], summary: dict[str, Any]) -> str:
    """Create one dependency-free HTML report with an inline SVG timeline."""
    intervals = running_intervals(decoded)
    releases = _records(decoded, "task_release")
    misses = _records(decoded, "deadline_miss")
    preemptions = _records(decoded, "task_preempt")
    deadlines = [item["details"]["absolute_deadline"] for item in releases]
    domain = max([summary["final_tick"], *deadlines, 1])
    left, width, row_height = 145, 980, 64
    top = 52
    svg_height = top + row_height * len(TASKS) + 52

    def x(tick: int) -> float:
        return left + (tick / domain) * width

    svg: list[str] = [
        f'<svg viewBox="0 0 1180 {svg_height}" role="img" '
        'aria-label="AymOS scheduling timeline">',
        '<rect class="plot-bg" x="0" y="0" width="1180" '
        f'height="{svg_height}" rx="14"/>',
    ]
    for tick in range(0, domain + 1, 2):
        px = x(tick)
        svg.append(f'<line class="grid" x1="{px:.2f}" y1="36" '
                   f'x2="{px:.2f}" y2="{svg_height - 34}"/>')
        svg.append(f'<text class="tick" x="{px:.2f}" y="27">{tick}</text>')
    for task_id, task in TASKS.items():
        y = top + task_id * row_height
        svg.append(f'<text class="row-label" x="18" y="{y + 25}">'
                   f'{html.escape(str(task["name"]))}</text>')
        svg.append(f'<line class="lane" x1="{left}" y1="{y + 20}" '
                   f'x2="{left + width}" y2="{y + 20}"/>')
    for interval in intervals:
        y = top + interval["task"] * row_height + 8
        start, end = x(interval["start"]), x(interval["end"])
        task = TASKS[interval["task"]]
        label = (f'{task["name"]}: tick {interval["start"]} to '
                 f'{interval["end"]}')
        svg.append(f'<rect class="run" x="{start:.2f}" y="{y}" '
                   f'width="{max(end - start, 2):.2f}" height="24" '
                   f'fill="{task["color"]}"><title>{html.escape(label)}</title></rect>')
    for release in releases:
        task_id = release["task"]
        y = top + task_id * row_height + 3
        px = x(release["tick"])
        svg.append(f'<circle class="release" cx="{px:.2f}" cy="{y}" r="5">'
                   f'<title>{TASKS[task_id]["name"]} released at tick '
                   f'{release["tick"]}</title></circle>')
        deadline = release["details"]["absolute_deadline"]
        dx = x(deadline)
        svg.append(f'<path class="deadline" d="M {dx:.2f} {y - 7} '
                   f'v 38"><title>{TASKS[task_id]["name"]} absolute '
                   f'deadline {deadline}</title></path>')
    for event in preemptions:
        incoming = event["related"]
        y = top + incoming * row_height + 20
        px = x(event["tick"])
        svg.append(f'<path class="preempt" d="M {px:.2f} {y - 9} '
                   f'l 8 9 l -8 9 l -8 -9 z"><title>preemption: '
                   f'{TASKS[event["task"]]["name"]} to '
                   f'{TASKS[incoming]["name"]}</title></path>')
    for miss in misses:
        y = top + miss["task"] * row_height + 20
        px = x(miss["tick"])
        svg.append(f'<path class="miss" d="M {px - 7:.2f} {y - 7} '
                   f'l 14 14 M {px + 7:.2f} {y - 7} l -14 14">'
                   f'<title>deadline miss at tick {miss["tick"]}</title></path>')
    svg.append('</svg>')

    task_rows = "".join(
        "<tr>" + "".join(f"<td>{html.escape(str(value))}</td>" for value in (
            row["name"], row["released_jobs"], row["running_ticks"],
            row["preemptions"], row["deadlines_met"], row["deadline_misses"]))
        + "</tr>"
        for _, row in sorted(summary["tasks"].items(), key=lambda item: int(item[0]))
    )
    decision_rows = "".join(
        f'<tr><td>{item["tick"]}</td><td>{html.escape(item["purpose"])}</td>'
        f'<td>{html.escape(str(TASKS[item["selected"]]["name"]))}</td>'
        f'<td>{html.escape(item["reason"])}</td>'
        f'<td>{html.escape(item["explanation"])}</td></tr>'
        for item in summary["selection_decisions"]
    )
    miss_text = ("None."
                 if summary["first_deadline_miss"] is None else
                 f'{summary["first_deadline_miss"]["task_name"]} at tick '
                 f'{summary["first_deadline_miss"]["tick"]}.')
    title_mode = html.escape(str(summary["mode"]).title())
    return f'''<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AymOS scheduling report — {title_mode}</title>
<style>
:root{{--ink:#e5eef7;--muted:#94a3b8;--panel:#111827;--line:#263244;--accent:#38bdf8}}
*{{box-sizing:border-box}}body{{margin:0;background:#08111f;color:var(--ink);font:15px/1.5 ui-sans-serif,system-ui,sans-serif}}
main{{max-width:1240px;margin:auto;padding:40px 24px 72px}}h1{{font-size:34px;margin:0 0 8px}}h2{{margin-top:34px}}p{{color:#b8c5d6}}
.cards{{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:12px;margin:24px 0}}
.card,.panel{{background:var(--panel);border:1px solid var(--line);border-radius:14px;padding:16px}}.value{{font-size:26px;font-weight:700}}
.ok{{color:#2dd4bf}}.bad{{color:#fb7185}}.plot{{overflow-x:auto}}svg{{min-width:900px;width:100%}}.plot-bg{{fill:#0c1626}}
.grid{{stroke:#1e293b;stroke-width:1}}.lane{{stroke:#334155;stroke-width:1}}.tick{{fill:#64748b;text-anchor:middle;font-size:11px}}.row-label{{fill:#cbd5e1;font-weight:650}}
.run{{rx:5;opacity:.92}}.release{{fill:#f8fafc;stroke:#08111f;stroke-width:2}}.deadline{{stroke:#f8fafc;stroke-width:1.5;stroke-dasharray:4 4}}.preempt{{fill:#f472b6}}.miss{{stroke:#fb7185;stroke-width:4;stroke-linecap:round}}
table{{width:100%;border-collapse:collapse;background:var(--panel);border-radius:12px;overflow:hidden}}th,td{{padding:10px 12px;text-align:left;border-bottom:1px solid var(--line);vertical-align:top}}th{{color:#93c5fd}}code{{color:#bae6fd}}
</style></head><body><main>
<h1>AymOS scheduling report</h1><p>{title_mode} mode. This report uses the structured events emitted by the Cortex-M4 firmware.</p>
<div class="cards"><div class="card"><div>Result</div><div class="value {'ok' if summary['deadline_misses'] == 0 else 'bad'}">{html.escape(summary['result'])}</div></div>
<div class="card"><div>Guest ticks</div><div class="value">{summary['final_tick']}</div></div>
<div class="card"><div>Context switches</div><div class="value">{summary['context_switches']}</div></div>
<div class="card"><div>Idle ticks</div><div class="value">{summary['idle_ticks']}</div></div>
<div class="card"><div>First miss</div><div class="value {'ok' if summary['deadline_misses'] == 0 else 'bad'}">{html.escape(miss_text)}</div></div></div>
<h2>Schedule</h2><p>A circle marks a release. A dashed line marks an absolute deadline. A diamond marks a preemption. A red cross marks a miss.</p>
<div class="panel plot">{''.join(svg)}</div>
<h2>Task summary</h2><table><thead><tr><th>Task</th><th>Jobs</th><th>Running ticks</th><th>Preemptions</th><th>Met</th><th>Missed</th></tr></thead><tbody>{task_rows}</tbody></table>
<h2>Scheduler decisions</h2><p>Each explanation uses the candidate state stored in the trace. It does not infer a hidden decision.</p>
<table><thead><tr><th>Tick</th><th>Purpose</th><th>Selected</th><th>Reason</th><th>Explanation</th></tr></thead><tbody>{decision_rows}</tbody></table>
<h2>Measurement boundary</h2><p>Ticks and event order are functional emulator evidence. They are not physical execution time, interrupt latency, WCET, or a hard real-time guarantee.</p>
</main></body></html>'''


def _yaml(mode: str) -> str:
    lines = ["schema_version: 1", "name: aymos_deadline_lab",
             f"mode: {mode}", "random_seed: null", "tasks:"]
    for name, spec in workload(mode).items():
        lines.append(f"  {name}:")
        for key, value in spec.items():
            lines.append(f"    {key}: {value}")
    return "\n".join(lines) + "\n"


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _write_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def _runs_root_resolved() -> Path:
    repository = REPOSITORY_ROOT.resolve(strict=True)
    if RUNS_ROOT.is_symlink():
        raise DeadlineLabError(f"runs root may not be a symlink: {RUNS_ROOT}")
    if RUNS_ROOT.exists() and not RUNS_ROOT.is_dir():
        raise DeadlineLabError(f"runs root is not a directory: {RUNS_ROOT}")
    RUNS_ROOT.mkdir(exist_ok=True)
    resolved = RUNS_ROOT.resolve(strict=True)
    if resolved != repository / "runs":
        raise DeadlineLabError("runs root resolves outside the repository")
    return resolved


def _new_run_directory() -> tuple[str, Path]:
    root = _runs_root_resolved()
    for _ in range(8):
        timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        run_id = f"{timestamp}-{os.getpid()}-{secrets.token_hex(4)}"
        path = root / run_id
        try:
            path.mkdir()
        except FileExistsError:
            continue
        (path / RUN_MARKER).write_text(
            "schema=1\nstate=active\n", encoding="ascii")
        return run_id, path
    raise DeadlineLabError("could not create a unique run directory")


def _prune_runs(current: Path) -> None:
    root = _runs_root_resolved()
    if current.is_symlink() or current.resolve(strict=True).parent != root:
        raise DeadlineLabError("current run is not directly below the runs root")
    candidates: list[Path] = []
    for entry in root.iterdir():
        marker = entry / RUN_MARKER
        if (entry.is_dir() and not entry.is_symlink() and entry != current and
                RUN_ID_PATTERN.fullmatch(entry.name) and
                marker.is_file() and
                "state=active\n" not in marker.read_text(encoding="ascii") and
                entry.resolve(strict=True).parent == root):
            candidates.append(entry)
    candidates.sort(key=lambda path: path.name, reverse=True)
    for expired in candidates[RETAIN_RUNS - 1:]:
        shutil.rmtree(expired)


def _tool_version(command: list[str]) -> str:
    environment = dict(os.environ)
    environment.pop("PYTHONHOME", None)
    environment.pop("PYTHONPATH", None)
    result = subprocess.run(command, cwd=REPOSITORY_ROOT, env=environment,
                            check=True, capture_output=True, text=True,
                            timeout=10)
    return result.stdout.splitlines()[0].strip()


def _git_value(*arguments: str) -> str:
    result = subprocess.run(["git", *arguments], cwd=REPOSITORY_ROOT,
                            check=True, capture_output=True, text=True,
                            timeout=10)
    return result.stdout.strip()


def demo_environment(mode: str, mode_dir: Path) -> dict[str, str]:
    workload(mode)
    environment = dict(os.environ)
    environment.update({
        "AYMOS_APP": "deadline_lab",
        "AYMOS_WORKLOAD_MODE": mode,
        "AYMOS_RENODE_OUTPUT_DIR": str(mode_dir),
        "AYMOS_RENODE_HOST_TIMEOUT": DEMO_HOST_TIMEOUT_SECONDS,
        "AYMOS_RENODE_VIRTUAL_DURATION": DEMO_VIRTUAL_DURATION_SECONDS,
    })
    return environment


def _run_process_group(command: list[str], environment: dict[str, str]) -> None:
    process = subprocess.Popen(command, cwd=REPOSITORY_ROOT, env=environment,
                               start_new_session=True)
    try:
        status = process.wait(timeout=DEMO_OUTER_TIMEOUT_SECONDS)
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


def _run_mode(run_id: str, run_root: Path, mode: str) -> dict[str, Any]:
    mode_dir = run_root / mode
    build_dir = REPOSITORY_ROOT / "build/nucleo_f401re/deadline_lab" / mode
    elf = build_dir / "aymos.elf"
    firmware_map = build_dir / "aymos.map"
    build_metadata = build_dir / "build-metadata.txt"
    if (not elf.is_file() or not firmware_map.is_file() or
            not build_metadata.is_file()):
        raise DeadlineLabError(
            f"missing {mode} firmware; run make demo from the repository root")

    _run_process_group(
        [str(REPOSITORY_ROOT / "tools/renode/run.sh")],
        demo_environment(mode, mode_dir))
    shutil.rmtree(mode_dir / "home")
    shutil.rmtree(mode_dir / "xdg")

    trace_json = mode_dir / "trace.json"
    decoded = json.loads(trace_json.read_text(encoding="utf-8"))
    summary = build_summary(decoded, mode)
    _write_json(mode_dir / "summary.json", summary)
    (mode_dir / "timeline.html").write_text(
        render_timeline(decoded, summary), encoding="utf-8")
    (mode_dir / "workload.yaml").write_text(_yaml(mode), encoding="utf-8")
    shutil.copy2(elf, mode_dir / "firmware.elf")
    shutil.copy2(firmware_map, mode_dir / "firmware.map")
    shutil.copy2(build_metadata, mode_dir / "build-metadata.txt")

    compiler = (REPOSITORY_ROOT /
                ".tools/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-gcc")
    renode = REPOSITORY_ROOT / ".tools/renode-1.16.1-dotnet-x86_64/renode"
    command_text = (mode_dir / "command.txt").read_text(encoding="utf-8")
    metadata = {
        "schema_version": 1,
        "run_id": run_id,
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "git_commit": _git_value("rev-parse", "HEAD"),
        "repository_clean": not bool(_git_value("status", "--porcelain")),
        "board": "nucleo_f401re",
        "build_configuration": {
            "app": "deadline_lab", "workload_mode": mode,
            "architecture": "armv7e-m", "float_abi": "soft",
        },
        "toolchain_version": _tool_version([str(compiler), "--version"]),
        "renode_version": _tool_version([str(renode), "--version"]),
        "emulator_arguments": command_text.strip(),
        "execution_limits": {
            "host_timeout_seconds": int(DEMO_HOST_TIMEOUT_SECONDS),
            "virtual_duration_seconds": DEMO_VIRTUAL_DURATION_SECONDS,
            "collector_timeout_seconds": DEMO_OUTER_TIMEOUT_SECONDS,
        },
        "trace_schema_version": decoded["schema_version"],
        "trace_framing_version": decoded["framing_version"],
        "random_seed": None,
        "timing_claims": "functional_emulator_evidence_only",
        "files": {
            name: _sha256(mode_dir / name) for name in (
                "workload.yaml", "firmware.elf", "firmware.map",
                "build-metadata.txt", "trace.bin", "trace.json",
                "summary.json", "timeline.html", "emulator.log",
                "command.txt", "metadata.txt", "uart.bin", "uart.txt",
                "uart-validation.log")
        },
    }
    _write_json(mode_dir / "metadata.json", metadata)
    return summary


def run_demo() -> Path:
    run_id, run_root = _new_run_directory()
    try:
        summaries = {
            mode: _run_mode(run_id, run_root, mode)
            for mode in ("normal", "overload")
        }
        index = '''<!doctype html><html lang="en"><head><meta charset="utf-8">
<title>AymOS scheduling report</title><style>body{font:18px system-ui;background:#08111f;color:#e5eef7;max-width:760px;margin:60px auto;padding:20px}a{display:block;color:#38bdf8;background:#111827;border:1px solid #263244;border-radius:12px;padding:20px;margin:14px 0;text-decoration:none}</style></head><body><h1>AymOS scheduling report</h1><a href="normal/timeline.html">Normal mode — all deadlines met</a><a href="overload/timeline.html">Overload mode — first deadline miss</a></body></html>'''
        (run_root / "index.html").write_text(index, encoding="utf-8")
        _write_json(run_root / "summary.json", {
            "schema_version": 1, "run_id": run_id,
            "normal": summaries["normal"],
            "overload": summaries["overload"],
        })
        (run_root / RUN_MARKER).write_text(
            "schema=1\nstate=complete\n", encoding="ascii")
    except BaseException:
        (run_root / RUN_MARKER).write_text(
            "schema=1\nstate=failed\n", encoding="ascii")
        raise
    finally:
        _prune_runs(run_root)
    return run_root


def validate_capture(capture: Path, mode: str) -> tuple[dict[str, Any], bytes]:
    decoded, raw_records = decode(read_bounded(capture))
    validate_workload(decoded, mode)
    return decoded, raw_records


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("demo", help="run normal and overload modes")
    arguments = parser.parse_args(argv)
    try:
        if arguments.command == "demo":
            output = run_demo()
            print(f"AymOS scheduling reports: {output}")
            return 0
    except (DeadlineLabError, OSError, subprocess.SubprocessError) as error:
        print(f"scheduling: {error}", file=sys.stderr)
        return 1
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
