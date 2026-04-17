"""
CAT Language Lexer — Phase 1
src/lexer.py

Bootstrap implementation of the CAT lexer in Python.
Targets spec v1.0.0 as defined in docs/LEXER_SPEC.md.

Design goals:
  - Full spec compliance: all token kinds, all error codes E001–E009
  - Error recovery: collect all errors, never abort on bad input
  - Catppuccin-colored diagnostics with source context and caret
  - Zero-allocation style: tokens reference source slices
"""

from __future__ import annotations
import sys
import enum
import json
from dataclasses import dataclass, field
from typing import List, Optional, Tuple


# ─── ANSI / Catppuccin palette ────────────────────────────────────────────────

RESET  = "\033[0m"
BOLD   = "\033[1m"
RED    = "\033[38;2;243;139;168m"   # Catppuccin Red   (error)
YELLOW = "\033[38;2;249;226;175m"   # Catppuccin Yellow (warning)
BLUE   = "\033[38;2;137;180;250m"   # Catppuccin Blue  (info)
CYAN   = "\033[38;2;137;220;235m"   # Catppuccin Sky
GREEN  = "\033[38;2;166;227;161m"   # Catppuccin Green
GRAY   = "\033[38;2;108;112;134m"   # Catppuccin Overlay0
PINK   = "\033[38;2;245;194;231m"   # Catppuccin Pink


# ─── Token kinds ──────────────────────────────────────────────────────────────

class TK(enum.Enum):
    # Keywords
    Fn = "fn"; Type = "type"; Const = "const"; Global = "global"; Local = "local"
    Import = "import"; Export = "export"; If = "if"; Else = "else"
    While = "while"; For = "for"; Return = "return"; Break = "break"
    Continue = "continue"; True_ = "true"; False_ = "false"; Null = "null"

    # Built-in types
    TyI8="i8"; TyI16="i16"; TyI32="i32"; TyI64="i64"; TyI128="i128"
    TyU8="u8"; TyU16="u16"; TyU32="u32"; TyU64="u64"; TyU128="u128"
    TyF32="f32"; TyF64="f64"; TyBool="bool"; TyVoid="void"; TyPtr="ptr"

    # Instructions
    Mov="mov"; Lea="lea"; Add="add"; Sub="sub"; Mul="mul"; Div="div"
    Mod="mod"; Neg="neg"; And="and"; Orr="orr"; Xor="xor"; Not="not"
    Shl="shl"; Shr="shr"; Sar="sar"; Rol="rol"; Ror="ror"
    Cmp="cmp"; Ceq="ceq"; Cne="cne"; Clt="clt"; Cle="cle"; Cgt="cgt"; Cge="cge"
    Jmp="jmp"; Je="je"; Jne="jne"; Jl="jl"; Jle="jle"; Jg="jg"; Jge="jge"
    Jz="jz"; Jnz="jnz"; Call="call"; Ret="ret"; Sel="sel"

    # Literals / identifiers
    IntLit   = "<int>"
    FloatLit = "<float>"
    StrLit   = "<string>"
    CharLit  = "<char>"
    Ident    = "<ident>"

    # Symbols
    LParen="("; RParen=")"; LBrace="{"; RBrace="}"; LBracket="["; RBracket="]"
    Colon=":"; Semi=";"; Comma=","; Dot="."; Arrow="->"; Walrus=":="
    Assign="="; Eq="=="; Ne="!="; Le="<="; Ge=">="; Lt="<"; Gt=">"
    ShlSym="<<"; ShrSym=">>"; Plus="+"; Minus="-"; Star="*"; Slash="/"
    Percent="%"; Amp="&"; Pipe="|"; Caret="^"; Tilde="~"; Bang="!"
    LogAnd="&&"; LogOr="||"

    # Special
    Directive = "<directive>"
    Flag      = "&c4"
    Eof       = "<eof>"
    Error     = "<error>"


