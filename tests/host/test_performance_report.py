import hashlib
import gzip
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from tools.aymos_lab import performance_report, signal_lab


def json_bytes(value):
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")


def expected_trace():
    records = [
        {
            "event": event, "task": task, "related": related, "tick": tick,
            "value0": value0, "value1": value1, "value2": value2,
        }
        for event, task, related, tick, value0, value1, value2
        in signal_lab.EXPECTED_SEMANTIC_PROJECTION
    ]
    return {
        "schema_version": 1,
        "framing_version": 1,
        "record_size": 32,
        "record_count": signal_lab.EXPECTED_RECORD_COUNT,
        "footer": {
            "attempted": signal_lab.EXPECTED_RECORD_COUNT,
            "emitted": signal_lab.EXPECTED_RECORD_COUNT,
            "dropped": 0,
            "final_sequence": signal_lab.EXPECTED_RECORD_COUNT - 1,
            "flags": 0,
            "final_tick": signal_lab.FINAL_TICK,
        },
        "records": records,
    }


def fixed_result(implementation):
    return {
        "frames": 4, "implementation": implementation,
        "input_crc32": signal_lab.INPUT_CRC,
        "output_crc32": signal_lab.OUTPUT_CRC,
        "samples_per_frame": 128, "seed": signal_lab.SEED,
        "status": "PASS", "taps": 16, "total_outputs": 452,
        "valid_outputs_per_frame": 113, "work_units": 7232,
    }


def execution_summary(implementation, execution_gzip):
    if implementation == "scalar":
        dynamic = {
            "smlalbb": {
                "address": "0x08001000", "opcode_hex": "8c01cefb",
                "executed_count": 7232,
            }
        }
        function = "aymos_fir_q15_scalar"
        hits = 42140
    else:
        dynamic = {
            "smlald": {
                "address": "0x08002000", "opcode_hex": "cbc2cefb",
                "executed_count": 3616,
            },
            "ssat": {
                "address": "0x08002004", "opcode_hex": "0f0000f3",
                "executed_count": 452,
            },
        }
        function = "aymos_fir_q15_m4"
        hits = 60252
    return {
        "claim": "functional executed-instruction evidence; not a timing measurement",
        "compressed_bytes": len(execution_gzip),
        "dynamic_instructions": dynamic,
        "entry_count": 1,
        "format": "ReTrace-v4-PCAndOpcode",
        "function_address": "0x08001000",
        "function_call_count": 4,
        "function_instruction_hits": hits,
        "function_size_bytes": 100,
        "schema_version": 1,
        "selected_function": function,
        "target": "thumb cortex-m4",
        "timing_claims": "none",
        "uncompressed_bytes": len(gzip.decompress(execution_gzip)),
    }


def metadata_text(implementation, commit, firmware_hash):
    return (
        "schema=1\nboard=nucleo_f401re\napp=signal_lab\n"
        f"signal_impl={implementation}\ngit_commit={commit}\n"
        "repository_clean=true\n"
        f"firmware_sha256={firmware_hash}\n"
        f"architecture_flags={signal_lab.EXPECTED_ARCHITECTURE_FLAGS}\n"
        "float_abi=soft\nrenode_version=1.16.1\n"
        "trace_schema_version=1\n"
        "execution_trace_format=ReTrace-v4-PCAndOpcode\n"
        f"host_timeout_seconds={signal_lab.RENODE_HOST_TIMEOUT_SECONDS}\n"
        f"virtual_duration_seconds={signal_lab.RENODE_VIRTUAL_DURATION_SECONDS}\n"
        "emulator_arguments=--console --disable-gui --plain\n"
        "timing_claims=none_emulator_functional_test_only\n"
    ).encode("utf-8")


def build_metadata_text(implementation, commit):
    return (
        "project=aymos\nboard=nucleo_f401re\napp=signal_lab\n"
        f"git_commit={commit}\nrepository_clean=true\n"
        "dependencies_verified=true\ndependencies_clean=true\n"
        f"compiler={signal_lab.EXPECTED_TOOLCHAIN_VERSION}\n"
        f"architecture_flags={signal_lab.EXPECTED_ARCHITECTURE_FLAGS}\n"
        "float_abi=soft\ndsp_optimization=-O2\nrenode=1.16.1\n"
        f"signal_impl={implementation}\ntrace_schema_version=1\n"
    ).encode("utf-8")


