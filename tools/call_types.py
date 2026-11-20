#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Indirect calls whose declared argument types disagree with the functions they reach.

A game address is 32 bits and a pointer here is 64: a call through a stored function pointer
whose type says "an int" for an argument the function takes as a pointer hands over the game
address as it is (and the other way round hands over half a pointer). Such a call compiles and
faults only when it runs. This reads the decompilation's sources and its class map
(config/us/classes.tsv) and reports, without running anything:

  slots     a class's method slot whose functions (the class's and its descendants') disagree
            with each other, or with the slot's declared type in the class's descriptor struct
  casts     calls through a function-pointer cast, with the type cast to
  fields    function-pointer members and typedefs that are not descriptor slots, with the
            functions assigned to a member of that name

    python3 tools/call_types.py [--all] [slots|casts|fields ...]

Kinds: P pointer, I integer of 32 bits or fewer, L 64-bit integer, F float, D double, S a class
by value, V void, ? not known. Only a P against an I or an L is reported unless --all.
"""
import collections
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent / "sly1-decomp"
INTS = {"s32", "u32", "int", "unsigned", "s16", "u16", "s8", "u8", "char", "short", "bool", "uint", "size_t", "signed",
        "undefined4", "wchar_t", "BOOL"}
LONGS = {"s64", "u64", "long", "u128", "s128"}
QUAL = {"const", "volatile", "struct", "class", "enum", "union", "register", "unsigned", "signed", "static", "inline", "extern"}


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: " " * len(m.group(0)) if "\n" not in m.group(0) else re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


class Types:
    def __init__(self):
        self.typedefs = {}
        self.enums = set()
        self.records = set()

    def learn(self, text):
        for m in re.finditer(r"\btypedef\s+([^;{}()]+?)\s+(\w+)\s*;", text):
            self.typedefs[m.group(2)] = m.group(1)
        for m in re.finditer(r"\btypedef\s+[^;{}]*\(\s*\*\s*(\w+)\s*\)\s*\(", text):
            self.typedefs[m.group(1)] = "void*"
        for m in re.finditer(r"\benum\s+(\w+)", text):
            self.enums.add(m.group(1))
        for m in re.finditer(r"\b(?:struct|class|union)\s+(\w+)", text):
            self.records.add(m.group(1))

    def kind(self, t, depth=0):
        t = t.strip()
        if not t:
            return "?"
        if "*" in t or "&" in t or "[" in t or "(" in t:
            return "P"
        words = [w for w in re.findall(r"[\w:]+", t) if w not in QUAL or w in ("unsigned", "signed")]
        if not words:
            return "?"
        if "long" in words:
            return "L"
        if any(w in ("float", "f32") for w in words):
            return "F"
        if any(w in ("double", "f64") for w in words):
            return "D"
        if words == ["void"]:
            return "V"
        base = words[-1]
        if base in INTS or any(w in INTS for w in words):
            return "I"
        if base in LONGS:
            return "L"
        if base in self.enums:
            return "I"
        if base in self.typedefs and depth < 8:
            return self.kind(self.typedefs[base], depth + 1)
        if base in self.records:
            return "S"
        return "?"


def split_params(text):
    out, depth, cur = [], 0, ""
    for ch in text:
        if ch in "(<[":
            depth += 1
        elif ch in ")>]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return [p.strip() for p in out]


def param_type(p):
    p = re.sub(r"=.*", "", p).strip()
    if p == "...":
        return "..."
    if "(" in p:   # a function pointer
        return "void*"
    m = re.match(r"^(.*?)(\b\w+)\s*((?:\[[^\]]*\])*)$", p, re.S)
    if not m:
        return p
    head, name, arr = m.group(1).strip(), m.group(2), m.group(3)
    if not head or all(w in QUAL for w in re.findall(r"\w+", head)) and "*" not in head and "&" not in head:
        return p + arr   # the last word is the type (an unnamed parameter)
    return head + ("*" if arr else "")


def kinds_of(types, ret, params):
    ps = split_params(params)
    if len(ps) == 1 and ps[0].strip() == "void":
        ps = []
    return types.kind(ret), [("..." if param_type(p) == "..." else types.kind(param_type(p))) for p in ps], [param_type(p) for p in ps]


def sig_text(k):
    return "%s(%s)" % (k[0], " ".join(k[1]))


def clash(a, b, everything):
    """Positions where two parameter lists disagree: P against I or L, or (everything) any difference."""
    # Floats and doubles go in registers of their own, on the PS2 and here alike, so a cast that
    # moves a float among the other arguments changes nothing: only the rest is compared, in order.
    out = []
    ia = [(i, x) for i, x in enumerate(a) if x not in ("F", "D")]
    ib = [(i, x) for i, x in enumerate(b) if x not in ("F", "D")]
    for (i, x), (_, y) in zip(ia, ib):
        if x == y or "?" in (x, y) or "..." in (x, y):
            continue
        if {x, y} <= {"I", "L", "P"} and "P" in (x, y) or everything:
            out.append(i)
    return out


def main():
    # --gen=DIR: the port's generated copy (build-fast/gen), which is what is compiled: the rules of
    # tools/gen.py and the named patches have put many of the decompilation's mismatches right.
    global ROOT
    gen = [a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--gen=")]
    classes_tsv = ROOT / "config/us/classes.tsv"
    if gen:
        ROOT = Path(gen[0]).resolve()
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    everything = "--all" in sys.argv
    want = set(args) or {"slots", "casts", "fields"}
    names = {}
    for m in re.finditer(r"^#define\s+(\w+)\s+(func_[0-9A-F]{8})\s*$", (ROOT / "include/names.h").read_text(), re.M):
        names[m.group(1)] = m.group(2)
    readable = {v: k for k, v in names.items()}
    files = sorted((ROOT / "src/main").glob("*.cpp")) + sorted((ROOT / "src/lib").glob("*.c*")) + sorted((ROOT / "src/libm").glob("*.c*"))
    headers = sorted((ROOT / "include").rglob("*.h"))
    texts = {f: strip_comments(f.read_text(errors="replace")) for f in files + headers}
    types = Types()
    for t in texts.values():
        types.learn(t)

    # every function's definition: its address, the kinds of its result and parameters
    defs = collections.defaultdict(list)
    pattern = re.compile(r"^(?!\s)((?:[\w:\*&<>]+[\s\*&]+)+?)([A-Za-z_]\w*)\s*\(([^;{}]*?)\)\s*(?:const\s*)?\{", re.M | re.S)
    for f in files:
        for m in pattern.finditer(texts[f]):
            ret, name, params = m.group(1), m.group(2), m.group(3)
            if name in ("if", "while", "for", "switch", "return", "sizeof") or re.search(r"\b(typedef|else|return)\b", ret):
                continue
            func = names.get(name, name)
            if not re.fullmatch(r"func_[0-9A-F]{8}", func):
                continue
            k = kinds_of(types, re.sub(r"\b(static|inline|extern)\b|\"C\"", "", ret), params)
            defs[func].append((k, f.name, texts[f].count("\n", 0, m.start()) + 1))

    def show(func):
        d = defs.get(func)
        return "%s %s %s" % (func, readable.get(func, "-"), sig_text(d[0][0]) if d else "(no C definition found)")

    # the classes: each one's slots, and the function in each
    classes = {}
    rows = [l.split("\t") for l in classes_tsv.read_text().splitlines()[1:]]
    for r in rows:
        if r[2] != "class":
            continue
        slots = [s.lstrip("+") if s.lstrip("+").startswith("func_") else None for s in r[12].split()]
        classes[int(r[0], 16)] = {"parent": int(r[4], 16) if r[4] != "-" else None, "slots": slots}

    def descends(c, a):
        while c is not None:
            if c == a:
                return True
            c = classes.get(c, {}).get("parent")
        return False

    if "slots" in want:
        # a descriptor struct's members by offset, for the classes with an accessor (descNN())
        struct_of = {}
        for t in texts.values():
            for m in re.finditer(r"const\s+(\w+)\s*\*\s*desc([0-9A-Fa-f]{2})\s*\(\s*\)", t):
                struct_of[int(m.group(2), 16)] = m.group(1)
        raw = {f: f.read_text(errors="replace") for f in headers + files}
        members = {}   # struct -> offset -> (name, kinds)
        def struct_body(sname):
            for f, t in raw.items():
                m = re.search(r"\bstruct\s+%s\s*(?::\s*(?:public\s+)?(\w+)\s*)?\{(.*?)\n\s*\};" % sname, t, re.S)
                if m:
                    return (struct_body(m.group(1)) if m.group(1) else "") + "\n" + m.group(2)
            return ""
        for sname in set(struct_of.values()):
            for body in [struct_body(sname)]:
                if not body:
                    continue
                at, table = 0, {}
                for line in body.splitlines():
                    code = line.split("//")[0].strip()
                    if not code or code.startswith("#"):
                        continue
                    off = re.search(r"//\s*0x([0-9A-Fa-f]+)", line)
                    if off:
                        at = int(off.group(1), 16)
                    fm = re.match(r"(.+?)\(\s*\*\s*(\w+)\s*\)\s*\((.*)\)\s*;", code)
                    pm = re.match(r"(?:port::)?Ptr32<\s*(.+?)\((.*)\)\s*>\s*(\w+)\s*;", code)
                    if pm:
                        fm = re.match(r"(.*)\x00(.*)\x00(.*)", "%s\x00%s\x00%s" % (pm.group(1), pm.group(3), pm.group(2)))
                    arr = re.search(r"\[\s*(0x[0-9A-Fa-f]+|\d+)\s*\]\s*;", code)
                    if fm:
                        table[at] = (fm.group(2), kinds_of(types, fm.group(1), fm.group(3)))
                    at += 4 * (int(arr.group(1), 0) if arr and not fm else 1)
                members[sname] = table
        print("== slots: a class's method slot, the functions in it and the type it is called by")
        found = 0
        nslots = max(len(c["slots"]) for c in classes.values())
        for top in sorted(classes):
            for i in range(len(classes[top]["slots"])):
                parent = classes[top]["parent"]
                if parent in classes and i < len(classes[parent]["slots"]):
                    continue   # the slot begins in an ancestor: reported there
                funcs = sorted({classes[c]["slots"][i] for c in classes if descends(c, top) and i < len(classes[c]["slots"]) and classes[c]["slots"][i]})
                offset = 0x10 + 4 * i
                declared = []
                for c, sname in struct_of.items():
                    if descends(c, top) or descends(top, c):
                        mem = members.get(sname, {}).get(offset)
                        if mem and descends(c, top):
                            declared.append((sname, mem))
                sigs = [(fn, defs[fn][0][0]) for fn in funcs if fn in defs]
                problems = []
                for a in range(len(sigs)):
                    for b in range(a + 1, len(sigs)):
                        pos = clash(sigs[a][1][1], sigs[b][1][1], everything)
                        if pos:
                            problems.append("  functions disagree at argument %s: %s | %s" % (pos, show(sigs[a][0]), show(sigs[b][0])))
                for sname, (mname, mk) in declared:
                    for fn, k in sigs:
                        pos = clash(mk[1], k[1], everything)
                        if pos or (everything and len(mk[1]) != len(k[1])):
                            problems.append("  %s::%s is %s, at argument %s against %s" % (sname, mname, sig_text(mk), pos, show(fn)))
                if problems:
                    found += 1
                    print("class 0x%02X slot %d (+0x%X), %d functions:" % (top, i, offset, len(funcs)))
                    for p in sorted(set(problems))[:12]:
                        print(p)
        print("%d slots with a disagreement; descriptor structs read: %s" % (found, ", ".join("%s (%d members)" % (s, len(m)) for s, m in sorted(members.items()))))

    if "casts" in want:
        print("== casts: calls through a function-pointer cast")
        cast = re.compile(r"\(\(\s*([\w\s\*:]+?)\(\s*\*\s*\)\s*\(([^()]*)\)\s*\)\s*([^;{}]*?)\)\s*\(|FromGameAddr<\s*([\w\s\*:]+?)\(([^()<>]*)\)\s*>\s*\(([^;{}]*?)\)\s*\)?\s*\(")
        rows = []
        for f in files:
            for m in cast.finditer(texts[f]):
                g = m.groups()
                g = g[:3] if g[0] is not None else g[3:]
                k = kinds_of(types, g[0], g[1])
                target = g[2].strip()
                func = names.get(target, target)
                line = texts[f].count("\n", 0, m.start()) + 1
                note = ""
                if func in defs:
                    pos = clash(k[1], defs[func][0][0][1], everything)
                    note = "DISAGREES at %s with %s" % (pos, show(func)) if pos else "agrees with its definition"
                # through a class's slot (a member mXX is slot 0xXX): against every function a class
                # of that family has there
                sm = re.search(r"desc([0-9A-Fa-f]{2})?(?:\(\))?\s*->\s*m([0-9A-Fa-f]{2})\s*$", target)
                if sm:
                    i = int(sm.group(2), 16)
                    top = int(sm.group(1), 16) if sm.group(1) else None
                    funcs = sorted({c["slots"][i] for n, c in classes.items() if i < len(c["slots"]) and c["slots"][i] and (top is None or descends(n, top))})
                    bad = ["%s at %s" % (show(fn), clash(k[1], defs[fn][0][0][1], everything)) for fn in funcs if fn in defs and clash(k[1], defs[fn][0][0][1], everything)]
                    note = ("DISAGREES with %d of the slot's %d functions: " % (len(bad), len(funcs)) + "; ".join(bad[:4])) if bad else "agrees with the slot's %d functions" % len(funcs)
                rows.append((f.name, line, sig_text(k), target[:60], note))
        for r in rows:
            if everything or "DISAGREES" in r[4] or (not r[4] and ("I" in r[2].split("(")[1] or "L" in r[2].split("(")[1])):
                print("%s:%d: %s through %s %s" % r)
        print("%d casts; shown: those that disagree with a named function, and those through a stored value with an integer argument" % len(rows))

    if "fields" in want:
        print("== fields: function-pointer members, typedefs and variables that are not descriptor slots")
        decl = re.compile(r"([\w\s\*:]+?)\(\s*\*\s*(\w+)\s*\)\s*\(([^()]*)\)\s*;|Ptr32<\s*([\w\s\*:]+?)\(([^()<>]*)\)\s*>\s*(\w+)\s*;")
        seen = collections.OrderedDict()
        for f in headers + files:
            for m in decl.finditer(texts[f]):
                if m.group(1) is None:
                    m = re.match(r"(.*)\x00(.*)\x00(.*)", "%s\x00%s\x00%s" % (m.group(4), m.group(6), m.group(5)), re.S)
                name = m.group(2)
                if re.fullmatch(r"m[0-9A-Fa-f]{1,3}", name):
                    continue
                k = kinds_of(types, re.sub(r"\btypedef\b", "", m.group(1)), m.group(3))
                seen.setdefault((name, sig_text(k)), []).append("%s:%d" % (f.name, texts[f].count("\n", 0, m.start()) + 1))
        # the functions assigned to a member or variable of that name, anywhere
        assigned = collections.defaultdict(set)
        fn_names = set(names) | set(defs)
        for f in files:
            for m in re.finditer(r"(?:\.|->)?\b(\w+)\s*=\s*(?:\([^()]*\)\s*)*&?\s*(\w+)\s*[;,)]", texts[f]):
                if m.group(2) in fn_names and names.get(m.group(2), m.group(2)) in defs:
                    assigned[m.group(1)].add(names.get(m.group(2), m.group(2)))
        shown = 0
        for (name, sig), where in seen.items():
            ks = sig.split("(")[1].rstrip(")").split()
            problems = []
            for func in sorted(assigned.get(name, ())):
                pos = clash(ks, defs[func][0][0][1], everything)
                if pos:
                    problems.append("  DISAGREES at %s with %s" % (pos, show(func)))
            if problems or everything:
                shown += 1
                print("%s %s (%s%s), %d functions assigned to that name" % (name, sig, where[0], " and %d more" % (len(where) - 1) if len(where) > 1 else "", len(assigned.get(name, ()))))
                for p in problems[:10]:
                    print(p)
        print("%d declarations, %d shown" % (len(seen), shown))
    print("functions with a C definition read: %d" % len(defs))


if __name__ == "__main__":
    main()
