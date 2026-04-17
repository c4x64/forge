"""
CAT Lexer — Test Suite
tests/test_lexer.py

Covers every token kind and every error code E001–E009.
Run from the repo root:  python tests/test_lexer.py
"""

import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from lexer import Lexer, TK

PASS = "\033[38;2;166;227;161m✔\033[0m"
FAIL = "\033[38;2;243;139;168m✘\033[0m"
BOLD = "\033[1m"
RESET = "\033[0m"
CYAN = "\033[38;2;137;220;235m"


passed = 0
failed = 0
_suite = None

def suite(name: str):
    global _suite
    _suite = name
    print(f"\n{BOLD}{CYAN}{name}{RESET}")

def check(label: str, cond: bool, detail: str = "") -> None:
    global passed, failed
    if cond:
        print(f"  {PASS} {label}")
        passed += 1
    else:
        print(f"  {FAIL} {label}" + (f"  — {detail}" if detail else ""))
        failed += 1

def lex(src: str):
    l = Lexer(src, "<test>")
    l.tokenize()
    return l

def kinds(l: Lexer):
    return [t.kind for t in l.tokens]

def lexemes(l: Lexer):
    return [t.lexeme for t in l.tokens]


# ─── 1. Keywords ─────────────────────────────────────────────────────────────

suite("1. Keywords")
kw_src = "fn type const global local import export if else while for return break continue true false null"
l = lex(kw_src)
expected_kws = [
    TK.Fn, TK.Type, TK.Const, TK.Global, TK.Local,
    TK.Import, TK.Export, TK.If, TK.Else, TK.While,
    TK.For, TK.Return, TK.Break, TK.Continue,
    TK.True_, TK.False_, TK.Null, TK.Eof,
]
check("all 17 keywords tokenized", kinds(l) == expected_kws,
      f"got {[k.name for k in kinds(l)]}")

# ─── 2. Built-in types ───────────────────────────────────────────────────────

suite("2. Built-in types")
ty_src = "i8 i16 i32 i64 i128 u8 u16 u32 u64 u128 f32 f64 bool void ptr"
l = lex(ty_src)
expected_types = [
    TK.TyI8, TK.TyI16, TK.TyI32, TK.TyI64, TK.TyI128,
    TK.TyU8, TK.TyU16, TK.TyU32, TK.TyU64, TK.TyU128,
    TK.TyF32, TK.TyF64, TK.TyBool, TK.TyVoid, TK.TyPtr,
    TK.Eof,
]
check("all 15 built-in types tokenized", kinds(l) == expected_types)

# ─── 3. Instructions ─────────────────────────────────────────────────────────

suite("3. Instructions")
instr_src = "mov lea add sub mul div mod neg and orr xor not shl shr sar rol ror cmp ceq cne clt cle cgt cge jmp je jne jl jle jg jge jz jnz call ret sel"
l = lex(instr_src)
# Just check they're not Ident
non_eof = [t for t in l.tokens if t.kind != TK.Eof]
check("instructions not lexed as Ident",
      all(t.kind != TK.Ident for t in non_eof))
check("correct count (36 instructions)", len(non_eof) == 36, f"got {len(non_eof)}")

# ─── 4. Directives ───────────────────────────────────────────────────────────

suite("4. Directives")
l = lex("@inline @extern @atomic_cas @unknown_directive")
toks = [t for t in l.tokens if t.kind != TK.Eof]
check("all four lexed as Directive", all(t.kind == TK.Directive for t in toks))
check("lexemes preserve '@' prefix",  all(t.lexeme.startswith("@") for t in toks))

# ─── 5. Flag ─────────────────────────────────────────────────────────────────

suite("5. Flag &c4")
l = lex("&c4")
check("&c4 lexed as Flag", l.tokens[0].kind == TK.Flag)
check("lexeme is '&c4'",   l.tokens[0].lexeme == "&c4")

# ─── 6. Symbols ──────────────────────────────────────────────────────────────

