"""Audit of indexed addressing on the reference interpreter (ref65816.py).

    python3 -m tools.smkx.refaudit INPUT FRAMES TABLES_OUT

Runs the ROM from reset with the scripted input (DKCX_INPUT syntax) up to
game frame FRAMES and reports
 - abs,X/Y and long,X whose index carries into the next bank (F030SMK's
   `$FFFF,Y` and bank-$7F cases);
 - bases in a low-RAM mirror (banks $00-$3F/$80-$BF, below $2000) whose
   index runs to $2000 or beyond, where a translation that takes the base
   for WRAM reads WRAM + index (DKC's CODE_BCB052);
 - table jumps JMP/JSR (abs,X): the largest index per site, written to
   TABLES_OUT for a comparison with the translated tables (F030CT's short
   tables).
It sees only what the run executes; the translation's handling of each
reported site is to be checked in build/dkcx/*.s."""
import sys
from collections import defaultdict
from pathlib import Path
from tools.smkx import ref65816
from tools.smkx.__main__ import load_rom

IN = sys.argv[1]
FRAMES = int(sys.argv[2])
ABSX = {0x1D,0x3D,0x5D,0x7D,0x9D,0xBD,0xDD,0xFD,0x1E,0x3E,0x5E,0x7E,0xDE,0xFE,0xBC,0x3C,0x9E}
ABSY = {0x19,0x39,0x59,0x79,0x99,0xB9,0xD9,0xF9,0xBE}
LONGX = {0x1F,0x3F,0x5F,0x7F,0x9F,0xBF,0xDF,0xFF}
TABLE = {0x7C, 0xFC}
m = ref65816.Machine(load_rom(Path("build/dkcx/dkc.sfc")), ref65816.load_script(IN))
m.reset()
cross = defaultdict(lambda: [0, None])
lowram = defaultdict(lambda: [0, None])
tables = defaultdict(int)
step0 = m.step
rd = m.read
def step():
    pc = m.pbr << 16 | m.pc
    op = rd(pc)
    if op in ABSX or op in ABSY or op in LONGX:
        lo = rd((pc + 1) & 0xFFFFFF) | rd((pc + 2) & 0xFFFFFF) << 8
        if op in LONGX:
            base = lo | rd((pc + 3) & 0xFFFFFF) << 16
            idx = m.x
        else:
            base = m.dbr << 16 | lo
            idx = m.x if op in ABSX else m.y
        eff = (base + idx) & 0xFFFFFF
        bank = base >> 16
        if eff >> 16 != bank:
            e = cross[pc]; e[0] += 1; e[1] = (base, idx, m.game_frames)
        elif bank not in (0x7E, 0x7F) and (bank & 0x7F) < 0x40 and (base & 0xFFFF) < 0x2000 and (eff & 0xFFFF) >= 0x2000:
            e = lowram[pc]; e[0] += 1; e[1] = (base, idx, m.game_frames)
    elif op in TABLE:
        tables[pc] = max(tables[pc], m.x)
    step0()
m.step = step
try:
    m.run(FRAMES)
except ref65816.Stop as e:
    print("stopped:", e)
print(f"ran to game frame {m.game_frames}, {m.steps} instructions")
print(f"bank-crossing indexed accesses: {len(cross)} sites")
for pc, (n, (b, i, f)) in sorted(cross.items()):
    print(f"  ${pc:06X}: {n}x, e.g. base ${b:06X} + ${i:04X} (frame {f})")
print(f"low-RAM-mirror base, index to $2000+: {len(lowram)} sites")
for pc, (n, (b, i, f)) in sorted(lowram.items()):
    print(f"  ${pc:06X}: {n}x, e.g. base ${b:06X} + ${i:04X} (frame {f})")
with open(sys.argv[3], "w") as out:
    for pc, x in sorted(tables.items()):
        out.write(f"{pc:06X} {x}\n")
print(f"table-jump sites used: {len(tables)} (max index per site in {sys.argv[3]})")
