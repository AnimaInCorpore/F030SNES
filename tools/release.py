#!/usr/bin/env python3
"""Pack a release: one self-contained TOS program per game (the translated program, the ROM's data and the DSP
programs are built into each .TOS, nothing is loaded at run time) and a README.TXT.  The ROMs are not part of git.

    python3 tools/release.py <version> [--no-build]     -> release/F030SNES-<version>/ and release/F030SNES-<version>.zip

Each game is built with its default settings plus the make variables listed below.  A program must be a TOS
executable; a game whose ROM is missing stops the release (put the ROM named in games/<game>/assets/README.md there).
"""
import argparse
import hashlib
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# game, make target, program (in games/<game>/build/), extra make variables, title, state.
# Keep the states in step with the games' README.md and ROADMAP.md.  Super Mario All-Stars is not listed: it has no
# translation yet.
GAMES = [
    ("smk", "smkx", "smkx/SMKX.TOS", [], "Super Mario Kart (USA)",
     "runs to the title screen; 16 colours per frame (four planes), Mode 7 on the DSP"),
    ("smw", "smwx", "smwx/SMWX.TOS", [], "Super Mario World (USA)",
     "title screen and attract demo; 256 colours"),
    ("smw2", "yix", "yix/YIX.TOS", ["YIX_GSU=interp"], "Super Mario World 2: Yoshi's Island (USA)",
     "logo, intro, title and file menu (SuperFX interpreted); stops in game mode $38, no sound CPU yet"),
    ("dkc", "dkcx", "dkcx/DKCX.TOS", [], "Donkey Kong Country (USA)",
     "intro, file select and player-mode menu"),
    ("ct", "ctx", "ctx/CTX.TOS", [], "Chrono Trigger (USA)",
     "intro, title, attract scenes and the menu shell; music on the SPC700 interpreter"),
    ("ff6", "ff6x", "ff6x/FF6X.TOS", [], "Final Fantasy VI (Japan, v1.0)",
     "title, opening and the first dialogue at Narshe"),
    ("sciv", "scivx", "scivx/SCIVX.TOS", [], "Super Castlevania IV (USA)",
     "intro, title menu, name entry and stage 1"),
]

README = """F030SNES {version}
{underline}

Super Nintendo games on the Atari Falcon030: each game's 65816 program is statically translated to 68030 code, the
picture is composed by the DSP56001.  Source and documentation:
https://github.com/AnimaInCorpore/F030SNES  (commit {commit})

This is a preview: none of the games is complete yet.  The states below are measured in the patched Hatari of the
F030Arcade project; speed on a real Falcon has not been measured for most of them.

Requirements: a Falcon030 with 14 MB of ST-RAM, an RGB or VGA monitor.  Start the programs from the desktop (a hard
disk or a GEMDOS drive: they are 3-12 MB each).  Every program is self-contained: nothing else is needed.

Controls (all games)
--------------------

The keyboard is pad 1, by key position on a QWERTY layout:
  cursor keys  D-pad        X  B        Z  Y        S  A        A  X
  Q  L         W  R         Return  Start            Space  Select        Esc  quit to the desktop

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

    sums, texts = [], []
    for game, target, program, make_vars, title, state in GAMES:
        if not a.no_build:
            r = subprocess.run(["make", "-C", str(ROOT / "games" / game), target] + make_vars,
                               capture_output=True, text=True)
            if r.returncode:
                sys.stdout.write(r.stdout[-4000:] + r.stderr[-4000:])
                sys.exit("build of %s failed" % game)
        prg = ROOT / "games" / game / "build" / program
        data = check(prg)
        (out / prg.name).write_bytes(data)
        sums.append("%s  %s" % (hashlib.sha256(data).hexdigest(), prg.name))
        texts.append("%-10s %s\n  %s.\n" % (prg.name, title, state[0].upper() + state[1:]))
        print("%-10s %9d bytes" % (prg.name, len(data)))

    text = README.format(version=a.version, underline="=" * len("F030SNES " + a.version), commit=commit,
                         games="\n".join(texts), sums="\n".join(sums) + "\n")
    (out / "README.TXT").write_bytes(text.replace("\n", "\r\n").encode("ascii"))

    archive = out.parent / (name + ".zip")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in sorted(out.iterdir()):
            z.write(f, "%s/%s" % (name, f.name))
    print("%s: %d bytes" % (archive.relative_to(ROOT), archive.stat().st_size))


if __name__ == "__main__":
    main()
