#!/usr/bin/env python3
"""Validate the bounded UART contract for the PR 2 boot smoke."""

from __future__ import annotations

import argparse
from pathlib import Path

MAX_UART_BYTES = 1024 * 1024
REQUIRED_LINES = (
    b"AYMOS READY\r\n",
    b"AYMOS SMOKE SYSTICK=1 SVC=1\r\n",
)


class UartValidationError(ValueError):
    """The captured UART stream did not satisfy the boot contract."""


def validate_uart(data: bytes) -> None:
    if len(data) > MAX_UART_BYTES:
        raise UartValidationError(
            f"UART capture is {len(data)} bytes; limit is {MAX_UART_BYTES}"
        )

    previous_offset = -1
    for required in REQUIRED_LINES:
        count = data.count(required)
        if count != 1:
            line = required.rstrip().decode("ascii")
            raise UartValidationError(
                f"expected exactly one {line!r} line, observed {count}"
            )
        offset = data.index(required)
        if offset <= previous_offset:
            raise UartValidationError("required UART lines are out of order")
        previous_offset = offset


def main() -> int:
    parser = argparse.ArgumentParser(
        description="validate AymOS's raw PR 2 UART boot capture"
    )
    parser.add_argument("capture", type=Path)
    args = parser.parse_args()

    try:
        data = args.capture.read_bytes()
        validate_uart(data)
    except (OSError, UartValidationError) as error:
        parser.error(str(error))

    print(data.decode("ascii", errors="backslashreplace"), end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
