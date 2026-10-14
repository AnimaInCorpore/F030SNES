#!/usr/bin/env python3
"""Extract source-addressed Mario Circuit 1 assets from the matching ROM.

The pinned third_party/Super-Mario-Kart-Disassembly submodule is the reference:
SMK/RomMap/ROM_Map_SMK_U1.asm identifies the build; CODE_81E745 and
CODE_84E09E perform the nested track-map decompression; CODE_81E6D4 loads the
Mode 7 graphics and CODE_81E72E loads the course palette; CODE_84E378 and
CODE_84E3C7 expand Mode 7 tiles. This tool does not generate gameplay state,
screen layout, sprite poses, or character palette assignments.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import struct
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SMK_ROOT = ROOT / "third_party/Super-Mario-Kart-Disassembly/SMK"
U1_MAP = SMK_ROOT / "RomMap/ROM_Map_SMK_U1.asm"
ROUTINES = SMK_ROOT / "Routine_Macros_SMK.asm"
ASSET_POINTERS = SMK_ROOT / "AsarScripts/AssetPointersAndFiles.asm"
DB16 = (
    "140c1c", "442434", "30346d", "4e4a4e", "854c30", "346524",
    "d04648", "757161", "597dce", "d27d2c", "8595a1", "6daa2c",
    "d2aa99", "6dc2ca", "dad45e", "deeed6",
)
PALETTE = tuple(bytes.fromhex(c) for c in DB16)


def lz5(data: bytes, expected: int | None = None) -> bytes:
    """SNES LZ5; reject truncated commands, invalid copies and trailing data."""
    out = bytearray()
    p = 0

    def take(n: int) -> bytes:
        nonlocal p
        if p + n > len(data):
            raise ValueError("truncated LZ5 command")
        value = data[p:p + n]
        p += n
        return value

    while True:
        head = take(1)[0]
        if head == 0xFF:
            break
        if head >= 0xE0:
            command = (head >> 2) & 7
            length = (((head & 3) << 8) | take(1)[0]) + 1
        else:
            command = head >> 5
            length = (head & 31) + 1
        if expected is not None and len(out) + length > expected:
            raise ValueError("LZ5 output exceeded expected length")
        if command == 0:
            out.extend(take(length))
        elif command == 1:
            out.extend(take(1) * length)
        elif command == 2:
            pair = take(2)
            out.extend((pair * ((length + 1) // 2))[:length])
        elif command == 3:
            start = take(1)[0]
            out.extend((start + i) & 255 for i in range(length))
        elif command in (4, 5):
            lo, hi = take(2)
            source = lo | (hi << 8)
            for i in range(length):
                if source + i >= len(out):
                    raise ValueError("invalid absolute LZ5 copy")
                out.append(out[source + i] ^ (255 if command == 5 else 0))
        else:
            distance = take(1)[0]
            if not distance or distance > len(out):
                raise ValueError("invalid relative LZ5 copy")
            for _ in range(length):
                out.append(out[-distance] ^ (255 if command == 7 else 0))
    if p != len(data) or (expected is not None and len(out) != expected):
        raise ValueError(f"LZ5 size mismatch: {p}/{len(data)} input, "
                         f"{len(out)}/{expected} output")
    return bytes(out)


def source_label_address(label: str) -> int:
    text = ROUTINES.read_text(encoding="utf-8")
    match = re.search(
        rf"^{re.escape(label)}:\s*\r?\n\s*;\$([0-9A-Fa-f]{{6}})",
        text, re.MULTILINE,
    )
    if not match:
        raise ValueError(f"could not resolve source label {label}")
    return int(match.group(1), 16)


def rom_offset(snes_address: int) -> int:
    return (((snes_address >> 16) & 0x3F) << 16) | (snes_address & 0xFFFF)


def source_label_range(start_label: str, end_label: str) -> tuple[int, int]:
    start = rom_offset(source_label_address(start_label))
    end = rom_offset(source_label_address(end_label))
    if start >= end:
        raise ValueError(f"invalid source range {start_label}..{end_label}")
    return start, end


def source_asset_range(asset: str) -> tuple[int, int]:
    text = ASSET_POINTERS.read_text(encoding="utf-8")
    match = re.search(
        rf"^\s*dl\s+\$([0-9A-Fa-f]{{6}}),\s*\$([0-9A-Fa-f]{{6}}),"
        rf"\s*{re.escape(asset)}\s*,",
        text, re.MULTILINE,
    )
    if not match:
        raise ValueError(f"could not resolve source asset {asset}")
    start, end = (rom_offset(int(value, 16)) for value in match.groups())
    if start >= end:
        raise ValueError(f"invalid source asset range for {asset}")
    return start, end


def decompress_rom_range(rom: bytes, region: tuple[int, int], expected: int) -> bytes:
    start, end = region
    if not 0 <= start < end <= len(rom):
        raise ValueError(f"ROM range outside U1 image: {start:#x}..{end:#x}")
    decoded = lz5(rom[start:end])
    if len(decoded) != expected:
        raise ValueError(f"unexpected decoded size for {start:#x}..{end:#x}: "
                         f"{len(decoded)} != {expected}")
    return decoded


def expand_tiles(track: bytes, common: bytes) -> bytes:
    # CODE_84E3C7: a per-tile high palette nibble, then 32 packed low-nibble
    # bytes. Zero is left as transparent index 0; nonzero gains the high nibble.
    result = bytearray(256 * 64)
    for first, data, count in ((0, track, 192), (192, common, 64)):
        if len(data) != 256 + 32 * count:
            raise ValueError("unexpected Mode 7 tile section length")
        for tile in range(count):
            high = data[tile]
            for i in range(32):
                pair = data[256 + tile * 32 + i]
                for half, nibble in enumerate((pair & 15, pair >> 4)):
                    result[(first + tile) * 64 + i * 2 + half] = (
                        high | nibble if nibble else 0
                    )
    return bytes(result)


def snes_rgb(word: int) -> tuple[int, int, int]:
    return tuple((v << 3) | (v >> 2)
                 for v in ((word & 31), ((word >> 5) & 31), ((word >> 10) & 31)))


def palette_lookup(snes_palette: bytes) -> bytes:
    # Fixed DawnBringer 16 swatches. A source palette index always has the
    # same target index: no spatial dithering that would shimmer in Mode 7.
    lookup = bytearray(256)
    for i in range(256):
        word = struct.unpack_from("<H", snes_palette, i * 2)[0]
        rgb = snes_rgb(word)
        lookup[i] = min(range(16), key=lambda j: sum(
            (rgb[channel] - PALETTE[j][channel]) ** 2 for channel in range(3)
        ))
    return bytes(lookup)


def make_texture(course_map: bytes, tiles: bytes, lookup: bytes) -> bytes:
    texture = bytearray(1024 * 1024)
    for my in range(128):
        for mx in range(128):
            tile = course_map[my * 128 + mx]
            for ty in range(8):
                src = tile * 64 + ty * 8
                dst = (my * 8 + ty) * 1024 + mx * 8
                texture[dst:dst + 8] = bytes(lookup[p] for p in tiles[src:src + 8])
    return bytes(texture)


def source_rom_md5() -> str:
    text = U1_MAP.read_text(encoding="utf-8")
    match = re.search(
        r'!Define_Global_BaseROMMD5Hash\s*=\s*"([0-9a-f]{32})"', text,
        re.IGNORECASE,
    )
    if not match:
        raise ValueError(f"could not read U1 base-ROM hash from {U1_MAP}")
    return match.group(1).lower()


def require_source_asset_match(rom: bytes, region: tuple[int, int], path: Path) -> None:
    start, end = region
    if rom[start:end] != path.read_bytes():
        raise ValueError(f"ROM range {start:#x}..{end:#x} differs from {path}")


def ppm(path: Path, width: int, height: int, indexed: bytes) -> None:
    with path.open("wb") as f:
        f.write(f"P6\n{width} {height}\n255\n".encode())
        f.write(b"".join(PALETTE[p] for p in indexed))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom_zip", type=Path)
    parser.add_argument("out", type=Path)
    args = parser.parse_args()
    with zipfile.ZipFile(args.rom_zip) as archive:
        members = [n for n in archive.namelist() if n.lower().endswith(".sfc")]
        if len(members) != 1:
            raise ValueError("expected exactly one .sfc in ROM ZIP")
        rom = archive.read(members[0])
    expected_md5 = source_rom_md5()
    digest = hashlib.md5(rom).hexdigest()
    if len(rom) != 524288 or digest != expected_md5:
        raise ValueError(f"expected 512 KiB U1 ROM MD5 {expected_md5}, "
                         f"got {digest}")
    map_region = source_label_range(
        "SMK_LevelData_MarioCircuit1", "SMK_LevelData_MarioCircuit4"
    )
    palette_region = source_label_range(
        "SMK_PaletteData_MarioCircuitCourse", "SMK_PaletteData_GhostValleyCourse"
    )
    require_source_asset_match(
        rom, map_region, SMK_ROOT / "TrackData/MarioCircuit1.lz5"
    )
    require_source_asset_match(
        rom, palette_region, SMK_ROOT / "Palettes/MarioCircuitCourse.lz5"
    )
    nested_map = decompress_rom_range(rom, map_region, 4413)
    course_map = lz5(nested_map, 16384)
    track = decompress_rom_range(
        rom, source_asset_range("GFX_Mode7_MarioCircuitTrack"), 6400
    )
    common = decompress_rom_range(
        rom, source_asset_range("GFX_Mode7_GlobalTrackTiles"), 2304
    )
    source_palette = decompress_rom_range(rom, palette_region, 512)
    tiles = expand_tiles(track, common)
    lookup = palette_lookup(source_palette)
    texture = make_texture(course_map, tiles, lookup)
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "mc1_map.bin").write_bytes(course_map)
    (args.out / "mc1_texture.bin").write_bytes(texture)
    # Falcon extended CLUT: red, green, reserved, blue; low two bits ignored.
    (args.out / "db16_clut.bin").write_bytes(b"".join(
        bytes((rgb[0] & 0xFC, rgb[1] & 0xFC, 0, rgb[2] & 0xFC))
        for rgb in PALETTE
    ))
    overview = bytes(texture[(y * 4) * 1024 + x * 4]
                     for y in range(256) for x in range(256))
    ppm(args.out / "mc1-map.ppm", 256, 256, overview)
    print(f"USA original ROM {digest}: Mario Circuit 1 map 128x128, "
          "texture 1024x1024, course palette quantized to DawnBringer 16")


if __name__ == "__main__":
    main()
