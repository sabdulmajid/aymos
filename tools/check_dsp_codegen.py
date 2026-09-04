#!/usr/bin/env python3
"""Check the Cortex-M4 FIR instruction selection inside exact functions."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import tempfile
from pathlib import Path


FUNCTION_HEADER = re.compile(r"^([0-9a-fA-F]+) <([^>]+)>:$")
INSTRUCTION = re.compile(
    r"^\s*([0-9a-fA-F]+):\s+([a-zA-Z][a-zA-Z0-9.]*)\b"
)


class CheckError(RuntimeError):
    """A code-generation contract failed."""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(65536), b""):
            digest.update(block)
    return digest.hexdigest()


def run_objdump(objdump: Path, arguments: list[str]) -> str:
    result = subprocess.run(
        [str(objdump), *arguments],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise CheckError(f"objdump failed: {detail}")
    return result.stdout


def symbol_range(objdump: Path, object_path: Path, name: str) -> tuple[int, int]:
    symbols = run_objdump(objdump, ["--syms", str(object_path)])
    matches: list[tuple[int, int]] = []
    for line in symbols.splitlines():
        fields = line.split()
        if len(fields) >= 6 and fields[-1] == name and "F" in fields[1:-2]:
            matches.append((int(fields[0], 16), int(fields[-2], 16)))
    if len(matches) != 1:
        raise CheckError(f"expected one function symbol {name}, found {len(matches)}")
    start, size = matches[0]
    if size == 0:
        raise CheckError(f"function symbol {name} has zero size")
    return start, size


def exact_function(
    objdump: Path, object_path: Path, name: str
) -> tuple[list[str], int, int]:
    start_address, size = symbol_range(objdump, object_path, name)
    disassembly = run_objdump(
        objdump,
        [f"--disassemble={name}", "--no-show-raw-insn", str(object_path)],
    )
    lines = disassembly.splitlines()
    matches = [
        index
        for index, line in enumerate(lines)
        if (match := FUNCTION_HEADER.match(line.strip())) and match.group(2) == name
    ]
    if len(matches) != 1:
        raise CheckError(f"expected one {name} function, found {len(matches)}")
    header_address = int(FUNCTION_HEADER.match(lines[matches[0]].strip()).group(1), 16)
    if header_address != start_address:
        raise CheckError(f"symbol and disassembly addresses differ for {name}")

    start = matches[0] + 1
    body = lines[start:]
    decoded_addresses = [
        int(match.group(1), 16)
        for line in body
        if (match := INSTRUCTION.match(line))
    ]
    if not decoded_addresses:
        raise CheckError(f"function {name} has no decoded instructions")
    end_address = start_address + size
    if any(
        address < start_address or address >= end_address
        for address in decoded_addresses
    ):
        raise CheckError(f"disassembly escaped the symbol address range for {name}")
    return body, start_address, size


def mnemonics(body: list[str]) -> list[str]:
    decoded: list[str] = []
    for line in body:
        match = INSTRUCTION.match(line)
        if match:
            decoded.append(match.group(2).lower())
    return decoded


def check_function(
    body: list[str],
    name: str,
    start_address: int,
    size: int,
    required: set[str],
    forbidden: set[str],
) -> dict[str, object]:
    decoded = mnemonics(body)
    opcode_set = set(decoded)
    missing = sorted(required - opcode_set)
    present_forbidden = sorted(forbidden & opcode_set)
    if missing:
        raise CheckError(f"{name} is missing required instruction(s): {', '.join(missing)}")
    if present_forbidden:
        raise CheckError(
            f"{name} contains forbidden instruction(s): {', '.join(present_forbidden)}"
        )
    vfp = sorted({opcode for opcode in opcode_set if opcode.startswith("v")})
    if vfp:
        raise CheckError(f"{name} contains VFP instruction(s): {', '.join(vfp)}")
    if any("memcpy" in line.lower() for line in body):
        raise CheckError(f"{name} contains an unexpected memcpy reference")

    return {
        "function": name,
        "address": f"0x{start_address:08x}",
        "size_bytes": size,
        "instruction_count": len(decoded),
        "required_instructions": sorted(required),
        "forbidden_instructions": sorted(forbidden),
        "observed_required_counts": {
            opcode: decoded.count(opcode) for opcode in sorted(required)
        },
        "vfp_instructions": [],
        "memcpy_reference": False,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--objdump", required=True, type=Path)
    parser.add_argument("--build-config", required=True, type=Path)
    parser.add_argument("--m4-object", required=True, type=Path)
    parser.add_argument("--scalar-object", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.unlink(missing_ok=True)
    for path in (
        args.objdump,
        args.build_config,
        args.m4_object,
        args.scalar_object,
    ):
        if not path.is_file():
            raise CheckError(f"required file does not exist: {path}")

    m4_name = "aymos_fir_q15_m4"
    scalar_name = "aymos_fir_q15_scalar"
    m4_body, m4_start, m4_size = exact_function(
        args.objdump, args.m4_object, m4_name
    )
    scalar_body, scalar_start, scalar_size = exact_function(
        args.objdump, args.scalar_object, scalar_name
    )
    report = {
        "schema": 1,
        "claim": "static Cortex-M4 instruction evidence; not a timing measurement",
        "build_config": {
            "path": str(args.build_config),
            "sha256": sha256(args.build_config),
        },
        "objects": {
            "m4": {
                "path": str(args.m4_object),
                "sha256": sha256(args.m4_object),
            },
            "scalar": {
                "path": str(args.scalar_object),
                "sha256": sha256(args.scalar_object),
            },
        },
        "checks": [
            check_function(
                m4_body, m4_name, m4_start, m4_size,
                {"smlald", "ssat"}, set()
            ),
            check_function(
                scalar_body, scalar_name, scalar_start, scalar_size,
                set(), {"smlald", "ssat"}
            ),
        ],
    }
    temporary_name: str | None = None
    try:
        with tempfile.NamedTemporaryFile(
            "w",
            dir=args.output.parent,
            encoding="utf-8",
            prefix=f".{args.output.name}.",
            delete=False,
        ) as temporary:
            temporary.write(json.dumps(report, indent=2, sort_keys=True) + "\n")
            temporary_name = temporary.name
        os.replace(temporary_name, args.output)
        temporary_name = None
    finally:
        if temporary_name is not None:
            Path(temporary_name).unlink(missing_ok=True)
    print(f"DSP code-generation checks passed: {args.output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except CheckError as error:
        raise SystemExit(f"DSP code-generation check failed: {error}") from error
