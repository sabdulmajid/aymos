from __future__ import annotations

import unittest

from tools.renode.verify_allocator_uart import EXPECTED, MAX_UART_BYTES, validate


class ValidateAllocatorUartTests(unittest.TestCase):
    def test_accepts_exact_guest_contract(self) -> None:
        validate(EXPECTED)

    def test_rejects_missing_iteration(self) -> None:
        with self.assertRaisesRegex(ValueError, "exact allocator contract"):
            validate(
                EXPECTED.replace(b"ALLOCATOR WORKER ITER=5 SLOT=2\r\n", b"")
            )

    def test_rejects_changed_reuse_slot(self) -> None:
        with self.assertRaisesRegex(ValueError, "exact allocator contract"):
            validate(EXPECTED.replace(b"REUSED SLOT=2", b"REUSED SLOT=3"))

    def test_rejects_host_fabricated_suffix(self) -> None:
        with self.assertRaisesRegex(ValueError, "exact allocator contract"):
            validate(EXPECTED + b"ALLOCATOR PASS\r\n")

    def test_rejects_unbounded_capture(self) -> None:
        with self.assertRaisesRegex(ValueError, "limit"):
            validate(b"x" * (MAX_UART_BYTES + 1))


if __name__ == "__main__":
    unittest.main()
