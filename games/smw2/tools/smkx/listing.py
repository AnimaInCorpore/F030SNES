"""Place the pinned SMK listing on the ROM.

The listing (SMK/Routine_Macros_SMK.asm) supplies code/data boundaries,
labels and mnemonics; the ROM supplies every opcode and operand byte. Each
line is given a file offset by summing item sizes from the bank macros, and
every label whose name carries its SNES address (CODE_80FF70, DATA_C01F4C,
.DATA_85D980, ...) re-checks that sum. An instruction is accepted only when
the ROM opcode at its offset decodes to the listing's mnemonic with the
listing's operand length, which also fixes the M/X width of every immediate.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

from .opcodes import OPCODES, IMM_M, IMM_X, FIXED_LEN, BLK

SHORT_BRANCHES = {"BPL", "BMI", "BVC", "BVS", "BRA", "BCC", "BCS", "BNE", "BEQ"}
SUFFIX_LEN = {"b": 1, "w": 2, "l": 3}
LABEL_RE = re.compile(r"^(\.?)([A-Za-z_][A-Za-z0-9_]*):")
HINT_RE = re.compile(r"_([0-9A-F]{6})$")
INSN_RE = re.compile(r"^\s+([A-Z]{3})(?:\.([bwl]))?(?:\s+([^;]*?))?\s*(?:;.*)?$")
DATA_RE = re.compile(r"^\s+(db|dw|dl|dd)\s+(.*)$")
INCBIN_RE = re.compile(r'^\s+incbin\s+"([^"]+)"(:\S+)?')
COMMENT_HINT_RE = re.compile(r"^;\$([0-9A-F]{6})\s*$")
FREE_RE = re.compile(r"^\s*%FREE_BYTES\(\s*([^,]+),\s*([^,]+),\s*([^)]+)\)")
BANK_MACRO_RE = re.compile(r"^macro\s+SMKBank(C[0-7])Macros?\b")


def rom_offset(snes: int) -> int:
    """HiROM file offset of a 512 KiB image for a label-name address."""
    return ((snes >> 16) & 0x07) << 16 | (snes & 0xFFFF)


def num(text: str) -> int:
    text = text.strip()
    if text.startswith("$"):
        return int(text[1:], 16)
    if text.startswith("%"):
        return int(text[1:], 2)
    return int(text, 10)


def split_elements(text: str) -> list[str]:
    parts, depth, quote, cur = [], 0, False, []
    for ch in text:
        if ch == '"':
            quote = not quote
        elif not quote and ch in "([":
            depth += 1
        elif not quote and ch in ")]":
            depth -= 1
        elif not quote and depth == 0 and ch == ";":
            break
        if ch == "," and not quote and depth == 0:
            parts.append("".join(cur).strip())
            cur = []
            continue
        cur.append(ch)
    if "".join(cur).strip():
        parts.append("".join(cur).strip())
    return parts


def split_statements(line: str) -> list[str]:
    """Split Asar's `a : b` statement separators; keep the first part's indent."""
    code, quote, depth, cut = line, False, 0, []
    for i, ch in enumerate(line):
        if ch == '"':
            quote = not quote
        elif quote:
            continue
        elif ch == ";":
            break
        elif ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        elif ch == ":" and depth == 0 and 0 < i < len(line) - 1 and line[i - 1] in " \t" and line[i + 1] in " \t":
            cut.append(i)
    if not cut:
        return [code]
    parts, start = [], 0
    for i in cut:
        parts.append(line[start:i].rstrip())
        start = i + 1
    parts.append(line[start:])
    return [parts[0]] + ["\t" + x.strip() for x in parts[1:]]


@dataclass
class Label:
    name: str          # global name, or Global.local for local labels
    offset: int
    line: int


@dataclass
class Insn:
    mnem: str
    suffix: str | None
    operand: str
    offset: int
    size: int
    line: int
    opcode: int = 0
    mode: str = ""
    raw: bytes = b""
    labels: list[str] = field(default_factory=list)

    @property
    def value(self) -> int:
        """Little-endian operand value from the ROM bytes."""
        v = 0
        for i, b in enumerate(self.raw[1:]):
            v |= b << (8 * i)
        return v


@dataclass
class Data:
    offset: int
    size: int | None
    line: int
    text: str
    words: list[str] = field(default_factory=list)   # dw/dl symbolic elements
    unit: int = 1


@dataclass
class Listing:
    items: list            # Label | Insn | Data, in listing order
    labels: dict[str, int]
    insn_at: dict[int, Insn]
    rom: bytes
    warnings: list = field(default_factory=list)
    mid_labels: list = field(default_factory=list)

    def label_of(self, offset: int) -> list[str]:
        return self._by_offset.get(offset, [])

    def finish(self) -> None:
        self._by_offset: dict[int, list[str]] = {}
        for name, off in self.labels.items():
            self._by_offset.setdefault(off, []).append(name)


class PlacementError(Exception):
    pass


def parse(listing_path: Path, rom: bytes) -> Listing:
    smk_root = listing_path.parent
    lines = listing_path.read_text(encoding="utf-8", errors="replace").splitlines()
    items: list = []
    labels: dict[str, int] = {}
    insn_at: dict[int, Insn] = {}
    bank = -1
    off = 0
    unsure = False          # the running offset follows an unsized item
    last_global = ""
    unsure_at_label = False
    errors: list[str] = []
    warnings: list[str] = []
    mid_labels: list[tuple[str, int, int]] = []

    stream = []
    for lineno, raw_line in enumerate(lines, 1):
        for part in split_statements(raw_line):
            stream.append((lineno, part))

    for lineno, line in stream:
        stripped = line.strip()
        cm = COMMENT_HINT_RE.match(stripped)
        if cm and items and isinstance(items[-1], Label):
            want = rom_offset(int(cm.group(1), 16))
            lab = items[-1]
            if want != lab.offset:
                if not unsure_at_label:
                    errors.append(f"{lineno}: {lab.name} placed at {lab.offset:#07x}, comment says {want:#07x}")
                lab.offset = want
                labels[lab.name] = want
                off = want
            unsure = False
            continue
        if not stripped or stripped.startswith(";"):
            continue
        m = BANK_MACRO_RE.match(stripped)
        if m:
            bank = int(m.group(1)[1], 16)
            continue
        if stripped.startswith("%BANK_START"):
            off = bank << 16
            unsure = False
            continue
        if stripped.startswith("%HiROMBankSplit"):
            split = (bank << 16) | 0x8000
            if off > split and not unsure:
                errors.append(f"{lineno}: lower half overruns $8000 ({off:#x})")
            off = split
            unsure = False
            continue
        if stripped.startswith(("%BANK_END", "%BeginROMMirroring", "%EndROMMirroring",
                                "endmacro", "macro ")):
            continue
        m = FREE_RE.match(line)
        if m:
            where = m.group(1).strip()
            if where != "NULLROM":
                off = rom_offset(num(where) & 0xFFFFFF)
                unsure = False
            n = num(m.group(2))
            items.append(Data(off, n, lineno, stripped))
            off += n
            continue
        m = LABEL_RE.match(stripped)
        if m:
            local, name = m.group(1), m.group(2)
            full = f"{last_global}{name}" if local else name
            if not local:
                last_global = name
            hint = HINT_RE.search(name)
            at = off
            if hint:
                want = rom_offset(int(hint.group(1), 16))
                if want != off:
                    prev = items[-1] if items else None
                    if unsure:
                        off = at = want
                    elif isinstance(prev, Insn) and prev.offset < want < prev.offset + prev.size:
                        at = want                      # entry inside the previous instruction
                        mid_labels.append((full, want, lineno))
                    else:
                        warnings.append(f"{lineno}: {full} sits at {off:#07x}; its name says {want:#07x}")
                unsure = False
            unsure_at_label = unsure
            labels[full] = at
            items.append(Label(full, at, lineno))
            continue
        m = DATA_RE.match(line)
        if m:
            kind, rest = m.group(1), m.group(2)
            unit = {"db": 1, "dw": 2, "dl": 3, "dd": 4}[kind]
            elems = split_elements(rest)
            size = 0
            for e in elems:
                if e.startswith('"') and e.endswith('"') and kind == "db":
                    size += len(e) - 2
                else:
                    size += unit
            items.append(Data(off, size, lineno, stripped, elems if kind != "db" else [], unit))
            off += size
            continue
        m = INCBIN_RE.match(line)
        if m:
            path = smk_root / m.group(1)
            if m.group(2) or not path.exists():
                items.append(Data(off, None, lineno, stripped))
                unsure = True
                continue
            size = path.stat().st_size
            items.append(Data(off, size, lineno, stripped))
            off += size
            continue
        m = INSN_RE.match(line)
        if m:
            mnem, suffix, operand = m.group(1), m.group(2), (m.group(3) or "").strip()
            repeat = re.fullmatch(r"#(\d+)", operand)
            if repeat and not suffix:
                # Asar shorthand: `NOP #8` assembles eight implied instructions.
                for _ in range(int(repeat.group(1))):
                    insn = Insn(mnem, None, "", off, 1, lineno)
                    items.append(insn)
                    insn_at[off] = insn
                    off += 1
                continue
            if mnem in ("MVN", "MVP"):
                size = 3
            elif mnem in SHORT_BRANCHES:
                size = 2                  # Asar ignores a .w on BRA; BRL is its own mnemonic
            elif suffix:
                size = 1 + SUFFIX_LEN[suffix]
            else:
                size = 1
            insn = Insn(mnem, suffix, operand, off, size, lineno)
            if unsure:
                errors.append(f"{lineno}: instruction after an unsized item")
            items.append(insn)
            insn_at[off] = insn
            off += size
            continue
        errors.append(f"{lineno}: unparsed: {stripped[:60]}")

    # Verify every instruction against the ROM.
    for insn in insn_at.values():
        op = rom[insn.offset]
        mnem, mode = OPCODES[op]
        insn.opcode, insn.mode = op, mode
        insn.raw = rom[insn.offset:insn.offset + insn.size]
        alias = {"JML": "JMP"}.get(insn.mnem, insn.mnem)
        if mnem != insn.mnem and {"JML": "JMP"}.get(mnem, mnem) != alias:
            errors.append(f"{insn.line}: ROM ${op:02X} is {mnem}, listing says {insn.mnem}")
            continue
        if mode in (IMM_M, IMM_X):
            if insn.size not in (2, 3):
                errors.append(f"{insn.line}: immediate size {insn.size}")
        elif mode != BLK and 1 + FIXED_LEN[mode] != insn.size:
            errors.append(f"{insn.line}: {mnem} {mode} is {1 + FIXED_LEN[mode]} bytes, listing {insn.size}")

    if errors:
        raise PlacementError("\n".join(errors[:60]) + (f"\n... {len(errors)} errors" if len(errors) > 60 else ""))
    listing = Listing(items, labels, insn_at, rom)
    listing.warnings = warnings
    listing.mid_labels = mid_labels
    listing.finish()
    for item in items:
        if isinstance(item, Label) and item.offset in insn_at:
            insn_at[item.offset].labels.append(item.name)
    return listing


# -- Yoshi's Island (Yoshifanatic1 framework v1.1.0, SuperFX ROM layout) ------

YI_BANK_MACRO_RE = re.compile(r"^macro\s+YIBank([0-9A-F]{2})Macros\(")
YI_MACRO_RE = re.compile(r"^macro\s+((?:ROUTINE|DATATABLE)_YI_\w+)\(")
YI_CALL_RE = re.compile(r"^\s*%((?:ROUTINE|DATATABLE)_YI_\w+)\(\s*([^)]+)\)")
YI_AT_RE = re.compile(r"^\s*%(InsertNextPreCompiledCodeBlock|InsertGarbageData|InsertMacroAtXPosition)\(\s*([^,)]+)")


def yi_offset(snes: int) -> int | None:
    """File offset of a SNES address in YI's 2 MiB SuperFX ROM, or None.

    Banks $00-$3F (and $80-$BF) map ROM LoROM-style at $8000-$FFFF; banks
    $40-$5F (and $C0-$DF) map the same 2 MiB linearly, 64 KiB per bank."""
    bank, pc = (snes >> 16) & 0xFF, snes & 0xFFFF
    b = bank & 0x7F
    if b < 0x40:
        return (b << 15) | (pc & 0x7FFF) if pc >= 0x8000 else None
    if b < 0x60:
        return ((b - 0x40) << 16) | pc
    return None


def yi_snes(offset: int, bank_macro: int) -> int:
    """The SNES address a label at `offset` carries in the bank macro that
    places it: LoROM for banks $00-$17, linear ($4C-$5F) for the rest."""
    if bank_macro < 0x40:
        return ((offset >> 15) << 16) | 0x8000 | (offset & 0x7FFF)
    return 0x400000 + offset


def yi_condition(cond: str, bank: int) -> bool:
    cond = cond.split(";", 1)[0].strip()
    if "ROM_YI_U2" in cond:              # !Define_Global_ROMToAssemble&(!ROM_YI_U2) != $00
        return False                     # U.S. v1.0 is ROM_YI_U1
    if "EnableDebugFeatures" in cond:
        return False
    if cond.replace(" ", "") == "!CurrentBank!=$00":
        return bank != 0
    raise PlacementError(f"unknown condition: {cond}")


def parse_yi(listing_path: Path, rom: bytes) -> Listing:
    """Place YI/Routine_Macros_YI.asm on the ROM.

    Same checks as parse() (every instruction decoded from the ROM byte at
    its offset, label names that carry addresses re-checked), with what the
    YI framework adds: `if`/`else` on the ROM version (U.S. v1.0 is
    assembled), `%ROUTINE_YI_*`/`%DATATABLE_YI_*` macros inserted at a given
    address under a namespace, pre-compiled SuperFX blocks and garbage data
    (unsized: the next address-bearing label resynchronises), and the
    SuperFX memory map (yi_offset)."""
    root = listing_path.parent
    lines = listing_path.read_text(encoding="utf-8", errors="replace").splitlines()
    # Collect the insertable macros' bodies first.
    macros: dict[str, list[tuple[int, str]]] = {}
    cur = None
    for lineno, raw_line in enumerate(lines, 1):
        s = raw_line.strip()
        m = YI_MACRO_RE.match(s)
        if m:
            cur = m.group(1)
            macros[cur] = []
            continue
        if cur is not None:
            if s.startswith("endmacro"):
                cur = None
            else:
                macros[cur].append((lineno, raw_line))

    items: list = []
    labels: dict[str, int] = {}
    symbols: dict[str, int] = {}
    insn_at: dict[int, Insn] = {}
    errors: list[str] = []
    warnings: list[str] = []
    mid_labels: list[tuple[str, int, int]] = []
    st = {"bank": -1, "off": 0, "unsure": False, "glob": "", "unsure_at_label": False}

    def emit_line(lineno: int, line: str, ns: str) -> None:
        stripped = line.strip()
        off = st["off"]
        cm = COMMENT_HINT_RE.match(stripped)
        if cm and items and isinstance(items[-1], Label):
            want = yi_offset(int(cm.group(1), 16))
            lab = items[-1]
            if want is not None and want != lab.offset:
                if not st["unsure_at_label"]:
                    errors.append(f"{lineno}: {lab.name} placed at {lab.offset:#08x}, comment says {want:#08x}")
                lab.offset = want
                labels[lab.name] = want
                symbols[lab.name] = yi_snes(want, st["bank"])
                st["off"] = want
            st["unsure"] = False
            return
        if not stripped or stripped.startswith(";"):
            return
        if stripped.startswith(("%BANK_START", "%BANK_END", "%EnableSuperFXHiROMMirroring", "namespace",
                                "table ", "cleartable", "check bankcross")):
            return
        m = YI_AT_RE.match(stripped)
        if m:
            kind, where = m.group(1), m.group(2).strip()
            at = yi_offset(num(where) & 0xFFFFFF)
            if at is None:
                errors.append(f"{lineno}: {kind} at {where}")
                return
            st["off"] = at
            st["unsure"] = kind != "InsertMacroAtXPosition"
            if st["unsure"]:
                items.append(Data(at, None, lineno, stripped))
            return
        m = FREE_RE.match(line)
        if m:
            where = m.group(1).strip()
            if where != "NULLROM":
                st["off"] = off = yi_offset(num(where) & 0xFFFFFF)
                st["unsure"] = False
            n = num(m.group(2))
            items.append(Data(off, n, lineno, stripped))
            st["off"] = off + n
            return
        m = LABEL_RE.match(stripped)
        if m:
            local, name = m.group(1), m.group(2)
            if ns and not local:
                name = f"{ns}_{name}"
            full = f"{st['glob']}_{name}" if local else name
            if not local:
                st["glob"] = name
            hint = HINT_RE.search(name)
            want = yi_offset(int(hint.group(1), 16)) if hint else None
            at = off
            if want is not None:
                if want != off:
                    prev = items[-1] if items else None
                    if st["unsure"]:
                        st["off"] = at = want
                    elif isinstance(prev, Insn) and prev.offset < want < prev.offset + prev.size:
                        at = want
                        mid_labels.append((full, want, lineno))
                    else:
                        warnings.append(f"{lineno}: {full} sits at {off:#08x}; its name says {want:#08x}")
                st["unsure"] = False
            st["unsure_at_label"] = st["unsure"]
            if st["unsure"]:
                warnings.append(f"{lineno}: label {full} after an unsized item")
            if full in labels and labels[full] != at:
                errors.append(f"{lineno}: {full} placed twice")
            labels[full] = at
            symbols[full] = yi_snes(at, st["bank"])
            lab = Label(full, at, lineno)
            lab.glob = st["glob"]
            items.append(lab)
            return
        m = DATA_RE.match(line)
        if m:
            kind, rest = m.group(1), m.group(2)
            unit = {"db": 1, "dw": 2, "dl": 3, "dd": 4}[kind]
            elems = split_elements(rest)
            size = 0
            for e in elems:
                if e.startswith('"') and e.endswith('"') and kind == "db":
                    size += len(e) - 2
                else:
                    size += unit
            items.append(Data(off, size, lineno, stripped, elems if kind != "db" else [], unit))
            st["off"] = off + size
            return
        m = INCBIN_RE.match(line)
        if m:
            path = root / m.group(1)
            if m.group(2) or not path.exists():
                items.append(Data(off, None, lineno, stripped))
                st["unsure"] = True
                return
            size = path.stat().st_size
            items.append(Data(off, size, lineno, stripped))
            st["off"] = off + size
            return
        m = INSN_RE.match(line)
        if m:
            mnem, suffix, operand = m.group(1), m.group(2), (m.group(3) or "").strip()
            repeat = re.fullmatch(r"#(\d+)", operand)
            if repeat and not suffix:
                for _ in range(int(repeat.group(1))):
                    insn = Insn(mnem, None, "", st["off"], 1, lineno)
                    items.append(insn)
                    insn_at[st["off"]] = insn
                    st["off"] += 1
                return
            if mnem in ("MVN", "MVP"):
                size = 3
            elif mnem in SHORT_BRANCHES:
                size = 2
            elif suffix:
                size = 1 + SUFFIX_LEN[suffix]
            elif operand.startswith("#"):
                # no size hint: Asar sizes an immediate by its value
                size = 3 if num(operand[1:]) > 0xFF else 2
            else:
                size = 1
            insn = Insn(mnem, suffix, operand, off, size, lineno)
            if st["unsure"]:
                errors.append(f"{lineno}: instruction after an unsized item")
            items.append(insn)
            insn_at[off] = insn
            st["off"] = off + size
            return
        errors.append(f"{lineno}: unparsed: {stripped[:60]}")

    active: list[bool] = []          # one entry per open if: this branch assembles
    taken: list[bool] = []           # an earlier branch of this if assembled
    in_bank = False

    def on() -> bool:
        return all(active)

    for lineno, raw_line in enumerate(lines, 1):
        s = raw_line.strip()
        m = YI_BANK_MACRO_RE.match(s)
        if m:
            st["bank"] = int(m.group(1), 16)
            base = yi_offset((st["bank"] << 16) | (0x8000 if st["bank"] < 0x40 else 0))
            st["off"], st["unsure"], in_bank = base, False, True
            continue
        if not in_bank:
            continue
        if s.startswith("endmacro"):
            in_bank = False
            if active:
                errors.append(f"{lineno}: unclosed if")
            continue
        code = s.split(";", 1)[0].strip()
        if re.match(r"^if\b", code):
            c = on() and yi_condition(code[2:], st["bank"])
            active.append(c)
            taken.append(c)
            continue
        if re.match(r"^elseif\b", code):
            c = not taken[-1] and yi_condition(code[6:], st["bank"])
            active[-1] = c
            taken[-1] |= c
            continue
        if code == "else":
            active[-1] = not taken[-1]
            taken[-1] = True
            continue
        if code == "endif":
            active.pop()
            taken.pop()
            continue
        if not on():
            continue
        m = YI_CALL_RE.match(raw_line)
        if m:
            name, arg = m.group(1), m.group(2).strip()
            ns = name.split("_", 1)[1]              # ROUTINE_YI_x -> YI_x (the macro's namespace)
            saved_glob = st["glob"]
            for ml, mline in macros[name]:
                mline = mline.replace("<Address>", arg)
                for part in split_statements(mline):
                    emit_line(ml, part, ns)
            st["glob"] = saved_glob
            continue
        for part in split_statements(raw_line):
            emit_line(lineno, part, "")

    for insn in insn_at.values():
        op = rom[insn.offset]
        mnem, mode = OPCODES[op]
        insn.opcode, insn.mode = op, mode
        insn.raw = rom[insn.offset:insn.offset + insn.size]
        alias = {"JML": "JMP"}.get(insn.mnem, insn.mnem)
        if mnem != insn.mnem and {"JML": "JMP"}.get(mnem, mnem) != alias:
            errors.append(f"{insn.line}: ROM ${op:02X} at {insn.offset:#08x} is {mnem}, listing says {insn.mnem}")
            continue
        if mode in (IMM_M, IMM_X):
            if insn.size not in (2, 3):
                errors.append(f"{insn.line}: immediate size {insn.size}")
        elif mode != BLK and 1 + FIXED_LEN[mode] != insn.size:
            errors.append(f"{insn.line}: {mnem} {mode} is {1 + FIXED_LEN[mode]} bytes, listing {insn.size}")

    if errors:
        raise PlacementError("\n".join(errors[:60]) + (f"\n... {len(errors)} errors" if len(errors) > 60 else ""))
    listing = Listing(items, labels, insn_at, rom)
    listing.symbols = symbols
    listing.warnings = warnings
    listing.mid_labels = mid_labels
    listing.finish()
    for item in items:
        if isinstance(item, Label) and item.offset in insn_at:
            insn_at[item.offset].labels.append(item.name)
    return listing
