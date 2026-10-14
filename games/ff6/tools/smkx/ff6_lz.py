"""Final Fantasy VI's LZSS decompressor (Decompress, $C0xxxx in bank C0 and its
copies), for programs the game decompresses into WRAM and runs there.

The block starts with its compressed length (16 bits, counting those two
bytes). Then groups of a header byte and eight items, low bit first: a set bit
is a literal byte, a clear one a 16-bit reference, word w = length in bits
15-11 (plus 3) and the ring buffer position in bits 10-0. The ring buffer is
2 KiB, starts zeroed and is written from position $7DE. (tools/ff6_lzss.py in
the disassembly is the reference this follows.)
"""

from __future__ import annotations


def decompress(read, src: int) -> bytes:
    """`read(addr)` gives the ROM byte at a CPU address; `src` is the block's."""
    length = read(src) | read(src + 1) << 8
    ring = bytearray(0x800)
    b = 0x800 - 34
    out = bytearray()
    s = 2
    while s < length:
        header = read(src + s)
        s += 1
        for _ in range(8):
            if header & 1:
                c = read(src + s)
                s += 1
                ring[b] = c
                b = (b + 1) & 0x7FF
                out.append(c)
            else:
                w = read(src + s) | read(src + s + 1) << 8
                s += 2
                for i in range(((w >> 11) + 3)):
                    c = ring[(w + i) & 0x7FF]
                    ring[b] = c
                    b = (b + 1) & 0x7FF
                    out.append(c)
            if s >= length:
                break
            header >>= 1
    return bytes(out)
