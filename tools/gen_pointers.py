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
import re
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


GLOBAL_NAME = re.compile(r"^D_[0-9A-F]{8}$")


def pr_named(type_text, name):
    """A declaration of name from a type written without a name (`u8 []`, `Ptr32<T>`): the
    name goes before an array's bounds, after the type otherwise."""
    i = type_text.find("[")
    if i >= 0:
        return type_text[:i].rstrip() + " " + name + type_text[i:]
    return type_text + " " + name


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


def underlying(t):
    """A typedef's type, or None for any other type."""
    if t.kind != TK.TYPEDEF:
        return None
    d = t.get_declaration()
    return d.underlying_typedef_type if d is not None and d.kind in (CK.TYPEDEF_DECL, CK.TYPE_ALIAS_DECL) else None


def needs(t, storage):
    """Whether type t, in a storage or register position, changes under the rewrite. A typedef
    is kept as written unless a stored pointer hides in it (a typedef of a pointer, stored; a
    typedef of a pointer to pointers anywhere): typedefs are not rewritten, so `(Fn)x` stays a
    native cast and a parameter of type Fn a native pointer."""
    t = canon(t)
    k = t.kind
    u = underlying(t)
    if u is not None:
        return needs(u, storage)
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
    orig = t
    t = canon(t)
    k = t.kind
    u = underlying(t)
    if u is not None and needs(u, storage):
        s = pr(u, inner, storage)
        return ("const " if orig.is_const_qualified() and not s.startswith("const ") else "") + s
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
    s = leaf(t)
    # Qualifiers on the sugar canon() removed (`const u16`, an elaborated typedef) stay.
    words = s.split()
    for q in ("volatile", "const"):
        if getattr(orig, "is_%s_qualified" % q)() and q not in words:
            s = q + " " + s
    return _join(s, inner)


# ---------------------------------------------------------------------------------------------
# Edits


def _define_ranges(data):
    """Byte ranges of the #define directives in data, with their continuation lines."""
    import re
    return [(m.start(), m.end()) for m in re.finditer(rb"(?m)^[ \t]*#[ \t]*define\b(?:[^\n]*\\\n)*[^\n]*", data)]


