# SPDX-License-Identifier: AGPL-3.0-or-later
"""The layout passes of tools/gen.py (docs/DESIGN.md, section 4).

Tail padding. g++ 2.95 never places a derived class's members in its base's tail padding (the
bytes between the base's data and its size, there for alignment). Today's Itanium ABI does, on
Clang and GCC alike, when the base is not "POD for the purpose of layout" in C++03's sense: a class
that has a base class of its own, or (GCC) holds a member with constructors such as Ptr32. Class
0x51's LightObject would start 4 bytes inside SortKeyedObject. So every class of the game that
another class derives from, and that has tail padding, gets an explicit member over it,
`u8 port_tail_[N];`: its data then ends at its size and nothing can move in. Sizes, alignments
and initialisers are unchanged. The pass works bottom-up in rounds: a class is padded once none of
its own fields sits in a base's tail any more (its bases are done), and each round re-measures.

A class with virtual functions is left alone: its hidden table pointer is 8 bytes on a 64-bit
host, and such a class is a patch of its own (the movie player's TransferSink family).

Offset assertions. Every field whose retail offset the decompilation states in a comment gets a
`static_assert(offsetof(...))` after its record, so each compiler that builds the game (the host's
Clang, the Switch's GCC) checks retail's layout itself (tools/layout_check.py reads the same
comments through libclang).
"""

import collections
import re
from pathlib import Path

import clang.cindex as ci

CK = ci.CursorKind
RECORDS = (CK.STRUCT_DECL, CK.CLASS_DECL)


def _qualified(c):
    parts = []
    while c is not None and c.kind in RECORDS + (CK.NAMESPACE,):
        if not c.spelling or c.is_anonymous():
            return None
        parts.append(c.spelling)
        c = c.semantic_parent
    return "::".join(reversed(parts))


def _probe_dsize(index, path, args, records):
    """{key: offset of a char member of a class derived from the record}: where today's ABI would
    put the first member after the record, its data size. Measured by the compiler itself: a probe
    `struct P : B { char c; }` appended to the unit."""
    if not records:
        return {}
    text = Path(path).read_text(errors="replace")
    probes = []
    for i, (key, q) in enumerate(records):
        probes.append("struct port_probe_%d : %s { char port_c; };" % (i, q))
    tu = index.parse(path, args=args, unsaved_files=[(path, text + "\n" + "\n".join(probes) + "\n")])
    found = {}
    for c in tu.cursor.get_children():
        if c.kind == CK.STRUCT_DECL and c.spelling.startswith("port_probe_"):
            i = int(c.spelling[len("port_probe_"):])
            for k in c.get_children():
                if k.kind == CK.FIELD_DECL and k.spelling == "port_c" and k.get_field_offsetof() >= 0:
                    found[records[i][0]] = k.get_field_offsetof() // 8
    return found