suite("6. Symbols")
sym_src = "( ) { } [ ] : ; , . -> := = == != <= >= < > << >> + - * / % & | ^ ~ ! && ||"
l = lex(sym_src)
toks = [t for t in l.tokens if t.kind != TK.Eof]
expected_sym_kinds = [
    TK.LParen, TK.RParen, TK.LBrace, TK.RBrace, TK.LBracket, TK.RBracket,
    TK.Colon, TK.Semi, TK.Comma, TK.Dot, TK.Arrow, TK.Walrus,
    TK.Assign, TK.Eq, TK.Ne, TK.Le, TK.Ge, TK.Lt, TK.Gt,
    TK.ShlSym, TK.ShrSym, TK.Plus, TK.Minus, TK.Star, TK.Slash,
    TK.Percent, TK.Amp, TK.Pipe, TK.Caret, TK.Tilde, TK.Bang,
    TK.LogAnd, TK.LogOr,
]
check("all 33 symbols tokenized", [t.kind for t in toks] == expected_sym_kinds,
      f"got {[t.kind.name for t in toks]}")

# ─── 7. Integer literals ─────────────────────────────────────────────────────

suite("7. Integer literals")

l = lex("42")
check("decimal: value", l.tokens[0].int_val == 42)

l = lex("1_000_000")
check("decimal with underscores: value", l.tokens[0].int_val == 1_000_000)

l = lex("0xFF")
check("hex: value", l.tokens[0].int_val == 255)

l = lex("0b1010")
check("binary: value", l.tokens[0].int_val == 0b1010)

l = lex("0o755")
check("octal: value", l.tokens[0].int_val == 0o755)

l = lex("255u8")
check("suffix u8 stored", l.tokens[0].suffix == "u8")
check("suffix u8 value",  l.tokens[0].int_val == 255)

l = lex("42i32")
check("suffix i32 stored", l.tokens[0].suffix == "i32")

l = lex("0xDEAD_BEEFu32")
check("hex with underscores + suffix", l.tokens[0].int_val == 0xDEADBEEF)

# ─── 8. Float literals ───────────────────────────────────────────────────────

suite("8. Float literals")

l = lex("3.14")
check("float 3.14", l.tokens[0].kind == TK.FloatLit)
check("float value ≈ 3.14", abs(l.tokens[0].float_val - 3.14) < 1e-9)

l = lex("2.5e10")
check("float with exponent", l.tokens[0].kind == TK.FloatLit)

l = lex("3.14f32")
check("float f32 suffix", l.tokens[0].suffix == "f32")

l = lex("2.5f64")
check("float f64 suffix", l.tokens[0].suffix == "f64")

# ─── 9. String literals ──────────────────────────────────────────────────────

suite("9. String literals")

l = lex('"hello"')
check("simple string", l.tokens[0].kind == TK.StrLit)
check("string value",  l.tokens[0].str_val == "hello")

l = lex('"line\\nnewline"')
check("\\n escape",    l.tokens[0].str_val == "line\nnewline")

l = lex('"tab\\there"')
check("\\t escape",    l.tokens[0].str_val == "tab\there")

l = lex('"hex\\x41"')
check("\\x41 == 'A'",  l.tokens[0].str_val == "hexA")

l = lex('"cat\\u{1F408}"')
check("\\u{1F408} == 🐈", l.tokens[0].str_val == "cat🐈")

# ─── 10. Char literals ───────────────────────────────────────────────────────

suite("10. Char literals")

l = lex("'A'")
check("char 'A'",      l.tokens[0].kind == TK.CharLit)
check("char value 'A'",l.tokens[0].str_val == "A")

l = lex(r"'\n'")
check("char \\n",      l.tokens[0].str_val == "\n")

l = lex(r"'\u{2665}'")
check("char \\u{2665} == ♥", l.tokens[0].str_val == "♥")

# ─── 11. Comments ────────────────────────────────────────────────────────────

suite("11. Comments")

l = lex("42 # this is a comment\n99")
toks = [t for t in l.tokens if t.kind != TK.Eof]
check("line comment stripped", len(toks) == 2)
check("line comment: tokens are 42 and 99",
      toks[0].int_val == 42 and toks[1].int_val == 99)

l = lex("/* block */ 42")
toks = [t for t in l.tokens if t.kind != TK.Eof]
check("block comment stripped", len(toks) == 1 and toks[0].int_val == 42)

l = lex("/* outer /* inner */ outer */ 7")
toks = [t for t in l.tokens if t.kind != TK.Eof]
check("nested block comment", len(toks) == 1 and toks[0].int_val == 7)