class Edits:
    """Replacements and nested insertions per file."""

    def __init__(self):
        self.replace = collections.defaultdict(dict)    # file -> {(start, end): text}
        # file -> {(pos, order, text, pair)}: pair is shared by a wrap's opening and closing
        self.insert = collections.defaultdict(set)
        self.counts = collections.Counter()
        self.kinds = {}                                 # (file, start, end) or insertion -> kind
        self.unsupported = []

    def rep(self, f, start, end, text, kind):
        old = self.replace[f].get((start, end))
        if old is not None and old != text:
            self.unsupported.append("%s:%d: two rewrites of one place (%r, %r)" % (f, start, old, text))
            return
        if old is None and kind:
            self.counts[kind] += 1
            self.kinds[(f, start, end)] = kind
        self.replace[f][(start, end)] = text

    def wrap(self, f, start, end, before, after, depth, kind):
        # Opening text: outer expressions first at one position (smaller depth, smaller order);
        # closing text: inner expressions first.
        pair = (start, end, depth, before, after)
        opening = (start, -1000 + depth, before, pair)
        if opening not in self.insert[f]:
            self.counts[kind] += 1
            self.kinds[(f,) + opening] = kind
        self.insert[f].add(opening)
        self.insert[f].add((end, 1000 - depth, after, pair))

    def apply(self, root):
        files = set(self.replace) | set(self.insert)
        for f in sorted(files):
            path = Path(f)
            text = path.read_bytes().decode("utf-8", "replace")
            data = text.encode()
            defines = _define_ranges(data)

            def in_define(a, b):
                return any(a < e and b > s for s, e in defines)
            def line(a):
                return data.count(b"\n", 0, a) + 1
            for (a, b) in [k for k in self.replace[f] if in_define(*k)]:
                self.unsupported.append("%s:%d: inside a #define, not rewritten" % (f, line(a)))
                del self.replace[f][(a, b)]
            # A wrap is dropped whole when either half is inside a #define.
            dropped = {i[3] for i in self.insert[f] if in_define(i[0], i[0] + 1)}
            for pair in dropped:
                self.unsupported.append("%s:%d: inside a #define, not rewritten" % (f, line(pair[0])))
            self.insert[f] = {i for i in self.insert[f] if i[3] not in dropped}
            ops = []   # (pos, order, kind, end, text)
            spans = sorted(self.replace[f].items())
            for (a, b), t in spans:
                ops.append((a, 0, "r", b, t))
            for pos, order, t, _ in self.insert[f]:
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
        self.texts = {}          # file -> bytes, for char_at
        self.macro_starts = set()  # (file, offset) where a macro is expanded, in this unit
        self.rename = {}           # (file, offset) of a parameter -> its name in the signature
        self.global_defs = set()   # D_XXXXXXXX globals the C defines
        self.global_decls = []     # their declarations: (name, file, start, end, type, shared, line)

    def char_at(self, loc):
        f = loc.file
        if f is None:
            return ""
        data = self.texts.get(f.name)
        if data is None:
            data = self.texts[f.name] = Path(f.name).read_bytes()
        return chr(data[loc.offset]) if loc.offset < len(data) else ""

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
                    nm = self.rename.get((f, c.location.offset), nm)
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
        otoks = _tokens(operand)
        end = otoks[-1].extent.end.offset if otoks else c.extent.end.offset
        if end <= b or end > c.extent.end.offset or toks[-1].extent.end.offset != end:
            # Not one contiguous run of the file's own tokens: a cast made by a macro.
            if target_kind == TK.POINTER and operand_kind in INTEGERS or \
                    target_kind in INTEGERS and operand_kind in {TK.POINTER} | ARRAYS | FUNCS:
                self.edits.unsupported.append("%s:%d: a pointer/integer cast inside a macro" % (f, c.location.line))
            return
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

    # --- initialising stored pointers (Ptr32 has no constructors: port::P32 makes a game word)

    def _own_text(self, e):
        """The expression's source text when its extent is a plausible piece of the file's own
        text (short, balanced): read from the file, as tokenizing an extent can cost a whole
        table's tokens. None when the extent is a macro's or otherwise not trustworthy."""
        s, t = e.extent.start, e.extent.end
        if s.file is None or t.file is None or s.file.name != t.file.name:
            return None
        a, b = s.offset, t.offset
        if not 0 <= a < b or b - a > 2000:
            return None
        data = self.texts.get(s.file.name)
        if data is None:
            data = self.texts[s.file.name] = Path(s.file.name).read_bytes()
        text = data[a:b].decode("utf-8", "replace")
        code = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', '""', text)  # literals' contents aside
        if code.count("(") != code.count(")") or code.count("{") != code.count("}"):
            return None
        return text

    def _wrap_pointer_value(self, e, depth, kind):
        """port::P32(e) around a pointer-valued initialiser of a stored pointer; {} for a null
        constant (an aggregate's zero)."""
        f = e.location.file.name if e.location.file else None
        if f is None:
            return
        text = self._own_text(e)
        if (f, e.extent.start.offset) in self.macro_starts:
            text = None   # the value comes from a macro's body: a named patch's business
        if text is None:
            self.edits.unsupported.append("%s:%d: a stored pointer's initialiser inside a macro"
                                          % (f, e.location.line))
            return
        words = text.split()
        a, b = e.extent.start.offset, e.extent.end.offset
        if words in (["0"], ["NULL"], ["nullptr"]):
            self.edits.rep(f, a, b, "{}", kind)
            return
        self.edits.wrap(f, a, b, "port::P32(", ")", depth + 1, kind)

    def initializer(self, c, depth):
        """A stored pointer variable's initialiser: `Ptr32<T> g = port::P32(&x);`."""
        if c.type.get_canonical().kind != TK.POINTER:
            return
        kids = [k for k in c.get_children() if k.kind not in (CK.TYPE_REF, CK.TEMPLATE_REF,
                                                              CK.NAMESPACE_REF, CK.PARM_DECL)]
        if not kids or kids[-1].kind == CK.INIT_LIST_EXPR:
            return
        init = kids[-1]
        toks = _tokens(c)
        if not any(t.spelling == "=" for t in toks):
            return
        self._wrap_pointer_value(init, depth, "stored pointer initialisers")

    def init_list(self, c, depth):
        """The elements of a braced list that initialise stored pointers (a structure's pointer
        fields, an array of pointers: every pointer in a list is stored)."""
        elements = list(c.get_children())
        lt = c.type.get_canonical()
        if lt.kind == TK.RECORD:
            fields = list(lt.get_fields())
            if len(elements) > len(fields):
                fields = None    # braces elided: positions are not fields
            targets = [f.type for f in fields] if fields is not None else None
        elif lt.kind in ARRAYS:
            targets = [lt.element_type] * len(elements)
        else:
            targets = None
        for i, e in enumerate(elements):
            if e.kind == CK.INIT_LIST_EXPR:
                continue
            if targets is not None:
                target = targets[i].get_canonical()
                # A plain value for an aggregate initialises its first member (a union's, or a
                # structure's under brace elision), down to a scalar. A whole structure (a call
                # returning one) initialises the aggregate itself.
                whole = e.type.get_canonical().kind == TK.RECORD
                for _ in range(0 if whole else 8):
                    if target.kind != TK.RECORD:
                        break
                    first = next(iter(target.get_fields()), None)
                    if first is None:
                        break
                    target = first.type.get_canonical()
                pointer = target.kind == TK.POINTER
            else:
                # Braces elided, positions unknown: a pointer, a function, or an array that is not
                # a string literal (an array cannot initialise an array: it decays to a pointer).
                ek = e.type.get_canonical().kind
                pointer = ek in {TK.POINTER} | FUNCS or ek in ARRAYS and e.kind != CK.STRING_LITERAL
            if pointer:
                self._wrap_pointer_value(e, depth, "stored pointer initialisers")

    def record_global(self, c):
        """A global of the game: its definition, or a declaration with what replacing it needs."""
        name = c.spelling
        if not GLOBAL_NAME.match(name):
            return
        if c.is_definition():
            self.global_defs.add(name)
            return
        f = c.location.file.name
        start, end = c.extent.start.offset, c.extent.end.offset
        data = self.texts.get(f)
        if data is None:
            data = self.texts[f] = Path(f).read_bytes()
        j = end
        while j < len(data) and data[j:j + 1] in b" \t":
            j += 1
        shared = data[j:j + 1] == b","            # `extern s32 D_a, D_b;`: not one declaration
        if data[j:j + 1] == b";":
            j += 1
        try:
            type_text = pr(c.type, "", True)
        except Unsupported:
            type_text = None
        self.global_decls.append((name, f, start, j, type_text, shared, c.location.line))

    def member_initialisers(self, c, depth):
        """`: a(p)` in a constructor, for a member a that is a stored pointer: `a(port::P32(p))`."""
        kids = list(c.get_children())
        for i, k in enumerate(kids[:-1]):
            if k.kind != CK.MEMBER_REF or k.referenced is None:
                continue
            if k.referenced.type.get_canonical().kind != TK.POINTER:
                continue
            e = kids[i + 1]
            text = self._own_text(e)
            f = e.location.file.name if e.location.file else None
            if text is None or f is None:
                if f:
                    self.edits.unsupported.append("%s:%d: a member initialiser not rewritten" % (f, e.location.line))
                continue
            self.edits.wrap(f, e.extent.start.offset, e.extent.end.offset, "port::P32(", ")",
                            depth + 1, "stored pointer initialisers")

    def addressed_parameters(self, c):
        """A parameter whose address is taken is stored (its address reaches T** parameters, now
        Ptr32<T>*): the signature keeps the native pointer, under another name, and the body
        starts with a Ptr32 of the parameter's own name."""
        body = next((k for k in c.get_children() if k.kind == CK.COMPOUND_STMT), None)
        if body is None:
            return
        f = c.location.file.name
        decls = []
        for p in c.get_children():
            if p.kind != CK.PARM_DECL or not p.spelling:
                continue
            if (p.location.file.name, p.location.offset) not in self.addressed:
                continue
            if p.type.get_canonical().kind != TK.POINTER:
                continue
            name = p.spelling
            try:
                decl = pr(p.type, name, True)
            except Unsupported as e:
                self.edits.unsupported.append("%s:%d: %s" % (f, p.location.line, e))
                continue
            if needs(p.type, False):
                self.rename[(f, p.location.offset)] = "port_arg_" + name   # with its new type
            else:
                self.edits.rep(f, p.location.offset, p.location.offset + len(name), "port_arg_" + name,
                               "addressed parameters")
            decls.append(" %s = port::P32(port_arg_%s);" % (decl, name))
        if decls:
            brace = body.extent.start.offset
            self.edits.wrap(f, brace + 1, brace + 1, "".join(decls), "", 0, "addressed parameters")

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
        if c.type.get_canonical().kind != TK.POINTER:
            return
        kids = list(c.get_children())
        if len(kids) != 3:
            return
        f = c.location.file.name
        for arm in kids[1:]:
            if [t.spelling for t in _tokens(arm)] in (["0"], ["NULL"], ["nullptr"]):
                continue  # a null pointer constant: `+(0)` would be the integer 0
            if arm.type.get_canonical().kind in {TK.POINTER} | ARRAYS | FUNCS:
                atoks = _tokens(arm)
                if not atoks or atoks[0].extent.start.offset != arm.extent.start.offset or \
                        atoks[-1].extent.end.offset != arm.extent.end.offset:
                    continue
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
            if arg.type.get_canonical().kind in {TK.POINTER} | ARRAYS:
                atoks = _tokens(arg)
                if not atoks or atoks[0].extent.start.offset != arg.extent.start.offset or \
                        atoks[-1].extent.end.offset != arg.extent.end.offset or \
                        atoks[0].extent.start.offset < c.extent.start.offset or \
                        atoks[-1].extent.end.offset > c.extent.end.offset:
                    continue
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
                # Only the operator's first character is needed: read it from the file, as
                # tokenizing a unary operator's extent can cost a whole table's tokens.
                if self.char_at(c.extent.start) == "&":
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
                if not local:
                    self.record_global(ch)
                addressed = (ch.location.file.name, ch.location.offset) in self.addressed
                self.declaration(ch, not local or addressed)
                if not local or addressed:
                    self.initializer(ch, depth)
            elif k == CK.PARM_DECL:
                parent = ch.semantic_parent
                if parent is not None and parent.kind in (CK.FUNCTION_DECL, CK.CXX_METHOD, CK.CONSTRUCTOR,
                                                          CK.FUNCTION_TEMPLATE):
                    self.declaration(ch, False)
            elif k in (CK.FUNCTION_DECL, CK.CXX_METHOD, CK.FUNCTION_TEMPLATE):
                self.function_result(ch)
                self.addressed_parameters(ch)
            elif k == CK.INIT_LIST_EXPR:
                self.init_list(ch, depth)
            elif k == CK.CONSTRUCTOR:
                self.member_initialisers(ch, depth)
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


