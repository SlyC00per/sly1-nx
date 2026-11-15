# SPDX-License-Identifier: AGPL-3.0-or-later
"""Write the decompilation's game code again for the port (docs/DESIGN.md, section 3).

    venv/bin/python tools/gen.py --out build-host/gen src/main/heap.cpp ...   # given units
    venv/bin/python tools/gen.py --out build-host/gen --all                   # every unit

The decompilation (sly1-decomp/) is never changed: its include/ and src/main/ are copied under
--out and rewritten there.

Text stage, on every file:
  - g++ 2.95's named return values (`T F(args) return r {`), with sly1-decomp's own
    tools/modern_check.py (imported, not copied);
  - g++'s minimum and maximum operators (`a <? b`, `a >? b`, `a >?= b`) become calls of
    PORT_MIN, PORT_MAX and PORT_MAX_ASSIGN (port/include/port/gxx_ext.h). Their operands follow
    g++ 2.95's grammar: the operators bind tighter than `&` and looser than `==`, left to right;
  - a string literal broken over a line (g++ 2.95 kept the newline in the string) keeps its
    newline as an escape;
  - the helper headers with assembly the port replaces (vu0.h, fpu.h, system.h, mul_wide.h,
    asm_func.h) are reduced to declarations for the spike: their asm statements are dropped.
    A function whose body was asm is then not usable; the port's own versions replace them.

AST stage (libclang), see rewrite_pointers().
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DECOMP = ROOT / "sly1-decomp"
PORT_INCLUDE = ROOT / "port" / "include"
sys.path.insert(0, str(DECOMP / "tools"))
from modern_check import standard_returns  # noqa: E402  (sly1-decomp's, unchanged)

HELPERS = {"include/vu0.h", "include/fpu.h", "include/system.h", "include/mul_wide.h"}

# ---------------------------------------------------------------------------------------------
# Tokens


TOKEN = re.compile(r"""
    (?P<ws>\s+)
  | (?P<comment>//[^\n]*|/\*.*?\*/)
  | (?P<string>"(?:\\.|[^"\\])*")
  | (?P<char>'(?:\\.|[^'\\])*')
  | (?P<ident>[A-Za-z_]\w*)
  | (?P<number>\.?\d(?:[eEpP][+-]|[\w.])*)
  | (?P<minmax>[<>]\?=?)
  | (?P<punct>->\*|<<=|>>=|\.\.\.|::|->|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||[-+*/%&|^]=|[^\s\w])