def _measure(job):
    """A worker: what each class of the game looks like, and which classes are bases."""
    units, out, args = job
    index = ci.Index.create()
    info = {}
    bases = set()
    dsize = {}
    root = str(out)
    for unit in units:
        path = str(out / unit)
        tu = index.parse(path, args=args)
        probe = {}
        for c in tu.cursor.walk_preorder():
            if c.kind == CK.CXX_BASE_SPECIFIER:
                d = c.type.get_declaration()
                if d is not None and d.location.file is not None:
                    d = d.get_definition() or d
                    key = (d.location.file.name, d.extent.end.offset - 1)
                    bases.add(key)
                    q = _qualified(d)
                    parent = d.semantic_parent
                    local = parent is not None and parent.kind not in RECORDS + (CK.NAMESPACE, CK.TRANSLATION_UNIT)
                    if q and not local and key not in dsize:
                        probe[key] = q
                continue
            if c.kind not in RECORDS or not c.is_definition():
                continue
            f = c.location.file
            if f is None or not f.name.startswith(root) or c.is_anonymous():
                continue
            key = (f.name, c.extent.end.offset - 1)
            if key in info:
                continue
            size = c.type.get_size()
            kids = list(c.get_children())
            virtual = any(k.kind in (CK.CXX_METHOD, CK.DESTRUCTOR) and k.is_virtual_method() for k in kids) or \
                any(k.kind == CK.CXX_BASE_SPECIFIER and any(t.spelling == "virtual" for t in k.get_tokens())
                    for k in kids)
            own_end, own_first, base_end, ok = 0, None, 0, size > 0
            padded = False
            for k in kids:
                if k.kind == CK.FIELD_DECL:
                    off, fs = k.get_field_offsetof(), k.type.get_size()
                    if off < 0 or fs < 0:
                        ok = False
                        break
                    own_end = max(own_end, off // 8 + fs)
                    own_first = off // 8 if own_first is None else min(own_first, off // 8)
                    padded |= k.spelling == "port_tail_"
                elif k.kind == CK.CXX_BASE_SPECIFIER:
                    bs = k.type.get_size()
                    if bs < 0:
                        ok = False
                        break
                    base_end = max(base_end, bs)
            if ok:
                info[key] = (c.spelling, size, own_end, own_first, base_end, virtual, padded)
        dsize.update(_probe_dsize(index, path, args, sorted(probe.items())))
    return info, bases, dsize


def _parallel(fn, units, out, args, jobs=None):
    import multiprocessing
    import os
    jobs = jobs or min(len(units), os.cpu_count() or 1) or 1
    ordered = sorted(units, key=lambda u: -(out / u).stat().st_size)
    shares = [ordered[i::jobs] for i in range(jobs)]
    if jobs == 1:
        return [fn((shares[0], out, args))]
    with multiprocessing.get_context("spawn").Pool(jobs) as pool:
        return pool.map(fn, [(s, out, args) for s in shares])


def pad_tails(units, out, args, report, rounds=16):
    total = 0
    for r in range(rounds):
        info, bases, dsize = {}, set(), {}
        for i, b, d in _parallel(_measure, units, out, args):
            info.update(i)
            bases |= b
            dsize.update(d)
        todo = collections.defaultdict(list)
        for key, (name, size, own_end, own_first, base_end, virtual, padded) in info.items():
            if key not in bases or virtual or padded or key not in dsize:
                continue
            if own_first is not None and own_first < base_end:
                continue  # a field of its own is still in a base's tail: its bases go first
            end = dsize[key]   # the compiler's own measure of where the data ends
            if end < size:
                todo[key[0]].append((key[1], name, size - end))
        if not todo:
            break
        for f, items in todo.items():
            data = Path(f).read_bytes()
            for closing, name, n in sorted(items, reverse=True):
                if data[closing:closing + 1] != b"}":
                    report.setdefault("tail_unsupported", []).append("%s: %s" % (f, name))
                    continue
                member = ("    u8 port_tail_[%d]; // port: retail's tail padding, kept from "
                          "derived classes (tools/gen_layout.py)\n" % n).encode()
                data = data[:closing] + member + data[closing:]
                total += 1
            Path(f).write_bytes(data)
        report["tail_rounds"] = r + 1
    report["tails"] = total
    return total


# ---------------------------------------------------------------------------------------------
# Offset assertions

OFFSET_COMMENT = re.compile(rb"//\s*\+?0x([0-9A-Fa-f]+)\b")


def _offsets(job):
    """A worker: {(file, closing): (qualified record, [(field, retail offset, here)])}."""
    units, out, args = job
    index = ci.Index.create()
    found = {}
    texts = {}
    root = str(out)
    for unit in units:
        tu = index.parse(str(out / unit), args=args)
        for c in tu.cursor.walk_preorder():
            if c.kind not in RECORDS or not c.is_definition():
                continue
            f = c.location.file
            if f is None or not f.name.startswith(root) or c.is_anonymous():
                continue
            key = (f.name, c.extent.end.offset - 1)
            if key in found:
                continue
            q = _qualified(c)
            if q is None or c.type.get_size() <= 0:
                continue
            if f.name not in texts:
                texts[f.name] = Path(f.name).read_bytes().split(b"\n")
            lines = texts[f.name]
            rows, seen_lines = [], set()
            for k in c.get_children():
                if k.kind != CK.FIELD_DECL or k.location.line in seen_lines:
                    continue
                seen_lines.add(k.location.line)   # one comment per line: its first field's
                m = OFFSET_COMMENT.search(lines[k.location.line - 1][k.location.column:])
                if not m or k.get_field_offsetof() < 0 or not k.spelling:
                    continue
                rows.append((k.spelling, int(m.group(1), 16), k.get_field_offsetof() // 8))
            if rows:
                found[key] = (q, rows)
    return found


def assert_offsets(units, out, args, report):
    """Writes a static_assert per commented field after its record's definition; returns the
    records whose fields differ from retail here (all of them by one constant are views of a larger
    object, whose comments give the outer offsets, and get no assertions)."""
    found = {}
    for r in _parallel(_offsets, units, out, args):
        found.update(r)
    by_file = collections.defaultdict(list)
    views, differ = [], []
    asserted = 0
    for (f, closing), (q, rows) in found.items():
        shifts = {want - here for _, want, here in rows}
        if len(shifts) == 1 and shifts != {0} and len(rows) > 1:
            views.append(q)     # every field off by one constant: a view of a larger object
            continue
        if shifts != {0}:
            # Reported, not asserted: the build would stop on it. It must go to zero by a patch.
            differ.append((f, q, [(n, w, h) for n, w, h in rows if w != h]))
            continue
        by_file[f].append((closing, q, rows))
    for f, items in by_file.items():
        data = Path(f).read_bytes()
        for closing, q, rows in sorted(items, reverse=True):
            # After the `;` that ends the record's declaration.
            end = data.find(b";", closing)
            if end < 0:
                continue
            checks = "".join(" static_assert(__builtin_offsetof(%s, %s) == 0x%X, \"retail offset\");"
                             % (q, n, w) for n, w, _ in rows)
            data = data[:end + 1] + checks.encode() + data[end + 1:]
            asserted += len(rows)
        Path(f).write_bytes(data)
    report["offset_asserts"] = asserted
    report["views"] = sorted(set(views))
    report["layout_differ"] = differ
    return differ
