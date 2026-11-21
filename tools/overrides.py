# SPDX-License-Identifier: AGPL-3.0-or-later
"""The port's own versions of whole game functions (docs/DESIGN.md, "The port's own functions").

The decompilation is never changed. A named patch (tools/patches.py) replaces a line or two of
it; this replaces a whole function's definition, for the places where the port does a thing its
own way and not the PS2's (the renderer's draw functions first). Gameplay code stays as
decompiled: an override is for what the port must do differently on its hardware.

An override is a file

    port/game/overrides/<the file under sly1-decomp/>/<function>.cpp

for example port/game/overrides/src/main/world_listed_object.cpp/func_0013A618.cpp. It holds:

- a first comment saying why the port has its own version (required: the file must begin with
  `// SPDX-License-Identifier:` and then `// Why:`);
- the function's definition, complete, under the name the decompilation defines it by (its
  address name or its readable name from include/names.h), with the same parameters and result.

tools/gen.py puts it in the generated copy in place of the decompilation's definition, which it
finds by name: the definition at the outermost level of the file, from its first line to its
closing brace. As with a patch, an override that does not find exactly one definition stops the
generation: after a submodule bump nothing goes stale silently.

    python3 tools/overrides.py list         every override, with its reason
    python3 tools/overrides.py check        each override's file and function exist in the decompilation
    python3 tools/overrides.py self-test    the definition finder against every source file
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DECOMP = ROOT / "sly1-decomp"
OVERRIDES = ROOT / "port" / "game" / "overrides"


class OverrideError(Exception):
    pass


def _blank(text):
    """text with its comments, strings and character constants made spaces (newlines kept), so
    that braces and names can be looked for by position."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == "//":
            j = text.find("\n", i)
            j = n if j < 0 else j
        elif two == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j = min(n, j + 1)
        else:
            i += 1
            continue
        for k in range(i, j):
            if out[k] != "\n":
                out[k] = " "
        i = j
    return "".join(out)


def definitions(text):
    """Every function definition at the outermost level of text: name -> list of (start, end),
    start at the beginning of the line its declarator begins on, end after its closing brace."""
    blank = _blank(text)
    found = {}
    depth = 0
    i, n = 0, len(blank)
    last_end = 0          # after the last ';' or '}' at depth 0: where the next declaration begins
    while i < n:
        c = blank[i]
        if c == "{":
            if depth == 0:
                # a function's body when what precedes, since the last declaration ended, is
                # `... name ( ... )` with nothing but qualifiers after the parenthesis
                head = blank[last_end:i]
                m = re.search(r"([A-Za-z_]\w*)\s*\(", head)
                close = head.rstrip()
                is_def = False
                if m and re.search(r"\)\s*(const)?\s*$", close) and not re.match(r"\s*(struct|class|union|enum|namespace|extern\s+\"C\"\s*$)", head.lstrip()):
                    # the name is the identifier before the parenthesis that opens the parameter
                    # list: the one whose matching ')' ends the head
                    k = len(close) - 1
                    if close.endswith("const"):
                        k = len(close[:-5].rstrip()) - 1
                    level = 0
                    while k >= 0:
                        if close[k] == ")":
                            level += 1
                        elif close[k] == "(":
                            level -= 1
                            if level == 0:
                                break
                        k -= 1
                    mm = re.search(r"([A-Za-z_]\w*)\s*$", close[:k]) if k >= 0 else None
                    if mm and mm.group(1) not in ("if", "while", "for", "switch", "return", "sizeof", "static_assert", "__attribute__"):
                        name = mm.group(1)
                        # start: the line on which the head's first non-space character is
                        first = last_end + (len(head) - len(head.lstrip()))
                        start = text.rfind("\n", 0, first) + 1
                        if start < last_end:
                            start = first
                        is_def = True
                        body_from = i
                if is_def:
                    d = 0
                    j = i
                    while j < n:
                        if blank[j] == "{":
                            d += 1
                        elif blank[j] == "}":
                            d -= 1
                            if d == 0:
                                break
                        j += 1
                    found.setdefault(name, []).append((start, j + 1))
                    i = j + 1
                    last_end = i
                    continue
            depth += 1
        elif c == "}":
            depth = max(0, depth - 1)
            if depth == 0:
                last_end = i + 1
        elif c == ";" and depth == 0:
            last_end = i + 1
        elif c == "#" and depth == 0 and blank[blank.rfind("\n", 0, i) + 1:i].strip() == "":
            # a preprocessor line (with its continuations) ends a declaration's head
            j = i
            while True:
                e = blank.find("\n", j)
                if e < 0:
                    e = n
                    break
                if blank[e - 1] != "\\":
                    break
                j = e + 1
            i = e
            last_end = i
            continue
        i += 1
    return found


