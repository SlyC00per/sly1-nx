# SPDX-License-Identifier: AGPL-3.0-or-later
"""The pointer rewrite of tools/gen.py (docs/DESIGN.md, section 4), with libclang.

The game keeps retail's layouts, so a pointer the game stores in memory is a 4-byte Ptr32<T>
(port/include/port/ptr32.h). This stage changes declarations, not expressions:

  storage   a structure field, a global or static, an array element, what a pointer or a
            reference refers to, a typedef, a local whose address is taken
            -> a pointer here is Ptr32<T>
  register  a local, a parameter, a function's result, a cast's result
            -> a pointer here stays T*, and what it points at is storage

so `T**` becomes `Ptr32<T>*` and a function-pointer field `Ptr32<R (A...)>`. Expressions keep
their text except:

  (int)p, (sptr)p ...     a pointer made an integer is its game address: port::GameAddrOf(p)
  (T*)n                   an integer made a pointer: port::FromGameAddr<T>(n)
  c ? p : q               pointer arms are made native (+p), so a Ptr32 and a T* agree
  printf("%s", p)         pointer arguments in a variadic position are made native (+p)
  sizeof(T*)              the size of a stored pointer: sizeof(Ptr32<T>)

Edits are replacements of a declaration's type and insertions around an expression; insertions
at one position nest by the depth of their expression, so edits never overwrite each other.
"""

import collections
from pathlib import Path

import clang.cindex as ci

CK = ci.CursorKind
TK = ci.TypeKind

ARRAYS = {TK.CONSTANTARRAY, TK.INCOMPLETEARRAY, TK.VARIABLEARRAY, TK.DEPENDENTSIZEDARRAY}
REFS = {TK.LVALUEREFERENCE, TK.RVALUEREFERENCE}
FUNCS = {TK.FUNCTIONPROTO, TK.FUNCTIONNOPROTO}
INTEGERS = {TK.BOOL, TK.CHAR_U, TK.UCHAR, TK.CHAR16, TK.CHAR32, TK.USHORT, TK.UINT, TK.ULONG,
            TK.ULONGLONG, TK.UINT128, TK.CHAR_S, TK.SCHAR, TK.WCHAR, TK.SHORT, TK.INT, TK.LONG,
            TK.LONGLONG, TK.INT128, TK.ENUM}
STORAGE_WORDS = {"extern", "static", "inline", "__inline__", "__inline", "register", "mutable",
                 "typedef", "virtual", "friend", "constexpr"}
DECLARATOR_STOP = {"=", ";", ",", "{", ":", "__attribute__"}


class Unsupported(Exception):
    pass


def canon(t):
    """The type with sugar that hides its kind removed (elaborated and unexposed wrappers), but
    typedefs kept: a typedef is a leaf, rewritten at its own declaration."""
    while t.kind in (TK.ELABORATED, TK.UNEXPOSED, getattr(TK, "ATTRIBUTED", TK.UNEXPOSED)):
        if t.kind == TK.ELABORATED:
            nt = t.get_named_type()
            if nt.kind == TK.TYPEDEF:
                return nt
            if nt.kind in (TK.RECORD, TK.ENUM):
                return t
            t = nt
        else:
            c = t.get_canonical()
            if c.kind == t.kind:
                return t
            if c.kind not in (TK.POINTER,) and c.kind not in ARRAYS | REFS | FUNCS:
                return t
            t = c
    return t


def needs(t, storage):
    """Whether type t, in a storage or register position, changes under the rewrite."""
    t = canon(t)
    k = t.kind
    if k == TK.POINTER:
        return True if storage else needs(t.get_pointee(), True)
    if k in REFS:
        return needs(t.get_pointee(), True)
    if k in ARRAYS:
        return needs(t.element_type, True)
    if k == TK.FUNCTIONPROTO:
        return needs(t.get_result(), False) or any(needs(a, False) for a in t.argument_types())
    if k == TK.FUNCTIONNOPROTO:
        return needs(t.get_result(), False)
    return False


def _join(spec, inner):
    return spec + (" " + inner if inner else "")


def _cv(t):
    return ("const " if t.is_const_qualified() else "") + ("volatile " if t.is_volatile_qualified() else "")


def leaf(t):
    s = t.spelling
    if "(unnamed" in s or "(anonymous" in s or "(lambda" in s:
        raise Unsupported("an unnamed type: %s" % s)
    return s