# Keyword / type / instruction lookup table
_KEYWORD_MAP: dict[str, TK] = {
    "fn": TK.Fn, "type": TK.Type, "const": TK.Const, "global": TK.Global,
    "local": TK.Local, "import": TK.Import, "export": TK.Export,
    "if": TK.If, "else": TK.Else, "while": TK.While, "for": TK.For,
    "return": TK.Return, "break": TK.Break, "continue": TK.Continue,
    "true": TK.True_, "false": TK.False_, "null": TK.Null,
    # types
    "i8": TK.TyI8, "i16": TK.TyI16, "i32": TK.TyI32, "i64": TK.TyI64, "i128": TK.TyI128,
    "u8": TK.TyU8, "u16": TK.TyU16, "u32": TK.TyU32, "u64": TK.TyU64, "u128": TK.TyU128,
    "f32": TK.TyF32, "f64": TK.TyF64, "bool": TK.TyBool, "void": TK.TyVoid, "ptr": TK.TyPtr,
    # instructions
    "mov": TK.Mov, "lea": TK.Lea, "add": TK.Add, "sub": TK.Sub, "mul": TK.Mul,
    "div": TK.Div, "mod": TK.Mod, "neg": TK.Neg, "and": TK.And, "orr": TK.Orr,
    "xor": TK.Xor, "not": TK.Not, "shl": TK.Shl, "shr": TK.Shr, "sar": TK.Sar,
    "rol": TK.Rol, "ror": TK.Ror, "cmp": TK.Cmp, "ceq": TK.Ceq, "cne": TK.Cne,
    "clt": TK.Clt, "cle": TK.Cle, "cgt": TK.Cgt, "cge": TK.Cge,
    "jmp": TK.Jmp, "je": TK.Je, "jne": TK.Jne, "jl": TK.Jl, "jle": TK.Jle,
    "jg": TK.Jg, "jge": TK.Jge, "jz": TK.Jz, "jnz": TK.Jnz,
    "call": TK.Call, "ret": TK.Ret, "sel": TK.Sel,
}

_TWO_CHAR_SYMBOLS: dict[str, TK] = {
    "->": TK.Arrow, ":=": TK.Walrus, "==": TK.Eq, "!=": TK.Ne,
    "<=": TK.Le, ">=": TK.Ge, "<<": TK.ShlSym, ">>": TK.ShrSym,
    "&&": TK.LogAnd, "||": TK.LogOr,
}

_ONE_CHAR_SYMBOLS: dict[str, TK] = {
    "(": TK.LParen, ")": TK.RParen, "{": TK.LBrace, "}": TK.RBrace,
    "[": TK.LBracket, "]": TK.RBracket, ":": TK.Colon, ";": TK.Semi,
    ",": TK.Comma, ".": TK.Dot, "=": TK.Assign, "<": TK.Lt, ">": TK.Gt,
    "+": TK.Plus, "-": TK.Minus, "*": TK.Star, "/": TK.Slash,
    "%": TK.Percent, "&": TK.Amp, "|": TK.Pipe, "^": TK.Caret,
    "~": TK.Tilde, "!": TK.Bang,
}


# ─── Span / Token / LexError ─────────────────────────────────────────────────

@dataclass
class Span:
    line: int   # 1-based
    col:  int   # 1-based
    offset: int # byte offset in source

@dataclass
class Token:
    kind:   TK
    lexeme: str
    span:   Span
    # Extra parsed value for literals
    int_val:   Optional[int]   = field(default=None, repr=False)
    float_val: Optional[float] = field(default=None, repr=False)
    str_val:   Optional[str]   = field(default=None, repr=False)
    suffix:    Optional[str]   = field(default=None, repr=False)  # "u32", "f64", …

@dataclass
class LexError:
    code:    str   # E001 … E009
    message: str
    span:    Span
    note:    str = ""


# ─── Lexer ───────────────────────────────────────────────────────────────────

