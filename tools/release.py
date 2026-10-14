#!/usr/bin/env python3
"""Pack a release: one TOS program per game and a README.TXT.  No game content is packed: each program reads the
player's own ROM (and, for Super Mario Kart, the DSP-1 dump) from next to itself at start, checks it against the
build's checksum and says on screen when a file is missing or wrong (falcon/smkx_info.s, tools/port_info.py).

    python3 tools/release.py <version> [--no-build]     -> release/F030SNES-<version>/ and release/F030SNES-<version>.zip

Each game is built with its default settings plus the make variables in tools/games.py, with the info screen held
for INFO_WAIT seconds and the version in its first line.  A program must be a TOS executable, and it must not carry
the game: a 32-byte block of the ROM (or the DSP-1 dump) that also occurs in a program stops the release.  The
translated program is derived from the game's code, but none of the game's bytes are in it.  These are engineering
checks, not a rights review.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import textwrap
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from games import CONTROLS, GAMES  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
BLOCK = 32
INFO_WAIT = 10

README = """F030SNES {version}
{underline}

Super Nintendo games on the Atari Falcon030: each game's 65816 program is statically translated to 68030 code, the
picture is composed by the DSP56001.  Source and documentation:
https://github.com/AnimaInCorpore/F030SNES  (commit {commit})

HIGHLY EXPERIMENTAL.  In their current form the ports are far too slow to play, the rendering above all: they show
how far each translation gets, not a game you can sit down with.  The states below are measured in the patched
Hatari of the F030Arcade project; speed on a real Falcon has not been measured for most of them.

Requirements: a Falcon030 with 14 MB of ST-RAM, an RGB or VGA monitor, and your own copy of each game.  Start the
programs from the desktop (a hard disk or a GEMDOS drive).

Your game files
---------------

No game is included.  Put your own ROM next to the program, under the name below (a .SFC; a .SMC with a 512-byte
copier header works too, named like the .SFC but ending in .SMC).  Each program checks the file against the exact
release it was translated from and says on screen when a file is missing, has the wrong size or is another
revision; it waits a few seconds on its info screen (any key skips) and then starts.

{files}
Keys (all games)
----------------

The keyboard is pad 1, by key position on a QWERTY layout:
{controls}

Games
-----
{games}
Files (SHA-256)
---------------
{sums}"""


def check(prg):
    data = prg.read_bytes()
    if data[:2] != b"\x60\x1a":
        sys.exit("%s: not a TOS program" % prg)
    return data


def ramp(b):
    """A counting sequence (00 01 02 .., an identity table) is in many programs and says nothing."""
    return len({(b[i + 1] - b[i]) & 0xFF for i in range(len(b) - 1)}) == 1


def embedded(program, files):
    """Bytes of the files that also occur in the program (aligned 32-byte blocks of 8+ distinct values, not ramps)."""
    found = {}
    for f in files:
        data = f.read_bytes()
        blocks = {b for b in (data[i:i + BLOCK] for i in range(0, len(data) - BLOCK + 1, BLOCK))
                  if len(set(b)) >= 8 and not ramp(b)}
        hits = sum(1 for i in range(len(program) - BLOCK + 1) if program[i:i + BLOCK] in blocks)
        if hits:
            found[f.name] = hits
    return found


def player_files(game, make_dir):
    """The player's files as the build used them (games/<game>/build/<target>/: the ROM without a copier header)."""
    names = [n for n, _ in GAMES[game]["files"]]
    return [p for n in names for p in [make_dir / n.lower()] if p.exists()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("version")
    ap.add_argument("--no-build", action="store_true", help="pack the programs in games/*/build/ as they are")
    a = ap.parse_args()

    name = "F030SNES-" + a.version
    out = ROOT / "release" / name
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, capture_output=True,
                            text=True).stdout.strip()
    dirty = subprocess.run(["git", "status", "--porcelain", "--untracked-files=no"], cwd=ROOT, capture_output=True,
                           text=True).stdout.strip()
    if dirty:
        commit += " + local changes"

    env = dict(os.environ, F030SNES_VERSION=a.version)
    sums, texts, files = [], [], []
    for game, g in GAMES.items():
        if not a.no_build:
            r = subprocess.run(["make", "-C", str(ROOT / "games" / game), g["target"], "INFO_WAIT=%d" % INFO_WAIT]
                               + g["make_vars"], capture_output=True, text=True, env=env)
            if r.returncode:
                sys.stdout.write(r.stdout[-4000:] + r.stderr[-4000:])
                sys.exit("build of %s failed" % game)
        prg = ROOT / "games" / game / "build" / g["program"]
        data = check(prg)
        theirs = player_files(game, prg.parent)
        if not theirs:
            sys.exit("%s: the ROM the build used is missing, the program cannot be checked" % game)
        found = embedded(data, theirs)
        if found:
            sys.exit("%s carries game content: %s" % (prg.name, ", ".join("%s %d blocks" % kv for kv in found.items())))
        (out / prg.name).write_bytes(data)
        sums.append("%s  %s" % (hashlib.sha256(data).hexdigest(), prg.name))
        texts.append("%-10s %s\n%s\n" % (prg.name, g["title"], textwrap.fill(
            g["state"][0].upper() + g["state"][1:] + ".", 116, initial_indent="  ", subsequent_indent="  ")))
        for fname, what in g["files"]:
            files.append("  %-10s %-9s %s, %s" % (prg.name, fname, what, g["title"]))
        print("%-10s %9d bytes, no game content" % (prg.name, len(data)))

    text = README.format(version=a.version, underline="=" * len("F030SNES " + a.version), commit=commit,
                         files="\n".join(files) + "\n", controls="\n".join("  " + c for c in CONTROLS),
                         games="\n".join(texts), sums="\n".join(sums) + "\n")
    (out / "README.TXT").write_bytes(text.replace("\n", "\r\n").encode("ascii"))

    archive = out.parent / (name + ".zip")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in sorted(out.iterdir()):
            z.write(f, "%s/%s" % (name, f.name))
    print("%s: %d bytes" % (archive.relative_to(ROOT), archive.stat().st_size))


if __name__ == "__main__":
    main()
