"""Build the Final Fantasy VI disassembly (third_party/ff6, ca65) out of tree.

    python3 -m tools.smkx.ff6src "assets/Final Fantasy VI (Japan).sfc"

The disassembly keeps no ROM data: `make rip` extracts graphics, text and
tables from the user's ROM into the module directories, and `make LANG=jp`
assembles and links a Japanese ROM. Both write into the source tree, so the
pinned submodule is copied to build/ff6src and built there; the linker's
debug file (build/ff6src/build/jp/rom/ff6-jp.dbg) is what
listing.parse_ff6 places the code from. Everything here is derived from the
ROM and stays untracked.

Needs cc65 (ca65, ld65), GNU make, git and network access for the first
run: the disassembly's nested `tools/romtools` submodule is cloned at the
commit the submodule records, and numpy (its ripping scripts import it) goes
into build/venv when the running Python lacks it. Rebuilds only when the
disassembly's commit or the ROM changes.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SUBMODULE = ROOT / "third_party/ff6"
BUILD = ROOT / "build/ff6src"
DBG = BUILD / "build/jp/rom/ff6-jp.dbg"
STAMP = BUILD / "stamp"


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    subprocess.run(cmd, check=True, **kw)


def submodule_commit(path: str) -> str:
    """The commit `git ls-tree` records for a nested submodule of ff6."""
    out = subprocess.run(["git", "-C", str(SUBMODULE), "ls-tree", "HEAD", path],
                         check=True, capture_output=True, text=True).stdout.split()
    return out[2]


def head_commit() -> str:
    return subprocess.run(["git", "-C", str(SUBMODULE), "rev-parse", "HEAD"],
                          check=True, capture_output=True, text=True).stdout.strip()


def python_with_numpy() -> str:
    if subprocess.run([sys.executable, "-c", "import numpy"], capture_output=True).returncode == 0:
        return sys.executable
    venv = ROOT / "build/venv"
    py = venv / "bin/python3"
    if not py.exists():
        run([sys.executable, "-m", "venv", str(venv)])
    if subprocess.run([str(py), "-c", "import numpy"], capture_output=True).returncode != 0:
        run([str(py), "-m", "pip", "install", "-q", "numpy"])
    return str(py)


def ensure(rom: bytes) -> Path:
    """Build the JP disassembly for `rom` if not done yet; the .dbg path."""
    for tool in ("ca65", "ld65", "make", "git"):
        if shutil.which(tool) is None:
            raise SystemExit(f"{tool} not found: the Final Fantasy VI disassembly needs cc65 and GNU make "
                             "(brew install cc65)")
    if not (SUBMODULE / "Makefile").exists():
        raise SystemExit("third_party/ff6 is empty: git submodule update --init third_party/ff6")
    want = f"{head_commit()} {zlib.crc32(rom) & 0xFFFFFFFF:08X}"
    if STAMP.exists() and STAMP.read_text() == want and DBG.exists():
        return DBG
    shutil.rmtree(BUILD, ignore_errors=True)
    BUILD.mkdir(parents=True)
    run(["rsync", "-a", "--exclude", ".git", f"{SUBMODULE}/", f"{BUILD}/"])
    romtools = BUILD / "tools/romtools"
    if not (romtools / "romtools").exists():
        shutil.rmtree(romtools, ignore_errors=True)
        run(["git", "clone", "-q", "https://github.com/everything8215/romtools.git", str(romtools)])
        run(["git", "-C", str(romtools), "checkout", "-q", submodule_commit("tools/romtools")])
        shutil.rmtree(romtools / ".git")
    (BUILD / "vanilla/ff6-jp.sfc").write_bytes(rom)
    py = python_with_numpy()
    env = dict(os.environ, PYTHONPATH="tools/romtools", PATH=f"{Path(py).parent}:{os.environ['PATH']}")
    run([py, "tools/extract_assets.py"], cwd=BUILD, env=env)
    run(["make", "LANG=jp", f"PYTHON={py}"], cwd=BUILD, env=env)
    built = (BUILD / "build/jp/rom/ff6-jp.sfc").read_bytes()
    if zlib.crc32(built) & 0xFFFFFFFF != 0x059728E2:
        raise SystemExit("the rebuilt ROM is not the CRC32 the disassembly documents (059728E2)")
    STAMP.write_text(want)
    return DBG


if __name__ == "__main__":
    from . import __main__ as M
    print(ensure(M.load_rom(Path(sys.argv[1]))))
