#!/usr/bin/env python3
"""Validate the exact guest-generated PR 5 allocator stress sequence."""

from __future__ import annotations

import pathlib
import sys

MAX_UART_BYTES = 1024 * 1024

EXPECTED = b"".join(
    line + b"\r\n"
    for line in (
        b"AYMOS READY",
        b"ALLOCATOR BEGIN",
        b"ALLOCATOR WORKER ITER=1 SLOT=2",
        b"ALLOCATOR WORKER ITER=2 SLOT=2",
        b"ALLOCATOR WORKER ITER=3 SLOT=2",
        b"ALLOCATOR WORKER ITER=4 SLOT=2",
        b"ALLOCATOR WORKER ITER=5 SLOT=2",
        b"ALLOCATOR WORKER ITER=6 SLOT=2",
        b"ALLOCATOR WORKER ITER=7 SLOT=2",
        b"ALLOCATOR WORKER ITER=8 SLOT=2",
        b"ALLOCATOR REUSED SLOT=2 COUNT=8",
        b"ALLOCATOR STATS ALLOCATED=0 FREE_BLOCKS=1 INVALID_FREES=16",
        b"AYMOS ALLOCATOR PASS",
    )
)


def validate(data: bytes) -> None:
    if len(data) > MAX_UART_BYTES:
        raise ValueError(
            f"UART capture is {len(data)} bytes; limit is {MAX_UART_BYTES}"
        )
    if data != EXPECTED:
        raise ValueError(
            "UART stream differs from the exact allocator contract: "
            f"expected {len(EXPECTED)} bytes, received {len(data)}"
        )


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: verify_allocator_uart.py UART_CAPTURE", file=sys.stderr)
        return 2
    capture = pathlib.Path(sys.argv[1])
    try:
        data = capture.read_bytes()
        validate(data)
    except (OSError, ValueError) as error:
        print(f"allocator-uart: {error}", file=sys.stderr)
        return 1
    sys.stdout.buffer.write(data)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
