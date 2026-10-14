#!/usr/bin/env python3
"""Capture RGB and VGA Videl output in F030Arcade's patched Hatari.

Requires Pillow for pixel comparison. The viewer's screen and palette must
match the host reference exactly after the Falcon CLUT's 6-bit quantization.
"""

from __future__ import annotations

import stat
import subprocess
import time
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
HATARI = ROOT.parents[2] / "F030Arcade/third_party/hatari/build/src/hatari"
TOS = ROOT.parents[2] / "F030Arcade/third_party/tos/tos404.img"
PROGRAM = ROOT / "build/viewer.tos"
REFERENCE = ROOT / "out/frame.ppm"


def send(fifo: Path, command: str) -> None:
    with fifo.open("w") as stream:
        stream.write(command + "\n")


def wait_for(predicate, what: str, timeout: float = 30.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.05)
    raise TimeoutError(what)


def quantized(rgb: tuple[int, int, int]) -> tuple[int, int, int]:
    return tuple((value & 0xFC) | ((value & 0xFC) >> 6) for value in rgb)


def image_origin(image: Image.Image, sky: tuple[int, int, int]) -> tuple[int, int]:
    """Find the first 512-pixel sky scanline, avoiding the Hatari status bar."""
    for y in range(image.height - 448 + 1):
        run = 0
        for x in range(image.width):
            run = run + 1 if image.getpixel((x, y)) == sky else 0
            if run == 512:
                return x - 511, y
    raise AssertionError("256-pixel active image not found in Hatari screenshot")


def compare_capture(capture: Path) -> None:
    image = Image.open(capture).convert("RGB")
    reference = Image.open(REFERENCE).convert("RGB")
    assert reference.size == (256, 224)
    origin_x, origin_y = image_origin(image, quantized(reference.getpixel((0, 0))))
    bad_pixels = 0
    bad_blocks = 0
    for y in range(224):
        for x in range(256):
            expected = quantized(reference.getpixel((x, y)))
            bx, by = origin_x + x * 2, origin_y + y * 2
            actual = image.getpixel((bx, by))
            bad_pixels += actual != expected
            bad_blocks += any(image.getpixel((bx + dx, by + dy)) != actual
                              for dy in range(2) for dx in range(2))
    if bad_pixels or bad_blocks:
        raise AssertionError(f"{capture.name}: {bad_pixels} mismatched pixels, "
                             f"{bad_blocks} nonuniform 2x2 blocks")
    print(f"{capture.name}: 57,344/57,344 pixels match "
          f"at ({origin_x}, {origin_y})")


def verify(monitor: str) -> None:
    out = ROOT / "out"
    fifo = out / f"hatari-{monitor}.fifo"
    trace = out / f"hatari-{monitor}-trace.log"
    capture = out / f"hatari-{monitor}.png"
    if fifo.exists():
        if not stat.S_ISFIFO(fifo.stat().st_mode):
            raise RuntimeError(f"refusing to replace non-FIFO: {fifo}")
        fifo.unlink()
    if capture.exists():
        capture.unlink()
    if trace.exists():
        trace.unlink()
    command = [
        str(HATARI), "--machine", "falcon", "--dsp", "emu",
        "--memsize", "14", "--ttram", "0", "--tos", str(TOS),
        "--fast-boot", "true", "--cpu-exact", "true",
        "--compatible", "true", "--patch-tos", "true",
        "--monitor", monitor, "--sound", "off", "--conout", "2",
        "--confirm-quit", "off", "--alert-level", "fatal",
        "--trace", "event", "--trace-file", str(trace),
        "--cmd-fifo", str(fifo), "--screenshot-dir", str(out),
        "--screenshot-format", "png", str(PROGRAM),
    ]
    process = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True)
    try:
        wait_for(lambda: fifo.exists() and trace.exists() and
                 "EVENT: Program exec" in trace.read_text(),
                 f"{monitor} viewer launch")
        time.sleep(0.2)
        send(fifo, f"hatari-debug screenshot {capture}")
        wait_for(lambda: capture.exists() and capture.stat().st_size > 0,
                 f"{monitor} screenshot")
        time.sleep(0.1)
        compare_capture(capture)
        send(fifo, "hatari-event keypress 57")  # space key exits the viewer
        wait_for(lambda: "EVENT: Program termination" in trace.read_text(),
                 f"{monitor} viewer clean exit")
        send(fifo, "hatari-debug quit")
        process.wait(timeout=10)
        if process.returncode:
            raise RuntimeError(f"Hatari exited with {process.returncode}")
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
        if process.stdout:
            output = process.stdout.read()
            if process.returncode:
                print(output)


def main() -> None:
    if not HATARI.is_file() or not TOS.is_file() or not PROGRAM.is_file():
        raise SystemExit("patched F030Arcade Hatari, TOS 4.04 or viewer is missing")
    for monitor in ("rgb", "vga"):
        verify(monitor)


if __name__ == "__main__":
    main()
