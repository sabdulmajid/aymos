import unittest
from pathlib import Path
import os
import signal
import subprocess
import tempfile
from unittest import mock

from tools.aymos_lab import deadline_lab
from tools.aymos_lab.deadline_lab import (
    DeadlineLabError,
    EXPECTED_INTERVALS,
    EXPECTED_SELECTIONS,
    demo_environment,
    explain_selection,
    render_timeline,
    running_intervals,
    validate_schedule_projection,
    validate_workload,
    workload,
)


def context(sequence, tick, outgoing, incoming):
    return {
        "event": "context_switch", "sequence": sequence, "tick": tick,
        "task": outgoing, "related": incoming,
    }


def projected_schedule(mode):
    records = []
    sequence = 0
    for (tick, selected, incumbent, purpose, reason, ready_mask, excluded,
         candidates) in EXPECTED_SELECTIONS[mode]:
        records.append({
            "event": "task_select", "sequence": sequence, "tick": tick,
            "task": selected, "related": incumbent,
            "details": {
                "purpose": purpose, "reason": reason,
                "ready_mask": ready_mask, "excluded": excluded,
                "candidate_count": len(candidates),
            },
        })
        sequence += 1
        for (task, deadline, priority, state, excluded_marker,
             incumbent_marker, eligible) in candidates:
            records.append({
                "event": "select_candidate", "sequence": sequence,
                "tick": tick, "task": task, "related": selected,
                "details": {
                    "absolute_deadline": deadline, "priority": priority,
                    "state": state, "excluded": excluded_marker,
                    "incumbent": incumbent_marker, "eligible": eligible,
                },
            })
            sequence += 1
        if purpose == "dispatch":
            records.append(context(sequence, tick, incumbent, selected))
            sequence += 1
    return {"records": records, "footer": {"final_tick": 21}}