def pr(t, inner, storage):
    """The declaration of `inner` with type t, under the rewrite."""
    t = canon(t)
    k = t.kind
    if k == TK.POINTER:
        p = t.get_pointee()
        if storage:
            return _join(_cv(t) + "Ptr32<%s>" % pr(p, "", True), inner)
        q = (" const" if t.is_const_qualified() else "") + (" volatile" if t.is_volatile_qualified() else "")
        decl = "*" + q.strip() + (" " if q else "") + inner if inner else "*" + q
        if canon(p).kind in FUNCS | ARRAYS:
            decl = "(" + decl + ")"
        return pr(p, decl, True)
    if k in REFS:
        p = t.get_pointee()
        decl = ("&" if k == TK.LVALUEREFERENCE else "&&") + inner
        if canon(p).kind in FUNCS | ARRAYS:
            decl = "(" + decl + ")"
        return pr(p, decl, True)
    if k in ARRAYS:
        n = t.element_count if k == TK.CONSTANTARRAY else None
        return pr(t.element_type, inner + ("[%d]" % n if n is not None else "[]"), True)
    if k in FUNCS:
        args = [pr(a, "", False) for a in t.argument_types()] if k == TK.FUNCTIONPROTO else []
        if k == TK.FUNCTIONPROTO and t.is_function_variadic():
            args.append("...")
        return pr(t.get_result(), inner + "(" + ", ".join(args) + ")", False)
    return _join(leaf(t), inner)


# ---------------------------------------------------------------------------------------------
# Edits


class Edits:
    """Replacements and nested insertions per file."""

    def __init__(self):
        self.replace = collections.defaultdict(dict)    # file -> {(start, end): text}
        self.insert = collections.defaultdict(set)      # file -> {(pos, order, text)}
        self.counts = collections.Counter()
        self.unsupported = []

    def rep(self, f, start, end, text, kind):
        old = self.replace[f].get((start, end))
        if old is not None and old != text:
            self.unsupported.append("%s:%d: two rewrites of one place (%r, %r)" % (f, start, old, text))
            return
        if old is None:
            self.counts[kind] += 1
        self.replace[f][(start, end)] = text

    def wrap(self, f, start, end, before, after, depth, kind):
        # Opening text: outer expressions first at one position (smaller depth, smaller order);
        # closing text: inner expressions first.
        if (start, -1000 + depth, before) not in self.insert[f]:
            self.counts[kind] += 1
        self.insert[f].add((start, -1000 + depth, before))
        self.insert[f].add((end, 1000 - depth, after))

    def apply(self, root):
        files = set(self.replace) | set(self.insert)
        for f in sorted(files):
            path = Path(f)
            text = path.read_bytes().decode("utf-8", "replace")
            data = text.encode()
            ops = []   # (pos, order, kind, end, text)
            spans = sorted(self.replace[f].items())
            for (a, b), t in spans:
                ops.append((a, 0, "r", b, t))
            for pos, order, t in self.insert[f]:
                ops.append((pos, order, "i", pos, t))
            # Check replacements do not overlap each other or contain an insertion point.
            last = -1
            for (a, b), t in spans:
                if a < last:
                    self.unsupported.append("%s:%d: overlapping rewrites" % (f, a))
                last = max(last, b)
            ops.sort(key=lambda o: (o[0], o[1]))
            out, pos = [], 0
            for at, order, kind, end, t in ops:
                if at < pos:
                    if kind == "i":
                        self.unsupported.append("%s:%d: an insertion inside a rewritten type" % (f, at))
                    continue
                out.append(data[pos:at])
                out.append(t.encode())
                pos = end if kind == "r" else at
            out.append(data[pos:])
            new = b"".join(out)
            if new != data:
                path.write_bytes(new)
        return files


# ---------------------------------------------------------------------------------------------
# Walking a translation unit


def _tokens(cursor):
    return list(cursor.get_tokens())


