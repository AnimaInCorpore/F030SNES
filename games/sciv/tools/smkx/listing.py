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


# -- Final Fantasy VI (everything8215/ff6, ca65) ---------------------------------

FF6_ALIASES = {"INA": "INC", "DEA": "DEC", "BGE": "BCS", "BLT": "BCC", "SWA": "XBA", "TAD": "TCD",
               "TDA": "TDC", "TAS": "TCS", "TSA": "TSC", "CPA": "CMP"}
FF6_MNEMONICS = {m for m, _ in OPCODES.values()} | set(FF6_ALIASES)
FF6_DATA_UNIT = {".addr": 2, ".word": 2, ".faraddr": 3, ".dword": 4, ".dbyt": 2, ".byte": 1, ".byt": 1,
                 ".res": 1, ".incbin": 1, ".lobytes": 1, ".hibytes": 1, ".bankbytes": 1, ".asciiz": 1,
                 ".literal": 1}
# Macros whose body starts with a macro parameter: ca65 has no other way to
# name the instruction (op_n, array_op) or the directive (_ptr_tbl, _end_ptr).
FF6_PARAM_INSN = {"op_n", "array_op"}
FF6_LABEL_PREFIX = re.compile(r"^(?:(?:@?[A-Za-z0-9_]+|::[A-Za-z_][A-Za-z0-9_:]*)::?(?!=)|:(?![+\-]))\s*")
FF6_MACRO_RE = re.compile(r"^\s*\.mac(?:ro)?\s+([A-Za-z_][A-Za-z0-9_]*)\s*(.*?)\s*(?:;.*)?$")
FF6_ROM_LO, FF6_ROM_HI = 0xC00000, 0xF00000     # the 3 MiB image: banks $C0-$EF


def ff6_rom_offset(snes: int) -> int:
    return snes & 0x3FFFFF


