import binascii
from pathlib import Path
import struct
import tempfile
import unittest

from tools.aymos_lab import trace


def frame(frame_type: int, payload: bytes, *, framing: int = 1,
          schema: int = 1, flags: int = 0) -> bytes:
    header = trace.HEADER.pack(trace.MAGIC, framing, frame_type, schema,
                               len(payload), flags)
    crc = struct.pack("<I", binascii.crc32(header[4:] + payload) & 0xFFFFFFFF)
    return header + payload + crc


def record(sequence: int, event: int, *, tick: int | None = None,
           task: int = 1, related: int = trace.INVALID_TASK_ID,
           flags: int = 0, schema: int = 1, values=(0, 0, 0)) -> bytes:
    return frame(trace.FRAME_RECORD, trace.RECORD.pack(
        sequence if tick is None else tick, sequence, schema, event, flags,
        task, related, *values))


def footer(count: int, *, emitted: int | None = None, dropped: int = 0,
           final: int | None = None, flags: int = 0,
           final_tick: int = 0xFFFFFFFFFFFFFFFF) -> bytes:
    if emitted is None:
        emitted = count - dropped
    if final is None:
        final = 0xFFFFFFFF if count == 0 else count - 1
    return frame(trace.FRAME_FOOTER, trace.FOOTER.pack(
        count, emitted, dropped, final, flags, final_tick))


def complete(records: list[bytes], footer_frame: bytes | None = None) -> bytes:
    return (trace.PREAMBLE + b"".join(records) +
            (footer(len(records)) if footer_frame is None else footer_frame) +
            trace.TRAILER)


def selection_summary(reason: int, purpose: int = 1,
                      excluded: int = 0xFF, eligible: int = 2) -> int:
    return reason | (purpose << 8) | (excluded << 16) | (eligible << 24)


def candidate_meta(priority: int, state: int = 2, *, excluded: bool = False,
                   incumbent: bool = False, eligible: bool = True) -> int:
    return (priority | (state << 8) | (int(excluded) << 16) |
            (int(incumbent) << 17) | (int(eligible) << 18))