def _collect(job):
    """A worker: the edits of a share of the units, as plain data."""
    units, out, args = job
    index = ci.Index.create()
    edits = Edits()
    walker = Walker(edits, out)
    diagnostics = 0
    for unit in units:
        tu = index.parse(str(out / unit), args=args,
                         options=ci.TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD)
        # Function-like macro calls only (`PROP(id, n)`): an object-like macro that stands for a
        # name (names.h's readable names) covers just that name and is a plain value.
        walker.macro_starts = {(c.location.file.name, c.extent.start.offset)
                               for c in tu.cursor.get_children()
                               if c.kind == CK.MACRO_INSTANTIATION and c.location.file is not None
                               and c.extent.end.offset - c.extent.start.offset > len(c.spelling)}
        fatal = [d for d in tu.diagnostics if d.severity >= ci.Diagnostic.Fatal]
        if fatal:
            raise SystemExit("gen: %s does not parse: %s" % (unit, fatal[0]))
        diagnostics += sum(1 for d in tu.diagnostics if d.severity >= ci.Diagnostic.Error)
        cursors = list(walker.top_level(tu, str(out / unit)))
        walker.collect_addressed(cursors)
        walker.walk_cursors(cursors)
        walker.flush_groups()
    return ({f: dict(r) for f, r in edits.replace.items()}, {f: set(i) for f, i in edits.insert.items()},
            edits.unsupported, diagnostics, edits.kinds, walker.global_defs, walker.global_decls)