def create_evidence(root):
    commit = "d36d3aa15b59d46b7da4a9d8fbea5ab781468cf9"
    root.mkdir(parents=True)
    (root / performance_report.EVIDENCE_MARKER).write_text(
        "schema=1\n", encoding="ascii"
    )
    runs = []
    common_trace_json = json_bytes(expected_trace())
    target = b"thumb cortex-m4"
    execution_raw = (
        b"ReTrace\x04" + bytes((4, 1, 0, len(target))) + target
        + (0x08000000).to_bytes(4, "little") + bytes((2,))
        + b"\x00\xbf" + bytes((0,))
    )
    execution_gzip = gzip.compress(execution_raw, mtime=0)
    for implementation in signal_lab.IMPLEMENTATIONS:
        impl_root = root / implementation
        impl_root.mkdir()
        firmware = f"ELF-{implementation}".encode("ascii")
        firmware_hash = hashlib.sha256(firmware).hexdigest()
        result = fixed_result(implementation)
        execution = execution_summary(implementation, execution_gzip)
        values = {
            "firmware.elf": firmware,
            "firmware.map": f"MAP-{implementation}".encode("ascii"),
            "build-metadata.txt": build_metadata_text(implementation, commit),
            "metadata.txt": metadata_text(implementation, commit, firmware_hash),
            "command.txt": b"renode --console\n",
            "emulator.log": b"done\n",
            "execution-summary.json": json_bytes(execution),
            "execution.bin.gz": execution_gzip,
            "result.json": json_bytes(result),
            "trace.bin": b"same trace bytes",
            "trace.json": common_trace_json,
            "uart-validation.log": b"",
            "uart.bin": b"same trace plus result",
            "uart.txt": b"AYMOS SIGNAL PASS\n",
        }
        manifest = {}
        for name in performance_report.ARTIFACT_NAMES:
            data = values[name]
            (impl_root / name).write_bytes(data)
            manifest[name] = {
                "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
            }
        runs.append({
            "artifacts": manifest,
            "build_configuration": {
                "app": "signal_lab", "board": "nucleo_f401re",
                "float_abi": "soft", "signal_impl": implementation,
            },
            "execution": execution,
            "execution_limits": {
                "compressed_trace_bytes": signal_lab.MAX_EXECUTION_COMPRESSED_BYTES,
                "execution_entries": signal_lab.MAX_EXECUTION_ENTRIES,
                "host_timeout_seconds": int(signal_lab.RENODE_HOST_TIMEOUT_SECONDS),
                "outer_timeout_seconds": signal_lab.OUTER_TIMEOUT_SECONDS,
                "uncompressed_trace_bytes": signal_lab.MAX_EXECUTION_BYTES,
                "virtual_duration_seconds": signal_lab.RENODE_VIRTUAL_DURATION_SECONDS,
            },
            "git_commit": commit,
            "implementation": implementation,
            "record_count": signal_lab.EXPECTED_RECORD_COUNT,
            "renode_version": "1.16.1",
            "repository_clean": True,
            "result": result,
            "toolchain_version": signal_lab.EXPECTED_TOOLCHAIN_VERSION,
        })
    summary = {
        "board": "nucleo_f401re",
        "random_seed": "0x1A2B3C4D",
        "result_equality": True,
        "runs": runs,
        "schema_version": 1,
        "structured_trace_equality": True,
        "timing_claims": "none_emulator_functional_test_only",
        "trace_schema_version": 1,
    }
    (root / "summary.json").write_bytes(json_bytes(summary))


def replace_artifact(root, implementation, name, data):
    path = root / implementation / name
    path.write_bytes(data)
    summary_path = root / "summary.json"
    summary = json.loads(summary_path.read_text())
    run = next(
        item for item in summary["runs"]
        if item["implementation"] == implementation
    )
    run["artifacts"][name] = {
        "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
    }
    summary_path.write_bytes(json_bytes(summary))


