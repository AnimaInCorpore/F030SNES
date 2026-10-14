"""Walk an Asar source tree far enough to place every line on the ROM.

This is not an assembler: it produces no bytes. It follows what decides
where things go - `incsrc`, `ORG`, `base`, `arch`, defines, `if`/`while`,
functions and macros - and emits, in assembly order, the labels,
instructions and data statements with their SNES address and ROM file
offset. Instruction sizes come from the caller (who decodes the ROM
opcode at the offset); data sizes from the statements themselves.

Where a size cannot be known (SPC700 code, an expression that does not
evaluate) the walk becomes "unsure" and resynchronises at the next label
whose address the symbol file gives. Every other label is checked against
the symbol file, which is what makes the placement trustworthy.

The subset is what SMWDisX (Asar 1.91) uses.
"""

from __future__ import annotations

import math
import re
from dataclasses import dataclass, field
from pathlib import Path


class AsarError(Exception):
    pass


class EvalError(Exception):
    pass


def lorom_offset(addr: int) -> int | None:
    """ROM file offset of a LoROM address, or None outside ROM."""
    bank, lo = (addr >> 16) & 0x7F, addr & 0xFFFF
    if bank >= 0x7E:
        return None
    if lo < 0x8000:
        if bank < 0x40 or bank >= 0x70:
            return None
    return (bank << 15) | (lo & 0x7FFF)


def lorom_address(offset: int) -> int:
    return ((offset >> 15) << 16) | 0x8000 | (offset & 0x7FFF)


# -- source text helpers --------------------------------------------------------

def strip_comment(line: str) -> str:
    quote = False
    for i, ch in enumerate(line):
        if ch == '"':
            quote = not quote
        elif ch == ";" and not quote:
            return line[:i].rstrip()
    return line.rstrip()


def split_top(text: str, sep: str = ",") -> list[str]:
    """Split at top-level separators (outside quotes, parentheses, brackets)."""
    parts, depth, quote, cur = [], 0, False, []
    for ch in text:
        if ch == '"':
            quote = not quote
        elif not quote and ch in "([":
            depth += 1
        elif not quote and ch in ")]":
            depth -= 1
        if ch == sep and not quote and depth == 0:
            parts.append("".join(cur).strip())
            cur = []
            continue
        cur.append(ch)
    tail = "".join(cur).strip()
    if tail or parts:
        parts.append(tail)
    return parts


def split_statements(line: str) -> list[str]:
    """Asar's ` : ` statement separator."""
    out, start, quote, depth = [], 0, False, 0
    for i, ch in enumerate(line):
        if ch == '"':
            quote = not quote
        elif quote:
            continue
        elif ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        elif ch == ":" and depth == 0 and 0 < i < len(line) - 1 and line[i - 1] == " " and line[i + 1] == " ":
            out.append(line[start:i].strip())
            start = i + 1
    out.append(line[start:].strip())
    return [x for x in out if x]


# -- expressions -------------------------------------------------------------------

TOKEN_RE = re.compile(r"""
    (?P<ws>\s+)
  | (?P<hex>\$[0-9A-Fa-f]+)
  | (?P<bin>%[01]+)
  | (?P<dec>\d+(?:\.\d+)?)
  | (?P<chr>'(?:[^'\\]|\\.)')
  | (?P<str>"[^"]*")
  | (?P<id>[A-Za-z_.?][A-Za-z0-9_.?]*)
  | (?P<op><<|>>|<=|>=|==|!=|&&|\|\||\*\*|[-+*/&|^~<>!(),])
""", re.X)

BINARY = [
    ("||",), ("&&",), ("|",), ("^",), ("&",), ("==", "!="),
    ("<", ">", "<=", ">="), ("<<", ">>"), ("+", "-"), ("*", "/"), ("**",),
]


def tokenize(text: str) -> list[tuple[str, str]]:
    toks, pos = [], 0
    while pos < len(text):
        m = TOKEN_RE.match(text, pos)
        if not m:
            raise EvalError(f"cannot tokenize {text[pos:pos + 20]!r}")
        pos = m.end()
        if m.lastgroup != "ws":
            toks.append((m.lastgroup, m.group()))
    return toks


def _int(v) -> int:
    return int(math.floor(v)) if isinstance(v, float) else int(v)


