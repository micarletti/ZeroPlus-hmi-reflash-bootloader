#!/usr/bin/env python3
"""
Generate a release .bin package (same format used by ZeroPlus-hmi binary_generator.py)
from a bootloader ELF image.
"""

from __future__ import annotations

import argparse
import re
import shutil
import sys
from pathlib import Path
from typing import Tuple

from generate_bootloader_image_header import (
    collect_bootloader_image,
    normalize_input_path,
    parse_int,
    read_elf_load_segments,
)

CHUNK_SIZE = 1024


def compute_crc(data: bytes) -> int:
    crc = 0xFFFF
    for value in data:
        crc ^= value
        for _ in range(8):
            if crc & 0x0001:
                crc >>= 1
                crc ^= 0xA001
            else:
                crc >>= 1
    return crc & 0xFFFF


def parse_bootloader_version(version_file: Path) -> Tuple[int, int]:
    if not version_file.exists():
        raise ValueError(f"Version file not found: {version_file}")

    content = version_file.read_text(encoding="utf-8", errors="ignore")

    major_match = re.search(r"#define\s+BTL_MAJOR_VERSION\s+(\d+)", content)
    minor_match = re.search(r"#define\s+BTL_MINOR_VERSION\s+(\d+)", content)

    if not major_match or not minor_match:
        raise ValueError(
            "Could not find BTL_MAJOR_VERSION / BTL_MINOR_VERSION in version file"
        )

    major = int(major_match.group(1))
    minor = int(minor_match.group(1))
    return major, minor


def derive_app_name_from_elf(elf_path: Path) -> str:
    name = elf_path.name
    match = re.match(r"^(.*)\.X\.[^.]+\.elf$", name, flags=re.IGNORECASE)
    if match:
        return match.group(1)
    return elf_path.stem


def build_release_payload(internal_data: bytes, version: int) -> bytes:
    padded = bytearray(internal_data)

    if len(padded) % CHUNK_SIZE != 0:
        padded.extend(b"\xFF" * (CHUNK_SIZE - (len(padded) % CHUNK_SIZE)))

    int_len = len(padded)
    int_chunk_n = (int_len // CHUNK_SIZE) + 1
    int_crc = compute_crc(padded)

    total_len = int_len
    total_chunk_n = int_chunk_n
    total_crc = int_crc

    header = bytearray()
    header.extend(total_len.to_bytes(4, byteorder="little"))
    header.extend(version.to_bytes(2, byteorder="little"))
    header.extend(total_chunk_n.to_bytes(2, byteorder="little"))
    header.extend(total_crc.to_bytes(2, byteorder="little"))
    header.extend(int_chunk_n.to_bytes(2, byteorder="little"))
    header.extend(int_crc.to_bytes(2, byteorder="little"))
    header.extend((0).to_bytes(2, byteorder="little"))  # ext_chunk = 0
    header.extend((0).to_bytes(2, byteorder="little"))  # ext_crc = 0
    header.extend(bytes(1006))

    if len(header) != CHUNK_SIZE:
        raise ValueError("Release header size mismatch")

    return bytes(header) + bytes(padded)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate Release/<APP_NAME><MAJOR><MINOR>.bin from bootloader ELF"
    )
    parser.add_argument("--elf", required=True, help="Input ELF path")
    parser.add_argument(
        "--release-dir",
        default="Release",
        help="Output release directory (default: Release)",
    )
    parser.add_argument(
        "--app-name",
        default=None,
        help="App name prefix for output filename (default: derived from ELF name)",
    )
    parser.add_argument(
        "--version-file",
        default="src/config/default/bootloader/bootloader_common.c",
        help="File that contains BTL_MAJOR_VERSION and BTL_MINOR_VERSION",
    )
    parser.add_argument(
        "--flash-start",
        default="0x9FC01000",
        help="Bootloader flash start address",
    )
    parser.add_argument(
        "--max-size",
        default="0x4000",
        help="Maximum bootloader size in bytes",
    )
    parser.add_argument(
        "--no-clean",
        action="store_true",
        help="Do not remove existing release directory before generating",
    )

    args = parser.parse_args()

    elf_path = normalize_input_path(args.elf)
    release_dir = Path(args.release_dir)
    version_file = Path(args.version_file)
    flash_start = parse_int(args.flash_start)
    max_size = parse_int(args.max_size)

    if not elf_path.exists():
        print(f"ERROR: ELF file not found: {elf_path}", file=sys.stderr)
        return 1

    try:
        major, minor = parse_bootloader_version(version_file)
        version = ((major & 0xFF) << 8) | (minor & 0xFF)

        elf_data = elf_path.read_bytes()
        segments = read_elf_load_segments(elf_data)
        internal_data = collect_bootloader_image(segments, flash_start, max_size)

        release_payload = build_release_payload(internal_data, version)

        if release_dir.exists() and not args.no_clean:
            shutil.rmtree(release_dir)

        release_dir.mkdir(parents=True, exist_ok=True)

        app_name = args.app_name if args.app_name else derive_app_name_from_elf(elf_path)
        output_path = release_dir / f"{app_name}{major:02d}{minor:02d}.bin"
        output_path.write_bytes(release_payload)

    except Exception as exc:  # noqa: BLE001
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    print("Release binary generated:")
    print(f"  input ELF    : {elf_path}")
    print(f"  version      : {major}.{minor}")
    print(f"  output       : {output_path}")
    print(f"  payload bytes: {len(release_payload)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