class Lexer:
    def __init__(self, src: str, filename: str = "<stdin>"):
        self.src      = src
        self.filename = filename
        self.pos      = 0
        self.line     = 1
        self.col      = 1
        self.tokens:  List[Token]    = []
        self.errors:  List[LexError] = []

    # ── helpers ──────────────────────────────────────────────────────────────

    def _peek(self, offset: int = 0) -> str:
        p = self.pos + offset
        return self.src[p] if p < len(self.src) else "\0"

    def _advance(self) -> str:
        ch = self.src[self.pos]
        self.pos += 1
        if ch == "\n":
            self.line += 1; self.col = 1
        else:
            self.col += 1
        return ch

    def _span(self) -> Span:
        return Span(self.line, self.col, self.pos)

    def _emit(self, kind: TK, lexeme: str, span: Span, **kwargs) -> Token:
        t = Token(kind=kind, lexeme=lexeme, span=span, **kwargs)
        self.tokens.append(t)
        return t

    def _error(self, code: str, msg: str, span: Span, note: str = "") -> None:
        self.errors.append(LexError(code=code, message=msg, span=span, note=note))

    # ── main entry ───────────────────────────────────────────────────────────

    def tokenize(self) -> List[Token]:
        while self.pos < len(self.src):
            self._scan_token()
        self._emit(TK.Eof, "", self._span())
        return self.tokens

    def _scan_token(self) -> None:
        # Skip whitespace
        while self.pos < len(self.src) and self.src[self.pos] in " \t\r\n":
            self._advance()
        if self.pos >= len(self.src):
            return

        span = self._span()
        ch   = self._peek()

        # ── line comment ──────────────────────────────────────────────────
        if ch == "#":
            while self.pos < len(self.src) and self._peek() != "\n":
                self._advance()
            return

        # ── block comment (nested) ────────────────────────────────────────
        if ch == "/" and self._peek(1) == "*":
            self._scan_block_comment(span)
            return

        # ── directive @ident ──────────────────────────────────────────────
        if ch == "@":
            self._advance()  # consume '@'
            name = self._read_ident()
            self._emit(TK.Directive, "@" + name, span)
            return

        # ── flag &c4 ──────────────────────────────────────────────────────
        if ch == "&" and self._peek(1) == "c" and self._peek(2) == "4":
            self._advance(); self._advance(); self._advance()
            self._emit(TK.Flag, "&c4", span)
            return

        # ── string literal ────────────────────────────────────────────────
        if ch == '"':
            self._scan_string(span)
            return

        # ── char literal ─────────────────────────────────────────────────
        if ch == "'":
            self._scan_char(span)
            return

        # ── number literals ───────────────────────────────────────────────
        if ch.isdigit() or (ch == "." and self._peek(1).isdigit()):
            self._scan_number(span)
            return

        # ── identifier or keyword ─────────────────────────────────────────
        if ch.isalpha() or ch == "_":
            name = self._read_ident()
            kind = _KEYWORD_MAP.get(name, TK.Ident)
            self._emit(kind, name, span)
            return

        # ── two-char symbols ──────────────────────────────────────────────
        two = ch + self._peek(1)
        if two in _TWO_CHAR_SYMBOLS:
            self._advance(); self._advance()
            self._emit(_TWO_CHAR_SYMBOLS[two], two, span)
            return

        # ── one-char symbols ──────────────────────────────────────────────
        if ch in _ONE_CHAR_SYMBOLS:
            self._advance()
            self._emit(_ONE_CHAR_SYMBOLS[ch], ch, span)
            return

        # ── unexpected character ──────────────────────────────────────────
        self._advance()
        self._error("E001", f"Unexpected character {ch!r}", span,
                    note="Remove this character")
        self._emit(TK.Error, ch, span)

    # ── block comment ────────────────────────────────────────────────────────

    def _scan_block_comment(self, span: Span) -> None:
        self._advance(); self._advance()  # consume '/*'
        depth = 1
        while self.pos < len(self.src):
            c = self._peek()
            n = self._peek(1)
            if c == "/" and n == "*":
                self._advance(); self._advance(); depth += 1
            elif c == "*" and n == "/":
                self._advance(); self._advance(); depth -= 1
                if depth == 0:
                    return
            else:
                self._advance()
        # EOF without closing
        self._error("E003", "Unterminated block comment", span,
                    note="Add closing '*/' to end the comment")

    # ── identifier ──────────────────────────────────────────────────────────

    def _read_ident(self) -> str:
        start = self.pos
        while self.pos < len(self.src) and (self._peek().isalnum() or self._peek() == "_"):
            self._advance()
        return self.src[start:self.pos]

    # ── number ──────────────────────────────────────────────────────────────

    def _scan_number(self, span: Span) -> None:
        ch = self._peek()

        # Determine base
        if ch == "0" and self._peek(1) in ("x", "X"):
            self._advance(); self._advance()
            self._scan_int_digits(span, base=16, base_name="hexadecimal",
                                  valid=lambda c: c in "0123456789abcdefABCDEF_")
            return
        if ch == "0" and self._peek(1) in ("b", "B"):
            self._advance(); self._advance()
            self._scan_int_digits(span, base=2, base_name="binary",
                                  valid=lambda c: c in "01_")
            return
        if ch == "0" and self._peek(1) in ("o", "O"):
            self._advance(); self._advance()
            self._scan_int_digits(span, base=8, base_name="octal",
                                  valid=lambda c: c in "01234567_")
            return

        # Decimal — may be float
        start = self.pos
        while self.pos < len(self.src) and (self._peek().isdigit() or self._peek() == "_"):
            self._advance()

        is_float = False
        # Fractional part
        if self._peek() == "." and self._peek(1).isdigit():
            is_float = True
            self._advance()  # '.'
            while self.pos < len(self.src) and (self._peek().isdigit() or self._peek() == "_"):
                self._advance()
        # Exponent
        if self._peek() in ("e", "E"):
            is_float = True
            self._advance()
            if self._peek() in ("+", "-"):
                self._advance()
            if not self._peek().isdigit():
                self._error("E004", "Invalid number format: expected exponent digits", span)
            while self.pos < len(self.src) and self._peek().isdigit():
                self._advance()

        raw = self.src[start:self.pos]

        if is_float:
            suffix = self._read_float_suffix()
            try:
                val = float(raw.replace("_", ""))
            except ValueError:
                self._error("E004", f"Invalid float literal {raw!r}", span)
                self._emit(TK.Error, raw, span)
                return
            self._emit(TK.FloatLit, raw + (suffix or ""), span,
                       float_val=val, suffix=suffix)
        else:
            suffix = self._read_int_suffix()
            clean = raw.replace("_", "")
            if not clean:
                self._error("E004", "Invalid decimal literal", span)
                self._emit(TK.Error, raw, span)
                return
            val = int(clean)
            if val > (1 << 127) - 1:
                self._error("E005", f"Integer overflow: {val} exceeds i128 range", span)
            self._emit(TK.IntLit, raw + (suffix or ""), span,
                       int_val=val, suffix=suffix)

    def _scan_int_digits(self, span: Span, base: int, base_name: str,
                          valid) -> None:
        start = self.pos
        while self.pos < len(self.src) and valid(self._peek()):
            self._advance()
        raw = self.src[start:self.pos]
        clean = raw.replace("_", "")
        if not clean:
            self._error("E004", f"Invalid {base_name} literal: no digits after prefix", span)
            self._emit(TK.Error, raw, span)
            return
        try:
            val = int(clean, base)
        except ValueError:
            self._error("E004", f"Invalid {base_name} literal {clean!r}", span)
            self._emit(TK.Error, raw, span)
            return
        if val > (1 << 127) - 1:
            self._error("E005", f"Integer overflow", span)
        suffix = self._read_int_suffix()
        self._emit(TK.IntLit, raw + (suffix or ""), span,
                   int_val=val, suffix=suffix)

    def _read_int_suffix(self) -> Optional[str]:
        start = self.pos
        if self._peek() in ("i", "u"):
            self._advance()
            while self.pos < len(self.src) and self._peek().isdigit():
                self._advance()
            return self.src[start:self.pos]
        return None

    def _read_float_suffix(self) -> Optional[str]:
        rest = self.src[self.pos:]
        for s in ("f32", "f64"):
            if rest.startswith(s) and not (len(rest) > len(s) and (rest[len(s)].isalnum() or rest[len(s)] == "_")):
                for _ in s:
                    self._advance()
                return s
        return None

    # ── string ──────────────────────────────────────────────────────────────

    def _scan_string(self, span: Span) -> None:
        self._advance()  # consume opening '"'
        chars: List[str] = []
        while self.pos < len(self.src):
            ch = self._peek()
            if ch == '"':
                self._advance()
                self._emit(TK.StrLit,
                           self.src[span.offset:self.pos],
                           span, str_val="".join(chars))
                return
            if ch == "\n":
                break  # treat as unterminated
            if ch == "\\":
                c, ok = self._scan_escape(span)
                if ok:
                    chars.append(c)
            else:
                chars.append(ch)
                self._advance()
        self._error("E002", "Unterminated string literal", span,
                    note="Add closing '\"' to end the string")
        self._emit(TK.Error, self.src[span.offset:self.pos], span)

    # ── char literal ────────────────────────────────────────────────────────

    def _scan_char(self, span: Span) -> None:
        self._advance()  # consume opening "'"
        if self._peek() == "'":
            self._advance()
            self._error("E008", "Empty character literal", span,
                        note="Character literals must contain exactly one character")
            self._emit(TK.Error, "''", span)
            return

        if self._peek() == "\\":
            ch, ok = self._scan_escape(span)
        else:
            ch = self._peek()
            ok = True
            self._advance()

        if self._peek() != "'":
            # multi-char or unterminated
            while self.pos < len(self.src) and self._peek() not in ("'", "\n"):
                self._advance()
            if self._peek() == "'":
                self._advance()
            self._error("E009", "Multi-character character literal", span,
                        note="Character literals must contain exactly one character")
            self._emit(TK.Error, self.src[span.offset:self.pos], span)
            return

        self._advance()  # closing "'"
        self._emit(TK.CharLit, self.src[span.offset:self.pos], span,
                   str_val=ch)

    # ── escape sequences ────────────────────────────────────────────────────

    def _scan_escape(self, span: Span) -> Tuple[str, bool]:
        self._advance()  # consume '\'
        if self.pos >= len(self.src):
            self._error("E006", "Unexpected end of input in escape sequence", span)
            return ("", False)
        c = self._peek()
        simple = {"n": "\n", "r": "\r", "t": "\t", "\\": "\\",
                  '"': '"', "'": "'", "0": "\0"}
        if c in simple:
            self._advance()
            return (simple[c], True)
        if c == "x":
            self._advance()
            hex_str = ""
            for _ in range(2):
                if self._peek() in "0123456789abcdefABCDEF":
                    hex_str += self._peek(); self._advance()
                else:
                    self._error("E006", f"Invalid \\x escape: expected 2 hex digits", span)
                    return ("", False)
            return (chr(int(hex_str, 16)), True)
        if c == "u":
            self._advance()
            if self._peek() != "{":
                self._error("E006", "Expected '{' after \\u", span)
                return ("", False)
            self._advance()
            hex_str = ""
            while self._peek() in "0123456789abcdefABCDEF":
                hex_str += self._peek(); self._advance()
                if len(hex_str) > 6:
                    break
            if self._peek() != "}":
                self._error("E006", "Expected '}' to close \\u{...}", span)
                return ("", False)
            self._advance()
            if not hex_str:
                self._error("E006", "Empty \\u{} escape", span)
                return ("", False)
            cp = int(hex_str, 16)
            if cp > 0x10FFFF:
                self._error("E007", f"Unicode codepoint 0x{cp:X} exceeds U+10FFFF", span)
                return ("", False)
            return (chr(cp), True)
        # Unknown escape
        self._error("E006", f"Invalid escape sequence '\\{c}'", span,
                    note="Valid: \\n \\r \\t \\\\ \\\" \\' \\0 \\xHH \\u{{...}}")
        self._advance()
        return ("", False)


