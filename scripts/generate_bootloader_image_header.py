#!/usr/bin/env python3
"""
Generate a C header containing bootloader bytes from a PIC32 ELF file.

The generated header is consumed by src/UpdateStateMachine.c.
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path
from typing import Iterable, List, Tuple

PT_LOAD = 1


def parse_int(value: str) -> int:
    return int(value, 0)


def normalize_input_path(path_str: str) -> Path:
    """Allow both Windows-style and POSIX-style paths."""
    direct_path = Path(path_str)
    if direct_path.exists():
        return direct_path

    win_drive_match = re.match(r"^([A-Za-z]):\\(.*)$", path_str)
    if win_drive_match:
        drive = win_drive_match.group(1).lower()
        suffix = win_drive_match.group(2).replace("\\", "/")
        return Path(f"/mnt/{drive}/{suffix}")
    return direct_path


def normalize_mips_kseg(addr: int) -> int:
    """Map kseg1 aliases into the matching kseg0 region."""
    if 0xA0000000 <= addr <= 0xBFFFFFFF:
        return addr - 0x20000000
    return addr


def read_elf_load_segments(elf_data: bytes) -> List[Tuple[int, int, bytes]]:
    if len(elf_data) < 0x34 or elf_data[0:4] != b"\x7fELF":
        raise ValueError("Input is not a valid ELF file")

    elf_class = elf_data[4]
    data_encoding = elf_data[5]

    if data_encoding == 1:
        endian = "<"
    elif data_encoding == 2:
        endian = ">"
    else:
        raise ValueError("Unsupported ELF endianness")

    if elf_class == 1:
        header_fmt = endian + "HHIIIIIHHHHHH"
        (
            _e_type,
            _e_machine,
            _e_version,
            _e_entry,
            e_phoff,
            _e_shoff,
            _e_flags,
            _e_ehsize,
            e_phentsize,
            e_phnum,
            _e_shentsize,
            _e_shnum,
            _e_shstrndx,
        ) = struct.unpack_from(header_fmt, elf_data, 16)
        ph_fmt = endian + "IIIIIIII"
    elif elf_class == 2:
        header_fmt = endian + "HHIQQQIHHHHHH"
        (
            _e_type,
            _e_machine,
            _e_version,
            _e_entry,
            e_phoff,
            _e_shoff,
            _e_flags,
            _e_ehsize,
            e_phentsize,
            e_phnum,
            _e_shentsize,
            _e_shnum,
            _e_shstrndx,
        ) = struct.unpack_from(header_fmt, elf_data, 16)
        ph_fmt = endian + "IIQQQQQQ"
    else:
        raise ValueError("Unsupported ELF class")

    segments: List[Tuple[int, int, bytes]] = []

    for idx in range(e_phnum):
        ph_offset = e_phoff + idx * e_phentsize
        if ph_offset + e_phentsize > len(elf_data):
            raise ValueError("ELF program header table is truncated")

        if elf_class == 1:
            (
                p_type,
                p_offset,
                p_vaddr,
                p_paddr,
                p_filesz,
                _p_memsz,
                _p_flags,
                _p_align,
            ) = struct.unpack_from(ph_fmt, elf_data, ph_offset)
        else:
            (
                p_type,
                _p_flags,
                p_offset,
                p_vaddr,
                p_paddr,
                p_filesz,
                _p_memsz,
                _p_align,
            ) = struct.unpack_from(ph_fmt, elf_data, ph_offset)

        if p_type != PT_LOAD or p_filesz == 0:
            continue

        data_end = p_offset + p_filesz
        if data_end > len(elf_data):
            raise ValueError("ELF load segment points outside file bounds")

        seg_data = elf_data[p_offset:data_end]
        segments.append((p_paddr, p_vaddr, seg_data))

    if not segments:
        raise ValueError("No PT_LOAD segments found in ELF")

    return segments


def collect_bootloader_image(
    segments: Iterable[Tuple[int, int, bytes]],
    flash_start: int,
    max_size: int,
) -> bytes:
    flash_end = flash_start + max_size
    pieces: List[Tuple[int, bytes]] = []
    image_end = flash_start

    for paddr, vaddr, seg_data in segments:
        placed = False
        for base in (normalize_mips_kseg(paddr), normalize_mips_kseg(vaddr)):
            seg_start = base
            seg_end = base + len(seg_data)
            overlap_start = max(seg_start, flash_start)
            overlap_end = min(seg_end, flash_end)

            if overlap_start >= overlap_end:
                continue

            rel_start = overlap_start - seg_start
            rel_end = rel_start + (overlap_end - overlap_start)
            pieces.append((overlap_start, seg_data[rel_start:rel_end]))
            image_end = max(image_end, overlap_end)
            placed = True
            break

        if not placed:
            continue

    if not pieces:
        raise ValueError(
            f"No ELF loadable data overlaps flash range 0x{flash_start:08X}-0x{flash_end:08X}"
        )

    image_size = image_end - flash_start
    if image_size <= 0 or image_size > max_size:
        raise ValueError("Computed image size is out of allowed bounds")

    image = bytearray([0xFF] * image_size)

    for piece_start, piece_data in pieces:
        dst_offset = piece_start - flash_start
        for idx, value in enumerate(piece_data):
            dst_idx = dst_offset + idx
            current = image[dst_idx]
            if current != 0xFF and current != value:
                raise ValueError(
                    f"Conflicting segment data at image offset 0x{dst_idx:08X}"
                )
            image[dst_idx] = value

    return bytes(image)


def emit_header(output_path: Path, symbol: str, image: bytes) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    guard = "GENERATED_BOOTLOADER_IMAGE_H"

    lines: List[str] = []
    lines.append("/* Auto-generated file. Do not edit manually. */")
    lines.append("#ifndef " + guard)
    lines.append("#define " + guard)
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append(f"#define BOOTLOADER_IMAGE_SIZE ({len(image)}U)")
    lines.append("")
    lines.append(f"static const uint8_t {symbol}[BOOTLOADER_IMAGE_SIZE] =")
    lines.append("{")

    bytes_per_line = 12
    for i in range(0, len(image), bytes_per_line):
        chunk = image[i : i + bytes_per_line]
        row = ", ".join(f"0x{b:02X}" for b in chunk)
        if i + bytes_per_line < len(image):
            row += ","
        lines.append("    " + row)

    lines.append("};")
    lines.append("")
    lines.append("#endif /* " + guard + " */")
    lines.append("")

    output_path.write_text("\n".join(lines), encoding="ascii")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Convert bootloader ELF loadable segments into a C byte array header"
    )
    parser.add_argument(
        "--elf",
        required=True,
        help="Path to input ELF (Windows or POSIX path)",
    )
    parser.add_argument(
        "--output",
        default="src/generated_bootloader_image.h",
        help="Path to generated header file",
    )
    parser.add_argument(
        "--symbol",
        default="g_bootloaderImage",
        help="C symbol name for generated byte array",
    )
    parser.add_argument(
        "--flash-start",
        default="0x9FC01000",
        help="Bootloader flash start address",
    )
    parser.add_argument(
        "--max-size",
        default="0x4000",
        help="Maximum image size in bytes",
    )

    args = parser.parse_args()

    elf_path = normalize_input_path(args.elf)
    output_path = Path(args.output)
    flash_start = parse_int(args.flash_start)
    max_size = parse_int(args.max_size)

    if not elf_path.exists():
        print(f"ERROR: ELF file not found: {elf_path}", file=sys.stderr)
        return 1

    try:
        elf_data = elf_path.read_bytes()
        segments = read_elf_load_segments(elf_data)
        image = collect_bootloader_image(segments, flash_start, max_size)
        emit_header(output_path, args.symbol, image)
    except Exception as exc:  # noqa: BLE001
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    print("Generated bootloader header:")
    print(f"  input ELF : {elf_path}")
    print(f"  output    : {output_path}")
    print(f"  symbol    : {args.symbol}")
    print(f"  flash base: 0x{flash_start:08X}")
    print(f"  image size: {len(image)} bytes")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