class PerformanceReportTests(unittest.TestCase):
    def setUp(self):
        self.real_uart_recheck = performance_report._validate_uart_evidence
        self.execution_recheck = mock.patch.object(
            performance_report, "_reanalyze_execution",
            side_effect=lambda files, implementation, expected: expected,
        )
        self.uart_recheck = mock.patch.object(
            performance_report, "_validate_uart_evidence",
            return_value=None,
        )
        self.execution_recheck.start()
        self.uart_recheck.start()

    def tearDown(self):
        self.uart_recheck.stop()
        self.execution_recheck.stop()

    def test_valid_evidence_builds_complete_model_and_visuals(self):
        with tempfile.TemporaryDirectory() as temporary:
            evidence_root = Path(temporary) / "evidence"
            create_evidence(evidence_root)
            model = performance_report.build_report_model(
                performance_report.load_evidence(evidence_root)
            )
            self.assertEqual(model["result"]["total_outputs"], 452)
            self.assertEqual(
                model["instruction_evidence"]["m4"]["packed_smlald"], 3616
            )
            self.assertEqual(len(model["schedule"]["intervals"]), 15)
            svg = performance_report.render_svg(model)
            self.assertIn("60,252 total FIR-body", svg)
            self.assertIn('x="1020.0" y="21"', svg)
            self.assertIn('font-size="10">23</text>', svg)
            self.assertIn("not CPU-active time", performance_report.render_html(model))

    def test_hash_and_size_mutations_fail_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            evidence_root = Path(temporary) / "evidence"
            create_evidence(evidence_root)
            (evidence_root / "scalar/firmware.elf").write_bytes(b"changed")
            with self.assertRaisesRegex(performance_report.PerformanceReportError,
                                        "byte size"):
                performance_report.load_evidence(evidence_root)
        with tempfile.TemporaryDirectory() as temporary:
            evidence_root = Path(temporary) / "evidence"
            create_evidence(evidence_root)
            summary_path = evidence_root / "summary.json"
            summary = json.loads(summary_path.read_text())
            summary["runs"][0]["artifacts"]["firmware.elf"]["bytes"] = (
                performance_report.ARTIFACT_LIMITS["firmware.elf"] + 1
            )
            summary_path.write_bytes(json_bytes(summary))
            with self.assertRaisesRegex(performance_report.PerformanceReportError,
                                        "byte limit"):
                performance_report.load_evidence(evidence_root)

    def test_unexpected_and_symlink_inputs_fail_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            evidence_root = base / "evidence"
            create_evidence(evidence_root)
            (evidence_root / "unexpected").write_text("x", encoding="ascii")
            with self.assertRaisesRegex(performance_report.PerformanceReportError,
                                        "root entries"):
                performance_report.load_evidence(evidence_root)
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            real_root = base / "real"
            create_evidence(real_root)
            link = base / "link"
            link.symlink_to(real_root, target_is_directory=True)
            with self.assertRaisesRegex(performance_report.PerformanceReportError,
                                        "cannot open"):
                performance_report.load_evidence(link)

    def test_same_length_artifact_mutation_reaches_sha256_check(self):
        with tempfile.TemporaryDirectory() as temporary:
            evidence_root = Path(temporary) / "evidence"
            create_evidence(evidence_root)
            path = evidence_root / "scalar/firmware.elf"
            original = path.read_bytes()
            path.write_bytes(bytes([original[0] ^ 1]) + original[1:])
            with self.assertRaisesRegex(performance_report.PerformanceReportError,
                                        "SHA-256"):
                performance_report.load_evidence(evidence_root)

    def test_strict_json_types_duplicate_keys_and_constants(self):
        mutations = (
            ("schema_version", True),
            ("trace_schema_version", 1.0),
        )
        for field, value in mutations:
            with self.subTest(field=field), tempfile.TemporaryDirectory() as temporary:
                evidence_root = Path(temporary) / "evidence"
                create_evidence(evidence_root)
                summary_path = evidence_root / "summary.json"
                summary = json.loads(summary_path.read_text())
                summary[field] = value
                summary_path.write_bytes(json_bytes(summary))
                with self.assertRaisesRegex(
                    performance_report.PerformanceReportError, "type mismatch"
                ):
                    performance_report.load_evidence(evidence_root)

        for label, data in (
            ("duplicate", b'{"schema_version":1,"schema_version":1}'),
            ("constant", b'{"schema_version":NaN}'),
        ):
            with self.subTest(label=label):
                with self.assertRaises(performance_report.PerformanceReportError):
                    performance_report._json_bytes(data, label)

    def test_rehashed_result_and_execution_type_mutations_are_rejected(self):
        mutations = ("result", "execution")
        for mutation in mutations:
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                evidence_root = Path(temporary) / "evidence"
                create_evidence(evidence_root)
                summary_path = evidence_root / "summary.json"
                summary = json.loads(summary_path.read_text())
                run = summary["runs"][0]
                if mutation == "result":
                    run["result"]["input_crc32"] = float(signal_lab.INPUT_CRC)
                    name = "result.json"
                    artifact_value = run["result"]
                else:
                    run["execution"]["schema_version"] = True
                    name = "execution-summary.json"
                    artifact_value = run["execution"]
                data = json_bytes(artifact_value)
                (evidence_root / "scalar" / name).write_bytes(data)
                run["artifacts"][name] = {
                    "bytes": len(data),
                    "sha256": hashlib.sha256(data).hexdigest(),
                }
                summary_path.write_bytes(json_bytes(summary))
                with self.assertRaisesRegex(
                    performance_report.PerformanceReportError, "type mismatch"
                ):
                    performance_report.load_evidence(evidence_root)

    def test_uart_comparison_is_type_strict(self):
        trace = expected_trace()
        result = fixed_result("scalar")
        files = {
            "uart.bin": b"prefix" + performance_report.trace_tool.TRAILER,
            "trace.bin": b"records",
            "uart.txt": (
                b"AYMOS SIGNAL LAB PASS IMPL=scalar RECORDS=103 "
                b"OUTPUT_CRC=0xAFC277C1\n"
            ),
            "uart-validation.log": b"",
        }
        changed_trace = json.loads(json.dumps(trace))
        changed_trace["schema_version"] = True
        with mock.patch.object(
                performance_report.trace_tool, "decode",
                return_value=(trace, b"records")), \
                mock.patch.object(
                    performance_report.signal_lab, "parse_result",
                    return_value=result), \
                mock.patch.object(
                    performance_report.signal_lab, "validate_schedule"):
            with self.assertRaisesRegex(
                performance_report.PerformanceReportError, "type mismatch"
            ):
                self.real_uart_recheck(
                    files, "scalar", changed_trace, result
                )

    def test_evidence_root_descriptor_resists_namespace_swap(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            evidence_root = base / "evidence"
            moved = base / "moved"
            external = base / "external"
            create_evidence(evidence_root)
            external.mkdir()
            (external / "summary.json").write_text("external", encoding="ascii")
            original_validate = performance_report._validate_evidence_root

            def swap_then_validate(descriptor):
                evidence_root.rename(moved)
                evidence_root.symlink_to(external, target_is_directory=True)
                return original_validate(descriptor)

            with mock.patch.object(
                performance_report, "_validate_evidence_root",
                side_effect=swap_then_validate,
            ):
                evidence = performance_report.load_evidence(evidence_root)
            self.assertEqual(
                evidence["git_commit"],
                "d36d3aa15b59d46b7da4a9d8fbea5ab781468cf9",
            )
            self.assertEqual(
                (external / "summary.json").read_text(encoding="ascii"),
                "external",
            )

    def test_schema_result_and_trace_mutations_fail_closed(self):
        mutations = ("schema", "result", "trace")
        for mutation in mutations:
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                evidence_root = Path(temporary) / "evidence"
                create_evidence(evidence_root)
                summary_path = evidence_root / "summary.json"
                summary = json.loads(summary_path.read_text())
                if mutation == "schema":
                    summary["extra"] = True
                    summary_path.write_bytes(json_bytes(summary))
                elif mutation == "result":
                    summary["runs"][1]["result"]["output_crc32"] ^= 1
                    summary_path.write_bytes(json_bytes(summary))
                else:
                    trace_path = evidence_root / "m4/trace.bin"
                    trace_path.write_bytes(b"different")
                    run = summary["runs"][1]
                    run["artifacts"]["trace.bin"] = {
                        "bytes": len(b"different"),
                        "sha256": hashlib.sha256(b"different").hexdigest(),
                    }
                    summary_path.write_bytes(json_bytes(summary))
                with self.assertRaises(performance_report.PerformanceReportError):
                    performance_report.load_evidence(evidence_root)

    def test_execution_recheck_is_an_independent_oracle(self):
        with tempfile.TemporaryDirectory() as temporary:
            evidence_root = Path(temporary) / "evidence"
            create_evidence(evidence_root)

            def reject_changed_raw(_files, implementation, expected):
                changed = dict(expected)
                changed["function_instruction_hits"] += 1
                performance_report._expect(
                    changed, expected, f"{implementation} execution recheck"
                )
                return changed

            with mock.patch.object(
                performance_report, "_reanalyze_execution",
                side_effect=reject_changed_raw,
            ):
                with self.assertRaisesRegex(
                    performance_report.PerformanceReportError,
                    "execution recheck.*mismatch",
                ):
                    performance_report.load_evidence(evidence_root)

    def test_malformed_execution_gzip_is_rejected_after_rehash(self):
        with tempfile.TemporaryDirectory() as temporary:
            evidence_root = Path(temporary) / "evidence"
            create_evidence(evidence_root)
            summary_path = evidence_root / "summary.json"
            summary = json.loads(summary_path.read_text())
            path = evidence_root / "scalar/execution.bin.gz"
            data = b"not-gzip"
            path.write_bytes(data)
            artifact = summary["runs"][0]["artifacts"]["execution.bin.gz"]
            artifact.update(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
            summary["runs"][0]["execution"]["compressed_bytes"] = len(data)
            execution_data = json_bytes(summary["runs"][0]["execution"])
            (evidence_root / "scalar/execution-summary.json").write_bytes(
                execution_data
            )
            execution_artifact = summary["runs"][0]["artifacts"][
                "execution-summary.json"
            ]
            execution_artifact.update(
                bytes=len(execution_data),
                sha256=hashlib.sha256(execution_data).hexdigest(),
            )
            summary_path.write_bytes(json_bytes(summary))
            with self.assertRaisesRegex(performance_report.PerformanceReportError,
                                        "invalid gzip"):
                performance_report.load_evidence(evidence_root)

    def test_publish_is_complete_and_hashes_every_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            repository = Path(temporary) / "repo"
            repository.mkdir()
            evidence_root = repository / "build/signal-lab/evidence"
            create_evidence(evidence_root)
            runs_root = repository / "runs"
            with mock.patch.object(performance_report, "REPOSITORY_ROOT", repository):
                published = performance_report.publish_report(
                    evidence_root, runs_root
                )
            self.assertRegex(published.name, performance_report.RUN_PATTERN)
            self.assertEqual(
                (published / performance_report.REPORT_MARKER).read_text(),
                performance_report.REPORT_MARKER_COMPLETE,
            )
            metadata = json.loads((published / "metadata.json").read_text())
            files = metadata["files"]
            actual = {
                path.relative_to(published).as_posix()
                for path in published.rglob("*")
                if path.is_file() and path.name not in (
                    performance_report.REPORT_MARKER, "metadata.json"
                )
            }
            self.assertEqual(set(files), actual)
            for name, entry in files.items():
                data = (published / name).read_bytes()
                self.assertEqual(entry["bytes"], len(data))
                self.assertEqual(entry["sha256"], hashlib.sha256(data).hexdigest())

    def test_publish_evidence_walk_resists_intermediate_parent_swap(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            repository = base / "repo"
            repository.mkdir()
            evidence_root = repository / "build/signal-lab/evidence"
            create_evidence(evidence_root)
            external_parent = base / "external-signal-lab"
            external_evidence = external_parent / "evidence"
            create_evidence(external_evidence)
            replace_artifact(
                external_evidence, "scalar", "command.txt", b"EXTERNAL\n"
            )
            held_parent = repository / "build/.held-signal-lab"
            original_load = performance_report._load_evidence_descriptor

            def swap_parent_after_walk(descriptor):
                (repository / "build/signal-lab").rename(held_parent)
                (repository / "build/signal-lab").symlink_to(
                    external_parent, target_is_directory=True
                )
                return original_load(descriptor)

            with mock.patch.object(performance_report, "REPOSITORY_ROOT", repository), \
                    mock.patch.object(
                        performance_report, "_load_evidence_descriptor",
                        side_effect=swap_parent_after_walk,
                    ):
                published = performance_report.publish_report(
                    evidence_root, repository / "runs"
                )
            self.assertEqual(
                (published / "scalar/command.txt").read_bytes(),
                b"renode --console\n",
            )
            self.assertEqual(
                (external_evidence / "scalar/command.txt").read_bytes(),
                b"EXTERNAL\n",
            )

    def test_failed_publish_removes_only_its_marked_stage(self):
        with tempfile.TemporaryDirectory() as temporary:
            repository = Path(temporary) / "repo"
            repository.mkdir()
            evidence_root = repository / "build/signal-lab/evidence"
            create_evidence(evidence_root)
            runs_root = repository / "runs"
            with mock.patch.object(performance_report, "REPOSITORY_ROOT", repository), \
                    mock.patch.object(performance_report, "render_html",
                                      side_effect=RuntimeError("injected")):
                with self.assertRaisesRegex(RuntimeError, "injected"):
                    performance_report.publish_report(evidence_root, runs_root)
            self.assertFalse(any(
                entry.name.startswith(".signal-report-stage-")
                for entry in runs_root.iterdir()
            ))

    def test_pruning_preserves_deadline_unmarked_and_active_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            repository = Path(temporary) / "repo"
            repository.mkdir()
            runs_root = repository / "runs"
            runs_root.mkdir()
            names = []
            for index in range(performance_report.RETAIN_REPORT_RUNS + 2):
                name = f"20260101T0000{index:02d}000000Z-12345678-signal"
                path = runs_root / name
                path.mkdir()
                (path / performance_report.REPORT_MARKER).write_text(
                    performance_report.REPORT_MARKER_COMPLETE, encoding="ascii"
                )
                names.append(name)
            deadline = runs_root / "20260101T000000Z-1-deadline"
            deadline.mkdir()
            unmarked = runs_root / "20260101T000099000000Z-12345678-signal"
            unmarked.mkdir()
            active = runs_root / "20260101T000098000000Z-12345678-signal"
            active.mkdir()
            (active / performance_report.REPORT_MARKER).write_text(
                performance_report.REPORT_MARKER_ACTIVE, encoding="ascii"
            )
            current = runs_root / names[-1]
            with mock.patch.object(performance_report, "REPOSITORY_ROOT", repository):
                performance_report._prune_report_runs(runs_root.resolve(), current)
            completed = [name for name in names if (runs_root / name).exists()]
            self.assertEqual(len(completed), performance_report.RETAIN_REPORT_RUNS)
            self.assertTrue(deadline.exists())
            self.assertTrue(unmarked.exists())
            self.assertTrue(active.exists())

    def test_pruning_keeps_current_during_clock_rollback(self):
        with tempfile.TemporaryDirectory() as temporary:
            repository = Path(temporary) / "repo"
            repository.mkdir()
            runs_root = repository / "runs"
            runs_root.mkdir()
            current = runs_root / "20250101T000000000000Z-12345678-signal"
            current.mkdir()
            (current / performance_report.REPORT_MARKER).write_text(
                performance_report.REPORT_MARKER_COMPLETE, encoding="ascii"
            )
            others = []
            for index in range(9):
                path = runs_root / (
                    f"20270101T0000{index:02d}000000Z-12345678-signal"
                )
                path.mkdir()
                (path / performance_report.REPORT_MARKER).write_text(
                    performance_report.REPORT_MARKER_COMPLETE, encoding="ascii"
                )
                others.append(path)
            with mock.patch.object(performance_report, "REPOSITORY_ROOT", repository):
                performance_report._prune_report_runs(runs_root, current)
            complete = [path for path in [current, *others] if path.exists()]
            self.assertEqual(len(complete), performance_report.RETAIN_REPORT_RUNS)
            self.assertTrue(current.exists())
            self.assertEqual(
                {path.name for path in complete if path != current},
                {path.name for path in others[-7:]},
            )

    def test_prune_namespace_swap_does_not_follow_external_target(self):
        with tempfile.TemporaryDirectory() as temporary:
            repository = Path(temporary) / "repo"
            repository.mkdir()
            runs_root = repository / "runs"
            runs_root.mkdir()
            names = []
            for index in range(9):
                path = runs_root / (
                    f"20260101T0000{index:02d}000000Z-12345678-signal"
                )
                path.mkdir()
                (path / performance_report.REPORT_MARKER).write_text(
                    performance_report.REPORT_MARKER_COMPLETE, encoding="ascii"
                )
                names.append(path)
            current = names[-1]
            target = names[0]
            held = runs_root / ".held-owned-report"
            external = Path(temporary) / "external"
            external.mkdir()
            payload = external / "do-not-remove.txt"
            payload.write_text("safe", encoding="ascii")
            real_rename = os.rename
            injected = False

            def swap_before_quarantine(source, destination, *args, **kwargs):
                nonlocal injected
                if source == target.name and not injected:
                    injected = True
                    real_rename(source, held.name, *args, **kwargs)
                    os.symlink(
                        external, source,
                        dir_fd=kwargs.get("src_dir_fd"),
                        target_is_directory=True,
                    )
                return real_rename(source, destination, *args, **kwargs)

            with mock.patch.object(performance_report, "REPOSITORY_ROOT", repository), \
                    mock.patch.object(performance_report.os, "rename",
                                      side_effect=swap_before_quarantine):
                with self.assertRaises(performance_report.PerformanceReportError):
                    performance_report._prune_report_runs(runs_root, current)
            self.assertTrue(payload.is_file())
            self.assertEqual(payload.read_text(encoding="ascii"), "safe")
            self.assertTrue(held.is_dir())
            self.assertTrue(target.is_symlink())

    def test_prune_refuses_cross_mount_before_any_removal(self):
        with tempfile.TemporaryDirectory() as temporary:
            repository = Path(temporary) / "repo"
            repository.mkdir()
            runs_root = repository / "runs"
            runs_root.mkdir()
            names = []
            for index in range(9):
                path = runs_root / (
                    f"20260101T0000{index:02d}000000Z-12345678-signal"
                )
                path.mkdir()
                (path / performance_report.REPORT_MARKER).write_text(
                    performance_report.REPORT_MARKER_COMPLETE, encoding="ascii"
                )
                names.append(path)
            target = names[0]
            child = target / "mounted-child"
            child.mkdir()
            payload = child / "do-not-remove.txt"
            payload.write_text("safe", encoding="ascii")
            with mock.patch.object(performance_report, "REPOSITORY_ROOT", repository), \
                    mock.patch.object(
                        performance_report, "_mount_id", side_effect=(10, 11)
                    ), \
                    mock.patch.object(
                        performance_report.os, "unlink",
                        wraps=performance_report.os.unlink,
                    ) as unlink:
                with self.assertRaisesRegex(
                    performance_report.PerformanceReportError,
                    "mount boundary",
                ):
                    performance_report._prune_report_runs(runs_root, names[-1])
            unlink.assert_not_called()
            self.assertTrue(payload.is_file())
            self.assertEqual(payload.read_text(encoding="ascii"), "safe")


if __name__ == "__main__":
    unittest.main()
