#!/usr/bin/env python3
"""Validate the exact guest-generated PR 4 EDF semantic sequence."""

from __future__ import annotations

import pathlib
import sys

MAX_UART_BYTES = 1024 * 1024

EXPECTED = b"".join(
    line + b"\r\n"
    for line in (
        b"AYMOS READY",
        b"EDF BEGIN",
        b"EDF SELECT TASK=2 RELEASE=0 DEADLINE=50",
        b"EDF RELEASE TASK=1 JOB=1 RELEASE=5 DEADLINE=15",
        b"EDF PREEMPT FROM=2 TO=1 JOB=1",
        b"EDF WAIT TASK=1 NEXT_RELEASE=20",
        b"EDF RELEASE TASK=1 JOB=2 RELEASE=20 DEADLINE=30",
        b"EDF PREEMPT FROM=2 TO=1 JOB=2",
        b"EDF EXIT TASK=1",
        b"EDF RECLAIM TASK=1",
        b"EDF RESUME TASK=2",
        b"EDF EXIT TASK=2",
        b"EDF RECLAIM TASK=2",
        b"AYMOS EDF PASS",
    )
)


def validate(data: bytes) -> None:
    if len(data) > MAX_UART_BYTES:
        raise ValueError(
            f"UART capture is {len(data)} bytes; limit is {MAX_UART_BYTES}"
        )
    if data != EXPECTED:
        raise ValueError(
            "UART stream differs from the exact EDF contract: "
            f"expected {len(EXPECTED)} bytes, received {len(data)}"
        )


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: verify_edf_uart.py UART_CAPTURE", file=sys.stderr)
        return 2
    capture = pathlib.Path(sys.argv[1])
    try:
        data = capture.read_bytes()
        validate(data)
    except (OSError, ValueError) as error:
        print(f"edf-uart: {error}", file=sys.stderr)
        return 1
    sys.stdout.buffer.write(data)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
