import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import unittest
from unittest import mock

from tools.aymos_lab import signal_lab
from tools.aymos_lab.signal_lab import (
    EXPECTED_RECORD_COUNT,
    EXPECTED_SEMANTIC_PROJECTION,
    SignalLabError,
    parse_result,
    semantic_projection,
    signal_environment,
    validate_schedule,
)


VALID_RESULT = (
    b"AYMOS SIGNAL PASS IMPL=m4 FRAMES=4 SAMPLES=128 TAPS=16 "
    b"VALID=113 OUTPUTS=452 WORK=7232 SEED=0x1A2B3C4D "
    b"INPUT_CRC=0x0CAF72FD OUTPUT_CRC=0xAFC277C1\r\n"
)


def expected_decoded():
    records = [
        {
            "event": event,
            "task": task,
            "related": related,
            "tick": tick,
            "value0": value0,
            "value1": value1,
            "value2": value2,
        }
        for event, task, related, tick, value0, value1, value2
        in EXPECTED_SEMANTIC_PROJECTION
    ]
    return {
        "record_count": EXPECTED_RECORD_COUNT,
        "footer": {
            "attempted": EXPECTED_RECORD_COUNT,
            "emitted": EXPECTED_RECORD_COUNT,
            "dropped": 0,
            "final_sequence": EXPECTED_RECORD_COUNT - 1,
            "flags": 0,
            "final_tick": 21,
        },
        "records": records,
    }


def record_at(decoded, event, tick, task):
    return next(
        record for record in decoded["records"]
        if record["event"] == event and record["tick"] == tick
        and record["task"] == task
    )


def execution_trace(entries, target=b"thumb cortex-m4"):
    data = bytearray(b"ReTrace\x04")
    data.extend((4, 1, 0, len(target)))
    data.extend(target)
    for pc, opcode in entries:
        data.extend(pc.to_bytes(4, "little"))
        data.append(len(opcode))
        data.extend(opcode)
        data.append(0)
    return bytes(data)


def marked_directory(path, payload):
    path.mkdir()
    (path / signal_lab.EVIDENCE_MARKER).write_text(
        "schema=1\n", encoding="ascii"
    )
    (path / "payload.txt").write_text(payload, encoding="ascii")


