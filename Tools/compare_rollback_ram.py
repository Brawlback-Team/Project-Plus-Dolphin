#!/usr/bin/env python3
"""Compare two rollback RAM diagnostic dump sets and print guest addresses."""

from __future__ import annotations

import argparse
import mmap
from pathlib import Path


PAGE_SIZE = 4096
REGIONS = (
    ("mem1", 0x80000000, True),
    ("mem2", 0x90000000, False),
    ("aram", 0x00000000, False),
    ("fakevmem", 0x7E000000, False),
    ("l1", 0xE0000000, True),
)


def dump_path(prefix: Path, region: str) -> Path:
    return Path(f"{prefix}-{region}.bin")


def differing_runs(left: bytes, right: bytes, base: int):
    start = None
    for index, (a, b) in enumerate(zip(left, right)):
        if a != b and start is None:
            start = index
        elif a == b and start is not None:
            yield base + start, left[start:index], right[start:index]
            start = None
    if start is not None:
        yield base + start, left[start:], right[start:]


def compare_region(left_path: Path, right_path: Path, guest_base: int, max_details: int) -> int:
    if not left_path.exists() or not right_path.exists():
        print(f"missing region: {left_path} or {right_path}")
        return 0
    if left_path.stat().st_size != right_path.stat().st_size:
        print(
            f"size mismatch: {left_path.name}={left_path.stat().st_size}, "
            f"{right_path.name}={right_path.stat().st_size}"
        )
        return 0

    differing_pages = 0
    details = 0
    with left_path.open("rb") as left_file, right_path.open("rb") as right_file:
        with mmap.mmap(left_file.fileno(), 0, access=mmap.ACCESS_READ) as left:
            with mmap.mmap(right_file.fileno(), 0, access=mmap.ACCESS_READ) as right:
                for offset in range(0, len(left), PAGE_SIZE):
                    left_page = left[offset : offset + PAGE_SIZE]
                    right_page = right[offset : offset + PAGE_SIZE]
                    if left_page == right_page:
                        continue
                    differing_pages += 1
                    if details >= max_details:
                        continue
                    address = guest_base + offset
                    print(f"  page 0x{address:08X} (region offset 0x{offset:08X})")
                    for run_address, left_bytes, right_bytes in differing_runs(
                        left_page, right_page, address
                    ):
                        if details >= max_details:
                            break
                        shown = min(len(left_bytes), 32)
                        suffix = " ..." if shown < len(left_bytes) else ""
                        print(
                            f"    0x{run_address:08X} length {len(left_bytes)}\n"
                            f"      P1: {left_bytes[:shown].hex(' ')}{suffix}\n"
                            f"      P2: {right_bytes[:shown].hex(' ')}{suffix}"
                        )
                        details += 1
    return differing_pages


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare P1/P2 rollback RAM dump prefixes produced at the first mismatch."
    )
    parser.add_argument("p1_prefix", type=Path, help="path ending in ...-p1, without -mem1.bin")
    parser.add_argument("p2_prefix", type=Path, help="path ending in ...-p2, without -mem1.bin")
    parser.add_argument("--max-details", type=int, default=128)
    args = parser.parse_args()

    total_pages = 0
    for region, guest_base, required in REGIONS:
        left_path = dump_path(args.p1_prefix, region)
        right_path = dump_path(args.p2_prefix, region)
        if not required and not left_path.exists() and not right_path.exists():
            continue
        print(f"{region.upper()}:")
        count = compare_region(
            left_path,
            right_path,
            guest_base,
            args.max_details,
        )
        total_pages += count
        print(f"  differing 4 KiB pages: {count}")

    if total_pages == 0:
        print("RAM dumps match.")
        return 0
    print(f"Total differing 4 KiB pages: {total_pages}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