# ─── Diagnostic renderer ──────────────────────────────────────────────────────

def render_errors(errors: List[LexError], src: str, filename: str) -> None:
    lines = src.splitlines()
    for err in errors:
        ln  = err.span.line
        col = err.span.col
        src_line = lines[ln - 1] if ln <= len(lines) else ""
        caret = " " * (col - 1) + "^"

        print(f"{BOLD}{RED}error[{err.code}]{RESET}{BOLD}: {err.message}{RESET}", file=sys.stderr)
        print(f"  {BLUE}-->{RESET} {filename}:{ln}:{col}", file=sys.stderr)
        print(f"   {BLUE}|{RESET}", file=sys.stderr)
        print(f"{GRAY}{ln:3}{RESET} {BLUE}|{RESET} {src_line}", file=sys.stderr)
        print(f"   {BLUE}|{RESET} {RED}{caret}{RESET}", file=sys.stderr)
        if err.note:
            print(f"   {BLUE}|{RESET}", file=sys.stderr)
            print(f"   {GREEN}= help:{RESET} {err.note}", file=sys.stderr)
        print(file=sys.stderr)


# ─── CLI ─────────────────────────────────────────────────────────────────────

def _token_repr(t: Token) -> dict:
    d: dict = {"kind": t.kind.name, "lexeme": t.lexeme,
               "line": t.span.line, "col": t.span.col}
    if t.int_val   is not None: d["int_val"]   = t.int_val
    if t.float_val is not None: d["float_val"] = t.float_val
    if t.str_val   is not None: d["str_val"]   = t.str_val
    if t.suffix    is not None: d["suffix"]    = t.suffix
    return d

