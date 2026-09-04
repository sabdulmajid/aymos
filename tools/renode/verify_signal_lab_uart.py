#!/usr/bin/env python3
"""Validate one complete AymOS Signal Lab UART stream."""

from __future__ import annotations

import json
import os
from pathlib import Path
import sys

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY_ROOT))

from tools.aymos_lab.signal_lab import SignalLabError, validate_capture


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("usage: verify_signal_lab_uart.py UART_CAPTURE", file=sys.stderr)
        return 2
    implementation = os.environ.get("AYMOS_SIGNAL_IMPL", "")
    if implementation not in ("scalar", "m4"):
        print(
            f"signal-lab-uart: invalid AYMOS_SIGNAL_IMPL: {implementation!r}",
            file=sys.stderr,
        )
        return 2
    capture = Path(argv[1])
    try:
        decoded, raw_records, result = validate_capture(capture, implementation)
        (capture.parent / "trace.bin").write_bytes(raw_records)
        (capture.parent / "trace.json").write_text(
            json.dumps(decoded, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        (capture.parent / "result.json").write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    except (OSError, SignalLabError) as error:
        print(f"signal-lab-uart: {error}", file=sys.stderr)
        return 1
    print(
        f"AYMOS SIGNAL LAB PASS IMPL={implementation} "
        f"RECORDS={decoded['record_count']} OUTPUT_CRC=0x{result['output_crc32']:08X}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
