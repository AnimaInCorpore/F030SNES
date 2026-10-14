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
        elif ch == ":" and depth == 0 and 0 < i < len(line) - 1 and line[i - 1] == " " and line[i + 1] == " ":
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
    traced: bool = True        # False: a Diz line without bytes (widths unverified)
    anon: bool = False         # an anonymous `-`/`+` label stands on this line

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


DIZ_COMMENT_RE = re.compile(r"^([0-9A-F]{6})(?:\|([0-9A-F]*)\s*\|)?")
DIZ_RANGE_RE = re.compile(r"^([0-9A-F]{6})\.\.([0-9A-F]{6,7})")
DIZ_EQU_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*\$([0-9A-Fa-f]+)\s*(?:;.*)?$")
DIZ_LABEL_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*):")
DIZ_ANON_RE = re.compile(r"^[-+]+(?=\s|$)")
DIZ_INSN_RE = re.compile(r"^([A-Z]{3})(?:\.([BWL]))?(?:\s+(.*?))?\s*$")
DIZ_DATA_RE = re.compile(r"^(db|dw|dl|dd)\s+(.*?)\s*$")
DIZ_INCBIN_RE = re.compile(r'^incbin\s+"([^"]+)":\$([0-9A-Fa-f]+)\.\.\$([0-9A-Fa-f]+)')
DIZ_INCSRC_RE = re.compile(r'^incsrc\s+"([^"]+)"')


def hirom_offset(snes: int) -> int:
    """File offset of a HiROM address in a 4 MiB image (banks $C0-$FF)."""
    return snes & 0x3FFFFF


def split_comment(line: str) -> tuple[str, str | None]:
    quote = False
    for i, ch in enumerate(line):
        if ch == '"':
            quote = not quote
        elif ch == ";" and not quote:
            return line[:i], line[i + 1:]
    return line, None


