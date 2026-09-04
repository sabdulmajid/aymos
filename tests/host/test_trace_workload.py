import unittest

from tools.renode.verify_trace_uart import (
    EXPECTED_PROJECTION,
    TraceWorkloadError,
    validate_ordered_projection,
)


def expected_records():
    return [
        {"event": event, "task": task, "related": related, "tick": tick}
        for event, task, related, tick in EXPECTED_PROJECTION
    ]


def event_index(records, event, occurrence=0):
    indices = [index for index, record in enumerate(records)
               if record["event"] == event]
    return indices[occurrence]


class TraceWorkloadProjectionTests(unittest.TestCase):
    def assert_mutation_rejected(self, event, field, value, occurrence=0):
        records = expected_records()
        records[event_index(records, event, occurrence)][field] = value
        with self.assertRaisesRegex(TraceWorkloadError,
                                    "ordered semantic projection"):
            validate_ordered_projection(records)

    def test_rejects_create_and_release_mutations(self):
        self.assert_mutation_rejected("task_create", "task", 2)
        self.assert_mutation_rejected("task_release", "tick", 2)

    def test_rejects_alloc_and_free_mutations(self):
        self.assert_mutation_rejected("alloc", "task", 2)
        self.assert_mutation_rejected("free", "event", "alloc")

    def test_rejects_preempt_and_committed_switch_mutations(self):
        self.assert_mutation_rejected("task_preempt", "related", 4)
        self.assert_mutation_rejected("context_switch", "related", 4,
                                      occurrence=1)

    def test_rejects_deadline_outcome_and_exit_mutations(self):
        self.assert_mutation_rejected("deadline_miss", "event",
                                      "deadline_met")
        self.assert_mutation_rejected("task_exit", "task", 2,
                                      occurrence=4)

    def test_rejects_idle_start_and_switch_mutations(self):
        self.assert_mutation_rejected("idle_start", "related", 4)
        self.assert_mutation_rejected("context_switch", "related", 1,
                                      occurrence=-1)


if __name__ == "__main__":
    unittest.main()
