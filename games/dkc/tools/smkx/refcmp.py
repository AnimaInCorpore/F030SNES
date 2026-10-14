"""Compare the Falcon's dumps with the reference interpreter.

    python3 -m tools.smkx.refcmp build/dkcx/D*.BIN [--input SPEC] [--watch ADDR ...]

Dnnnnnnn.BIN come from a build with DKCX_DUMP_AT/_EVERY/_COUNT and the
same DKCX_INPUT: WRAM, VRAM, CGRAM, OAM and the I/O shadow just before a
game frame starts. tools/smkx/ref65816.py runs the ROM from reset once and
compares at every dump's boundary. The stack page ($0100-$01FF) is left
out: below S it holds dead bytes, among them P values the translation
pushes without the I flag. With --watch ADDR (hex WRAM offset), every
write to it during the frame before --watch-at is printed with the 65816
address of the instruction, which is where to look in the translation.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from . import ref65816
from .__main__ import load_rom

REGIONS = (("wram", 0x20000), ("vram", 0x10000), ("cgram", 512), ("oam", 544), ("io", 0x6000),
           ("sram", 0x800))


def ranges(diff: list[int]) -> list[tuple[int, int]]:
    out = []
    for i in diff:
        if out and i == out[-1][1] + 1:
            out[-1][1] = i
        else:
            out.append([i, i])
    return [tuple(r) for r in out]


def load_dump(path: Path):
    d = path.read_bytes()
    if d[:8] != b"DKCXDUMP":
        raise SystemExit(f"{path}: not a dump")
    frame = int.from_bytes(d[8:12], "big")
    o, blocks = 12, {}
    for name, n in REGIONS:
        blocks[name] = d[o:o + n]
        o += n
    return frame, blocks


def compare(m, blocks, detail: int) -> int:
    total = 0
    for name, ref in (("wram", m.wram), ("vram", m.vram), ("cgram", m.cgram), ("oam", m.oam),
                      ("sram", m.sram)):
        fal = blocks[name]
        diff = [i for i in range(len(ref)) if ref[i] != fal[i] and not (name == "wram" and 0x100 <= i < 0x200)]
        total += len(diff)
        if diff:
            print(f"  {name}: {len(diff)} bytes differ")
            for a, b in ranges(diff)[:detail]:
                show = " ".join(f"{fal[i]:02X}/{ref[i]:02X}" for i in range(a, min(b + 1, a + 8)))
                print(f"    ${a:05X}-${b:05X}  falcon/ref {show}")
    return total


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("dumps", type=Path, nargs="+")
    ap.add_argument("--rom", type=Path, default=Path("build/dkcx/dkc.sfc"))
    ap.add_argument("--input", default="", help="the DKCX_INPUT of the Falcon build")
    ap.add_argument("--detail", type=int, default=12, help="ranges listed per region")
    ap.add_argument("--stop", action="store_true", help="stop at the first differing dump")
    ap.add_argument("--watch", nargs="*", default=[])
    ap.add_argument("--watch-at", type=int, default=0, help="game frame whose predecessor is watched")
    ap.add_argument("--pc", help="hex 65816 address: the dump was taken at its --hit-th execution")
    ap.add_argument("--hit", type=int, default=1)
    args = ap.parse_args()
    dumps = sorted(load_dump(p) for p in args.dumps)
    if args.pc:
        pc_dump(args, dumps[0][1])
        return
    by_frame = {f: b for f, b in dumps}
    rom = load_rom(args.rom)
    m = ref65816.Machine(rom, ref65816.load_script(args.input))
    m.reset()
    watch = [int(w, 16) for w in args.watch]
    t0 = time.time()
    first_bad = [None]

    def report(a, old, new, cpu):
        print(f"  frame {cpu.game_frames}: ${a:05X} {old:02X} -> {new:02X} by ${cpu.ipc:06X}")

    def on_frame(n):
        if watch and n == args.watch_at - 1:
            for w in watch:
                m.watch[w] = report
        if n in by_frame:
            print(f"game frame {n} ({m.steps} instructions, {time.time() - t0:.0f}s):")
            bad = compare(m, by_frame[n], args.detail)
            if not bad:
                print("  equal")
            elif first_bad[0] is None:
                first_bad[0] = n
                if args.stop:
                    raise ref65816.Stop("first difference")
    m.frame_hook = on_frame
    try:
        m.run(dumps[-1][0] + 1)
    except ref65816.Stop as e:
        print(f"stopped at game frame {m.game_frames}: {e}")
    if first_bad[0] is not None:
        print(f"first differing dump: game frame {first_bad[0]}")


def pc_dump(args, blocks) -> None:
    """One dump taken at the --hit-th execution of --pc: run to the same
    point (before that instruction executes) and compare."""
    rom = load_rom(args.rom)
    m = ref65816.Machine(rom, ref65816.load_script(args.input))
    m.reset()
    pc = int(args.pc, 16)
    hits = [0]

    def hook(cpu):
        hits[0] += 1
        if hits[0] == args.hit:
            print(f"${pc:06X}, execution {args.hit}, game frame {cpu.game_frames}, {cpu.steps} instructions:")
            if not compare(cpu, blocks, args.detail):
                print("  equal")
            raise ref65816.Stop("done")
    m.pc_hook[pc] = hook
    try:
        m.run(10**9)
    except ref65816.Stop:
        pass


if __name__ == "__main__":
    main()