class DeadlineLabModelTests(unittest.TestCase):
    def test_only_load_demand_changes_between_modes(self):
        normal = workload("normal")
        overload = workload("overload")
        self.assertEqual(normal["load"]["execution_ticks"], 1)
        self.assertEqual(overload["load"]["execution_ticks"], 8)
        normal["load"]["execution_ticks"] = 99
        self.assertEqual(workload("normal")["load"]["execution_ticks"], 1)
        for name in ("sampler", "controller", "telemetry"):
            self.assertEqual(normal[name], overload[name])

    def test_unknown_mode_fails_closed(self):
        with self.assertRaisesRegex(DeadlineLabError, "unknown.*mode"):
            workload("fast")

    def test_context_switches_form_positive_intervals(self):
        decoded = {
            "records": [
                context(0, 0, None, 1),
                context(1, 2, 1, 0),
                context(2, 2, 0, 2),
                context(3, 5, 2, 0),
            ],
            "footer": {"final_tick": 8},
        }
        self.assertEqual(running_intervals(decoded), [
            {"task": 1, "start": 0, "end": 2},
            {"task": 2, "start": 2, "end": 5},
            {"task": 0, "start": 5, "end": 8},
        ])

    def test_interval_model_rejects_reverse_time(self):
        decoded = {
            "records": [context(0, 3, None, 1),
                        context(1, 2, 1, 0)],
            "footer": {"final_tick": 4},
        }
        with self.assertRaisesRegex(DeadlineLabError, "not monotonic"):
            running_intervals(decoded)

    def test_selection_explanation_uses_emitted_candidates(self):
        record = {
            "task": 1,
            "details": {"reason": "deadline"},
        }
        candidates = [
            {"task": 1, "details": {"absolute_deadline": 3,
                                      "priority": 0}},
            {"task": 4, "details": {"absolute_deadline": 12,
                                      "priority": 3}},
        ]
        explanation = explain_selection(record, candidates)
        self.assertIn("earliest absolute deadline", explanation)
        self.assertIn("sampler (deadline 3, priority 0)", explanation)
        self.assertIn("load (deadline 12, priority 3)", explanation)

    def test_idle_explanation_does_not_invent_candidates(self):
        explanation = explain_selection(
            {"task": 0, "details": {"reason": "idle"}}, [])
        self.assertEqual(
            explanation,
            "The kernel selected idle because no user task was ready.")

    def test_capacity_gate_runs_before_workload_assertions(self):
        decoded = {
            "record_count": 256,
            "footer": {"final_tick": 21, "dropped": 0},
            "records": [],
        }
        with self.assertRaisesRegex(DeadlineLabError, "capacity is 256"):
            validate_workload(decoded, "normal")

    def test_report_escapes_selection_text(self):
        decoded = {
            "records": [context(0, 0, None, 0)],
            "footer": {"final_tick": 1},
        }
        summary = {
            "mode": "normal", "result": "all_deadlines_met",
            "final_tick": 1, "context_switches": 1, "idle_ticks": 1,
            "deadline_misses": 0, "first_deadline_miss": None,
            "tasks": {
                str(task): {
                    "name": name, "released_jobs": 0, "running_ticks": 0,
                    "preemptions": 0, "deadlines_met": 0,
                    "deadline_misses": 0,
                }
                for task, name in enumerate(
                    ("idle", "sampler", "controller", "telemetry", "load"))
            },
            "selection_decisions": [{
                "tick": 0, "purpose": "dispatch", "selected": 0,
                "reason": "idle", "explanation": "<script>bad()</script>",
            }],
        }
        report = render_timeline(decoded, summary)
        self.assertNotIn("<script>bad()", report)
        self.assertIn("&lt;script&gt;bad()&lt;/script&gt;", report)

    def test_exact_overload_interval_order_is_required(self):
        decoded = projected_schedule("overload")
        validate_schedule_projection(decoded, "overload")
        switches = [item for item in decoded["records"]
                    if item["event"] == "context_switch"]
        switches[5]["related"] = 3
        switches[6]["task"] = 3
        switches[6]["related"] = 1
        switches[7]["task"] = 1
        with self.assertRaisesRegex(DeadlineLabError,
                                    "running interval projection"):
            validate_schedule_projection(decoded, "overload")

    def test_exact_selection_projection_is_required(self):
        decoded = projected_schedule("normal")
        validate_schedule_projection(decoded, "normal")
        selection = next(item for item in decoded["records"]
                         if item["event"] == "task_select" and
                         item["tick"] == 12)
        selection["details"]["reason"] = "priority"
        with self.assertRaisesRegex(DeadlineLabError,
                                    "selection projection"):
            validate_schedule_projection(decoded, "normal")

    def test_dispatch_must_precede_its_committed_switch(self):
        decoded = projected_schedule("normal")
        dispatch = next(item for item in decoded["records"]
                        if item["event"] == "task_select" and
                        item["details"]["purpose"] == "dispatch")
        switch = next(item for item in decoded["records"]
                      if item["event"] == "context_switch")
        dispatch["sequence"], switch["sequence"] = (
            switch["sequence"], dispatch["sequence"])
        with self.assertRaisesRegex(DeadlineLabError,
                                    "does not precede"):
            validate_schedule_projection(decoded, "normal")

    def test_demo_environment_overrides_timeout_inputs(self):
        with mock.patch.dict(os.environ, {
                "AYMOS_RENODE_HOST_TIMEOUT": "300",
                "AYMOS_RENODE_VIRTUAL_DURATION": "9"}):
            environment = demo_environment("overload", Path("run"))
        self.assertEqual(environment["AYMOS_RENODE_HOST_TIMEOUT"], "20")
        self.assertEqual(environment["AYMOS_RENODE_VIRTUAL_DURATION"], "0.5")
        self.assertEqual(environment["AYMOS_WORKLOAD_MODE"], "overload")

    def test_outer_timeout_terminates_the_process_group(self):
        process = mock.Mock(pid=42)
        process.wait.side_effect = [
            subprocess.TimeoutExpired(["runner"], 45), 0]
        with mock.patch.object(deadline_lab.subprocess, "Popen",
                               return_value=process) as popen, \
                mock.patch.object(deadline_lab.os, "killpg") as killpg:
            with self.assertRaises(subprocess.TimeoutExpired):
                deadline_lab._run_process_group(["runner"], {})
        self.assertTrue(popen.call_args.kwargs["start_new_session"])
        killpg.assert_called_once_with(42, signal.SIGTERM)

    def test_runs_root_symlink_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            repository = Path(temporary) / "repo"
            external = Path(temporary) / "external"
            repository.mkdir()
            external.mkdir()
            (repository / "runs").symlink_to(external, target_is_directory=True)
            with mock.patch.object(deadline_lab, "REPOSITORY_ROOT", repository), \
                    mock.patch.object(deadline_lab, "RUNS_ROOT",
                                      repository / "runs"):
                with self.assertRaisesRegex(DeadlineLabError, "symlink"):
                    deadline_lab._new_run_directory()

    def test_pruning_preserves_an_active_concurrent_run(self):
        with tempfile.TemporaryDirectory() as temporary:
            repository = Path(temporary) / "repo"
            runs = repository / "runs"
            runs.mkdir(parents=True)
            current = runs / "20260101T000020Z-20-00000014"
            for index in range(10, 21):
                path = runs / f"20260101T0000{index}Z-{index}-{index:08x}"
                path.mkdir()
                (path / deadline_lab.RUN_MARKER).write_text(
                    "schema=1\nstate=complete\n", encoding="ascii")
            active = runs / "20260101T000001Z-1-00000001"
            active.mkdir()
            (active / deadline_lab.RUN_MARKER).write_text(
                "schema=1\nstate=active\n", encoding="ascii")
            with mock.patch.object(deadline_lab, "REPOSITORY_ROOT", repository), \
                    mock.patch.object(deadline_lab, "RUNS_ROOT", runs):
                deadline_lab._prune_runs(current)
            self.assertTrue(active.is_dir())
            self.assertTrue(current.is_dir())


if __name__ == "__main__":
    unittest.main()