def all_overrides():
    """(file under sly1-decomp/, function, the override's path) for each override."""
    out = []
    if not OVERRIDES.is_dir():
        return out
    for path in sorted(OVERRIDES.rglob("*.cpp")):
        if not path.is_file():
            continue
        rel = path.parent.relative_to(OVERRIDES)
        if rel.suffix not in (".cpp", ".h"):
            continue          # (a file not under a directory named for a source file)
        out.append((str(rel), path.stem, path))
    return out


def reason(path):
    text = path.read_text()
    lines = text.split("\n")
    if not lines or not lines[0].startswith("// SPDX-License-Identifier:"):
        raise OverrideError("%s does not begin with its SPDX line" % path)
    m = re.search(r"(?m)^// Why:\s*(.*(?:\n//(?! Why:).*)*)", text)
    if not m:
        raise OverrideError("%s has no `// Why:` comment" % path)
    return " ".join(part.lstrip("/ ").strip() for part in m.group(1).split("\n")).strip()


def apply(rel, text):
    """The text of file rel with the port's own functions in place of the decompilation's, and
    how many were put in."""
    mine = [(f, path) for r, f, path in all_overrides() if r == rel]
    if not mine:
        return text, 0
    defs = definitions(text)
    spans = []
    for func, path in mine:
        reason(path)
        at = defs.get(func, [])
        if len(at) != 1:
            raise OverrideError("override %s: %s is defined %d times in %s, not once" % (path.relative_to(ROOT), func, len(at), rel))
        body = path.read_text().rstrip("\n")
        own = definitions(body)
        if list(own) != [func] or len(own[func]) != 1:
            raise OverrideError("override %s must define %s and nothing else (it defines %s)" % (path.relative_to(ROOT), func, ", ".join(own) or "nothing"))
        spans.append((at[0][0], at[0][1], "// port: the port's own %s (%s)\n%s" % (func, path.relative_to(ROOT), body)))
    for start, end, new in sorted(spans, reverse=True):
        text = text[:start] + new + text[end:]
    return text, len(spans)


def files():
    return {r for r, _, _ in all_overrides()}


def _self_test():
    """The finder against every source file of the decompilation: each function of
    config/us/functions.tsv that a file defines is found once, and a definition taken out and
    put back leaves the file as it was."""
    wanted = set()
    for line in (DECOMP / "config" / "us" / "functions.tsv").read_text().split("\n")[1:]:
        if line.strip():
            wanted.add(line.split("\t")[0])
    names = {}
    header = DECOMP / "include" / "names.h"
    if header.exists():
        for m in re.finditer(r"(?m)^#define\s+(\w+)\s+(func_[0-9A-F]{8})\s*$", header.read_text()):
            names[m.group(1)] = m.group(2)
    files_seen = found = twice = 0
    seen = set()
    for path in sorted((DECOMP / "src" / "main").glob("*.cpp")):
        text = path.read_text(errors="replace")
        defs = definitions(text)
        files_seen += 1
        for name, spans in defs.items():
            func = names.get(name, name)
            if func not in wanted:
                continue
            if len(spans) != 1:
                twice += 1
                continue
            found += 1
            seen.add(func)
            start, end = spans[0]
            piece = text[start:end]
            again = definitions(piece)
            if list(again) != [name]:
                raise SystemExit("self-test: %s in %s: taken out, it reads as %s" % (name, path.name, list(again)))
            if text[:start] + piece + text[end:] != text:
                raise SystemExit("self-test: %s in %s does not go back as it was" % (name, path.name))
    print("overrides: %d files, %d functions of the table found once, %d found more than once" % (files_seen, found, twice))
    return seen


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "list"
    if cmd == "self-test":
        _self_test()
        return
    rows = all_overrides()
    if cmd == "check":
        for rel, func, path in rows:
            src = DECOMP / rel
            if not src.exists():
                raise SystemExit("override %s: no %s in the decompilation" % (path.relative_to(ROOT), rel))
            apply(rel, src.read_text(errors="replace"))
        print("overrides: %d checked" % len(rows))
        return
    for rel, func, path in rows:
        print("%s  %s\n    %s" % (rel, func, reason(path)))
    print("%d functions are the port's own" % len(rows))


if __name__ == "__main__":
    try:
        main()
    except OverrideError as e:
        raise SystemExit("overrides: %s" % e)
