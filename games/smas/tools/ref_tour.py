#!/usr/bin/env python3
"""A scripted pad for tools/ref65816.c (REF_INPUT): from the title screen through the game select
screen into one of the four games, then running right with jumps.

    ref_tour.py GAME [FRAMES]       GAME: 0 Super Mario Bros., 1 The Lost Levels, 2 SMB2, 3 SMB3

Pad bits as $4218/$4219 (A $0080, X $0040, L $0020, R $0010, B $8000, Y $4000, Select $2000,
Start $1000, Up $0800, Down $0400, Left $0200, Right $0100). Timeline of the reference run (the
game's own frame counter, not the Falcon's): the title is up at about 1,000; the select screen at
1,200 with SMB1 in the middle, SMB3 to its left, The Lost Levels to its right."""
import sys

A, B, START, RIGHT, LEFT, UP = 0x0080, 0x8000, 0x1000, 0x0100, 0x0200, 0x0800


def tap(ev, f, pad, n=4):
    ev.append((f, pad))
    ev.append((f + n, 0))


def tour(game: int, frames: int = 8000) -> list[tuple[int, int]]:
    ev = [(0, 0)]
    tap(ev, 1000, START)                                   # title -> select
    moves = {0: [], 1: [RIGHT], 2: [RIGHT, RIGHT], 3: [LEFT]}[game]
    for i, m in enumerate(moves):
        tap(ev, 1250 + 40 * i, m)
    tap(ev, 1400, START)                                   # the game
    f = 1500
    if game == 3:                                          # titles, then the world map: right, up, into level 1
        while f < 2200:
            tap(ev, f, START)
            f += 100
        tap(ev, 2500, RIGHT, 20)
        tap(ev, 2600, UP, 24)
        tap(ev, 2700, A)
        f = 2900
    else:
        while f < 3000:                                    # file menu, titles, character select
            # (Start pauses SMB2 once the level is up: A alone there from 1,800 on)
            tap(ev, f, START if (game != 2 or f < 1800) and (f // 100) % 2 else A)
            f += 100
    while f < frames:                                      # run right, jump now and then
        ev.append((f, RIGHT | B))
        ev.append((f + 24, RIGHT | B | A))
        ev.append((f + 34, RIGHT | B))
        f += 60
    ev.sort()
    return ev


if __name__ == "__main__":
    g = int(sys.argv[1])
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 8000
    print(",".join(f"{f}:{p:X}" for f, p in tour(g, n)))