class Expr:
    """Recursive-descent evaluation with C-like precedence."""

    def __init__(self, walker: "Walker", toks, params=None):
        self.w, self.toks, self.i, self.params = walker, toks, 0, params or {}

    def peek(self):
        return self.toks[self.i] if self.i < len(self.toks) else (None, None)

    def take(self):
        t = self.peek()
        self.i += 1
        return t

    def parse(self):
        v = self.binary(0)
        if self.i != len(self.toks):
            raise EvalError(f"trailing tokens {self.toks[self.i:]}")
        return v

    def binary(self, level):
        if level == len(BINARY):
            return self.unary()
        v = self.binary(level + 1)
        while True:
            kind, op = self.peek()
            if kind != "op" or op not in BINARY[level]:
                return v
            self.take()
            r = self.binary(level + 1)
            v = self.apply(op, v, r)

    @staticmethod
    def apply(op, a, b):
        if op == "+": return a + b
        if op == "-": return a - b
        if op == "*": return a * b
        if op == "/":
            if b == 0:
                raise EvalError("division by zero")
            return a / b
        if op == "**": return a ** b
        if op == "<<": return _int(a) << _int(b)
        if op == ">>": return _int(a) >> _int(b)
        if op == "&": return _int(a) & _int(b)
        if op == "|": return _int(a) | _int(b)
        if op == "^": return _int(a) ^ _int(b)
        if op == "==": return int(a == b)
        if op == "!=": return int(a != b)
        if op == "<": return int(a < b)
        if op == ">": return int(a > b)
        if op == "<=": return int(a <= b)
        if op == ">=": return int(a >= b)
        if op == "&&": return int(bool(a) and bool(b))
        if op == "||": return int(bool(a) or bool(b))
        raise EvalError(op)

    def unary(self):
        kind, tok = self.peek()
        if kind == "op" and tok in "-~!+":
            self.take()
            v = self.unary()
            return {"-": lambda: -v, "~": lambda: ~_int(v), "!": lambda: int(not v), "+": lambda: v}[tok]()
        return self.primary()

    def primary(self):
        kind, tok = self.take()
        if kind == "hex":
            return int(tok[1:], 16)
        if kind == "bin":
            return int(tok[1:], 2)
        if kind == "dec":
            return float(tok) if "." in tok else int(tok)
        if kind == "chr":
            return ord(tok[1:-1])
        if kind == "str":
            return tok[1:-1]
        if kind == "op" and tok == "(":
            v = self.binary(0)
            if self.take() != ("op", ")"):
                raise EvalError("missing )")
            return v
        if kind == "id":
            if self.peek() == ("op", "("):
                self.take()
                args = []
                if self.peek() != ("op", ")"):
                    while True:
                        args.append(self.binary(0))
                        k, t = self.take()
                        if t == ")":
                            break
                        if t != ",":
                            raise EvalError("bad argument list")
                else:
                    self.take()
                return self.w.call_function(tok, args)
            if tok in self.params:
                return self.params[tok]
            return self.w.label_value(tok)
        raise EvalError(f"unexpected {tok!r}")


# -- the walk ----------------------------------------------------------------------

@dataclass
class Event:
    kind: str            # label | insn | data | unsure
    pc: int | None       # SNES address (logical, as labels see it)
    off: int | None      # ROM file offset, None outside ROM or when unsure
    where: str           # file:line
    name: str = ""       # label name / mnemonic / data directive
    suffix: str | None = None
    operand: str = ""
    size: int | None = None
    elems: list = field(default_factory=list)
    arch: str = "65816"
    anon: bool = False


DIRECTIVES = {"db", "dw", "dl", "dd", "incbin", "incsrc", "org", "base", "arch", "lorom",
              "hirom", "asar", "check", "pushpc", "pullpc", "skip", "warnpc", "print",
              "error", "assert", "warn", "fill", "padbyte", "pad", "optimize", "math",
              "namespace", "bank", "cleartable", "table", "function", "macro", "endmacro",
              "if", "elseif", "else", "endif", "while", "endwhile", "rep", "freecode",
              "freedata", "autoclean", "norom", "fillbyte", "undef"}