class TraceDecoderTests(unittest.TestCase):
    def test_accepts_schema_events_1_through_21(self):
        records = [
            record(0, 1, tick=0, task=trace.INVALID_TASK_ID,
                   related=trace.INVALID_TASK_ID, values=(0, 5, 0)),
            record(1, 2, tick=0, task=1, values=(0, 1, 2)),
            record(2, 3, tick=0, task=1, values=(10, 0, 1)),
            record(3, 4, tick=0, task=1,
                   values=(1 << 1, selection_summary(1, eligible=1), 1)),
            record(4, 5, tick=0, task=1, related=1,
                   values=(10, 0, candidate_meta(1))),
            record(5, 6, tick=0, task=1, values=(10, 0, 1)),
            record(6, 7, tick=0, task=1, related=2, values=(1, 0, 0)),
            record(7, 8, tick=0, task=1),
            record(8, 9, tick=0, task=1, values=(6, 0, 6)),
            record(9, 10, tick=0, task=1, values=(10, 0, 1)),
            record(10, 11, tick=0, task=1, values=(1, 0, 0)),
            record(11, 12, tick=0, task=1, related=2, values=(2, 2, 0)),
            record(12, 13, tick=0, task=0, related=1),
            record(13, 14, tick=0, task=0, related=1),
            record(14, 15, tick=0, task=1, values=(10, 0, 1)),
            record(15, 16, tick=0, task=1, values=(10, 0, 1)),
            record(16, 17, tick=0, task=1, values=(8, 24, 1)),
            record(17, 18, tick=0, task=1, values=(24, 1, 1)),
            record(18, 19, tick=0, task=1, values=(20, 0, 0)),
            record(19, 20, tick=0, task=1, values=(15, 0, 1)),
            record(20, 21, tick=0, task=1, values=(1, 1, 1)),
        ]
        decoded, _ = trace.decode(complete(records))
        self.assertEqual(
            [item["event_id"] for item in decoded["records"]],
            list(range(1, 22)))

    def test_rejects_overflow_event_in_complete_zero_loss_stream(self):
        item = record(0, 22, tick=0, task=trace.INVALID_TASK_ID,
                      related=trace.INVALID_TASK_ID, values=(1, 1, 0))
        self.assert_rejected(complete([item]), "zero-loss.*trace_overflow")
        invalid = record(0, 22, tick=0, task=trace.INVALID_TASK_ID,
                         related=trace.INVALID_TASK_ID, values=(0, 1, 1))
        self.assert_rejected(complete([invalid]), "invalid payload")

    def test_complete_selection_snapshot(self):
        records = [
            record(0, 1, tick=0, task=trace.INVALID_TASK_ID,
                   values=(0, 5, 0)),
            record(1, 4, tick=0, task=1, related=trace.INVALID_TASK_ID,
                   values=((1 << 1) | (1 << 2), selection_summary(2), 2)),
            record(2, 5, tick=0, task=1, related=1,
                   values=(10, 0, candidate_meta(2))),
            record(3, 5, tick=0, task=2, related=1,
                   values=(20, 0, candidate_meta(1))),
            record(4, 13, tick=1, task=0, related=1),
            record(5, 12, tick=1, task=1, related=0,
                   values=(4, 5, 0)),
        ]
        decoded, framed = trace.decode(complete(records))
        self.assertEqual(decoded["record_count"], 6)
        self.assertEqual(decoded["records"][1]["details"]["reason"], "deadline")
        self.assertEqual(len(framed), 6 * trace.RECORD.size)
        self.assertFalse(framed.startswith(trace.MAGIC))

    def test_framed_stream_without_uart_text(self):
        stream = (record(0, 1, tick=0, task=trace.INVALID_TASK_ID,
                         values=(0, 5, 0)) +
                  record(1, 13, tick=1, task=0, related=1) +
                  record(2, 12, tick=1, task=1, related=0,
                         values=(4, 5, 0)) + footer(3))
        decoded, copied = trace.decode(stream)
        self.assertEqual(decoded["footer"]["emitted"], 3)
        self.assertEqual(len(copied), 3 * trace.RECORD.size)

    def test_probe_incumbent_can_have_lower_or_higher_task_id(self):
        records = [
            record(0, 4, tick=0, task=2, related=1,
                   values=(1 << 2, selection_summary(2, purpose=2), 2)),
            record(1, 5, tick=0, task=1, related=2,
                   values=(20, 0, candidate_meta(
                       1, state=3, incumbent=True))),
            record(2, 5, tick=0, task=2, related=2,
                   values=(10, 0, candidate_meta(1))),
            record(3, 4, tick=1, task=1, related=2,
                   values=(1 << 1, selection_summary(4, purpose=2), 2)),
            record(4, 5, tick=1, task=1, related=1,
                   values=(10, 0, candidate_meta(1))),
            record(5, 5, tick=1, task=2, related=1,
                   values=(10, 0, candidate_meta(
                       1, state=3, incumbent=True))),
        ]
        decoded, _ = trace.decode(complete(records))
        summaries = [item for item in decoded["records"]
                     if item["event"] == "task_select"]
        self.assertEqual([item["task"] for item in summaries], [2, 1])
        self.assertEqual([item["related"] for item in summaries], [1, 2])

    def test_idle_probe_has_absent_idle_incumbent(self):
        records = [
            record(0, 4, tick=4, task=1, related=0,
                   values=(1 << 1, selection_summary(
                       1, purpose=2, eligible=1), 1)),
            record(1, 5, tick=4, task=1, related=1,
                   values=(20, 0, candidate_meta(1))),
        ]
        decoded, _ = trace.decode(complete(records))
        summary = decoded["records"][0]
        self.assertEqual(summary["related"], 0)
        self.assertEqual(summary["details"]["purpose"], "systick_probe")

    def test_rejects_impossible_probe_and_nonempty_idle_dispatch(self):
        idle_incumbent_marker = complete([
            record(0, 4, tick=4, task=1, related=0,
                   values=(2, selection_summary(
                       1, purpose=2, eligible=1), 1)),
            record(1, 5, tick=4, task=1, related=1,
                   values=(20, 0, candidate_meta(1, incumbent=True))),
        ])
        self.assert_rejected(idle_incumbent_marker, "idle probe")

        selected_incumbent = complete([
            record(0, 4, tick=4, task=1, related=1,
                   values=(0, selection_summary(
                       1, purpose=2, eligible=1), 1)),
            record(1, 5, tick=4, task=1, related=1,
                   values=(20, 0, candidate_meta(
                       1, state=3, incumbent=True))),
        ])
        self.assert_rejected(selected_incumbent, "impossible probe")

        one_candidate_user_probe = complete([
            record(0, 4, tick=4, task=2, related=1,
                   values=(4, selection_summary(
                       1, purpose=2, eligible=1), 1)),
            record(1, 5, tick=4, task=2, related=2,
                   values=(10, 0, candidate_meta(1))),
        ])
        self.assert_rejected(one_candidate_user_probe, "probe incumbent")

        nonempty_idle = complete([
            record(0, 4, tick=4, task=0, related=1,
                   values=(2, selection_summary(
                       5, eligible=1), 1)),
            record(1, 5, tick=4, task=1, related=0,
                   values=(20, 0, candidate_meta(1, incumbent=True))),
        ])
        self.assert_rejected(nonempty_idle, "invalid idle selection")

    def test_bounded_reader_rejects_oversized_and_sparse_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            exact = root / "exact.bin"
            with exact.open("wb") as output:
                output.truncate(trace.MAX_INPUT_BYTES)
            self.assertEqual(len(trace.read_bounded(exact)),
                             trace.MAX_INPUT_BYTES)

            oversized = root / "oversized.bin"
            oversized.write_bytes(b"x" * (trace.MAX_INPUT_BYTES + 1))
            with self.assertRaisesRegex(trace.TraceError, "limit"):
                trace.read_bounded(oversized)

            sparse = root / "sparse.bin"
            with sparse.open("wb") as output:
                output.seek(trace.MAX_INPUT_BYTES)
                output.write(b"x")
            with self.assertRaisesRegex(trace.TraceError, "limit"):
                trace.read_bounded(sparse)

    def assert_rejected(self, data: bytes, text: str):
        with self.assertRaisesRegex(trace.TraceError, text):
            trace.decode(data)

    def test_rejects_bounds_and_structure(self):
        self.assert_rejected(b"x" * (trace.MAX_INPUT_BYTES + 1), "limit")
        self.assert_rejected(b"wrong", "preamble")
        self.assert_rejected(trace.PREAMBLE + record(0, 1), "missing terminal footer")
        self.assert_rejected(complete([]) + b"x", "trailing garbage")
        self.assert_rejected(complete([], footer(0))[:-1], "terminal line")

    def test_rejects_header_contract(self):
        payload = trace.RECORD.pack(0, 0, 1, 1, 0,
                                    trace.INVALID_TASK_ID,
                                    trace.INVALID_TASK_ID, 0, 0, 0)
        self.assert_rejected(trace.PREAMBLE + frame(1, payload, framing=2), "framing")
        self.assert_rejected(trace.PREAMBLE + frame(1, payload, schema=2), "schema")
        self.assert_rejected(trace.PREAMBLE + frame(3, payload), "frame type")
        self.assert_rejected(trace.PREAMBLE + frame(1, payload, flags=1), "frame flags")
        self.assert_rejected(trace.PREAMBLE + frame(1, payload[:-1]), "payload length")

    def test_rejects_crc_and_truncation(self):
        data = bytearray(complete([record(0, 1, task=trace.INVALID_TASK_ID)]))
        data[len(trace.PREAMBLE) + trace.HEADER.size] ^= 1
        self.assert_rejected(bytes(data), "CRC32")
        valid = record(0, 1, task=trace.INVALID_TASK_ID)
        self.assert_rejected(trace.PREAMBLE + valid[:-1], "truncated")

    def test_rejects_record_contract(self):
        self.assert_rejected(complete([record(0, 99)]), "unknown event")
        self.assert_rejected(complete([record(0, 1, flags=1)]), "record flags")
        self.assert_rejected(complete([record(0, 1, schema=2)]), "record schema")
        self.assert_rejected(complete([record(0, 1, task=5)]), "impossible task")
        self.assert_rejected(complete([record(1, 1)]), "sequence gap")
        self.assert_rejected(complete([
            record(0, 1, tick=2), record(1, 2, tick=1)]), "nonmonotonic")

    def test_rejects_footer_contract(self):
        item = record(0, 1, task=trace.INVALID_TASK_ID)
        self.assert_rejected(complete([item], footer(1, emitted=0)), "emitted")
        self.assert_rejected(complete([item], footer(2, emitted=1, dropped=0)), "attempted")
        self.assert_rejected(complete([item], footer(1, final=4)), "final sequence")
        self.assert_rejected(complete([item], footer(2, emitted=1, dropped=1)), "dropped")
        self.assert_rejected(complete([item], footer(1, flags=1)), "footer reports")
        self.assert_rejected(
            complete([record(0, 1, tick=3, task=trace.INVALID_TASK_ID,
                             values=(0, 5, 0))],
                     footer(1, final_tick=2)),
            "final tick")
        duplicate = (trace.PREAMBLE + item + footer(1) + footer(1) + trace.TRAILER)
        self.assert_rejected(duplicate, "trailing garbage")

    def test_rejects_selection_explanation_mismatch(self):
        orphan = complete([record(0, 5, task=1, related=1)])
        self.assert_rejected(orphan, "orphan")
        incomplete = complete([record(0, 4, task=1, values=(2, 1, 1))])
        self.assert_rejected(incomplete, "incomplete")
        wrong_winner = complete([
            record(0, 4, task=2,
                   values=(6, selection_summary(2), 2)),
            record(1, 5, tick=0, task=1, related=2,
                   values=(10, 0, candidate_meta(1))),
            record(2, 5, tick=0, task=2, related=2,
                   values=(20, 0, candidate_meta(1))),
        ])
        self.assert_rejected(wrong_winner, "contradicts")

    def test_bounded_resync_is_diagnostic_only(self):
        item = bytearray(record(0, 1, task=trace.INVALID_TASK_ID))
        item[trace.HEADER.size:trace.HEADER.size + 4] = trace.MAGIC
        stream = trace.PREAMBLE + bytes(item) + footer(1) + trace.TRAILER
        self.assert_rejected(stream, "bounded diagnostic found next sync")

        broken = (trace.PREAMBLE + b"BAD!" + b"x" * 12 +
                  record(0, 1, task=trace.INVALID_TASK_ID) + footer(1) +
                  trace.TRAILER)
        self.assert_rejected(broken, "next sync")

    def test_rejects_event_payload_enums_and_booleans(self):
        cases = (
            record(0, 2, task=1, values=(2, 1, 2)),
            record(0, 3, task=1, values=(10, 0, 0)),
            record(0, 7, task=1, related=2, values=(4, 0, 0)),
            record(0, 12, task=1, related=2, values=(9, 2, 0)),
            record(0, 17, task=1, values=(8, 12, 2)),
            record(0, 17, task=1, values=(8, 12, 0)),
            record(0, 18, task=1, values=(12, 0, 1)),
            record(0, 18, task=1, values=(0xFFFFFFFF, 1, 1)),
            record(0, 21, task=1, values=(1, 2, 1)),
        )
        for item in cases:
            with self.subTest(item=item.hex()[:32]):
                self.assert_rejected(complete([item]), "invalid payload")

    def test_rejects_selection_reserved_metadata(self):
        invalid_summaries = (
            selection_summary(0, eligible=1),
            selection_summary(1, purpose=0, eligible=1),
            selection_summary(1, excluded=9, eligible=1),
        )
        for summary in invalid_summaries:
            stream = complete([
                record(0, 4, task=1, values=(2, summary, 1)),
                record(1, 5, tick=0, task=1, related=1,
                       values=(10, 0, candidate_meta(1))),
            ])
            self.assert_rejected(stream, "selection summary|reason|purpose|eligible")

        for metadata in (candidate_meta(1, state=1),
                         candidate_meta(1) | (1 << 31)):
            stream = complete([
                record(0, 4, task=1,
                       values=(2, selection_summary(1, eligible=1), 1)),
                record(1, 5, tick=0, task=1, related=1,
                       values=(10, 0, metadata)),
            ])
            self.assert_rejected(stream, "candidate metadata|READY mask")

    def test_rejects_frame_resource_limit(self):
        records = [record(sequence, 8, task=1, tick=sequence,
                          values=(0, 0, 0))
                   for sequence in range(trace.MAX_FRAMES + 1)]
        self.assert_rejected(complete(records), "frame count")


if __name__ == "__main__":
    unittest.main()