# ─── 12. Source positions ────────────────────────────────────────────────────

suite("12. Source positions")

l = lex("fn\nhello\n  world")
toks = [t for t in l.tokens if t.kind != TK.Eof]
check("token 'fn' at line 1", toks[0].span.line == 1)
check("token 'hello' at line 2", toks[1].span.line == 2)
check("token 'world' at line 3, col 3", toks[2].span.line == 3 and toks[2].span.col == 3)

# ─── 13. Error recovery (all error codes) ────────────────────────────────────

suite("13. Error recovery — E001–E009")

# E001: unexpected character
l = lex("fn $ hello")
check("E001: unexpected '$'",
      any(e.code == "E001" for e in l.errors))
check("E001: lexing continues after bad char",
      any(t.kind == TK.Ident and t.lexeme == "hello" for t in l.tokens))

# E002: unterminated string
l = lex('"not closed')
check("E002: unterminated string", any(e.code == "E002" for e in l.errors))

# E003: unterminated block comment
l = lex("/* never closed")
check("E003: unterminated block comment", any(e.code == "E003" for e in l.errors))

# E004: invalid number format
l = lex("0b")  # binary with no digits
check("E004: 0b with no digits", any(e.code == "E004" for e in l.errors))

l = lex("0x")  # hex with no digits
check("E004: 0x with no digits", any(e.code == "E004" for e in l.errors))

# E005: integer overflow
very_big = "1" + "0" * 40   # >> i128
l = lex(very_big)
check("E005: integer overflow", any(e.code == "E005" for e in l.errors))

# E006: invalid escape
l = lex('"\\q"')
check("E006: invalid escape \\q", any(e.code == "E006" for e in l.errors))

# E007: invalid unicode codepoint
l = lex('"\\u{FFFFFF1}"')  # > 0x10FFFF
check("E007: invalid unicode codepoint", any(e.code == "E007" for e in l.errors))

# E008: empty char literal
l = lex("''")
check("E008: empty char literal", any(e.code == "E008" for e in l.errors))

# E009: multi-char char literal
l = lex("'ab'")
check("E009: multi-char literal", any(e.code == "E009" for e in l.errors))

# ─── 14. Identifier vs keyword ───────────────────────────────────────────────

suite("14. Identifier disambiguation")

l = lex("fno  typecheck  returns")
toks = [t for t in l.tokens if t.kind != TK.Eof]
check("'fno' is Ident (not keyword)",      toks[0].kind == TK.Ident)
check("'typecheck' is Ident",              toks[1].kind == TK.Ident)
check("'returns' is Ident (not 'return')", toks[2].kind == TK.Ident)

# ─── 15. Real-world snippet ──────────────────────────────────────────────────

suite("15. Real-world snippet")

snippet = """
fn fibonacci(n: u32) -> u64 {
    # Base cases
    if n == 0 { return 0u64 }
    if n == 1 { return 1u64 }
    local a: u64 = 0u64
    local b: u64 = 1u64
    local i: u32 = 2u32
    while i <= n {
        local tmp: u64 = a + b
        a = b
        b = tmp
        i = i + 1u32
    }
    return b
}
"""
l = lex(snippet)
check("snippet: no errors", l.errors == [],
      str([e.message for e in l.errors]))
non_eof_toks = [t for t in l.tokens if t.kind != TK.Eof]
check("snippet: at least 50 tokens", len(non_eof_toks) >= 50,
      f"got {len(non_eof_toks)}")
check("snippet: 'fn' present",     any(t.kind == TK.Fn for t in l.tokens))
check("snippet: 'while' present",  any(t.kind == TK.While for t in l.tokens))
check("snippet: 'u32' type present", any(t.kind == TK.TyU32 for t in l.tokens))


# ─── Summary ─────────────────────────────────────────────────────────────────

total = passed + failed
print(f"\n{'─'*40}")
print(f"Results: {BOLD}{passed}/{total}{RESET} passed", end="")
if failed:
    print(f"  (\033[38;2;243;139;168m{failed} failed\033[0m)")
else:
    print(f"  \033[38;2;166;227;161m✔ all tests pass\033[0m")
sys.exit(0 if failed == 0 else 1)
