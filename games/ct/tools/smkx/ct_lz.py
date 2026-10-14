"""Chrono Trigger's LZ decompressor (Gfx_DecompressEntry, $C30557), in Python.

The game decompresses some of its code into WRAM and runs it there (the
program at $7E3000, see analyze.WRAM_OVERLAYS); the translator rebuilds
those bytes with this function to translate them at their WRAM addresses.

Format, as the 65816 routine reads it (D = $0300):
  word size; the first segment's bytes follow and its tail byte sits at
  src + 2 + size. Tail bits 7-6 = 0 select 12-bit offsets / 4-bit lengths,
  otherwise 11-bit offsets / 5-bit lengths. Flag bytes, LSB first: 0 =
  literal byte, 1 = back-reference word (offset in the low bits, length in
  the high bits of its second byte, length + 3 bytes copied, overlapping
  forward like MVN). A flag byte of 0 means eight literals. At a segment's
  end the tail byte's bits 5-0 give the next flag byte's bit count (0 ends)
  and the following word, plus the start, the next segment's end.
"""

from __future__ import annotations


def decompress(read, src: int) -> bytes:
    """read(addr24) -> byte; src = 24-bit address of the compressed block."""
    bank = src & 0xFF0000
    base = src & 0xFFFF

    def b(x):
        return read(bank | (x & 0xFFFF))

    def w(x):
        return b(x) | b(x + 1) << 8

    x = base + 2
    end = base + 2 + w(base)
    wide = (b(end) & 0xC0) == 0
    mask, shift = (0x0FFF, 4) if wide else (0x07FF, 3)
    out = bytearray()
    bits = 8
    while True:
        if x == end:
            t = b(x) & 0x3F
            if t == 0:
                return bytes(out)
            bits = t
            end = (w(x + 1) + base) & 0xFFFF
            x += 3
            continue
        flags = b(x)
        if flags == 0:
            out += bytes(b(x + 1 + i) for i in range(8))
            x += 9
            continue
        x += 1
        n = bits
        while True:
            if flags & 1:
                length = (b(x + 1) >> shift) + 3
                off = w(x) & mask
                start = len(out) - off
                for i in range(length):
                    out.append(out[start + i])
                x += 2
            else:
                out.append(b(x))
                x += 1
            flags >>= 1
            n -= 1
            if n == 0:
                break
        bits = 8
