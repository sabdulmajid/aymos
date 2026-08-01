from __future__ import annotations

import unittest

from tools.renode.verify_lifecycle_uart import EXPECTED, MAX_UART_BYTES, validate


class ValidateLifecycleUartTests(unittest.TestCase):
    def test_accepts_exact_guest_contract(self) -> None:
        validate(EXPECTED)

    def test_rejects_missing_preemption(self) -> None:
        with self.assertRaisesRegex(ValueError, "exact lifecycle contract"):
            validate(EXPECTED.replace(b"LIFECYCLE PREEMPT B_TO_A\r\n", b""))

    def test_rejects_panic_suffix(self) -> None:
        with self.assertRaisesRegex(ValueError, "exact lifecycle contract"):
            validate(EXPECTED + b"AYMOS PANIC TEST\r\n")

    def test_rejects_duplicate_or_reordered_events(self) -> None:
        duplicate = EXPECTED.replace(
            b"LIFECYCLE RETURN A\r\n",
            b"LIFECYCLE RETURN A\r\nLIFECYCLE RETURN A\r\n",
        )
        with self.assertRaisesRegex(ValueError, "exact lifecycle contract"):
            validate(duplicate)

    def test_rejects_unbounded_capture(self) -> None:
        with self.assertRaisesRegex(ValueError, "limit"):
            validate(b"x" * (MAX_UART_BYTES + 1))


if __name__ == "__main__":
    unittest.main()