def _decl_regions(cursor, toks):
    """(type_start, name_index, end) offsets of a declaration's type and declarator in toks."""
    loc = cursor.location.offset
    name = next((i for i, t in enumerate(toks) if t.extent.start.offset == loc), None)
    first = 0
    while first < len(toks) and (toks[first].spelling in STORAGE_WORDS or toks[first].spelling == "__attribute__"):
        if toks[first].spelling == "__attribute__":
            d, j = 0, first + 1
            while j < len(toks):
                d += toks[j].spelling == "("
                d -= toks[j].spelling == ")"
                j += 1
                if d == 0:
                    break
            first = j
        else:
            first += 1
    if name is None:  # an unnamed parameter: the whole extent is its type
        return first, None, len(toks)
    d0 = 0
    for t in toks[first:name]:
        d0 += t.spelling in ("(", "[")
        d0 -= t.spelling in (")", "]")
    d, j = d0, name + 1
    while j < len(toks):
        s = toks[j].spelling
        if d <= 0 and (s in DECLARATOR_STOP or s == ")" and d == 0):
            break
        if s in ("(", "["):
            d += 1
        elif s in (")", "]"):
            d -= 1
            if d < 0:
                break
        j += 1
    return first, name, j


class Walker:
    def __init__(self, edits, root):
        self.edits = edits
        self.root = str(root)
        self.addressed = set()   # (file, offset) of locals whose address is taken
        self.groups = collections.defaultdict(list)
        self.done = set()        # header declarations already walked in an earlier unit

    def ours(self, c):
        f = c.location.file
        return f is not None and f.name.startswith(self.root) and c.extent.start.file is not None \
            and c.extent.start.file.name == f.name

    # --- declarations

    def declaration(self, c, storage):
        if not needs(c.type if c.kind != CK.TYPEDEF_DECL else c.underlying_typedef_type, storage):
            return
        group = self.groups[(c.location.file.name, c.extent.start.offset)]
        if all(m.location.offset != c.location.offset for m, _ in group):
            group.append((c, storage))

    def flush_groups(self):
        for (f, start), members in self.groups.items():
            members.sort(key=lambda m: m[0].location.offset)
            first = members[0][0]
            try:
                toks = _tokens(first)
                ts, name, end = _decl_regions(first, toks)
                if any(t.spelling == "{" for t in toks[ts:name or 0]):
                    raise Unsupported("a declaration that defines its type")
                prefix = " ".join(t.spelling for t in toks[:ts])
                for i, (c, storage) in enumerate(members):
                    ctoks = _tokens(c)
                    _, cname, cend = _decl_regions(c, ctoks)
                    t = c.underlying_typedef_type if c.kind == CK.TYPEDEF_DECL else c.type
                    nm = ctoks[cname].spelling if cname is not None else ""
                    # keep what follows the name inside the declarator only through the type
                    text = pr(t, nm, storage)
                    if i == 0:
                        a = toks[ts].extent.start.offset
                        b = ctoks[cend - 1].extent.end.offset
                    else:
                        # from the comma before this declarator
                        j = cname
                        while j > 0 and ctoks[j].spelling != ",":
                            j -= 1
                        a = ctoks[j].extent.start.offset
                        b = ctoks[cend - 1].extent.end.offset
                        text = "; " + (prefix + " " if prefix else "") + text
                    self.edits.rep(f, a, b, text, "declarations")
            except Unsupported as e:
                self.edits.unsupported.append("%s:%d: %s" % (f, first.location.line, e))
        self.groups.clear()

    def function_result(self, c):
        rt = c.result_type
        if not needs(rt, False):
            return
        toks = _tokens(c)
        loc = c.location.offset
        name = next((i for i, t in enumerate(toks) if t.extent.start.offset == loc), None)
        if name is None:
            return
        first = 0
        while first < name and toks[first].spelling in STORAGE_WORDS:
            first += 1
        # a qualified name (Class::Name) starts earlier
        k = name
        while k - 2 >= first and toks[k - 1].spelling == "::":
            k -= 2
        try:
            text = pr(rt, "", False) + " "
        except Unsupported as e:
            self.edits.unsupported.append("%s:%d: %s" % (c.location.file.name, c.location.line, e))
            return
        if any(t.spelling == "(" for t in toks[first:k]):
            self.edits.unsupported.append("%s:%d: a function returning a function pointer"
                                          % (c.location.file.name, c.location.line))
            return
        self.edits.rep(c.location.file.name, toks[first].extent.start.offset,
                       toks[k].extent.start.offset, text, "function results")

    # --- expressions

    def cast(self, c, depth):
        toks = _tokens(c)
        if not toks or toks[0].spelling != "(":
            return
        d, j = 0, 0
        while j < len(toks):
            d += toks[j].spelling == "("
            d -= toks[j].spelling == ")"
            if d == 0:
                break
            j += 1
        if j >= len(toks):
            return
        f = c.location.file.name
        target = canon(c.type)
        target_kind = c.type.get_canonical().kind
        operand = [ch for ch in c.get_children() if ch.kind != CK.TYPE_REF and ch.kind != CK.TEMPLATE_REF]
        operand = operand[-1] if operand else None
        if operand is None:
            return
        ot = canon(operand.type)
        operand_kind = operand.type.get_canonical().kind
        a, b = toks[0].extent.start.offset, toks[j].extent.end.offset
        end = c.extent.end.offset
        if target_kind == TK.POINTER and operand_kind in INTEGERS:
            otoks = [t.spelling for t in _tokens(operand)]
            if otoks in (["0"], ["0", ")"]) or otoks == ["NULL"]:
                return
            try:
                pointee = pr(c.type.get_canonical().get_pointee() if target.kind != TK.POINTER
                             else target.get_pointee(), "", True)
            except Unsupported as e:
                self.edits.unsupported.append("%s:%d: %s" % (f, c.location.line, e))
                return
            self.edits.rep(f, a, b, "", "integer to pointer")
            self.edits.wrap(f, b, end, "port::FromGameAddr<%s>((u32)(" % pointee, "))", depth,
                            "integer to pointer")
            return
        if target_kind in INTEGERS and operand_kind in {TK.POINTER} | ARRAYS | FUNCS:
            self.edits.wrap(f, b, end, "port::GameAddrOf(", ")", depth, "pointer to integer")
            return
        if needs(c.type, False):
            try:
                self.edits.rep(f, toks[1].extent.start.offset, toks[j - 1].extent.end.offset,
                               pr(c.type, "", False), "cast types")
            except Unsupported as e:
                self.edits.unsupported.append("%s:%d: %s" % (f, c.location.line, e))

    def sizeof(self, c):
        toks = _tokens(c)
        if len(toks) < 4 or toks[0].spelling not in ("sizeof", "__alignof__", "alignof") or toks[1].spelling != "(":
            return
        if toks[-1].spelling != ")" or toks[-2].spelling != "*":
            return
        if any(ch.kind not in (CK.TYPE_REF,) for ch in c.get_children()):
            return
        f = c.location.file.name
        self.edits.rep(f, toks[2].extent.start.offset, toks[-1].extent.start.offset,
                       "Ptr32<void>", "sizeof of a pointer")

    def conditional(self, c, depth):
        if canon(c.type).kind != TK.POINTER:
            return
        kids = list(c.get_children())
        if len(kids) != 3:
            return
        f = c.location.file.name
        for arm in kids[1:]:
            if [t.spelling for t in _tokens(arm)] in (["0"], ["NULL"], ["nullptr"]):
                continue  # a null pointer constant: `+(0)` would be the integer 0
            if canon(arm.type).kind in {TK.POINTER} | ARRAYS | FUNCS:
                self.edits.wrap(f, arm.extent.start.offset, arm.extent.end.offset, "+(", ")",
                                depth + 1, "pointer arms of ?:")

    def call(self, c, depth):
        ref = c.referenced
        if ref is None or ref.kind not in (CK.FUNCTION_DECL, CK.CXX_METHOD):
            return
        ft = ref.type
        if ft.kind != TK.FUNCTIONPROTO or not ft.is_function_variadic():
            return
        fixed = len(list(ft.argument_types()))
        args = list(c.get_arguments())
        f = c.location.file.name
        for arg in args[fixed:]:
            if canon(arg.type).kind in {TK.POINTER} | ARRAYS:
                self.edits.wrap(f, arg.extent.start.offset, arg.extent.end.offset, "+(", ")",
                                depth + 1, "variadic pointer arguments")

    # --- the walk

    def top_level(self, tu, unit_file):
        """The top-level cursors of the game's files not yet done: a header's declarations are
        the same in every unit that includes it, so each is walked once."""
        for ch in tu.cursor.get_children():
            if not self.ours(ch):
                continue
            f = ch.location.file.name
            if f != unit_file:
                key = (f, ch.extent.start.offset, ch.extent.end.offset, ch.kind)
                if key in self.done:
                    continue
                self.done.add(key)
            yield ch

    def collect_addressed(self, cursors):
        stack = list(cursors)
        while stack:
            c = stack.pop()
            if c.kind == CK.UNARY_OPERATOR:
                toks = _tokens(c)
                if toks and toks[0].spelling == "&":
                    kids = list(c.get_children())
                    if kids and kids[0].kind == CK.DECL_REF_EXPR and kids[0].referenced is not None:
                        r = kids[0].referenced
                        if r.kind in (CK.VAR_DECL, CK.PARM_DECL) and canon(r.type).kind == TK.POINTER:
                            self.addressed.add((r.location.file.name, r.location.offset))
            stack.extend(ch for ch in c.get_children() if self.ours(ch))

    def walk_cursors(self, cursors):
        for ch in cursors:
            self.walk_one(ch, 0, False)

    def walk(self, c, depth=0, in_func=False):
        for ch in c.get_children():
            self.walk_one(ch, depth, in_func)

    def walk_one(self, ch, depth, in_func):
        if True:
            k = ch.kind
            if not self.ours(ch):
                if k in (CK.NAMESPACE, CK.LINKAGE_SPEC, CK.UNEXPOSED_DECL):
                    self.walk(ch, depth, in_func)
                return
            if k == CK.FIELD_DECL:
                self.declaration(ch, True)
            elif k == CK.VAR_DECL:
                local = in_func and ch.storage_class not in (ci.StorageClass.STATIC, ci.StorageClass.EXTERN)
                addressed = (ch.location.file.name, ch.location.offset) in self.addressed
                self.declaration(ch, not local or addressed)
            elif k == CK.PARM_DECL:
                parent = ch.semantic_parent
                if parent is not None and parent.kind in (CK.FUNCTION_DECL, CK.CXX_METHOD, CK.CONSTRUCTOR,
                                                          CK.FUNCTION_TEMPLATE):
                    addressed = (ch.location.file.name, ch.location.offset) in self.addressed
                    self.declaration(ch, addressed)
            elif k in (CK.TYPEDEF_DECL, CK.TYPE_ALIAS_DECL):
                self.declaration(ch, True)
            elif k in (CK.FUNCTION_DECL, CK.CXX_METHOD, CK.FUNCTION_TEMPLATE):
                self.function_result(ch)
            elif k == CK.CSTYLE_CAST_EXPR:
                self.cast(ch, depth)
            elif k == CK.CXX_UNARY_EXPR:
                self.sizeof(ch)
            elif k == CK.CONDITIONAL_OPERATOR:
                self.conditional(ch, depth)
            elif k == CK.CALL_EXPR:
                self.call(ch, depth)
            if k in (CK.PARM_DECL,) and ch.semantic_parent is not None and \
                    ch.semantic_parent.kind not in (CK.FUNCTION_DECL, CK.CXX_METHOD, CK.CONSTRUCTOR):
                return  # a function-pointer type's parameters belong to its declaration
            body = in_func or k in (CK.FUNCTION_DECL, CK.CXX_METHOD, CK.CONSTRUCTOR, CK.DESTRUCTOR,
                                    CK.FUNCTION_TEMPLATE, CK.LAMBDA_EXPR)
            self.walk(ch, depth + 1, body)


def rewrite(units, out, args, report):
    """Rewrites the pointer declarations of every file the units reach under out."""
    index = ci.Index.create()
    edits = Edits()
    walker = Walker(edits, out)
    diagnostics = 0
    for unit in units:
        tu = index.parse(str(out / unit), args=args)
        fatal = [d for d in tu.diagnostics if d.severity >= ci.Diagnostic.Fatal]
        if fatal:
            raise SystemExit("gen: %s does not parse: %s" % (unit, fatal[0]))
        errors = [d for d in tu.diagnostics if d.severity >= ci.Diagnostic.Error]
        diagnostics += len(errors)
        cursors = list(walker.top_level(tu, str(out / unit)))
        walker.collect_addressed(cursors)
        walker.walk_cursors(cursors)
        walker.flush_groups()
    files = edits.apply(out)
    report["files_changed"] = len(files)
    report["counts"] = dict(edits.counts)
    report["unsupported"] = edits.unsupported
    report["parse_errors_before"] = diagnostics
    return edits
