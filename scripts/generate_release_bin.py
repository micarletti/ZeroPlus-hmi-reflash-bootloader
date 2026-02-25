#!/usr/bin/env python3
"""
Generate a release .bin package in the same format used by
ZeroPlus-hmi v2.11 binary_generator.py.
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


def parse_release_version(version_file: Path) -> Tuple[int, int]:
    if not version_file.exists():
        raise ValueError(f"Version file not found: {version_file}")

    content = version_file.read_text(encoding="utf-8", errors="ignore")

    hmi_match = re.search(
        r"#define\s+HMI_FWREL\s+ENCODE_VERSION\(\s*(\d+)\s*,\s*(\d+)\s*\)",
        content,
    )
    if hmi_match:
        return int(hmi_match.group(1)), int(hmi_match.group(2))

    major_match = re.search(r"#define\s+BTL_MAJOR_VERSION\s+(\d+)", content)
    minor_match = re.search(r"#define\s+BTL_MINOR_VERSION\s+(\d+)", content)

    if not major_match or not minor_match:
        raise ValueError(
            "Could not find HMI_FWREL or BTL_MAJOR_VERSION / BTL_MINOR_VERSION in version file"
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

    total_len = len(padded)
    total_chunk_n = (total_len // CHUNK_SIZE) + 1
    total_crc = compute_crc(padded)

    # Build a header compatible with both:
    # - legacy bootloader: len(4), version(2), chunk(2), crc(2)
    # - newer bootloader: adds int_chunk/int_crc/ext_chunk/ext_crc in reserved area
    #
    # For this reflasher payload there is only internal data:
    # int_chunk == total_chunk_n, int_crc == total_crc, ext_chunk == 0.
    int_chunk = total_chunk_n
    int_crc = total_crc
    ext_chunk = 0
    ext_crc = 0

    header = bytearray()
    header.extend(total_len.to_bytes(4, byteorder="little"))
    header.extend(version.to_bytes(2, byteorder="little"))
    header.extend(total_chunk_n.to_bytes(2, byteorder="little"))
    header.extend(total_crc.to_bytes(2, byteorder="little"))
    header.extend(int_chunk.to_bytes(2, byteorder="little"))
    header.extend(int_crc.to_bytes(2, byteorder="little"))
    header.extend(ext_chunk.to_bytes(2, byteorder="little"))
    header.extend(ext_crc.to_bytes(2, byteorder="little"))
    header.extend(bytes(CHUNK_SIZE - len(header)))

    if len(header) != CHUNK_SIZE:
        raise ValueError("Release header size mismatch")

    return bytes(header) + bytes(padded)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate legacy Release package from raw .bin data (or ELF fallback)"
    )
    parser.add_argument(
        "--input-bin",
        default=None,
        help="Input raw binary path (preferred, matches v2.11 flow)",
    )
    parser.add_argument(
        "--elf",
        default=None,
        help="Input ELF path (fallback mode when --input-bin is not provided)",
    )
    parser.add_argument(
        "--release-dir",
        default="Release",
        help="Output release directory (default: Release)",
    )
    parser.add_argument(
        "--output-prefix",
        default=None,
        help="Full output prefix path (example: ../Release/ZP_D). If set, --release-dir and --app-name are ignored",
    )
    parser.add_argument(
        "--app-name",
        default=None,
        help="App name prefix for output filename (default: derived from ELF name)",
    )
    parser.add_argument(
        "--version-file",
        default="src/config/default/bootloader/bootloader_common.c",
        help="File that contains HMI_FWREL or BTL_MAJOR_VERSION / BTL_MINOR_VERSION",
    )
    parser.add_argument(
        "--flash-start",
        default="0x9D000000",
        help="Application flash start address",
    )
    parser.add_argument(
        "--max-size",
        default="0x200000",
        help="Maximum application image size in bytes",
    )
    parser.add_argument(
        "--no-clean",
        action="store_true",
        help="Do not remove existing release directory before generating",
    )

    args = parser.parse_args()

    version_file = Path(args.version_file)
    flash_start = parse_int(args.flash_start) if args.elf else 0
    max_size = parse_int(args.max_size) if args.elf else 0

    try:
        major, minor = parse_release_version(version_file)
        version = ((major & 0xFF) << 8) | (minor & 0xFF)

        input_label = ""
        if args.input_bin:
            input_path = normalize_input_path(args.input_bin)
            if not input_path.exists():
                print(f"ERROR: input binary not found: {input_path}", file=sys.stderr)
                return 1
            internal_data = input_path.read_bytes()
            input_label = str(input_path)
        elif args.elf:
            elf_path = normalize_input_path(args.elf)
            if not elf_path.exists():
                print(f"ERROR: ELF file not found: {elf_path}", file=sys.stderr)
                return 1

            elf_data = elf_path.read_bytes()
            segments = read_elf_load_segments(elf_data)
            internal_data = collect_bootloader_image(segments, flash_start, max_size)
            input_label = str(elf_path)
        else:
            print("ERROR: one of --input-bin or --elf must be provided", file=sys.stderr)
            return 1

        release_payload = build_release_payload(internal_data, version)

        if args.output_prefix:
            output_prefix = Path(args.output_prefix)
            output_path = Path(f"{output_prefix}{major:02d}{minor:02d}.bin")
            output_path.parent.mkdir(parents=True, exist_ok=True)
        else:
            release_dir = Path(args.release_dir)
            if release_dir.exists() and not args.no_clean:
                shutil.rmtree(release_dir)

            release_dir.mkdir(parents=True, exist_ok=True)

            if args.app_name:
                app_name = args.app_name
            elif args.elf:
                app_name = derive_app_name_from_elf(normalize_input_path(args.elf))
            else:
                app_name = "ZP_D"

            output_path = release_dir / f"{app_name}{major:02d}{minor:02d}.bin"
        output_path.write_bytes(release_payload)

    except Exception as exc:  # noqa: BLE001
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    print("Release binary generated:")
    print(f"  input        : {input_label}")
    print(f"  version      : {major}.{minor}")
    print(f"  output       : {output_path}")
    print(f"  payload bytes: {len(release_payload)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
