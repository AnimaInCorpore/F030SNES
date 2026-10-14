"""OKLab table for the adaptive palette: 32768 SNES BGR555 colours ->
(L, a, b) as big-endian int16, scaled by 4096 (L in 0..4096).

    python3 -m tools.smkx.oklab build/smkx/oklab555.bin
"""

import struct
import sys


def srgb_to_linear(c: float) -> float:
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def oklab(r8: int, g8: int, b8: int) -> tuple[float, float, float]:
    r, g, b = (srgb_to_linear(v / 255.0) for v in (r8, g8, b8))
    l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b
    m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b
    s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b
    l_, m_, s_ = (v ** (1.0 / 3.0) for v in (l, m, s))
    return (0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_,
            1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_,
            0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_)


def table() -> bytes:
    out = bytearray()
    for c in range(32768):
        r5, g5, b5 = c & 31, (c >> 5) & 31, (c >> 10) & 31
        r8, g8, b8 = ((v << 3) | (v >> 2) for v in (r5, g5, b5))
        out += struct.pack(">hhh", *(round(v * 4096) for v in oklab(r8, g8, b8)))
    return bytes(out)


if __name__ == "__main__":
    data = table()
    with open(sys.argv[1], "wb") as f:
        f.write(data)
    print(f"{sys.argv[1]}: {len(data)} bytes")
