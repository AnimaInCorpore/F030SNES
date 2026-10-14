#!/usr/bin/env python3
"""Compares a Hatari screenshot of the walker (tools: scripts/run_lcwalk.py shot) with the reference picture:
    shot_compare.py SHOT.png REF.ppm   prints the mismatch fraction at dx 0 and the best horizontal shift."""
import subprocess, sys

def rgb(path, args):
    return subprocess.run(["magick", path, *args, "-depth", "8", "rgb:-"], capture_output=True, check=True).stdout

shot = rgb(sys.argv[1], ["-crop", "512x448+114+40", "+repage"])
ref = rgb(sys.argv[2], ["-scale", "200%"])
W = 512
def px(D, x, y): return D[(y * W + x) * 3:(y * W + x) * 3 + 3]
def mismatch(dx, dy=0):
    bad = n = 0
    for y in range(10, 430, 5):
        for x in range(16, W - 16, 3):
            xa, ya = x + dx, y + dy
            if 0 <= xa < W and 0 <= ya < 448:
                n += 1
                p, q = px(shot, xa, ya), px(ref, x, y)
                bad += max(abs(p[i] - q[i]) for i in range(3)) > 12
    return bad / max(n, 1)
r0 = mismatch(0)
best = min((mismatch(dx), dx) for dx in range(-130, 131, 2))
print(f"mismatch at dx 0: {r0:.3f}; best {best[0]:.3f} at dx {best[1]} (2x pixels)")
sys.exit(0 if r0 < 0.02 else 1)