""", re.S | re.X)


def tokenize(text):
    """[(kind, text, start, end)] of text; comments and whitespace included."""
    out, pos = [], 0
    while pos < len(text):
        m = TOKEN.match(text, pos)
        if not m:
            raise ValueError("cannot tokenize at %r" % text[pos:pos + 40])
        out.append((m.lastgroup, m.group(), m.start(), m.end()))
        pos = m.end()
    return out


def fix_broken_strings(text):
    """A string literal that runs over a line end gets `\\n"`, the line break, and `"`: the same
    characters in the string, in a form a modern compiler accepts, on the same lines."""
    out, i, n, state = [], 0, len(text), "code"
    while i < n:
        ch, nx = text[i], text[i + 1] if i + 1 < n else ""
        if state == "code":
            if ch == "/" and nx == "/":
                state = "line"
            elif ch == "/" and nx == "*":
                out.append("/*")
                i += 2
                state = "block"
                continue
            elif ch == '"':
                state = "string"
            elif ch == "'":
                state = "char"
        elif state == "line":
            if ch == "\n":
                state = "code"
        elif state == "block":
            if ch == "*" and nx == "/":
                out.append("*/")
                i += 2
                state = "code"
                continue
        else:
            if ch == "\\":
                out.append(ch + nx)
                i += 2
                continue
            if ch == "\n" and state == "string":
                out.append('\\n"\n"')
                i += 1
                continue
            if (ch == '"' and state == "string") or (ch == "'" and state == "char"):
                state = "code"
        out.append(ch)
        i += 1
    return "".join(out)


# ---------------------------------------------------------------------------------------------
# g++'s minimum and maximum operators

# Operators that bind looser than `<?` and `>?` in g++ 2.95 (cp/parse.y: '&' '^' '|' && || ?: =
# and the comma), so an operand ends at them; everything tighter stays inside the operand.
LOOSER = {"&", "^", "|", "&&", "||", "?", ":", ",", ";", "=", "+=", "-=", "*=", "/=", "%=",
          "&=", "|=", "^=", "<<=", ">>=", "{", "}", "return", "case", "else"}
CONTROL = {"if", "while", "for", "switch"}
OPERAND_END = {"ident", "number", "string", "char"}


def _sig(tokens):
    """The indices of the significant tokens (no whitespace or comments)."""
    return [i for i, t in enumerate(tokens) if t[0] not in ("ws", "comment")]


TYPE_WORDS = {"s8", "u8", "s16", "u16", "s32", "u32", "s64", "u64", "f32", "f64", "sptr", "uptr",
              "u128", "s128", "int", "char", "short", "long", "float", "double", "unsigned",
              "signed", "void", "const", "volatile", "struct"}


def _is_cast(tokens, sig, j):
    """Whether the `)` at sig[j] closes a cast: `(T*)`, `(T&)` or `(builtin type words)`."""
    d, m = 0, j
    while m >= 0:
        s = tokens[sig[m]][1]
        if s == ")":
            d += 1
        elif s == "(":
            d -= 1
            if d == 0:
                break
        m -= 1
    inner = [tokens[sig[i]][1] for i in range(m + 1, j)]
    if not inner:
        return False
    return inner[-1] in ("*", "&") and inner[0] not in ("*", "&") or all(w in TYPE_WORDS for w in inner)


def _ends_operand(tokens, sig, j):
    """Whether the token at sig[j] ends an operand, so a `&` after it is binary."""
    tok = tokens[sig[j]]
    if tok[1] == ")":
        return not _is_cast(tokens, sig, j)
    return tok[0] in OPERAND_END and tok[1] not in LOOSER or tok[1] == "]"


def _left_start(tokens, sig, k):
    """The index in sig of the first token of the left operand of the operator at sig[k]."""
    j = k - 1
    depth = 0
    while j >= 0:
        t = tokens[sig[j]]
        s = t[1]
        if depth == 0:
            if s in ("(", "[") or t[0] == "minmax":
                break
            if s in LOOSER and not (s == "&" and not (j > 0 and _ends_operand(tokens, sig, j - 1))):
                break
        if s in (")", "]"):
            if depth == 0 and s == ")":
                # The condition of if/while/for/switch is not part of an operand that follows it.
                m, d = j, 0
                while m >= 0:
                    if tokens[sig[m]][1] == ")":
                        d += 1
                    elif tokens[sig[m]][1] == "(":
                        d -= 1
                        if d == 0:
                            break
                    m -= 1
                if m > 0 and tokens[sig[m - 1]][1] in CONTROL:
                    break
            depth += 1
        elif s in ("(", "["):
            depth -= 1
        j -= 1
    return j + 1


def _right_end(tokens, sig, k, assignment=False):
    """The index in sig after the last token of the right operand of the operator at sig[k]."""
    j = k + 1
    depth = 0
    prev = None  # index in sig of the previous token
    while j < len(sig):
        t = tokens[sig[j]]
        s = t[1]
        if depth == 0:
            if s in (")", "]") or (t[0] == "minmax" and not assignment):
                break
            if assignment:
                if s in (",", ";", "}", ":"):
                    break
            elif s in LOOSER and not (s == "&" and not (prev is not None and _ends_operand(tokens, sig, prev))):
                break
        if s in ("(", "["):
            depth += 1
        elif s in (")", "]"):
            depth -= 1
        prev = j
        j += 1
    return j


def rewrite_minmax(text):
    """Every `a <? b`, `a >? b` and `a >?= b` of text as PORT_MIN, PORT_MAX, PORT_*_ASSIGN calls."""
    count = 0
    while True:
        tokens = tokenize(text)
        sig = _sig(tokens)
        k = next((n for n, i in enumerate(sig) if tokens[i][0] == "minmax"), None)
        if k is None:
            return text, count
        op = tokens[sig[k]][1]
        assign = op.endswith("=")
        a = _left_start(tokens, sig, k)
        b = _right_end(tokens, sig, k, assignment=assign)
        if a >= k or b <= k + 1:
            raise ValueError("min/max operator without an operand near %r" %
                             text[tokens[sig[k]][2] - 40:tokens[sig[k]][3] + 40])
        start, opstart, opend = tokens[sig[a]][2], tokens[sig[k]][2], tokens[sig[k]][3]
        end = tokens[sig[b - 1]][3]
        name = ("PORT_MIN" if op[0] == "<" else "PORT_MAX") + ("_ASSIGN" if assign else "")
        left, right = text[start:opstart].strip(), text[opend:end].strip()
        text = "%s%s(%s, %s)%s" % (text[:start], name, left, right, text[end:])
        count += 1


# ---------------------------------------------------------------------------------------------
# The helper headers, for the spike


ASM_STMT = re.compile(r"__asm__\s*(?:volatile\s*|__volatile__\s*)?\(")


def strip_asm(text):
    """Drops every asm statement (balanced parentheses, through its `;`) and asm register names."""
    # A register variable's name: register s32 r __asm__("$2"), register u128 v __asm__("vf31").
    text = re.sub(r"__asm__\s*\(\s*\"\$?\w+\"\s*\)", "", text)
    out, pos = [], 0
    for m in ASM_STMT.finditer(text):
        if m.start() < pos:
            continue
        depth, i = 1, m.end()
        while depth:
            c = text[i]
            if c == '"':
                i += 1
                while text[i] != '"':
                    i += 2 if text[i] == "\\" else 1
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
            i += 1
        while text[i] in " \t":
            i += 1
        if text[i] == ";":
            i += 1
        out.append(text[pos:m.start()])
        out.append("/* asm removed for the spike */")
        pos = i
    out.append(text[pos:])
    text = "".join(out)
    # Scalar's g++ extension: the address of a cast (an lvalue in g++ 2.95). The value itself.
    text = re.sub(r"\*\(u128\s*\*\)\s*&\s*\(s128\)\s*(\w+)", r"\1", text)
    return text


ASM_FUNC_H = """// SPDX-License-Identifier: AGPL-3.0-or-later
// Written by tools/gen.py: the port has no assembly to include. The hand-written functions get
// C++ of the port's own (docs/DESIGN.md, section 3); linker leftovers have no role.
#ifndef ASM_FUNC_H
#define ASM_FUNC_H
#define INCLUDE_ASM(NAME)
#define ASM_FUNC(NAME)
#define LINKER_REMNANT(NAME)
#define LINKER_REMNANT_AFTER_INIT(NAME)
#define INCLUDE_DATA(LABEL)
#define INCLUDE_RODATA(LABEL)
#endif
"""


# ---------------------------------------------------------------------------------------------


def text_stage(out):
    stats = {"files": 0, "minmax": 0, "named_returns": 0, "broken_strings": 0}
    for d in ("include", "src/main"):
        for path in sorted((DECOMP / d).rglob("*")):
            if path.is_dir() or path.suffix not in (".h", ".cpp", ".inc"):
                continue
            rel = str(path.relative_to(DECOMP))
            text = path.read_text(errors="replace")
            if rel == "include/asm_func.h":
                text = ASM_FUNC_H
            else:
                fixed = fix_broken_strings(text)
                stats["broken_strings"] += fixed != text
                text, found = standard_returns(fixed)
                stats["named_returns"] += len(found)
                text, n = rewrite_minmax(text)
                stats["minmax"] += n
                if rel in HELPERS:
                    text = strip_asm(text)
            dst = out / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            if not dst.exists() or dst.read_text() != text:
                dst.write_text(text)
            stats["files"] += 1
    return stats


# The flags the game code is compiled with, for libclang and for the build (CMakeLists.txt).
COMPILE_FLAGS = ["-x", "c++", "-std=gnu++17", "-Wno-register", "-Wno-c++11-narrowing",
                 "-include", "port/gxx_ext.h", "-include", "port/ptr32.h"]


def system_includes():
    """The system compiler's include directories, for libclang, which has none of its own: the
    C++ standard library's headers and the compiler's built-in ones, as `c++ -E -v` lists them."""
    import os
    import subprocess
    cxx = os.environ.get("CXX", "c++")
    r = subprocess.run([cxx, "-x", "c++", "-std=gnu++17", "-E", "-v", "-"], input="",
                       capture_output=True, text=True)
    dirs, inside = [], False
    for line in r.stderr.splitlines():
        if line.startswith("#include <...> search starts here"):
            inside = True
        elif line.startswith("End of search list"):
            break
        elif inside:
            d = line.strip()
            if d.endswith("(framework directory)"):
                continue
            dirs.append(d)
    if not dirs:
        raise SystemExit("gen: cannot find the system include directories from %s -E -v" % cxx)
    flags = []
    for d in dirs:
        flags += ["-isystem", d]
    return flags


def clang_args(out):
    # -fms-extensions: a cast of a pointer to a smaller integer stays in the tree as a warning
    # (the analysis only; the build compiles without it).
    return COMPILE_FLAGS + ["-w", "-ferror-limit=0", "-fms-extensions", "-nostdinc", "-nostdinc++"] + system_includes() + \
        ["-I" + str(PORT_INCLUDE), "-I" + str(out / "include"), "-I" + str(out / "src")]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--all", action="store_true", help="every unit of src/main")
    ap.add_argument("--no-pointers", action="store_true", help="the text stage only")
    ap.add_argument("units", nargs="*", help="units to rewrite, as src/main/X.cpp")
    args = ap.parse_args()
    out = args.out.resolve()
    stats = text_stage(out)
    print("gen: text stage: %(files)d files, %(minmax)d min/max operators, "
          "%(named_returns)d named return values, %(broken_strings)d files with broken strings"
          % stats)
    if args.no_pointers:
        return 0
    units = sorted(str(p.relative_to(DECOMP)) for p in (DECOMP / "src/main").glob("*.cpp")) \
        if args.all else args.units
    import gen_pointers
    report = {}
    gen_pointers.rewrite(units, out, clang_args(out), report)
    print("gen: pointer stage: %d units, %d files changed" % (len(units), report["files_changed"]))
    for k, v in sorted(report["counts"].items()):
        print("  %6d %s" % (v, k))
    if report["unsupported"]:
        print("gen: %d places not rewritten:" % len(report["unsupported"]))
        for u in report["unsupported"][:40]:
            print("  " + u)
    return 0


if __name__ == "__main__":
    sys.exit(main())