DATA_UNIT = {"db": 1, "dw": 2, "dl": 3, "dd": 4}
LABEL_RE = re.compile(r"^(\.*[A-Za-z_?][A-Za-z0-9_?]*):(?!:)\s*(.*)$")
ASSIGN_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*=(?!=)\s*(.+)$")
ANON_RE = re.compile(r"^([-+]+)(?:\s+(.*))?$")
DEFINE_SET_RE = re.compile(r"^!([A-Za-z0-9_]+)\s*(=|#=|\?=|:=|\+=)\s*(.*)$")
DEFINE_USE_RE = re.compile(r"!([A-Za-z0-9_]+)")
MACRO_CALL_RE = re.compile(r"^%([A-Za-z0-9_]+)\((.*)\)$")
INSN_RE = re.compile(r"^([A-Za-z][A-Za-z0-9]{1,4})(?:\.([BWLbwl]))?(?:\s+(.*))?$")


class Walker:
    def __init__(self, root: Path, symbols: dict[str, int], defines: dict[str, str],
                 insn_size=None):
        self.root = root
        self.sym = symbols
        self.defines = dict(defines)
        self.functions: dict[str, tuple[list[str], list]] = {}
        self.macros: dict[str, tuple[list[str], list, str]] = {}
        self.insn_size = insn_size        # (offset, mnem, suffix, operand) -> size | None
        self.events: list[Event] = []
        self.errors: list[str] = []
        self.checked = 0
        self.pc = 0
        self.off: int | None = None
        self.base: int | None = None      # logical pc while `base` is active
        self.unsure = False
        self.arch = "65816"
        self.global_label = ""
        self.pcstack: list = []
        self.macro_serial = 0
        self.where = ""

    # -- expressions -----------------------------------------------------------
    def expand_defines(self, text: str, depth: int = 0) -> str:
        if "!" not in text:
            return text

        def sub(m):
            name = m.group(1)
            if name in self.defines:
                return self.defines[name]
            # `!=` and logical not are not defines
            raise AsarError(f"{self.where}: undefined define !{name}")
        out = []
        pos = 0
        for m in DEFINE_USE_RE.finditer(text):
            out.append(text[pos:m.start()])
            name = m.group(1)
            # longest defined prefix wins, as in Asar
            if name not in self.defines:
                cut = next((k for k in range(len(name), 0, -1) if name[:k] in self.defines), None)
                if cut is None:
                    raise AsarError(f"{self.where}: undefined define !{name}")
                out.append(self.defines[name[:cut]] + name[cut:])
            else:
                out.append(self.defines[name])
            pos = m.end()
        out.append(text[pos:])
        res = "".join(out)
        if "!" in res and DEFINE_USE_RE.search(res) and depth < 20:
            return self.expand_defines(res, depth + 1)
        return res

    def eval(self, text: str, params=None):
        return Expr(self, tokenize(text), params).parse()

    def label_value(self, name: str) -> int:
        if name == "pc":
            return self.pc
        full = name
        if name.startswith("."):
            full = f"{self.global_label}_{name.lstrip('.')}"
        elif "." in name:
            full = name.replace(".", "_")
        if full in self.sym:
            return self.sym[full]
        raise EvalError(f"unknown label {name}")

    def read_file(self, name: str) -> bytes:
        for base in (self.cur_dir, self.root):
            p = base / name
            if p.exists():
                return p.read_bytes()
        raise EvalError(f"missing file {name}")

    def call_function(self, name: str, args: list):
        b = lambda v: int(bool(v))
        builtin = {
            "equal": lambda a, c: b(a == c), "notequal": lambda a, c: b(a != c),
            "less": lambda a, c: b(a < c), "lessequal": lambda a, c: b(a <= c),
            "greater": lambda a, c: b(a > c), "greaterequal": lambda a, c: b(a >= c),
            "and": lambda a, c: b(a and c), "or": lambda a, c: b(a or c),
            "nand": lambda a, c: b(not (a and c)), "nor": lambda a, c: b(not (a or c)),
            "xor": lambda a, c: b(bool(a) != bool(c)), "not": lambda a: b(not a),
            "select": lambda s, a, c: a if s else c,
            "floor": lambda a: math.floor(a), "ceil": lambda a: math.ceil(a),
            "round": lambda a: round(a), "abs": abs, "min": min, "max": max,
            "sqrt": math.sqrt, "sin": math.sin, "cos": math.cos,
            "snestopc": lambda a: lorom_offset(_int(a)), "pctosnes": lambda a: lorom_address(_int(a)),
            "bank": lambda a: _int(a) >> 16,
            "filesize": lambda f: len(self.read_file(f)),
            "readfile1": lambda f, o, *d: self.read_file(f)[_int(o)],
            "readfile2": lambda f, o, *d: int.from_bytes(self.read_file(f)[_int(o):_int(o) + 2], "little"),
            "readfile3": lambda f, o, *d: int.from_bytes(self.read_file(f)[_int(o):_int(o) + 3], "little"),
            "defined": lambda n: b(n in self.defines),
            "stringsequal": lambda a, c: b(a == c),
            "stringsequalnocase": lambda a, c: b(str(a).lower() == str(c).lower()),
        }
        if name in self.functions:
            params, body = self.functions[name]
            if len(params) != len(args):
                raise EvalError(f"{name}: {len(args)} arguments for {len(params)}")
            return Expr(self, body, dict(zip(params, args))).parse()
        if name in builtin:
            return builtin[name](*args)
        raise EvalError(f"unknown function {name}")

    # -- position ----------------------------------------------------------------
    def set_org(self, addr: int) -> None:
        self.pc = addr
        self.off = lorom_offset(addr)
        self.unsure = False

    def advance(self, n: int) -> None:
        if self.off is not None:
            self.off += n
        if self.base is not None or self.off is None:
            self.pc += n
        else:
            self.pc = lorom_address(self.off)

    def emit(self, kind, **kw) -> Event:
        ev = Event(kind, self.pc, None if self.unsure else self.off, self.where, arch=self.arch, **kw)
        self.events.append(ev)
        return ev

    def go_unsure(self, why: str) -> None:
        if not self.unsure:
            self.unsure = True
            self.emit("unsure", name=why)

    # -- labels ------------------------------------------------------------------
    def define_label(self, name: str) -> None:
        if name.startswith("?"):
            return                                        # macro-local
        if name.startswith("."):
            full = f"{self.global_label}_{name.lstrip('.')}"
        else:
            full = name
            self.global_label = name
        want = self.sym.get(full)
        if want is not None:
            if self.unsure:
                if self.base is None:
                    self.set_org(want)
                else:
                    self.pc = want
            elif (want ^ self.pc) & 0x7FFFFF:
                self.errors.append(f"{self.where}: {full} placed at ${self.pc:06X}, symbols say ${want:06X}")
                self.pc = want
                if self.base is None:
                    self.off = lorom_offset(want)
            else:
                self.checked += 1
        self.emit("label", name=full, operand=self.global_label)

    # -- files and lines -------------------------------------------------------------
    def run(self, main: Path) -> None:
        self.include(main)

    def include(self, path: Path) -> None:
        saved = getattr(self, "cur_dir", self.root)
        self.cur_dir = path.parent
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        rel = path.relative_to(self.root) if path.is_relative_to(self.root) else path
        self.block([(f"{rel}:{i}", l) for i, l in enumerate(lines, 1)])
        self.cur_dir = saved

    def block(self, lines: list[tuple[str, str]]) -> None:
        """Process (where, text) lines: conditionals, loops, macro definitions."""
        cond: list[list[bool]] = []      # [active, taken, parent_active]
        i = 0
        n = len(lines)
        while i < n:
            where, raw = lines[i]
            i += 1
            self.where = where
            text = strip_comment(raw).strip()
            if not text:
                continue
            active = all(c[0] for c in cond)
            word = text.split(None, 1)[0].lower() if text else ""
            if word in ("if", "elseif", "else", "endif"):
                if word == "if":
                    if active:
                        v = self.cond_value(text[2:])
                        cond.append([v, v, True])
                    else:
                        cond.append([False, True, False])
                elif word == "elseif":
                    c = cond[-1]
                    if c[2] and not c[1]:
                        v = self.cond_value(text[6:])
                        c[0], c[1] = v, v
                    else:
                        c[0] = False
                elif word == "else":
                    c = cond[-1]
                    c[0] = c[2] and not c[1]
                    c[1] = True
                else:
                    cond.pop()
                continue
            if word in ("while", "macro"):
                end = "endwhile" if word == "while" else "endmacro"
                depth, j = 1, i
                while j < n:
                    w2 = strip_comment(lines[j][1]).strip().split(None, 1)
                    if w2 and w2[0].lower() == word:
                        depth += 1
                    elif w2 and w2[0].lower() == end:
                        depth -= 1
                        if depth == 0:
                            break
                    j += 1
                body = lines[i:j]
                i = j + 1
                if not active:
                    continue
                if word == "macro":
                    m = re.match(r"macro\s+([A-Za-z0-9_]+)\s*\((.*)\)\s*$", text)
                    if not m:
                        raise AsarError(f"{where}: bad macro header")
                    params = [p.strip() for p in m.group(2).split(",") if p.strip()]
                    self.macros[m.group(1)] = (params, body, str(self.cur_dir))
                else:
                    guard = 0
                    while self.cond_value(text[5:]):
                        self.block(body)
                        self.where = where
                        guard += 1
                        if guard > 1_000_000:
                            raise AsarError(f"{where}: runaway while")
                continue
            if not active:
                continue
            self.line(text)
        if cond:
            raise AsarError(f"{self.where}: unterminated if")

    def cond_value(self, text: str) -> bool:
        try:
            return bool(self.eval(self.expand_defines(text.strip())))
        except EvalError as e:
            raise AsarError(f"{self.where}: condition {text.strip()!r}: {e}")

    def line(self, text: str) -> None:
        m = DEFINE_SET_RE.match(text)
        if m:
            name, op, value = m.group(1), m.group(2), m.group(3).strip()
            if op == "=":
                self.defines[name] = value
            elif op == "?=":
                self.defines.setdefault(name, value)
            elif op == ":=":
                self.defines[name] = self.expand_defines(value)
            else:
                expr = self.expand_defines(value)
                if op == "+=":
                    expr = f"({self.defines[name]})+({expr})"
                try:
                    v = self.eval(expr)
                except EvalError as e:
                    raise AsarError(f"{self.where}: {text}: {e}")
                self.defines[name] = str(_int(v)) if float(v).is_integer() else repr(v)
            return
        if text.startswith("function "):
            m = re.match(r"function\s+([A-Za-z0-9_]+)\s*\(([^)]*)\)\s*=\s*(.*)$", text)
            if not m:
                raise AsarError(f"{self.where}: bad function")
            params = [p.strip() for p in m.group(2).split(",") if p.strip()]
            self.functions[m.group(1)] = (params, tokenize(self.expand_defines(m.group(3))))
            return
        text = self.expand_defines(text)
        for stmt in split_statements(text):
            self.statement(stmt)

    def statement(self, s: str) -> None:
        m = ANON_RE.match(s)
        if m and (m.group(2) is None or not m.group(2)[:1].isdigit()):
            self.emit("label", name=m.group(1), anon=True)
            if m.group(2):
                self.statement(m.group(2).strip())
            return
        m = LABEL_RE.match(s)
        if m:
            self.define_label(m.group(1))
            if m.group(2):
                self.statement(m.group(2).strip())
            return
        m = ASSIGN_RE.match(s)
        if m and m.group(1).lower() not in DIRECTIVES:
            return                                          # label = value: not a position
        if s.startswith("%"):
            self.macro_call(s)
            return
        word, _, rest = s.partition(" ")
        rest = rest.strip()
        # Directives are written in lower case (ORG excepted); REP, SEP and
        # the rest in capitals are instructions.
        lw = word.lower() if word.islower() or word == "ORG" else ""
        if lw in DATA_UNIT:
            self.data(lw, rest)
            return
        if lw == "incsrc":
            self.include(self.cur_dir / rest.strip('"'))
            return
        if lw == "incbin":
            self.incbin(rest)
            return
        if lw == "org":
            self.set_org(_int(self.eval(rest)))
            self.base = None
            return
        if lw == "base":
            if rest.lower() == "off":
                self.base = None
                if self.off is not None and not self.unsure:
                    self.pc = lorom_address(self.off)
            else:
                try:
                    self.base = self.pc = _int(self.eval(rest))
                except EvalError as e:
                    raise AsarError(f"{self.where}: base {rest}: {e}")
            return
        if lw == "arch":
            self.arch = rest.lower()
            return
        if lw == "skip":
            try:
                self.advance(_int(self.eval(rest)))
            except EvalError:
                self.go_unsure(f"skip {rest}")
            return
        if lw == "pushpc":
            self.pcstack.append((self.pc, self.off, self.base, self.unsure))
            return
        if lw == "pullpc":
            self.pc, self.off, self.base, self.unsure = self.pcstack.pop()
            return
        if lw == "error":
            raise AsarError(f"{self.where}: error directive reached")
        if lw == "assert":
            try:
                ok = self.eval(rest)
            except EvalError:
                return
            if not ok:
                raise AsarError(f"{self.where}: assert failed: {rest}")
            return
        if lw in DIRECTIVES:
            return                                          # lorom, asar, check, print, ...
        self.instruction(s)

    def macro_call(self, s: str) -> None:
        m = MACRO_CALL_RE.match(s)
        if not m:
            raise AsarError(f"{self.where}: bad macro call {s}")
        name = m.group(1)
        if name not in self.macros:
            raise AsarError(f"{self.where}: unknown macro {name}")
        params, body, _ = self.macros[name]
        args = split_top(m.group(2)) if m.group(2).strip() else []
        if len(args) != len(params):
            raise AsarError(f"{self.where}: {name}: {len(args)} arguments for {len(params)}")
        self.macro_serial += 1
        tag = self.macro_serial
        out = []
        for where, text in body:
            for p, a in zip(params, args):
                if a.startswith('"') and a.endswith('"'):
                    text = text.replace(f'"<{p}>"', a)     # a quoted argument in quotes
                text = text.replace(f"<{p}>", a)
            text = re.sub(r"\?([A-Za-z_][A-Za-z0-9_]*)", lambda mm: f"?m{tag}_{mm.group(1)}", text)
            out.append((f"{self.where}>{where}", text))
        caller = self.where
        self.block(out)
        self.where = caller

    def data(self, kind: str, rest: str) -> None:
        elems = split_top(rest)
        unit = DATA_UNIT[kind]
        size = 0
        for e in elems:
            if kind == "db" and e.startswith('"') and e.endswith('"'):
                size += len(e) - 2
            else:
                size += unit
        self.emit("data", name=kind, size=size, elems=elems)
        self.advance(size)

    def incbin(self, rest: str) -> None:
        m = re.match(r'"([^"]+)"(?::(\S+)-(\S+))?\s*$', rest)
        if not m:
            raise AsarError(f"{self.where}: incbin {rest}")
        path = self.cur_dir / m.group(1)
        if not path.exists():
            path = self.root / m.group(1)
        if not path.exists():
            self.emit("data", name="incbin", size=None, elems=[m.group(1)])
            self.go_unsure(f"missing {m.group(1)}")
            return
        size = path.stat().st_size
        if m.group(2):
            size = int(m.group(3), 16) - int(m.group(2), 16)
        self.emit("data", name="incbin", size=size, elems=[m.group(1)])
        self.advance(size)

    def instruction(self, s: str) -> None:
        m = INSN_RE.match(s)
        if not m:
            raise AsarError(f"{self.where}: cannot parse {s!r}")
        mnem, suffix, operand = m.group(1).upper(), m.group(2), (m.group(3) or "").strip()
        suffix = suffix.upper() if suffix else None
        if self.arch != "65816":
            self.emit("insn", name=mnem, suffix=suffix, operand=operand)
            self.go_unsure(f"{self.arch} code")
            return
        rep = re.fullmatch(r"#(\d+)", operand)
        if rep and not suffix and mnem in IMPLIED_REPEAT:
            for _ in range(int(rep.group(1))):
                self.emit("insn", name=mnem, size=1)
                self.advance(1)
            return
        if self.unsure or self.off is None:
            self.errors.append(f"{self.where}: instruction {s!r} at an unknown position")
            self.emit("insn", name=mnem, suffix=suffix, operand=operand)
            return
        size = self.insn_size(self, self.off, mnem, suffix, operand)
        if size is None:
            self.emit("insn", name=mnem, suffix=suffix, operand=operand)
            self.go_unsure(f"size of {s}")
            return
        self.emit("insn", name=mnem, suffix=suffix, operand=operand, size=size)
        self.advance(size)


IMPLIED_REPEAT = {"INX", "INY", "DEX", "DEY", "NOP", "INC", "DEC", "ASL", "LSR", "ROL", "ROR",
                  "CLC", "SEC", "XBA", "PHA", "PLA", "PHX", "PLX", "PHY", "PLY"}


def read_symbols(path: Path) -> dict[str, int]:
    """no$sns symbols from Asar: `BBAAAAAA name`, anonymous and macro labels skipped."""
    out = {}
    for line in path.read_text().splitlines():
        parts = line.split()
        if len(parts) != 2 or line.startswith(";") or parts[1].startswith(":"):
            continue
        out[parts[1]] = int(parts[0], 16)
    return out
