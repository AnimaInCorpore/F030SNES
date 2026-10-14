"""Assemble a DSP56001 source with Motorola ASM56000 under DOSBox-X and
convert it to a Dsp_ExecBoot image, or with --prog to a Dsp_ExecProg image.

    python3 -m tools.smkx.dsp_asm falcon/dsp/m7c2p.asm build/smkx/m7c2p.bin
    python3 -m tools.smkx.dsp_asm --prog falcon/dsp/ppu.asm build/ctx/ppu.bin
    python3 -m tools.smkx.dsp_asm --boot falcon/dsp/ppu.asm build/ctx/ppu.bin
    python3 -m tools.smkx.dsp_asm --boot --define M7K=1 falcon/dsp/ppu.asm build/ctx/ppu7.bin

--define NAME=VALUE (repeatable) puts "NAME equ VALUE" in front of the
source: one source, several programs.

An ExecProg image is the format Dsp_LodToBinary makes: per block of
consecutive words the memory space (0 P, 1 X, 2 Y), the start address and
the word count, then the words, all 3-byte big-endian DSP words; the size
Dsp_ExecProg takes counts DSP words, headers included.

A --boot image is for the runtime's own loader (falcon/smkx_dspboot.s):
internal P ($0000-$01FF) as the 512 words the DSP's bootstrap takes after
a reset, then the other blocks for the program's stage-2 loader as
above, then the word 3 (end); a program that fits internal P is the 512
words alone. All 3-byte big-endian words.

The toolchain is F030Arcade's third_party/asm56k (ASM56000, CLDLOD, DOS4GW,
ioequ.inc). DOSBox does not return the DOS exit status, so the listing's
error count decides. The image holds P-space words only, below P:$0200
(internal program RAM, the Dsp_ExecBoot limit), as 3-byte big-endian words.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT.parents[2] / "F030Arcade/third_party/asm56k"
DOSBOX = shutil.which("dosbox-x") or "/opt/homebrew/bin/dosbox-x"


def assemble(src: Path, prog: bool = False, defines: tuple = ()):
    name = src.stem.upper()[:8]
    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp)
        for f in ("ASM56000.EXE", "CLDLOD.EXE", "DOS4GW.EXE", "ioequ.inc"):
            shutil.copy(TOOLS / f, t / f)
        head = "".join(f"{k}\tequ\t{v}\r\n" for k, v in defines)
        (t / f"{name}.ASM").write_bytes(head.encode() + src.read_bytes())
        (t / "B.BAT").write_text(
            f"@ECHO OFF\r\nASM56000.EXE -q -a -b{name}.CLD -l{name}.LST {name}.ASM\r\n"
            f"CLDLOD.EXE {name}.CLD > {name}.LOD\r\nEXIT\r\n")
        subprocess.run([DOSBOX, "-nopromptfolder", "-nogui", "-nomenu", "-defaultconf", "-silent",
                        "-c", f"mount c {t}", "-c", "c:", "-c", "B.BAT"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)
        lst = (t / f"{name}.LST").read_text(errors="replace") if (t / f"{name}.LST").exists() else ""
        m = re.search(r"^\s*(\d+)\s+Errors", lst, re.M)
        if not m or int(m.group(1)):
            errors = [l for l in lst.splitlines() if "ERROR" in l.upper() and "Errors" not in l]
            raise SystemExit(f"{src}: assembly failed\n" + "\n".join(errors[:20]))
        lod = (t / f"{name}.LOD").read_text(errors="replace")
    return (parse_blocks(lod) if prog else parse_lod(lod)), lst


def parse_blocks(text: str) -> list[tuple[str, int, list[int]]]:
    """All _DATA blocks of a LOD, merged where consecutive."""
    blocks: list[tuple[str, int, list[int]]] = []
    space, addr = None, 0
    for line in text.splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "_DATA":
            space, addr = parts[1], int(parts[2], 16)
            if blocks and blocks[-1][0] == space and blocks[-1][1] + len(blocks[-1][2]) == addr:
                continue
            blocks.append((space, addr, []))
            continue
        if parts[0].startswith("_"):
            space = None
            continue
        if space is None:
            continue
        blocks[-1][2].extend(int(w, 16) for w in parts)
    if not blocks:
        raise SystemExit("empty program")
    return blocks


def prog_image(blocks) -> bytes:
    out = bytearray()
    for space, addr, words in blocks:
        out += "PXY".index(space).to_bytes(3, "big") + addr.to_bytes(3, "big") + len(words).to_bytes(3, "big")
        out += b"".join(w.to_bytes(3, "big") for w in words)
    return bytes(out)


def boot_image(blocks) -> bytes:
    boot = [0] * 0x200
    rest = []
    for space, addr, words in blocks:
        if space == "P" and addr < 0x200:
            inside = words[:0x200 - addr]
            boot[addr:addr + len(inside)] = inside
            words = words[len(inside):]
            addr += len(inside)
        if words:
            rest.append((space, addr, words))
    out = bytearray(b"".join(w.to_bytes(3, "big") for w in boot))
    if rest:                        # a program without a loader is only P
        out += prog_image(rest)
        out += (3).to_bytes(3, "big")
    return bytes(out)


def parse_lod(text: str) -> list[int]:
    words: dict[int, int] = {}
    space, addr = None, 0
    for line in text.splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "_DATA":
            space, addr = parts[1], int(parts[2], 16)
            continue
        if parts[0].startswith("_"):
            space = None
            continue
        if space is None:
            continue
        if space != "P":
            raise SystemExit(f"image has {space}-space data; ExecBoot loads P only")
        for w in parts:
            words[addr] = int(w, 16)
            addr += 1
    if not words:
        raise SystemExit("empty program")
    top = max(words) + 1
    if top > 0x200:
        raise SystemExit(f"program reaches P:${top:04X}; ExecBoot loads internal P below $0200")
    return [words.get(a, 0) for a in range(top)]


def main() -> None:
    args = sys.argv[1:]
    mode = args[0] if args[0] in ("--prog", "--boot") else None
    prog = mode is not None
    if prog:
        args = args[1:]
    defines = []
    while args and args[0] == "--define":
        name, _, value = args[1].partition("=")
        defines.append((name, value or "1"))
        args = args[2:]
    src, out = Path(args[0]), Path(args[1])
    result, lst = assemble(src, prog, tuple(defines))
    out.parent.mkdir(parents=True, exist_ok=True)
    (out.with_suffix(".lst")).write_text(lst)
    if prog:
        out.write_bytes(boot_image(result) if mode == "--boot" else prog_image(result))
        for space, addr, words in result:
            print(f"{src.name}: {space}:${addr:04X} {len(words)} words")
        return
    out.write_bytes(b"".join(w.to_bytes(3, "big") for w in result))
    print(f"{src.name}: {len(result)} program words")


if __name__ == "__main__":
    main()
