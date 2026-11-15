# SPDX-License-Identifier: AGPL-3.0-or-later
"""Check that the generated game code keeps retail's field offsets on this compiler.

    venv/bin/python tools/layout_check.py --out build-host/gen src/main/heap.cpp ...
    venv/bin/python tools/layout_check.py --out build-host/gen --all

The decompilation writes a field's retail offset in a comment on its line (`void* head; // 0x00`).
This parses the units tools/gen.py wrote (pointers already 4-byte Ptr32s) for the host, asks the
compiler where it placed each such field (libclang's offsetof, in the enclosing record), and
reports every field whose offset differs: a place where today's ABI lays a structure out
differently from g++ 2.95 (docs/DESIGN.md, section 4). The size checks (check_ typedefs) are
compile-time errors already; this covers the fields inside.
"""

import argparse
import re
import sys
from pathlib import Path

import clang.cindex as ci

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen  # noqa: E402

OFFSET_COMMENT = re.compile(r"//\s*(?:\+)?0x([0-9A-Fa-f]+)\b")


def check(units, out):
    index = ci.Index.create()
    args = [a for a in gen.clang_args(out) if a != "-fms-extensions"]
    seen = {}
    lines_cache = {}
    for unit in units:
        tu = index.parse(str(out / unit), args=args)
        errors = [d for d in tu.diagnostics if d.severity >= ci.Diagnostic.Error]
        if errors:
            print("layout: %s has %d compile errors; first: %s" % (unit, len(errors), errors[0]))
        for c in tu.cursor.walk_preorder():
            if c.kind != ci.CursorKind.FIELD_DECL or c.location.file is None:
                continue
            f = c.location.file.name
            if not f.startswith(str(out)):
                continue
            key = (f, c.location.line, c.location.column)
            if key in seen:
                continue
            if f not in lines_cache:
                lines_cache[f] = Path(f).read_text(errors="replace").split("\n")
            line = lines_cache[f][c.location.line - 1]
            m = OFFSET_COMMENT.search(line[c.location.column:])
            if not m:
                seen[key] = None
                continue
            parent = c.semantic_parent
            if parent is not None and parent.type.get_canonical().kind == ci.TypeKind.RECORD and \
                    parent.is_anonymous():
                seen[key] = None   # an anonymous record's member: its comment gives the outer offset
                continue
            bits = c.get_field_offsetof()
            if bits < 0:
                seen[key] = None
                continue
            want = int(m.group(1), 16)
            got = bits // 8
            seen[key] = (want, got, parent.spelling if parent else "?", c.spelling,
                         str(Path(f).relative_to(out)), c.location.line)
    # Fields declared on one line share its comment, which gives the first one's offset.
    first = {}
    for (f, line, col), v in seen.items():
        if (f, line) not in first or col < first[(f, line)][0]:
            first[(f, line)] = (col, v)
    checked = [v for _, v in first.values() if v]
    bad = [v for v in checked if v[0] != v[1]]
    return checked, bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--all", action="store_true")
    ap.add_argument("units", nargs="*")
    args = ap.parse_args()
    out = args.out.resolve()
    units = sorted("src/main/" + p.name for p in (out / "src/main").glob("*.cpp")) if args.all else args.units
    checked, bad = check(units, out)
    print("layout: %d fields with a retail offset checked, %d differ" % (len(checked), len(bad)))
    for want, got, rec, field, f, line in bad[:60]:
        print("  %s:%d %s::%s retail 0x%X, here 0x%X" % (f, line, rec, field, want, got))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
