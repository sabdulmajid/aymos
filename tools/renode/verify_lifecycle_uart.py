#!/usr/bin/env python3
"""Validate the exact PR 3 guest lifecycle event stream."""

from __future__ import annotations

import pathlib
import sys

MAX_UART_BYTES = 1024 * 1024

EXPECTED = b"".join(
    line + b"\r\n"
    for line in (
        b"AYMOS READY",
        b"LIFECYCLE BEGIN",
        b"LIFECYCLE START A ARG=165 PSP=1",
        b"LIFECYCLE YIELD A_TO_B",
        b"LIFECYCLE START B ARG=90 PSP=1",
        b"LIFECYCLE YIELD B_TO_A",
        b"LIFECYCLE RESUME A",
        b"LIFECYCLE SLEEP A",
        b"LIFECYCLE RESUME B",
        b"LIFECYCLE PREEMPT B_TO_A",
        b"LIFECYCLE RETURN A",
        b"LIFECYCLE RECLAIM A",
        b"LIFECYCLE CONTINUE B",
        b"LIFECYCLE RETURN B",
        b"LIFECYCLE RECLAIM B",
        b"LIFECYCLE IDLE PSP=1",
        b"AYMOS LIFECYCLE PASS",
    )
)


def validate(data: bytes) -> None:
    if len(data) > MAX_UART_BYTES:
        raise ValueError(
            f"UART capture is {len(data)} bytes; limit is {MAX_UART_BYTES}"
        )
    if data != EXPECTED:
        raise ValueError(
            "UART stream differs from the exact lifecycle contract: "
            f"expected {len(EXPECTED)} bytes, received {len(data)}"
        )


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: verify_lifecycle_uart.py UART_CAPTURE", file=sys.stderr)
        return 2
    capture = pathlib.Path(sys.argv[1])
    try:
        data = capture.read_bytes()
        validate(data)
    except (OSError, ValueError) as error:
        print(f"lifecycle-uart: {error}", file=sys.stderr)
        return 1
    sys.stdout.buffer.write(data)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