def main() -> None:
    import argparse
    ap = argparse.ArgumentParser(
        description="CAT Lexer — Phase 1 Bootstrap",
        epilog="Output: one JSON array of token objects, or pretty-printed table."
    )
    ap.add_argument("file", nargs="?", help="Source file (.cat). Reads stdin if omitted.")
    ap.add_argument("--json",  action="store_true", help="Emit token stream as JSON")
    ap.add_argument("--table", action="store_true", help="Emit tokens as ASCII table (default)")
    ap.add_argument("--stats", action="store_true", help="Print token-kind histogram")
    args = ap.parse_args()

    if args.file:
        try:
            with open(args.file, "r", encoding="utf-8") as fh:
                src = fh.read()
            filename = args.file
        except OSError as e:
            print(f"{RED}error{RESET}: {e}", file=sys.stderr)
            sys.exit(1)
    else:
        src = sys.stdin.read()
        filename = "<stdin>"

    lexer  = Lexer(src, filename)
    tokens = lexer.tokenize()

    if lexer.errors:
        render_errors(lexer.errors, src, filename)

    if args.json:
        print(json.dumps([_token_repr(t) for t in tokens], indent=2, ensure_ascii=False))
    elif args.stats:
        from collections import Counter
        counts = Counter(t.kind.name for t in tokens)
        print(f"\n{BOLD}Token histogram{RESET} — {filename}")
        for kind, n in counts.most_common():
            bar = "█" * min(n, 40)
            print(f"  {CYAN}{kind:<16}{RESET}  {GREEN}{bar}{RESET}  {n}")
    else:
        # Default: pretty table
        col_widths = (5, 16, 24, 6, 5)
        header = f"{'#':<{col_widths[0]}} {'Kind':<{col_widths[1]}} {'Lexeme':<{col_widths[2]}} {'Line':<{col_widths[3]}} {'Col':<{col_widths[4]}}"
        sep    = "─" * sum(col_widths + (len(col_widths)-1,))
        print(f"\n{BOLD}{CYAN}Tokens{RESET} — {PINK}{filename}{RESET}")
        print(GRAY + sep + RESET)
        print(BOLD + header + RESET)
        print(GRAY + sep + RESET)
        for i, t in enumerate(tokens):
            lex = repr(t.lexeme) if t.kind in (TK.StrLit, TK.CharLit) else t.lexeme
            lex = lex[:22] + "…" if len(lex) > 23 else lex
            kind_color = {
                TK.Error: RED,
                TK.Eof:   GRAY,
                TK.Ident: PINK,
                TK.IntLit: YELLOW, TK.FloatLit: YELLOW,
                TK.StrLit: GREEN,  TK.CharLit:  GREEN,
                TK.Directive: CYAN,
            }.get(t.kind, BLUE if t.kind.name.startswith("Ty") else RESET)
            print(f"{GRAY}{i:<{col_widths[0]}}{RESET}"
                  f" {kind_color}{t.kind.name:<{col_widths[1]}}{RESET}"
                  f" {lex:<{col_widths[2]}}"
                  f" {t.span.line:<{col_widths[3]}}"
                  f" {t.span.col:<{col_widths[4]}}")

    exit_code = 1 if lexer.errors else 0
    if lexer.errors:
        plural = "error" if len(lexer.errors) == 1 else "errors"
        print(f"\n{RED}{BOLD}aborting due to {len(lexer.errors)} {plural}{RESET}", file=sys.stderr)

    sys.exit(exit_code)


if __name__ == "__main__":
    main()
