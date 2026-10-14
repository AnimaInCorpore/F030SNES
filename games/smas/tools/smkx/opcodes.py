"""65816 opcode table.

Each entry is (mnemonic, mode). Operand length follows from the mode, except
for the immediate modes whose length depends on the M or X flag:
IMM_M (accumulator/memory width) and IMM_X (index width).
"""

from __future__ import annotations

# Addressing modes.
IMP = "imp"          # implied / accumulator
IMM_M = "imm_m"      # #const, width from M
IMM_X = "imm_x"      # #const, width from X
IMM8 = "imm8"        # REP/SEP/WDM/COP/BRK signature byte
DP = "dp"
DPX = "dp,x"
DPY = "dp,y"
DPI = "(dp)"
DPIX = "(dp,x)"
DPIY = "(dp),y"
DPIL = "[dp]"
DPILY = "[dp],y"
ABS = "abs"
ABSX = "abs,x"
ABSY = "abs,y"
ABSI = "(abs)"       # JMP (abs)
ABSIX = "(abs,x)"    # JMP/JSR (abs,x)
ABSIL = "[abs]"      # JML [abs]
LONG = "long"
LONGX = "long,x"
SR = "sr,s"
SRIY = "(sr,s),y"
REL8 = "rel8"
REL16 = "rel16"
BLK = "blk"          # MVN/MVP dst,src banks

FIXED_LEN = {
    IMP: 0, IMM8: 1, DP: 1, DPX: 1, DPY: 1, DPI: 1, DPIX: 1, DPIY: 1,
    DPIL: 1, DPILY: 1, ABS: 2, ABSX: 2, ABSY: 2, ABSI: 2, ABSIX: 2,
    ABSIL: 2, LONG: 3, LONGX: 3, SR: 1, SRIY: 1, REL8: 1, REL16: 2, BLK: 2,
}

_ALU = {  # ORA AND EOR ADC STA LDA CMP SBC column layout
    0x01: DPIX, 0x03: SR, 0x05: DP, 0x07: DPIL, 0x09: IMM_M, 0x0D: ABS,
    0x0F: LONG, 0x11: DPIY, 0x12: DPI, 0x13: SRIY, 0x15: DPX, 0x17: DPILY,
    0x19: ABSY, 0x1D: ABSX, 0x1F: LONGX,
}

OPCODES: dict[int, tuple[str, str]] = {}
for base, name in ((0x00, "ORA"), (0x20, "AND"), (0x40, "EOR"), (0x60, "ADC"),
                   (0x80, "STA"), (0xA0, "LDA"), (0xC0, "CMP"), (0xE0, "SBC")):
    for low, mode in _ALU.items():
        if name == "STA" and mode == IMM_M:
            continue
        OPCODES[base + low] = (name, mode)

