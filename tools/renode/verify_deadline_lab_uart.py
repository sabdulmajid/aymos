#!/usr/bin/env python3
"""Validate one completed AymOS scheduling workload UART stream."""

from __future__ import annotations

import json
import os
from pathlib import Path
import sys

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY_ROOT))

from tools.aymos_lab.deadline_lab import DeadlineLabError, validate_capture


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("usage: verify_deadline_lab_uart.py UART_CAPTURE",
              file=sys.stderr)
        return 2
    mode = os.environ.get("AYMOS_WORKLOAD_MODE", "")
    if mode not in ("normal", "overload"):
        print(f"scheduling-uart: invalid AYMOS_WORKLOAD_MODE: {mode!r}",
              file=sys.stderr)
        return 2
    capture = Path(argv[1])
    try:
        decoded, raw_records = validate_capture(capture, mode)
        (capture.parent / "trace.bin").write_bytes(raw_records)
        (capture.parent / "trace.json").write_text(
            json.dumps(decoded, sort_keys=True, indent=2) + "\n",
            encoding="utf-8")
    except (OSError, DeadlineLabError) as error:
        print(f"scheduling-uart: {error}", file=sys.stderr)
        return 1
    print(f"AYMOS SCHEDULING PASS MODE={mode} "
          f"RECORDS={decoded['record_count']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