def rewrite(units, out, args, report, jobs=None):
    """Rewrites the pointer declarations of every file the units reach under out. The units are
    shared among worker processes; their edits are merged, and one place rewritten two different
    ways is reported, never applied."""
    import multiprocessing
    import os
    jobs = jobs or min(len(units), os.cpu_count() or 1) or 1
    # Large units first, dealt round-robin, so the workers finish together.
    ordered = sorted(units, key=lambda u: -(out / u).stat().st_size)
    shares = [ordered[i::jobs] for i in range(jobs)]
    if jobs == 1:
        results = [_collect((shares[0], out, args))]
    else:
        with multiprocessing.get_context("spawn").Pool(jobs) as pool:
            results = pool.map(_collect, [(s, out, args) for s in shares])
    edits = Edits()
    diagnostics = 0
    kinds = {}
    global_defs, global_decls = set(), []
    for replace, insert, unsupported, diag, k, gdefs, gdecls in results:
        kinds.update(k)
        global_defs |= gdefs
        global_decls += gdecls
        for f, reps in replace.items():
            for (a, b), text in reps.items():
                edits.rep(f, a, b, text, None)
        for f, ins in insert.items():
            edits.insert[f] |= ins
        edits.unsupported += [u for u in unsupported if u not in edits.unsupported]
        diagnostics += diag
    # The counts, by kind, of the distinct edits after merging.
    edits.counts = collections.Counter()
    for f, reps in edits.replace.items():
        for (a, b) in reps:
            if (f, a, b) in kinds:
                edits.counts[kinds[(f, a, b)]] += 1
    for f, ins in edits.insert.items():
        for item in ins:
            if (f,) + item in kinds:
                edits.counts[kinds[(f,) + item]] += 1
    # Globals the C declares and nothing defines: each declaration becomes a macro of the same
    # name over port::RetailRef (port/retail_data.h), with that declaration's own type.
    undefined = {}
    # A declaration statement can name several globals (`extern u8 A[], B[];`): its members share
    # the statement's start. Such a statement is replaced whole, each member by itself.
    statements = collections.defaultdict(dict)
    for name, f, start, end, type_text, shared, line in global_decls:
        statements[(f, start)][name] = (end, type_text, line)
    for (f, start), members in statements.items():
        if all(n in global_defs for n in members):
            continue
        data = Path(f).read_bytes()
        end = max(e for e, _, _ in members.values())
        while end < len(data) and data[end - 1:end] != b";":
            end += 1                       # through the statement's own semicolon
        if any(tt is None for _, tt, _ in members.values()):
            line = min(l for _, _, l in members.values())
            edits.unsupported.append("%s:%d: a declaration of a global nothing defines, with a type "
                                     "the tool cannot write" % (f, line))
            continue
        text = ""
        for name, (_, type_text, line) in sorted(members.items(), key=lambda m: m[1][0]):
            if name in global_defs:
                text += "\nextern %s;" % pr_named(type_text, name)
            else:
                undefined.setdefault(name, []).append((f, line))
                text += "\n#undef %s\n#define %s (port::RetailRef< %s >(0x%08XU))" % (
                    name, name, type_text, int(name[2:], 16))
        text += "\n"
        reps = edits.replace.get(f, {})
        for key in [k for k in reps if start <= k[0] < end]:
            del reps[key]
        edits.insert[f] = {i for i in edits.insert.get(f, set()) if not (start <= i[0] < end)}
        edits.rep(f, start, end, text, "globals nothing defines")
    report["undefined_globals"] = undefined
    report["global_defs"] = global_defs
    files = edits.apply(out)
    report["files_changed"] = len(files)
    report["counts"] = dict(edits.counts)
    report["unsupported"] = edits.unsupported
    report["parse_errors_before"] = diagnostics
    return edits
