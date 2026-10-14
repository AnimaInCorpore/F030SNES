"""Write the runtime's scripted joypad input (SCIVX_INPUT) as vasm source.

    python3 -m tools.smkx.input_script "600:1000,606:0000" build/scivx/input.s

Each entry is FRAME:PAD, both hexadecimal for PAD and decimal for FRAME:
from that frame on, PAD ($4219 << 8 | $4218; Start = 1000, B = 8000,
Right = 0100, A = 0080) is ORed into the keyboard's buttons. The table ends
with -1.
"""

import sys
from pathlib import Path


def main() -> None:
    spec, out = sys.argv[1], Path(sys.argv[2])
    rows = []
    for item in filter(None, (x.strip() for x in spec.split(","))):
        frame, pad = item.split(":")
        rows.append((int(frame), int(pad, 16)))
    if rows != sorted(rows, key=lambda r: r[0]):
        raise SystemExit("SCIVX_INPUT: frames must ascend")
    lines = ["; Generated from SCIVX_INPUT by tools/smkx/input_script.py", "\txdef\tsmkx_input",
             "\tsection\tdata,data", "\teven", "smkx_input:"]
    lines += [f"\tdc.l\t{f},${p:04X}" for f, p in rows]
    lines.append("\tdc.l\t-1,0")
    text = "\n".join(lines) + "\n"
    if not out.exists() or out.read_text() != text:
        out.write_text(text)


if __name__ == "__main__":
    main()