def parse_ff6(source_dir: Path, rom: bytes) -> Listing:
    """Places the Final Fantasy VI disassembly (third_party/ff6, ca65) on the ROM.

    The disassembly is ca65 source, not an address-annotated export, so the
    placement comes from the linker: tools/smkx/ff6src.py builds the Japanese
    version out of tree and this reads the debug file ld65 writes for it
    (`--dbgfile`): every source line that produced bytes with the segment
    and offset of each piece (a *span*), and every label with its value and
    the lines that mention it. A span is classified by its source line:

    - a mnemonic (or ca65 alias) makes an instruction. The line is often a
      macro's body (`longa`, `jeq`, `inx2`), whose lines carry the spans of
      every expansion; the invocation line, whose token is the macro name,
      is skipped, so no byte is counted twice;
    - a directive (`.byte`, `.addr`, `.faraddr`, ...) makes data, with the
      element size as `unit`, which is what the jump-table reader wants;
    - a macro that names its instruction or directive by a parameter
      (`op_n`, `_ptr_tbl`) is told apart by its name.

    As in parse_diz, every instruction is then decoded from the user's ROM at
    its address and must match the source mnemonic and the size the
    assembler gave it (which also fixes the M/X width of every immediate),
    and its bytes must equal the rebuilt ROM's. The rebuilt ROM differs from
    the original in unused regions only, so data spans are compared too, and
    the count of differing data bytes is reported in `listing.warnings`.
    Code the game copies to WRAM (the cutscene program at $7E5000, stored
    LZ-compressed in bank $C2) is not in the ROM as written and is left out.

    Sets `listing.taken`: the addresses of labels on instructions that a
    directive, an immediate operand, `PEA`, or a data macro mentions, which
    is what analyze.address_taken reports for the other listing formats.
    """
    from . import ff6src

    dbg = ff6src.ensure(rom)
    root = ff6src.BUILD
    built = (root / "build/jp/rom/ff6-jp.sfc").read_bytes()
    segs: dict[int, tuple[str, int]] = {}
    files: dict[int, str] = {}
    spans: dict[int, tuple[int, int, int]] = {}
    lines: dict[int, tuple[int, int, list[int]]] = {}
    scopes: dict[int, tuple[str, int | None]] = {}
    syms: dict[int, dict] = {}

    def fields(rest: str) -> dict[str, str]:
        out = {}
        for part in re.findall(r'(\w+)=("[^"]*"|[^,]*)', rest):
            out[part[0]] = part[1].strip('"')
        return out

    for raw in dbg.open(encoding="utf-8", errors="replace"):
        kind, _, rest = raw.rstrip("\n").partition("\t")
        if kind == "span":
            d = fields(rest)
            spans[int(d["id"])] = (int(d["seg"]), int(d["start"]), int(d["size"]))
        elif kind == "line":
            d = fields(rest)
            if "span" in d:
                lines[int(d["id"])] = (int(d["file"]), int(d["line"]), [int(x) for x in d["span"].split("+")])
            else:
                lines[int(d["id"])] = (int(d["file"]), int(d["line"]), [])
        elif kind == "sym":
            if rest.endswith("type=lab"):
                d = fields(rest)
                if "val" in d and "seg" in d:
                    syms[int(d["id"])] = d
        elif kind == "seg":
            d = fields(rest)
            segs[int(d["id"])] = (d["name"], int(d["start"], 16))
        elif kind == "file":
            d = fields(rest)
            files[int(d["id"])] = d["name"]
        elif kind == "scope":
            d = fields(rest)
            scopes[int(d["id"])] = (d["name"], int(d["parent"]) if "parent" in d else None)

    src: dict[int, list[str]] = {}

    def text_of(fid: int, ln: int) -> str:
        if fid not in src:
            try:
                src[fid] = (root / files[fid]).read_text(encoding="utf-8", errors="replace").splitlines()
            except OSError:
                src[fid] = []
        a = src[fid]
        return a[ln - 1] if 0 < ln <= len(a) else ""

    macro_at: dict[int, list[tuple[int, int, str, list[str]]]] = {}

    def macro_of(fid: int, ln: int):
        """(name, parameters) of the macro definition around a source line."""
        if fid not in macro_at:
            found, cur = [], None
            text_of(fid, 1)
            for n, t in enumerate(src.get(fid, []), 1):
                m = FF6_MACRO_RE.match(t)
                if m:
                    cur = (n, m.group(1), [x.strip() for x in m.group(2).split(",") if x.strip()])
                elif cur and re.match(r"^\s*\.end(?:mac|macro)\b", t):
                    found.append((cur[0], n, cur[1], cur[2]))
                    cur = None
            macro_at[fid] = found
        for a, b, name, params in macro_at[fid]:
            if a < ln < b:
                return name, params
        return None

    def classify(fid: int, ln: int):
        """('insn', mnemonic, operand) | ('data', unit) | ('macro', name) | ('param', macro)."""
        t = split_comment(text_of(fid, ln))[0].strip()
        anon = False
        while True:
            m = FF6_LABEL_PREFIX.match(t)
            if not m:
                break
            anon |= m.group().startswith(":")
            t = t[m.end():]
        m = re.match(r"(\.?[A-Za-z_][A-Za-z0-9_]*)\s*(.*)$", t)
        if not m:
            return ("none", t, anon)
        tok, operand = m.group(1), m.group(2).strip()
        up = tok.upper()
        if not tok.startswith(".") and up in FF6_MNEMONICS:
            return ("insn", FF6_ALIASES.get(up, up), operand, anon)
        if tok.lower() in FF6_DATA_UNIT:
            return ("data", FF6_DATA_UNIT[tok.lower()], operand)
        mac = macro_of(fid, ln)
        if mac and tok in mac[1]:
            return ("param", mac[0], operand, anon)
        return ("macro", tok)

    errors: list[str] = []
    warnings: list[str] = []
    items: list = []
    insn_at: dict[int, Insn] = {}
    kinds: dict[int, tuple] = {}         # line id -> classification
    code_macros: set[str] = set()
    data_bytes = data_diff = 0
    ranges: list[tuple[int, int, str]] = []
    placed: dict[int, str] = {}

    for lid, (fid, ln, sp) in lines.items():
        pieces = []
        for sid in sp:
            seg, start, size = spans[sid]
            addr = segs[seg][1] + start
            if size and FF6_ROM_LO <= addr < FF6_ROM_HI:
                pieces.append((sid, addr, size))
        if not pieces:
            continue
        c = classify(fid, ln)
        kinds[lid] = c
        where = f"{files[fid]}:{ln}"
        if c[0] == "macro":
            continue                      # the invocation; its body lines carry the bytes
        if c[0] == "none":
            errors.append(f"{where}: bytes from a line that names no instruction or directive: {c[1][:60]!r}")
            continue
        if c[0] == "param":
            if c[1] in FF6_PARAM_INSN:
                c = ("insn", None, c[2], c[3])
            else:
                c = ("data", None, c[2])
        for sid, addr, size in pieces:
            if sid in placed:         # a body line ld65 lists twice (as macro text and as expansion)
                if placed[sid] != c[0]:
                    errors.append(f"{where}: the bytes at ${addr:06X} are also claimed as {placed[sid]}")
                continue
            placed[sid] = c[0]
            off = ff6_rom_offset(addr)
            ranges.append((off, off + size, where))
            if c[0] == "insn":
                op = rom[off]
                rmnem, mode = OPCODES[op]
                mnem = c[1]
                ok = mnem is None or rmnem == mnem or \
                    {"JML": "JMP", "JSL": "JSR"}.get(rmnem, rmnem) == {"JML": "JMP", "JSL": "JSR"}.get(mnem, mnem)
                if not ok:
                    errors.append(f"{where}: ROM ${op:02X} at ${addr:06X} is {rmnem}, source says {mnem}")
                    continue
                if mode in (IMM_M, IMM_X):
                    if size not in (2, 3):
                        errors.append(f"{where}: immediate of {size} bytes at ${addr:06X}")
                        continue
                elif mode != BLK and 1 + FIXED_LEN[mode] != size:
                    errors.append(f"{where}: {rmnem} {mode} is {1 + FIXED_LEN[mode]} bytes, assembled {size}")
                    continue
                if rom[off:off + size] != built[off:off + size]:
                    errors.append(f"{where}: bytes at ${addr:06X} differ from the rebuilt ROM: "
                                  f"{rom[off:off + size].hex()} / {built[off:off + size].hex()}")
                    continue
                insn = Insn(rmnem, None, c[2], off, size, ln)
                insn.opcode, insn.mode = op, mode
                insn.raw = rom[off:off + size]
                anon = c[3]
                if not anon and ln > 1:
                    k = ln - 1
                    while k > 1 and not split_comment(text_of(fid, k))[0].strip():
                        k -= 1
                    anon = split_comment(text_of(fid, k))[0].strip() == ":"
                insn.anon = anon
                items.append((off, 1, insn))
                insn_at[off] = insn
                macro = macro_of(fid, ln)
                if macro:
                    code_macros.add(macro[0])
            else:
                unit = c[1] if c[1] is not None else (size if size in (2, 3) else 1)
                if size % unit:
                    unit = 1
                d = Data(off, size, ln, c[2] if len(c) > 2 else "", [], unit)
                items.append((off, 1, d))
                data_bytes += size
                if rom[off:off + size] != built[off:off + size]:
                    data_diff += size

    ranges.sort()
    for (a0, a1, w0), (b0, b1, w1) in zip(ranges, ranges[1:]):
        if b0 < a1:
            errors.append(f"{w1}: bytes at ${0xC00000 | b0:06X} overlap those of {w0}")

    # labels
    def qualified(d: dict) -> str:
        name = d["name"]
        if "parent" in d and int(d["parent"]) in syms:
            name = syms[int(d["parent"])]["name"] + "." + name
        sc = int(d.get("scope", 0))
        chain = []
        while sc in scopes and scopes[sc][0]:
            chain.append(scopes[sc][0])
            sc = scopes[sc][1]
            if sc is None:
                break
        return "::".join(reversed(chain + [name]))

    labels: dict[str, int] = {}
    symbols: dict[str, int] = {}
    by_off: dict[int, list[str]] = {}
    label_syms: list[tuple[int, str, dict]] = []
    for d in syms.values():
        addr = int(d["val"], 16)
        if not FF6_ROM_LO <= addr < FF6_ROM_HI:
            continue
        name = qualified(d)
        if name in labels and labels[name] != ff6_rom_offset(addr):
            name = f"{name}_{addr:06X}"
        off = ff6_rom_offset(addr)
        labels[name] = off
        symbols[name] = addr
        by_off.setdefault(off, []).append(name)
        items.append((off, 0, Label(name, off, 0)))
        label_syms.append((addr, name, d))

    if errors:
        raise PlacementError("\n".join(errors[:60]) + (f"\n... {len(errors)} errors" if len(errors) > 60 else ""))
    items.sort(key=lambda t: (t[0], t[1]))
    listing = Listing([x[2] for x in items], labels, insn_at, rom)
    listing.symbols = symbols
    for off, names in by_off.items():
        if off in insn_at:
            insn_at[off].labels.extend(names)

    # entries into the middle of an instruction
    import bisect
    starts = sorted(insn_at)
    mids = []
    for addr, name, _ in label_syms:
        off = ff6_rom_offset(addr)
        k = bisect.bisect_right(starts, off) - 1
        if k >= 0 and starts[k] < off < starts[k] + insn_at[starts[k]].size:
            mids.append((name, off, 0))
    listing.mid_labels = mids

    # code addresses the source uses as data
    taken: set[int] = set()
    for addr, name, d in label_syms:
        if ff6_rom_offset(addr) not in insn_at or "ref" not in d:
            continue
        for lid in (int(x) for x in d["ref"].split("+")):
            c = kinds.get(lid)
            if c is None:
                if lid in lines:
                    fid, ln, _ = lines[lid]
                    c = classify(fid, ln)
                else:
                    continue
            if c[0] == "insn":
                if "#" in c[2] or c[1] == "PEA":
                    taken.add(addr)
            elif c[0] in ("data", "param"):
                taken.add(addr)
            elif c[0] == "macro" and c[1] not in code_macros:
                taken.add(addr)
    listing.taken = taken
    # Labels in WRAM (the cutscene program's, which the ROM keeps compressed)
    # with the text of the source lines that mention them, for scripts that
    # look for addresses the program uses as data (scripts/ff6x_overlay_tasks.py).
    listing.wram_refs = []
    for d in syms.values():
        addr = int(d["val"], 16)
        if 0x7E0000 <= addr < 0x800000:
            texts = []
            for lid in (int(x) for x in d.get("ref", "").split("+") if x):
                if lid in lines:
                    fid, ln, _ = lines[lid]
                    texts.append(split_comment(text_of(fid, ln))[0].strip())
            listing.wram_refs.append((addr, qualified(d), texts))
    listing.warnings = warnings
    warnings.append(f"data bytes placed: {data_bytes}; differing from the rebuilt ROM: {data_diff}")
    listing.finish()
    return listing


