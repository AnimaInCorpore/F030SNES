"""Replay a DSP-1 host access log through the firmware interpreter.

    python3 -m tools.smkx.dsp1_stream assets/dsp1.bin build/smkx/DSP1LOG.BIN

The log (falcon/smkx_upd.s, SMKX_DSP1_LOG) holds one big-endian word per
host access: bit 15 = write, bit 14 = SR, bits 7-0 = the byte. Every read
must equal what the interpreter answers, which re-checks the translated
firmware on the game's own traffic. The report splits the stream into
commands (a byte written while the program waits in its command poll),
counts them and the chip instructions each kind costs, and can write the
stream in the form tools/dsp1_hle_test.c replays.
"""

from __future__ import annotations

import argparse
import collections
import struct
from pathlib import Path

from .upd7725 import UPD, load_firmware

NAMES = {
    0x00: "multiply", 0x20: "multiply", 0x10: "inverse", 0x30: "inverse",
    0x04: "triangle", 0x24: "triangle", 0x08: "radius", 0x18: "range", 0x38: "range",
    0x28: "distance", 0x0C: "rotate", 0x2C: "rotate", 0x1C: "polar", 0x3C: "polar",
    0x02: "parameter", 0x12: "parameter", 0x22: "parameter", 0x32: "parameter",
    0x0A: "raster", 0x1A: "raster", 0x2A: "raster", 0x3A: "raster",
    0x06: "project", 0x16: "project", 0x26: "project", 0x36: "project",
    0x0E: "target", 0x1E: "target", 0x2E: "target", 0x3E: "target",
    0x01: "attitude A", 0x05: "objective A", 0x0D: "subjective A", 0x0B: "scalar A",
    0x14: "gyrate", 0x0F: "memory test", 0x1F: "memory dump", 0x2F: "memory size",
}


def command_pc(upd: UPD) -> int:
    """The program counter of the command poll, reached from reset."""
    upd.run()
    return upd.pc


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("firmware", type=Path)
    ap.add_argument("log", type=Path)
    ap.add_argument("--stream", type=Path, help="write the checked stream for dsp1_hle_test")
    args = ap.parse_args()
    program, rom = load_firmware(args.firmware)
    upd = UPD(program, rom)
    idle = command_pc(upd)
    raw = args.log.read_bytes()
    words = struct.unpack(f">{len(raw) // 2}H", raw[: len(raw) // 2 * 2])
    counts: collections.Counter = collections.Counter()
    steps: collections.Counter = collections.Counter()
    mismatches = []
    current = None
    started = 0
    for i, w in enumerate(words):
        write, sr, byte = w & 0x8000, w & 0x4000, w & 0xFF
        if write:
            upd.run()
            if upd.pc == idle and not (upd.sr & 0x1000):     # command poll, first byte
                if current is not None:
                    steps[current] += upd.steps - started
                current = byte
                counts[byte] += 1
                started = upd.steps
            upd.host_write_dr(byte)
        else:
            got = upd.host_read_sr() if sr else upd.host_read_dr()
            if got != byte:
                mismatches.append((i, "SR" if sr else "DR", byte, got))
    if current is not None:
        steps[current] += upd.steps - started
    print(f"{len(words)} host accesses, command poll at uPD pc {idle:#05x}")
    print(f"reads that differ from the interpreter: {len(mismatches)}")
    for m in mismatches[:10]:
        print(f"  access {m[0]}: {m[1]} logged ${m[2]:02X}, interpreter ${m[3]:02X}")
    total = sum(steps.values()) or 1
    print("commands (count, chip instructions, share):")
    for cmd, n in counts.most_common():
        print(f"  ${cmd:02X} {NAMES.get(cmd, '?'):12} {n:7}  {steps[cmd]:10}  {100 * steps[cmd] / total:5.1f}%")
    if args.stream:
        args.stream.write_bytes(raw)
        print(f"wrote {args.stream}")


if __name__ == "__main__":
    main()
