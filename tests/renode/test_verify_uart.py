from __future__ import annotations

import unittest

from tools.renode.verify_uart import MAX_UART_BYTES, UartValidationError, validate_uart


class ValidateUartTests(unittest.TestCase):
    def test_accepts_exact_boot_contract(self) -> None:
        validate_uart(b"AYMOS READY\r\nAYMOS SMOKE SYSTICK=1 SVC=1\r\n")

    def test_rejects_missing_smoke(self) -> None:
        with self.assertRaisesRegex(UartValidationError, "observed 0"):
            validate_uart(b"AYMOS READY\r\n")

    def test_rejects_duplicate_banner(self) -> None:
        with self.assertRaisesRegex(UartValidationError, "observed 2"):
            validate_uart(
                b"AYMOS READY\r\nAYMOS READY\r\n"
                b"AYMOS SMOKE SYSTICK=1 SVC=1\r\n"
            )

    def test_rejects_reverse_order(self) -> None:
        with self.assertRaisesRegex(UartValidationError, "out of order"):
            validate_uart(
                b"AYMOS SMOKE SYSTICK=1 SVC=1\r\nAYMOS READY\r\n"
            )

    def test_rejects_unbounded_capture(self) -> None:
        with self.assertRaisesRegex(UartValidationError, "limit"):
            validate_uart(b"x" * (MAX_UART_BYTES + 1))


if __name__ == "__main__":
    unittest.main()