def from_trace(rom: bytes, tr) -> Listing:
    """A Listing from tools/sciv_trace.py: the instructions the tracer reached,
    placed by ROM offset. There is no source for Super Castlevania IV, so
    this is the tracer's reading of the ROM, not a ground truth; the
    analysis' length check against it (analyze.step) then shows where the
    tracer and the analysis disagree. `taken` holds every address the tracer
    found as a jump/call table entry or a RAM-pointer value."""
    from tools import sciv_trace as T
    items: list = []
    insn_at: dict[int, Insn] = {}
    labels: dict[str, int] = {}
    for a in sorted(tr.insn):
        length, mn, mode, v, target = tr.insn[a]
        off = T.rom_index(a)
        if off is None:
            continue
        raw = rom[off:off + length]
        ins = Insn(mnem=mn, suffix=None, operand="", offset=off, size=length, line=0,
                   opcode=rom[off], mode=mode, raw=bytes(raw))
        name = tr.labels.get(a)
        if name:
            labels[name] = off
            ins.labels.append(name)
        items.append(ins)
        insn_at[off] = ins
    lst = Listing(items=items, labels=labels, insn_at=insn_at, rom=rom)
    lst.taken = {a for ents in tr.tables.values() for a in ents}
    lst.trace = tr
    lst.finish()
    return lst
