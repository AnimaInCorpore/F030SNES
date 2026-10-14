#!/usr/bin/env python3
"""Re-format the FAT16 volume of an AHDI .hda image so that the Falcon's GEMDOS accepts it.

The usual 240 MiB FAT16 volume with 512-byte sectors and 4 KiB clusters
(F030Arcade's make_gem_image.py) is listed by HDDRIVER as a BGM drive, but
TOS gives it no BPB: HDDRUTIL's "Check Boot Sector" reports, against the
GEMDOS limits, sectors per cluster 8 (<=2), sectors 491520 (<=65535) and
clusters 61175 (<=32766). GEMDOS wants a cluster of exactly two logical
sectors, at most 65535 logical sectors and at most 32766 clusters, but the
logical sector may be up to 32768 bytes. With 4096-byte logical sectors a
240 MiB volume is 61440 sectors of 2-sector (8 KiB) clusters, 30702 of them.

The AHDI root sector (partition type, start, size) stays as it is. Every file
and folder is copied out, the volume is made again with macOS' newfs_msdos
and everything is copied back, names and times kept.

Usage: gemdos_hda.py IMAGE.hda [--part-start 64] [--label NAME]

Needs mtools (mcopy, mdir) and macOS' newfs_msdos. Run on a copy of the card's
image if the files cannot be recreated.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

SECTOR = 512
LOGICAL = 4096


def run(*cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--part-start", type=int, default=64, help="first partition sector")
    ap.add_argument("--label", help="volume label (default: the old one)")
    args = ap.parse_args()

    with open(args.image, "rb") as f:
        f.seek(args.part_start * SECTOR + 11)
        bps, spc = struct.unpack("<HB", f.read(3))
        # AHDI root sector: partition size of the entry that starts here
        f.seek(0)
        root = f.read(SECTOR)
    size = None
    for i in range(4):
        o = 0x1C6 + i * 12
        if root[o] & 1 and struct.unpack(">I", root[o + 4:o + 8])[0] == args.part_start:
            size = struct.unpack(">I", root[o + 8:o + 12])[0]
    if size is None:
        sys.exit(f"no AHDI partition starts at sector {args.part_start}")
    if size % (LOGICAL // SECTOR):
        sys.exit(f"partition of {size} sectors is not a multiple of {LOGICAL} bytes")
    logical_sectors = size * SECTOR // LOGICAL
    if logical_sectors > 65535:
        sys.exit(f"{logical_sectors} logical sectors exceed 65535: use a larger logical sector size")
    offset = args.part_start * SECTOR
    spec = f"{args.image}@@{offset}"

    tmp = tempfile.mkdtemp(prefix="gemdos_hda_")
    try:
        files = os.path.join(tmp, "files")
        os.mkdir(files)
        label = args.label
        if not label:
            out = subprocess.run(["mdir", "-i", spec, "::"], capture_output=True, text=True, check=True).stdout
            # " Volume in drive : is ARCADE     " or "... is ARCADE_ (abbr=ARCADE     )"
            line = out.splitlines()[0]
            label = (line.split("(abbr=", 1)[1].rstrip(") ") if "(abbr=" in line
                     else line.split(": is", 1)[1]).strip() or "NO NAME"
        # the root entries (and so the long names) as mtools sees them
        run("mcopy", "-i", spec, "-s", "-m", "-n", "::/*", files)
        part = os.path.join(tmp, "part.img")
        with open(args.image, "rb") as f:
            f.seek(offset)
            with open(part, "wb") as p:
                p.truncate(size * SECTOR)
        run("newfs_msdos", "-F", "16", "-S", str(LOGICAL), "-c", "2", "-s", str(logical_sectors),
            "-r", "1", "-e", "512", "-v", label, part, stdout=subprocess.DEVNULL)
        names = sorted(os.listdir(files))
        if names:
            run("mcopy", "-i", part, "-s", "-m", "-n", *[os.path.join(files, n) for n in names], "::")
        # put the volume back into the image
        with open(part, "rb") as p, open(args.image, "r+b") as f:
            f.seek(offset)
            shutil.copyfileobj(p, f, 1 << 20)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"{args.image}: partition at sector {args.part_start}, {logical_sectors} x {LOGICAL}-byte "
          f"sectors, 2 per cluster (was {bps} x {spc})")


if __name__ == "__main__":
    main()