def parse_diz(main_asm: Path, rom: bytes) -> Listing:
    """Places the Chrono Trigger disassembly (third_party/ct_disassembly) on
    the ROM.

    It is a DiztinGUIsh export: main.asm includes bank_C0.asm..bank_FF.asm,
    and every placed line ends in a comment ";AAAAAA|bytes  |target" (code)
    or ";AAAAAA" (data) with its SNES address; incbin runs carry their range
    (";AAAAAA..BBBBBB"). Placement therefore needs no size inference: each
    instruction is decoded from the ROM byte at its address and must match
    the listing's mnemonic, with the length the listing's bytes (or, where a
    hand-edited line has none, its .B/.W/.L suffix) give; that fixes the M/X
    width of every immediate the trace saw. The bytes, where present, must
    equal the ROM's. Labels take the address of their line, or of the next
    placed line when they stand alone. Equates (`Name = $C30B74`) name data
    and entry points the trace did not mark as code.
    """
    root = main_asm.parent
    items: list = []
    labels: dict[str, int] = {}
    symbols: dict[str, int] = {}
    insn_at: dict[int, Insn] = {}
    errors: list[str] = []
    warnings: list[str] = []
    pending: list[Label] = []
    files = [m.group(1) for m in (DIZ_INCSRC_RE.match(split_comment(x)[0].strip())
                                  for x in main_asm.read_text().splitlines()) if m]
    last_end = -1

    def place(snes: int, where: str) -> int:
        nonlocal pending
        off = hirom_offset(snes)
        if snes >> 16 < 0xC0:
            errors.append(f"{where}: address ${snes:06X} outside banks $C0-$FF")
        for lab in pending:
            lab.offset = off
            labels[lab.name] = off
            symbols[lab.name] = snes
        pending = []
        return off

    def lines_of(fname: str):
        """The lines of a bank file with its own incsrc'd regions inlined."""
        for lineno, raw in enumerate((root / fname).read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            m = DIZ_INCSRC_RE.match(split_comment(raw)[0].strip())
            if m:
                yield from lines_of(m.group(1))
            else:
                yield fname, lineno, raw

    for top in files:
        if not top.startswith("bank_"):
            continue
        for fname, lineno, raw in lines_of(top):
            where = f"{fname}:{lineno}"
            code, comment = split_comment(raw)
            text = code.strip()
            if not text:
                continue
            m = DIZ_EQU_RE.match(text)
            if m:
                symbols[m.group(1)] = int(m.group(2), 16)
                continue
            m = DIZ_LABEL_RE.match(text)
            if m:
                lab = Label(m.group(1), -1, lineno)
                items.append(lab)
                pending.append(lab)
                text = text[m.end():].strip()
            m = DIZ_ANON_RE.match(text)
            anon = bool(m)
            if m:
                text = text[m.end():].strip()
            if not text:
                continue
            if text.startswith("ORG "):
                continue
            cm = DIZ_COMMENT_RE.match((comment or "").strip())
            m = DIZ_INCBIN_RE.match(text)
            if m:
                bank = int(re.search(r"bank_([0-9A-F]{2})", m.group(1)).group(1), 16)
                a, b = int(m.group(2), 16), int(m.group(3), 16)
                snes = (bank << 16) | a
                off = place(snes, where)
                if off < last_end:
                    errors.append(f"{where}: incbin at ${snes:06X} overlaps the previous item")
                items.append(Data(off, b - a, lineno, text))
                last_end = off + (b - a)
                continue
            m = DIZ_DATA_RE.match(text)
            prev = next((x for x in reversed(items) if not isinstance(x, Label)), None)
            if cm is None and m and isinstance(prev, Data) and last_end >= 0:
                snes = 0xC00000 | last_end          # data continuing the previous line
            elif cm is None:
                errors.append(f"{where}: no address comment: {text[:60]}")
                continue
            else:
                snes = int(cm.group(1), 16)
            if m:
                kind, rest = m.group(1), m.group(2)
                unit = {"db": 1, "dw": 2, "dl": 3, "dd": 4}[kind]
                elems = split_elements(rest)
                off = place(snes, where)
                if off < last_end:
                    errors.append(f"{where}: data at ${snes:06X} overlaps the previous item")
                items.append(Data(off, unit * len(elems), lineno, text,
                                  elems if kind != "db" else [], unit))
                last_end = off + unit * len(elems)
                continue
            m = DIZ_INSN_RE.match(text)
            if not m:
                errors.append(f"{where}: unparsed: {text[:60]}")
                continue
            mnem, suffix, operand = m.group(1), m.group(2), (m.group(3) or "").strip()
            off = place(snes, where)
            if off < last_end:
                errors.append(f"{where}: instruction at ${snes:06X} overlaps the previous item")
            op = rom[off]
            rmnem, mode = OPCODES[op]
            listed = bytes.fromhex(cm.group(2)) if cm.group(2) else None
            if listed:
                size = len(listed)
                if rom[off:off + size] != listed:
                    errors.append(f"{where}: bytes {listed.hex()} but the ROM has {rom[off:off + size].hex()}")
            elif mode in (IMM_M, IMM_X):
                # Lines without bytes were never traced: Diz decoded them
                # with guessed widths, often wrong (C20D58 `ADC.B` after a
                # 16-bit CMP). Their size is kept but not trusted.
                if suffix not in ("B", "W"):
                    errors.append(f"{where}: immediate without width")
                    continue
                size = 2 if suffix == "B" else 3
            else:
                size = 1 + FIXED_LEN[mode]
            if rmnem != mnem and {"JML": "JMP", "JSL": "JSR"}.get(rmnem, rmnem) != \
                    {"JML": "JMP", "JSL": "JSR"}.get(mnem, mnem):
                if listed:
                    errors.append(f"{where}: ROM ${op:02X} is {rmnem}, listing says {mnem}")
                else:
                    warnings.append(f"{where}: untraced line says {mnem}, ROM ${op:02X} is {rmnem}")
                continue
            if mode in (IMM_M, IMM_X):
                if size not in (2, 3):
                    errors.append(f"{where}: immediate size {size}")
            elif mode != BLK and 1 + FIXED_LEN[mode] != size:
                errors.append(f"{where}: {rmnem} {mode} is {1 + FIXED_LEN[mode]} bytes, listing {size}")
            insn = Insn(rmnem, suffix and suffix.lower(), operand, off, size, lineno)
            insn.opcode, insn.mode = op, mode
            insn.raw = rom[off:off + size]
            insn.traced = bool(listed)
            insn.anon = anon
            items.append(insn)
            insn_at[off] = insn
            last_end = off + size

    for lab in pending:
        errors.append(f"label {lab.name} at the end of the listing")
    if errors:
        raise PlacementError("\n".join(errors[:60]) + (f"\n... {len(errors)} errors" if len(errors) > 60 else ""))
    listing = Listing(items, labels, insn_at, rom)
    listing.symbols = symbols
    # Equates that name an address inside a listed instruction: entries into
    # the middle of an instruction (the analysis decodes them separately).
    starts = sorted(insn_at)
    import bisect
    mids = []
    for name, snes in symbols.items():
        if snes >> 16 < 0xC0 and not (0x40 <= snes >> 16 < 0x7E):
            continue
        off = hirom_offset(snes)
        k = bisect.bisect_right(starts, off) - 1
        if k >= 0 and starts[k] < off < starts[k] + insn_at[starts[k]].size:
            mids.append((name, off, 0))
    listing.mid_labels = mids
    listing.warnings = warnings
    listing.finish()
    for item in items:
        if isinstance(item, Label) and item.offset in insn_at:
            insn_at[item.offset].labels.append(item.name)
    return listing
