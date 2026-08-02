from __future__ import annotations

import unittest

from tools.renode.verify_edf_uart import EXPECTED, MAX_UART_BYTES, validate


class ValidateEdfUartTests(unittest.TestCase):
    def test_accepts_exact_guest_contract(self) -> None:
        validate(EXPECTED)

    def test_rejects_missing_preemption(self) -> None:
        with self.assertRaisesRegex(ValueError, "exact EDF contract"):
            validate(EXPECTED.replace(b"EDF PREEMPT FROM=2 TO=1 JOB=2\r\n", b""))

    def test_rejects_host_fabricated_suffix(self) -> None:
        with self.assertRaisesRegex(ValueError, "exact EDF contract"):
            validate(EXPECTED + b"EDF PASS\r\n")

    def test_rejects_reordered_selection(self) -> None:
        reordered = EXPECTED.replace(
            b"EDF SELECT TASK=2 RELEASE=0 DEADLINE=50\r\n"
            b"EDF RELEASE TASK=1 JOB=1 RELEASE=5 DEADLINE=15\r\n",
            b"EDF RELEASE TASK=1 JOB=1 RELEASE=5 DEADLINE=15\r\n"
            b"EDF SELECT TASK=2 RELEASE=0 DEADLINE=50\r\n",
        )
        with self.assertRaisesRegex(ValueError, "exact EDF contract"):
            validate(reordered)

    def test_rejects_unbounded_capture(self) -> None:
        with self.assertRaisesRegex(ValueError, "limit"):
            validate(b"x" * (MAX_UART_BYTES + 1))


if __name__ == "__main__":
    unittest.main()