OPCODES.update({
    0x89: ("BIT", IMM_M),
    0x00: ("BRK", IMM8), 0x02: ("COP", IMM8), 0x42: ("WDM", IMM8),
    0x04: ("TSB", DP), 0x0C: ("TSB", ABS), 0x14: ("TRB", DP), 0x1C: ("TRB", ABS),
    0x06: ("ASL", DP), 0x0A: ("ASL", IMP), 0x0E: ("ASL", ABS), 0x16: ("ASL", DPX), 0x1E: ("ASL", ABSX),
    0x26: ("ROL", DP), 0x2A: ("ROL", IMP), 0x2E: ("ROL", ABS), 0x36: ("ROL", DPX), 0x3E: ("ROL", ABSX),
    0x46: ("LSR", DP), 0x4A: ("LSR", IMP), 0x4E: ("LSR", ABS), 0x56: ("LSR", DPX), 0x5E: ("LSR", ABSX),
    0x66: ("ROR", DP), 0x6A: ("ROR", IMP), 0x6E: ("ROR", ABS), 0x76: ("ROR", DPX), 0x7E: ("ROR", ABSX),
    0x08: ("PHP", IMP), 0x28: ("PLP", IMP), 0x48: ("PHA", IMP), 0x68: ("PLA", IMP),
    0x0B: ("PHD", IMP), 0x2B: ("PLD", IMP), 0x4B: ("PHK", IMP), 0x6B: ("RTL", IMP),
    0x8B: ("PHB", IMP), 0xAB: ("PLB", IMP), 0xDA: ("PHX", IMP), 0xFA: ("PLX", IMP),
    0x5A: ("PHY", IMP), 0x7A: ("PLY", IMP),
    0x10: ("BPL", REL8), 0x30: ("BMI", REL8), 0x50: ("BVC", REL8), 0x70: ("BVS", REL8),
    0x80: ("BRA", REL8), 0x90: ("BCC", REL8), 0xB0: ("BCS", REL8), 0xD0: ("BNE", REL8),
    0xF0: ("BEQ", REL8), 0x82: ("BRL", REL16),
    0x18: ("CLC", IMP), 0x38: ("SEC", IMP), 0x58: ("CLI", IMP), 0x78: ("SEI", IMP),
    0xB8: ("CLV", IMP), 0xD8: ("CLD", IMP), 0xF8: ("SED", IMP),
    0x1A: ("INC", IMP), 0x3A: ("DEC", IMP), 0xE6: ("INC", DP), 0xEE: ("INC", ABS),
    0xF6: ("INC", DPX), 0xFE: ("INC", ABSX), 0xC6: ("DEC", DP), 0xCE: ("DEC", ABS),
    0xD6: ("DEC", DPX), 0xDE: ("DEC", ABSX),
    0x1B: ("TCS", IMP), 0x3B: ("TSC", IMP), 0x5B: ("TCD", IMP), 0x7B: ("TDC", IMP),
    0xAA: ("TAX", IMP), 0xA8: ("TAY", IMP), 0x8A: ("TXA", IMP), 0x98: ("TYA", IMP),
    0x9A: ("TXS", IMP), 0xBA: ("TSX", IMP), 0x9B: ("TXY", IMP), 0xBB: ("TYX", IMP),
    0xEB: ("XBA", IMP), 0xFB: ("XCE", IMP),
    0xE8: ("INX", IMP), 0xC8: ("INY", IMP), 0xCA: ("DEX", IMP), 0x88: ("DEY", IMP),
    0x20: ("JSR", ABS), 0xFC: ("JSR", ABSIX), 0x22: ("JSL", LONG),
    0x4C: ("JMP", ABS), 0x6C: ("JMP", ABSI), 0x7C: ("JMP", ABSIX),
    0x5C: ("JML", LONG), 0xDC: ("JML", ABSIL),
    0x60: ("RTS", IMP), 0x40: ("RTI", IMP),
    0x24: ("BIT", DP), 0x2C: ("BIT", ABS), 0x34: ("BIT", DPX), 0x3C: ("BIT", ABSX),
    0xA2: ("LDX", IMM_X), 0xA6: ("LDX", DP), 0xAE: ("LDX", ABS), 0xB6: ("LDX", DPY), 0xBE: ("LDX", ABSY),
    0xA0: ("LDY", IMM_X), 0xA4: ("LDY", DP), 0xAC: ("LDY", ABS), 0xB4: ("LDY", DPX), 0xBC: ("LDY", ABSX),
    0x86: ("STX", DP), 0x8E: ("STX", ABS), 0x96: ("STX", DPY),
    0x84: ("STY", DP), 0x8C: ("STY", ABS), 0x94: ("STY", DPX),
    0x64: ("STZ", DP), 0x74: ("STZ", DPX), 0x9C: ("STZ", ABS), 0x9E: ("STZ", ABSX),
    0xE0: ("CPX", IMM_X), 0xE4: ("CPX", DP), 0xEC: ("CPX", ABS),
    0xC0: ("CPY", IMM_X), 0xC4: ("CPY", DP), 0xCC: ("CPY", ABS),
    0xC2: ("REP", IMM8), 0xE2: ("SEP", IMM8),
    0xD4: ("PEI", DP), 0xF4: ("PEA", ABS), 0x62: ("PER", REL16),
    0x54: ("MVN", BLK), 0x44: ("MVP", BLK),
    0xEA: ("NOP", IMP), 0xCB: ("WAI", IMP), 0xDB: ("STP", IMP),
})

assert len(OPCODES) == 256, len(OPCODES)


def operand_length(mode: str, m8: bool | None, x8: bool | None) -> int | None:
    """Operand byte count; None when an immediate width is undetermined."""
    if mode == IMM_M:
        return None if m8 is None else (1 if m8 else 2)
    if mode == IMM_X:
        return None if x8 is None else (1 if x8 else 2)
    return FIXED_LEN[mode]