class SignalLabTests(unittest.TestCase):
    def test_dsp_firmware_flags_reject_command_line_override(self):
        result = subprocess.run(
            [
                "make", "-pn", "APP=signal_lab", "SIGNAL_IMPL=m4",
                "DSP_FIRMWARE_CFLAGS=-O0",
            ],
            cwd=signal_lab.REPOSITORY_ROOT,
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        match = re.search(
            r"^DSP_FIRMWARE_CFLAGS := (.+)$", result.stdout, re.MULTILINE
        )
        self.assertIsNotNone(match)
        flags = match.group(1).split()
        self.assertIn("-mcpu=cortex-m4", flags)
        self.assertIn("-mthumb", flags)
        self.assertIn("-mfloat-abi=soft", flags)
        self.assertNotIn("-Og", flags)
        self.assertNotIn("-O0", flags)
        self.assertEqual(flags[-1], "-O2")

    def test_exact_result_contract(self):
        result = parse_result(VALID_RESULT, "m4")
        self.assertEqual(result["output_crc32"], 0xAFC277C1)
        self.assertEqual(result["work_units"], 7232)

    def test_result_rejects_wrong_implementation_and_extra_bytes(self):
        with self.assertRaisesRegex(SignalLabError, "result"):
            parse_result(VALID_RESULT, "scalar")
        with self.assertRaisesRegex(SignalLabError, "exact ASCII"):
            parse_result(VALID_RESULT + b"extra", "m4")

    def test_complete_schedule_contract(self):
        decoded = expected_decoded()
        validate_schedule(decoded)
        self.assertEqual(
            semantic_projection(decoded), EXPECTED_SEMANTIC_PROJECTION)

    def test_schedule_rejects_reordered_equal_work(self):
        decoded = expected_decoded()
        first = next(
            index for index, record in enumerate(decoded["records"])
            if record["event"] == "task_release" and record["tick"] == 13
        )
        second = next(
            index for index, record in enumerate(decoded["records"])
            if record["event"] == "deadline_met" and record["tick"] == 13
        )
        decoded["records"][first], decoded["records"][second] = (
            decoded["records"][second], decoded["records"][first]
        )
        with self.assertRaisesRegex(SignalLabError, "semantic record projection"):
            validate_schedule(decoded)

    def test_schedule_rejects_changed_semantic_details(self):
        mutations = (
            ("task-create initial state", "task_create", 0, 1, "value2", 1),
            ("deadline and job", "deadline_met", 13, 1, "value2", 2),
            ("next release", "task_wait_period", 13, 1, "value0", 17),
            ("switch cause", "context_switch", 13, 1, "value0", 4),
            ("exit counter", "task_exit", 19, 1, "value0", 3),
            ("selection reason", "task_select", 12, 1, "value1", 0x01FF0203),
        )
        for label, event, tick, task, field, value in mutations:
            with self.subTest(label=label):
                decoded = expected_decoded()
                record_at(decoded, event, tick, task)[field] = value
                with self.assertRaisesRegex(
                        SignalLabError, "semantic record projection"):
                    validate_schedule(decoded)

    def test_retrace_parser_counts_exact_opcode_hits(self):
        watched = {0x08000100: b"\x01\x20"}
        data = execution_trace([
            (0x08000100, b"\x01\x20"),
            (0x08000102, b"\x00\xbf"),
            (0x08000100, b"\x01\x20"),
        ])
        count, hits, function_hits = signal_lab._parse_execution_entries(
            data, watched, (0x08000100, 4)
        )
        self.assertEqual(count, 3)
        self.assertEqual(hits[0x08000100], 2)
        self.assertEqual(function_hits, 3)

    def test_retrace_parser_rejects_opcode_and_target_mutations(self):
        with self.assertRaisesRegex(SignalLabError, "opcode mismatch"):
            signal_lab._parse_execution_entries(
                execution_trace([(0x100, b"\x00\xbf")]),
                {0x100: b"\x01\x20"}, (0x100, 2),
            )
        with self.assertRaisesRegex(SignalLabError, "thumb cortex-m4"):
            signal_lab._parse_execution_entries(
                execution_trace([], target=b"thumb cortex-m3"), {}, (0, 0)
            )

    def test_execution_reader_reports_corrupt_gzip(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "execution.bin.gz"
            path.write_bytes(b"\x1f\x8b\x08\x00corrupt")
            with self.assertRaisesRegex(SignalLabError, "invalid gzip"):
                signal_lab._read_execution_trace(path)

    def test_json_reader_reports_corrupt_data(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "result.json"
            path.write_text("{not-json", encoding="ascii")
            with self.assertRaisesRegex(SignalLabError, "invalid JSON"):
                signal_lab._read_json(path)

    def test_missing_metadata_key_has_a_controlled_error(self):
        with self.assertRaisesRegex(SignalLabError, "missing git_commit"):
            signal_lab._expect_metadata(
                {"app": "signal_lab"}, {"git_commit": "expected"},
                "run metadata",
            )

    def test_signal_environment_pins_bounded_run_inputs(self):
        with mock.patch.dict(os.environ, {
                "AYMOS_RENODE_HOST_TIMEOUT": "300",
                "AYMOS_RENODE_VIRTUAL_DURATION": "9"}):
            environment = signal_environment(
                "m4", Path("out"), Path("snapshot.elf")
            )
        self.assertEqual(environment["AYMOS_RENODE_HOST_TIMEOUT"], "25")
        self.assertEqual(environment["AYMOS_RENODE_VIRTUAL_DURATION"], "0.03")
        self.assertEqual(environment["AYMOS_SIGNAL_IMPL"], "m4")
        self.assertEqual(environment["AYMOS_FIRMWARE_ELF"], "snapshot.elf")

    def test_outer_timeout_terminates_the_process_group(self):
        process = mock.Mock(pid=42)
        process.wait.side_effect = [
            subprocess.TimeoutExpired(["runner"], 45), 0]
        with mock.patch.object(signal_lab.subprocess, "Popen",
                               return_value=process) as popen, \
                mock.patch.object(signal_lab.os, "killpg") as killpg:
            with self.assertRaises(subprocess.TimeoutExpired):
                signal_lab._run_process_group(["runner"], {})
        self.assertTrue(popen.call_args.kwargs["start_new_session"])
        killpg.assert_called_once_with(42, signal.SIGTERM)

    def test_evidence_symlink_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            repository = Path(temporary) / "repo"
            external = Path(temporary) / "external"
            (repository / "build/signal-lab").mkdir(parents=True)
            external.mkdir()
            evidence = repository / "build/signal-lab/evidence"
            evidence.symlink_to(external, target_is_directory=True)
            stage = repository / "build/signal-lab/.evidence-stage-1"
            marked_directory(stage, "new")
            with mock.patch.object(signal_lab, "REPOSITORY_ROOT", repository):
                with self.assertRaisesRegex(SignalLabError, "unsafe"):
                    signal_lab._replace_evidence(stage, evidence)

    def test_evidence_replacement_keeps_only_new_complete_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            repository = Path(temporary) / "repo"
            base = repository / "build/signal-lab"
            base.mkdir(parents=True)
            evidence = base / "evidence"
            stage = base / ".evidence-stage-1"
            marked_directory(evidence, "old")
            marked_directory(stage, "new")
            with mock.patch.object(signal_lab, "REPOSITORY_ROOT", repository):
                signal_lab._replace_evidence(stage, evidence)
            self.assertEqual(
                (evidence / "payload.txt").read_text(encoding="ascii"), "new"
            )
            self.assertFalse(stage.exists())
            self.assertFalse((base / ".evidence-backup").exists())

    def test_evidence_replacement_rolls_back_failed_install(self):
        with tempfile.TemporaryDirectory() as temporary:
            repository = Path(temporary) / "repo"
            base = repository / "build/signal-lab"
            base.mkdir(parents=True)
            evidence = base / "evidence"
            stage = base / ".evidence-stage-1"
            marked_directory(evidence, "old")
            marked_directory(stage, "new")
            real_replace = os.replace

            def fail_stage_install(source, destination):
                if Path(source) == stage and Path(destination) == evidence:
                    raise OSError("injected install failure")
                return real_replace(source, destination)

            with mock.patch.object(signal_lab, "REPOSITORY_ROOT", repository), \
                    mock.patch.object(signal_lab.os, "replace",
                                      side_effect=fail_stage_install):
                with self.assertRaisesRegex(OSError, "injected"):
                    signal_lab._replace_evidence(stage, evidence)
            self.assertEqual(
                (evidence / "payload.txt").read_text(encoding="ascii"), "old"
            )
            self.assertEqual(
                (stage / "payload.txt").read_text(encoding="ascii"), "new"
            )
            self.assertFalse((base / ".evidence-backup").exists())

    def test_live_build_input_change_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "firmware.elf"
            snapshot = root / "snapshot.elf"
            source.write_bytes(b"first")
            state = signal_lab._copy_stable_file(source, snapshot)
            signal_lab._assert_build_inputs_unchanged({"firmware.elf": state})
            self.assertEqual(snapshot.read_bytes(), b"first")
            source.write_bytes(b"second")
            with self.assertRaisesRegex(SignalLabError, "changed"):
                signal_lab._assert_build_inputs_unchanged({
                    "firmware.elf": state,
                })


if __name__ == "__main__":
    unittest.main()
