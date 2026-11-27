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
        self.global_defs = {}      # D_XXXXXXXX globals the C defines -> their size in bytes
        self.global_inits = []     # (file, name, its initial value's name), per definition moved
        self.global_kept = set()   # globals left as objects of their own (see define_global)
        self.functions = []        # (file, func_XXXXXXXX) per game function the file defines
        self.result_kinds = []     # (func_XXXXXXXX, kind of result, definition?, file, line)
        self.declarations = []     # (func_XXXXXXXX, kind, file, where, adapter details or None)
        self.fn_adapters = {}      # (file, function, cast type) -> its adapter's name
        self.address_params = set()          # (function, parameter count, index) used only as addresses
        self.address_param_cursors = set()   # those parameters' (file, offset) in their definitions
        self.param_sites = []                # (key, file, start, end) of every declared parameter's type
        self.call_args = []                  # (key, file, start, end) of direct calls' integer arguments
        self.rooms = {}            # locals viewed as larger types: (file, offset) -> (cursor, [needs])
        self.room_plans = {}       # the same as plain data, for the merge (resolve_rooms)
        self.sizeof_locals = []    # (local's key, file, start, end, its size) of each sizeof(local)
        self.record_sizes = {}     # every record defined in the files walked: name -> size
        self.top_cursors = []      # the unit's top-level declarations (function_cast)
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
        self.function_cast(c, operand, f)
        self.roomy_cast(c, operand)
        if target_kind == TK.POINTER and operand_kind in INTEGERS and self.is_address_parameter(operand):
            return   # `(T*)a2`, a2 now a void* (address_parameters): a pointer cast as written
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
            if target.kind != TK.POINTER and c.type.get_canonical().get_pointee().kind in FUNCS and \
                    target.get_declaration().kind == CK.TYPEDEF_DECL:
                # (a function pointer's typedef: its own name, so that its parameter types are
                # the typedef's on every target, where `long` and `long long` may differ)
                pointee = "std::remove_pointer<%s>::type" % target.spelling
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

    def function_cast(self, c, operand, f):
        """A function cast to a function pointer type whose result or a parameter disagrees with
        the function's own, pointer against integer (`(PropertyTarget)EnsureTimerRecord`): a call
        through it would pass a 64-bit pointer where the function reads a 32-bit word, or the other
        way round. The cast takes an adapter of the file's own instead, which converts between
        game address and pointer and calls the function (written before the declaration that
        holds the cast)."""
        pt = c.type.get_canonical()
        if pt.kind != TK.POINTER or pt.get_pointee().kind != TK.FUNCTIONPROTO:
            return
        e = operand
        while e is not None and e.kind in (CK.UNEXPOSED_EXPR, CK.PAREN_EXPR, CK.UNARY_OPERATOR):
            kids = list(e.get_children())
            e = kids[0] if len(kids) == 1 else None
        if e is None or e.kind != CK.DECL_REF_EXPR or e.referenced is None or \
                e.referenced.kind != CK.FUNCTION_DECL:
            return
        fn = e.referenced
        want = pt.get_pointee()
        have = fn.type.get_canonical()
        if have.kind != TK.FUNCTIONPROTO:
            return
        wanted = list(want.argument_types())
        # A class result the cast passes the place of, first, as the PS2 does: one with its own
        # copy or destructor always; any other record too when the cast says so (one parameter
        # more, a pointer first). MultiplyMatrices returns a Mat4A, which ARM64 returns through x8:
        # the place the cast passed went unwritten, and the result went where x8 pointed.
        record_by_cast = have.get_result().get_canonical().kind == TK.RECORD and wanted and \
            wanted[0].get_canonical().kind == TK.POINTER
        if (returned_in_place(have.get_result()) or record_by_cast) and len(wanted) == \
                len(list(have.argument_types())) + 1:
            self.in_place_cast(c, e, fn, have, f)
            return

        def kind(ty):
            k = ty.get_canonical().kind
            return ("pointer" if k == TK.POINTER else "integer" if k in INTEGERS else
                    "float" if k in (TK.FLOAT, TK.DOUBLE) else "void" if k == TK.VOID else "other")
        cast_args = list(want.argument_types())
        fn_args = list(have.argument_types())
        pairs = [(want.get_result(), have.get_result())] + list(zip(cast_args, fn_args))
        # A result that disagrees either way, or a pointer the cast passes where the function reads
        # a word, needs converting. A word the cast's type passes where the function takes a
        # pointer does not: the method tables' types are placeholders (PathM02's s32 slots, called
        # through each caller's own cast with pointers), and converting there would cut the pointers.
        clash = {kind(pairs[0][0]), kind(pairs[0][1])} == {"pointer", "integer"} or \
            any(kind(a) == "pointer" and kind(b) == "integer" for a, b in pairs[1:])
        if not clash:
            return
        where = "%s:%d" % (f, c.location.line)
        # Any other disagreement (a float where the function has a pointer: MIPS passes them in
        # different registers) is a register pun the adapter cannot follow by position.
        if any(kind(a) != kind(b) and {kind(a), kind(b)} != {"pointer", "integer"}
               and not (a is pairs[0][0] and kind(a) == "void") for a, b in pairs):
            self.edits.unsupported.append("%s: a cast of %s that also mixes floats or other kinds, not "
                                          "adapted" % (where, fn.spelling))
            return
        if want.is_function_variadic() or have.is_function_variadic() or len(cast_args) < len(fn_args):
            self.edits.unsupported.append("%s: a cast of %s to a function type that disagrees with it, "
                                          "not adapted (variadic or fewer arguments)" % (where, fn.spelling))
            return
        top = next((t for t in self.top_cursors if t.location.file and t.location.file.name == f
                    and t.extent.start.offset <= c.extent.start.offset < t.extent.end.offset), None)
        if top is None:
            self.edits.unsupported.append("%s: a cast of %s that disagrees with it, outside any declaration"
                                          % (where, fn.spelling))
            return
        try:
            c_args = [pr(a, "", False) for a in cast_args]
            f_args = [pr(a, "", False) for a in fn_args]
            c_res = pr(want.get_result(), "", False)
        except Unsupported as ex:
            self.edits.unsupported.append("%s: %s" % (where, ex))
            return
        key = (f, fn.spelling, want.spelling)
        name = self.fn_adapters.get(key)
        if name is None:
            name = "port_fncast_%s_%d" % (fn.spelling, len(self.fn_adapters))
            self.fn_adapters[key] = name
            args = []
            for i in range(len(fn_args)):
                ck, fk = kind(cast_args[i]), kind(fn_args[i])
                if ck == "pointer" and fk == "integer":
                    args.append("(%s)port::GameAddrOf(a%d)" % (f_args[i], i))
                elif ck == "integer" and fk == "pointer":
                    args.append("(%s)(uptr)a%d" % (f_args[i], i))   # passed on as it came
                else:
                    args.append("(%s)a%d" % (f_args[i], i))
            call = "%s(%s)" % (fn.spelling, ", ".join(args))
            rk_c, rk_f = kind(want.get_result()), kind(have.get_result())
            if want.get_result().get_canonical().kind == TK.VOID:
                body = "%s;" % call
            elif rk_c == "pointer" and rk_f == "integer":
                body = "return (%s)port::Native((u32)%s);" % (c_res, call)
            elif rk_c == "integer" and rk_f == "pointer":
                body = "return (%s)port::GameAddr((const void*)%s);" % (c_res, call)
            else:
                body = "return (%s)%s;" % (c_res, call)
            params = ", ".join(pr_named(ty, "a%d" % i) for i, ty in enumerate(c_args)) or "void"
            text = ("// port: %s called through a function type that disagrees with it (tools/gen_pointers.py)\n"
                    "static %s %s(%s)\n{\n    %s\n}\n\n" % (fn.spelling, c_res, name, params, body))
            s = top.extent.start.offset
            self.edits.wrap(f, s, s, text, "", 0, "function casts adapted")
        self.edits.rep(f, e.extent.start.offset, e.extent.end.offset, name, "function casts adapted")

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
        # A string literal the game stores in its data may be edited in place (the menus wrap their
        # text by writing newlines into it, as the PS2 allowed): it gets a writable copy.
        helper = "port::P32Str(" if text.lstrip().startswith('"') else "port::P32("
        self.edits.wrap(f, a, b, helper, ")", depth + 1, kind)

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
            size = c.type.get_size()
            self.global_defs[name] = max(self.global_defs.get(name, 0), size if size > 0 else 0)
            self.define_global(c, name)
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

    def in_place_cast(self, c, e, fn, have, f):
        """`((ScriptValue* (*)(ScriptValue*, ...))EvalScriptAnd)(&u, ...)`: a function returning a
        class by value, called with the result's place first as on the PS2. The cast takes an
        adapter of the file's own that constructs the result there (in_place_adapter)."""
        top = next((t for t in self.top_cursors if t.location.file and t.location.file.name == f
                    and t.extent.start.offset <= c.extent.start.offset < t.extent.end.offset), None)
        if top is None:
            return
        try:
            params = [pr(a, "", False) for a in have.argument_types()]
            result = pr(have.get_result(), "", False)
        except Unsupported:
            return
        name = "port_inplace_cast_" + fn.spelling
        if (f, name) not in self.fn_adapters:
            self.fn_adapters[(f, name)] = name
            # declared before the use (and before any `static` the definition is given: depth -1
            # puts it first), defined at the end, where the class is complete
            s = top.extent.start.offset
            decl = ", ".join(["void*"] + params)
            self.edits.wrap(f, s, s, "// port: %s's result in the place its caller passes (tools/gen_pointers.py)\n"
                            "static void* %s(%s);\n" % (fn.spelling, name, decl), "", -1, "results in place adapted")
            data = self.texts.get(f)
            if data is None:
                data = self.texts[f] = Path(f).read_bytes()
            end = len(data)
            self.edits.wrap(f, end, end, "\n" + in_place_adapter(fn.spelling, name, result, params), "", 0,
                            "results in place adapted")
        self.edits.rep(f, e.extent.start.offset, e.extent.end.offset, name, "results in place adapted")

    def record_sizeof_local(self, c):
        """`sizeof(local)` or `sizeof local`: if the merge gives that local room (resolve_rooms), the
        expression keeps the local's own size, as retail's code computed it."""
        kids = list(c.get_children())
        if len(kids) != 1 or c.location.file is None:
            return
        e = kids[0]
        while e is not None and e.kind in (CK.UNEXPOSED_EXPR, CK.PAREN_EXPR):
            k2 = list(e.get_children())
            e = k2[0] if len(k2) == 1 else None
        if e is None or e.kind != CK.DECL_REF_EXPR or e.referenced is None or e.referenced.kind != CK.VAR_DECL:
            return
        var = e.referenced
        if var.location.file is None:
            return
        f = c.location.file.name
        data = self.texts.get(f)
        if data is None:
            data = self.texts[f] = Path(f).read_bytes()
        if not data[c.extent.start.offset:c.extent.start.offset + 6] == b"sizeof":
            return
        self.sizeof_locals.append(((var.location.file.name, var.location.offset), f, c.extent.start.offset,
                                   c.extent.end.offset, var.type.get_canonical().get_size()))

    def roomy_cast(self, c, operand):
        """`(Mat3*)&a` with `a` a smaller local (or `(ScaledTransform*)tmp`, an array): the game
        hands the local's address on as the larger type and has it written whole, which retail's
        frame (its neighbours, its padding) took. The local is noted with the size it needs, or
        the record's name when this file only declares it; the merge (resolve_rooms) gives it the
        room of the largest view, knowing every record's size."""
        pt = c.type.get_canonical()
        if pt.kind != TK.POINTER:
            return
        pointee = pt.get_pointee()
        size = pointee.get_size()
        need = size if size > 0 else (pointee.get_declaration().spelling
                                      if pointee.kind == TK.RECORD and pointee.get_declaration().spelling else None)
        if need is None:
            return
        e = operand
        while e is not None and e.kind in (CK.UNEXPOSED_EXPR, CK.PAREN_EXPR):
            kids = list(e.get_children())
            e = kids[0] if len(kids) == 1 else None
        if e is None:
            return
        if e.kind == CK.DECL_REF_EXPR:
            ref = e   # an array, decaying to its first element's address
        elif e.kind == CK.UNARY_OPERATOR:
            toks = _tokens(e)
            if not toks or toks[0].spelling != "&":
                return
            kids = list(e.get_children())
            ref = kids[0] if len(kids) == 1 else None
        else:
            return
        while ref is not None and ref.kind in (CK.UNEXPOSED_EXPR, CK.PAREN_EXPR):
            k2 = list(ref.get_children())
            ref = k2[0] if len(k2) == 1 else None
        if ref is None or ref.kind != CK.DECL_REF_EXPR or ref.referenced is None:
            return
        var = ref.referenced
        if var.kind != CK.VAR_DECL or var.storage_class in (ci.StorageClass.STATIC, ci.StorageClass.EXTERN):
            return
        parent = var.semantic_parent
        if parent is None or parent.kind not in (CK.FUNCTION_DECL, CK.CXX_METHOD, CK.CONSTRUCTOR,
                                                 CK.DESTRUCTOR, CK.FUNCTION_TEMPLATE):
            return
        vt = var.type.get_canonical()
        if vt.kind not in (TK.RECORD, TK.CONSTANTARRAY) or vt.get_size() <= 0:
            return
        if isinstance(need, int) and need <= vt.get_size():
            # A record or array local whose address goes out through any cast may be read or
            # stored as quadwords (`(f32*)&v` written as a Vec4, `(u32*)hdr` copied as one): it
            # gets its 16-byte slot, as in retail.
            if vt.get_size() % 16 == 0:
                return
            need = (vt.get_size() + 15) // 16 * 16
        if e.kind == CK.DECL_REF_EXPR and vt.kind != TK.CONSTANTARRAY:
            return
        key = (var.location.file.name, var.location.offset)
        if key not in self.rooms:
            self.rooms[key] = (var, [])
        self.rooms[key][1].append(need)

    def roomy_address(self, e):
        """`&local` of a record whose size is not a multiple of 16, anywhere: the address may reach
        code that copies or stores it as a larger, quadword-aligned record (a 72-byte draw record
        copied as an 80-byte ScaledTransform through a method slot), which retail's 16-byte stack
        slots allowed. The local gets its slot's room."""
        # The operator from the file's own byte (tokenising a whole operand is costly).
        f = e.location.file.name if e.location.file else None
        if f is None:
            return
        data = self.texts.get(f)
        if data is None:
            data = self.texts[f] = Path(f).read_bytes()
        if data[e.extent.start.offset:e.extent.start.offset + 1] != b"&":
            return
        kids = list(e.get_children())
        ref = kids[0] if len(kids) == 1 else None
        while ref is not None and ref.kind in (CK.UNEXPOSED_EXPR, CK.PAREN_EXPR):
            k2 = list(ref.get_children())
            ref = k2[0] if len(k2) == 1 else None
        if ref is None or ref.kind != CK.DECL_REF_EXPR or ref.referenced is None:
            return
        var = ref.referenced
        if var.kind != CK.VAR_DECL or var.storage_class in (ci.StorageClass.STATIC, ci.StorageClass.EXTERN):
            return
        parent = var.semantic_parent
        if parent is None or parent.kind not in (CK.FUNCTION_DECL, CK.CXX_METHOD, CK.CONSTRUCTOR,
                                                 CK.DESTRUCTOR, CK.FUNCTION_TEMPLATE):
            return
        vt = var.type.get_canonical()
        if vt.kind != TK.RECORD or vt.get_size() <= 0 or vt.get_size() % 16 == 0:
            return
        key = (var.location.file.name, var.location.offset)
        if key not in self.rooms:
            self.rooms[key] = (var, [])
        self.rooms[key][1].append((vt.get_size() + 15) // 16 * 16)

    def flush_rooms(self):
        """The noted locals as plain data for the merge: how to give each more room."""
        for (f, off), (var, needs) in self.rooms.items():
            toks = _tokens(var)
            vt = var.type.get_canonical()
            line = var.location.line
            plan = {"file": f, "line": line, "size": vt.get_size(), "needs": needs, "name": var.spelling}
            # Where the declaration ends, for the audit build's guard (PORT_ROOM_AUDIT): after its
            # semicolon, unless that is not a plain statement's (a for's first clause).
            data = self.texts.get(f)
            if data is None:
                data = self.texts[f] = Path(f).read_bytes()
            j = data.find(b";", var.extent.end.offset)
            between = data[var.extent.end.offset:j] if j >= 0 else b")"
            if b")" not in between and b"{" not in between:
                plan["after"] = j + 1
            if vt.kind == TK.RECORD and vt.get_declaration().kind == CK.UNION_DECL:
                plan["error"] = "a union local viewed as a larger type; not given room"
            elif vt.kind == TK.CONSTANTARRAY:
                spell = [tk.spelling for tk in toks]
                i = spell.index("[") if "[" in spell else -1
                try:
                    ok = i >= 0 and i + 2 < len(toks) and spell[i + 2] == "]" and \
                        int(spell[i + 1].rstrip("uUlL"), 0) >= 0
                except ValueError:
                    ok = False
                if not ok:
                    plan["error"] = "an array local viewed as a larger type, its bound not a number; not given room"
                else:
                    plan.update(kind="array", elem=vt.get_array_element_type().get_size(),
                                at=(toks[i + 1].extent.start.offset, toks[i + 1].extent.end.offset))
            elif len(toks) < 2 or toks[1].extent.start.offset != var.location.offset or \
                    (f, toks[0].extent.start.offset) in self.macro_starts:
                plan["error"] = "a local viewed as a larger type, its type not one word; not given room"
            else:
                plan.update(kind="record", type=toks[0].spelling,
                            at=(toks[0].extent.start.offset, toks[0].extent.end.offset))
            self.room_plans[(f, off)] = plan
        self.rooms = {}

    def preemption_point(self, c, kids, body):
        """Every loop tests port::Preempt() with its condition (`while ((port::Preempt(), x))`),
        or, a for loop, first thing in its body: where the PS2 would preempt a thread that loops
        while an interrupt readies one of higher priority."""
        if c.kind in (CK.WHILE_STMT, CK.DO_STMT):
            cond = kids[0] if c.kind == CK.WHILE_STMT else kids[-1]
            if cond.kind == CK.VAR_DECL or cond.location.file is None:
                return
            f = cond.location.file.name
            if (f, cond.extent.start.offset) in self.macro_starts:
                return
            self.edits.wrap(f, cond.extent.start.offset, cond.extent.end.offset, "(port::Preempt(), ", ")",
                            0, "preemption points")
        elif body.kind == CK.COMPOUND_STMT and body.location.file is not None:
            f = body.location.file.name
            if (f, body.extent.start.offset) in self.macro_starts or not list(body.get_children()):
                return
            a = body.extent.start.offset + 1
            self.edits.wrap(f, a, a, " port::Preempt();", "", 0, "preemption points")

    def float_division(self, c, depth):
        """`a / b` and `a /= b` in floats: the PS2's FPU divides by zero to the largest float with
        the quotient's sign (0 / 0 too), where IEEE gives infinity or NaN. port::FDiv divides as
        it does (port/retail_data.h); an overflow already saturates, as game threads round toward
        zero."""
        if c.type.get_canonical().kind != TK.FLOAT or c.location.file is None:
            return
        kids = list(c.get_children())
        if len(kids) != 2:
            return
        lhs, rhs = kids
        f = c.location.file.name
        if lhs.location.file is None or rhs.location.file is None or lhs.location.file.name != f or \
                rhs.location.file.name != f:
            return
        data = self.texts.get(f)
        if data is None:
            data = self.texts[f] = Path(f).read_bytes()
        a, b = lhs.extent.end.offset, rhs.extent.start.offset
        between = data[a:b]
        op = between.strip()
        if op not in (b"/", b"/="):
            return
        if (f, c.extent.start.offset) in self.macro_starts or c.extent.start.offset >= c.extent.end.offset:
            self.edits.unsupported.append("%s:%d: a float division inside a macro" % (f, c.location.line))
            return
        if c.kind == CK.COMPOUND_ASSIGNMENT_OPERATOR and lhs.type.get_canonical().kind != TK.FLOAT:
            return
        i = a + between.index(op)
        self.edits.rep(f, i, i + len(op), ",", "float divisions as the PS2 divides")
        helper = "port::FDivAssign(" if op == b"/=" else "port::FDiv("
        self.edits.wrap(f, c.extent.start.offset, c.extent.end.offset, helper, ")", depth, "float divisions as the PS2 divides")

    def wait_loop(self, c):
        """A loop with an empty body waits for another thread or an interrupt handler: it calls
        port::Spin() each turn (port/retail_data.h), where the PS2 would have preempted it."""
        kids = list(c.get_children())
        if not kids:
            return
        body = kids[0] if c.kind == CK.DO_STMT else kids[-1]
        self.preemption_point(c, kids, body)
        if c.kind == CK.FOR_STMT:
            # only for (;;): a counted empty for is a delay or computes its counter
            toks = [tk.spelling for tk in _tokens(c)[:5]]
            if toks[:5] != ["for", "(", ";", ";", ")"]:
                return
        f = body.location.file.name if body.location.file else None
        if f is None or (f, body.extent.start.offset) in self.macro_starts:
            return
        a, b = body.extent.start.offset, body.extent.end.offset
        if body.kind == CK.NULL_STMT:
            self.edits.rep(f, a, b, "{ port::Spin(); }", "wait loops")
        elif body.kind == CK.COMPOUND_STMT and not list(body.get_children()):
            self.edits.rep(f, a, b, "{ port::Spin(); }", "wait loops")

    @staticmethod
    def address_param_key(fn, index):
        return (fn.spelling, len(list(fn.get_arguments())), index)

    def address_parameters(self, c):
        """A parameter of integer type whose every use is a cast to a pointer (`(BodyContact*)a2`)
        carries an address: the game calls such functions through method tables with pointers,
        which a 32-bit word cannot hold here. Noted by (function, parameter count, index), from the
        definition; the merge retypes it void* in every declaration (retype_address_parameters),
        and its casts stay pointer casts (cast). Each declaration's parameter types are recorded
        for that."""
        if c.kind != CK.FUNCTION_DECL or c.location.file is None:
            return
        args = list(c.get_arguments())
        f = c.location.file.name
        for i, a in enumerate(args):
            toks = _tokens(a)
            name_at = [k for k, tk in enumerate(toks) if tk.extent.start.offset == a.location.offset]
            end = name_at[0] if name_at and a.spelling else len(toks)
            if end > 0 and toks and (f, toks[0].extent.start.offset) not in self.macro_starts:
                self.param_sites.append((self.address_param_key(c, i), f, toks[0].extent.start.offset,
                                         toks[end - 1].extent.end.offset))
        if not c.is_definition():
            return
        body = next((k for k in c.get_children() if k.kind == CK.COMPOUND_STMT), None)
        if body is None:
            return
        for i, a in enumerate(args):
            if a.type.get_canonical().kind not in INTEGERS or a.type.get_canonical().get_size() != 4:
                continue
            uses = []

            def visit(n, chain):
                if n.kind == CK.DECL_REF_EXPR and n.referenced is not None and n.referenced == a:
                    uses.append(chain)
                    return
                for k in n.get_children():
                    visit(k, chain + [n])
            visit(body, [])
            if not uses:
                continue
            ok = True
            for chain in uses:
                up = [n for n in chain if n.kind not in (CK.UNEXPOSED_EXPR, CK.PAREN_EXPR)]
                cast = up[-1] if up else None
                if cast is None or cast.kind != CK.CSTYLE_CAST_EXPR or \
                        cast.type.get_canonical().kind != TK.POINTER:
                    ok = False
                    break
            if ok:
                self.address_params.add(self.address_param_key(c, i))
                self.address_param_cursors.add((a.location.file.name, a.location.offset))

    def record_call_arguments(self, c):
        """A direct call's integer arguments (not a literal 0), for the merge: where the callee's
        parameter turns out to be an address (address_parameters), the argument, a game address
        held as a word, becomes the pointer."""
        fn = c.referenced
        if fn is None or fn.kind != CK.FUNCTION_DECL:
            return
        args = list(c.get_arguments())
        n = len(list(fn.get_arguments()))
        for i, a in enumerate(args[:n]):
            if a.type.get_canonical().kind not in INTEGERS or a.location.file is None:
                continue
            text = self._own_text(a)
            if text is None or text.strip() in ("0", "0U", "0u"):
                continue
            f = a.location.file.name
            if (f, a.extent.start.offset) in self.macro_starts:
                continue
            self.call_args.append(((fn.spelling, n, i), f, a.extent.start.offset, a.extent.end.offset))

    def is_address_parameter(self, operand):
        e = operand
        while e is not None and e.kind in (CK.UNEXPOSED_EXPR, CK.PAREN_EXPR):
            kids = list(e.get_children())
            e = kids[0] if len(kids) == 1 else None
        if e is None or e.kind != CK.DECL_REF_EXPR or e.referenced is None or e.referenced.location.file is None:
            return False
        return (e.referenced.location.file.name, e.referenced.location.offset) in self.address_param_cursors

    def record_function(self, c):
        """A game function's definition (func_XXXXXXXX, at namespace scope, in a source file): an
        entry of the function table by retail address (port/ptr32.h). Every declaration's kind of
        result too: the return type is not in a C++ function's link name, so one file's `s32 f()`
        and another's `T* f()` meet at link time, which the PS2's 32-bit registers absorbed."""
        if c.kind != CK.FUNCTION_DECL or not re.fullmatch(r"func_[0-9A-F]{8}", c.spelling):
            return
        rk = c.result_type.get_canonical().kind
        kind = ("pointer" if rk == TK.POINTER else "void" if rk == TK.VOID else
                "integer" if rk in (TK.INT, TK.UINT, TK.LONG, TK.ULONG, TK.SHORT, TK.USHORT, TK.CHAR_S,
                                    TK.SCHAR, TK.UCHAR, TK.CHAR_U, TK.BOOL, TK.ENUM) else "other")
        self.result_kinds.append((c.spelling, kind, c.is_definition(), c.location.file.name, c.location.line))
        if not c.is_definition():
            self.record_declaration(c, kind)
            return
        parent = c.semantic_parent
        if parent is None or parent.kind not in (CK.TRANSLATION_UNIT, CK.LINKAGE_SPEC, CK.UNEXPOSED_DECL):
            return
        f = c.location.file.name
        if f.endswith(".cpp"):
            # The definition's own type picks it where the file also declares other overloads.
            # Its type under the rewrite: parameters and result in register positions.
            try:
                params = ["void *" if self.address_param_key(c, i) in self.address_params else pr(a.type, "", False)
                          for i, a in enumerate(c.get_arguments())]
                if c.type.is_function_variadic():
                    params.append("...")
                spelling = "%s (%s)" % (pr(c.result_type, "", False), ", ".join(params))
            except Unsupported:
                spelling = None
            if spelling and ("(unnamed" in spelling or "(anonymous" in spelling or "(lambda" in spelling):
                spelling = None
            adapter = None
            if spelling and returned_in_place(c.result_type) and not c.type.is_function_variadic():
                adapter = in_place_adapter(c.spelling, "port_inplace_" + c.spelling, pr(c.result_type, "", False),
                                           params)
            self.functions.append((f, c.spelling, spelling, adapter))

    def record_declaration(self, c, kind):
        """What an adapter needs, should this declaration's result disagree with the definition's
        (see adapt_results): where its name and end are, and its types under the rewrite."""
        if kind not in ("pointer", "integer"):
            return
        f = c.location.file.name
        parent = c.semantic_parent
        where = "%s:%d" % (f, c.location.line)
        if not f.endswith(".cpp") or parent is None or parent.kind not in (
                CK.TRANSLATION_UNIT, CK.LINKAGE_SPEC, CK.UNEXPOSED_DECL):
            self.declarations.append((c.spelling, kind, f, where, None))
            return
        data = self.texts.get(f)
        if data is None:
            data = self.texts[f] = Path(f).read_bytes()
        j = c.extent.end.offset
        while j < len(data) and data[j:j + 1] in b" \t\r\n":
            j += 1
        loc = c.location.offset
        m = re.match(rb"[A-Za-z_]\w*", data[loc:])
        if data[j:j + 1] != b";" or m is None or c.type.is_function_variadic():
            self.declarations.append((c.spelling, kind, f, where, None))
            return
        try:
            params = [pr(a.type, "a%d" % i, False) for i, a in enumerate(c.get_arguments())]
            result = pr(c.result_type, "", False)
        except Unsupported:
            self.declarations.append((c.spelling, kind, f, where, None))
            return
        self.declarations.append((c.spelling, kind, f, where,
                                  (loc, len(m.group(0)), j + 1, result, params)))

    def define_global(self, c, name):
        """A definition of a game global at namespace scope: its value becomes port_init_<name>
        (static, the same type and initialiser), copied to the global's retail address at start-up,
        and the name a macro over that address (port/retail_data.h)."""
        parent = c.semantic_parent
        if parent is None or parent.kind not in (CK.TRANSLATION_UNIT, CK.NAMESPACE, CK.LINKAGE_SPEC,
                                                 CK.UNEXPOSED_DECL):
            return                          # a function's static: a real object of its own
        f = c.location.file.name
        if not f.endswith(".cpp"):
            self.global_kept.add(name)
            self.edits.unsupported.append("%s:%d: %s is defined in a header; kept as a global of its own"
                                          % (f, c.location.line, name))
            return
        data = self.texts.get(f)
        if data is None:
            data = self.texts[f] = Path(f).read_bytes()
        end = c.extent.end.offset
        j = end
        while j < len(data) and data[j:j + 1] in b" \t\r\n":
            j += 1
        if data[j:j + 1] != b";":
            self.global_kept.add(name)  # `T A = 1, B = 2;`: one statement, several globals
            self.edits.unsupported.append("%s:%d: %s is defined in a statement that defines others; kept "
                                          "as a global of its own" % (f, c.location.line, name))
            return
        line = data.rfind(b"\n", 0, c.extent.start.offset) + 1
        if data[line:c.extent.start.offset].strip():
            self.global_kept.add(name)
            self.edits.unsupported.append("%s:%d: %s's definition shares its line; kept as a global of its own"
                                          % (f, c.location.line, name))
            return
        try:
            type_text = pr(c.type, "", True)
        except Unsupported as e:
            self.global_kept.add(name)
            self.edits.unsupported.append("%s:%d: %s: %s" % (f, c.location.line, name, e))
            return
        new_name = "port_init_" + name
        loc = c.location.offset
        m = re.match(rb"[A-Za-z_]\w*", data[loc:])
        spelled = m.group(0).decode() if m else name
        if needs(c.type, True):
            self.rename[(f, loc)] = new_name            # the declaration's rewrite writes the name
        else:
            self.edits.rep(f, loc, loc + len(spelled), new_name, "globals at retail addresses")
        toks = _tokens(c)
        words = [tk.spelling for tk in toks[:3]]
        if words[:1] == ["extern"]:
            stop = 2 if len(words) > 1 and words[1].startswith('"') else 1
            self.edits.rep(f, toks[0].extent.start.offset, toks[stop - 1].extent.end.offset, "static",
                           "globals at retail addresses")
        # The macro comes first, on its own lines: an initialiser may take the global's own address
        # (a registry pointing into itself), which is then its retail one. One insertion, with the
        # `static` the definition needs, as both go at the line's start.
        prefix = "#undef %s\n#define %s %s\n" % (name, name, retail_macro(type_text, int(name[2:], 16)))
        if words[:1] != ["extern"] and "static" not in words:
            start = c.extent.start.offset
            if start == line:
                prefix += "static "
            else:
                self.edits.wrap(f, start, start, "static ", "", 0, "globals at retail addresses")
        self.edits.wrap(f, line, line, prefix, "", 0, "globals at retail addresses")
        self.global_inits.append((f, name, new_name))

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
        self.record_sizeof_local(c)
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

    def align_local(self, c):
        """A local struct or array of 16 bytes or more starts on a 16-byte boundary, as the PS2's
        g++ placed them in a frame (the quadword loads and stores and the DMA need it): a packet
        built in a local (FadeScreenToBlack's FadePacket, a PacketWriter of 4-byte words) was put
        where the host's compiler chose, and a DMA chain from it started 16 bytes early."""
        try:
            t = c.type.get_canonical()
        except Exception:
            return
        if t.kind not in (TK.RECORD, TK.CONSTANTARRAY) or t.get_size() < 16 or c.location.file is None:
            return
        f = c.location.file.name
        start = c.extent.start.offset
        if c.extent.start.file is None or c.extent.start.file.name != f:
            return   # in a macro
        data = self.texts.get(f)
        if data is None:
            data = self.texts[f] = Path(f).read_bytes()
        j = start - 1
        while j >= 0 and data[j:j + 1] in b" \t":
            j -= 1
        if data[j:j + 1] == b"(" and not data[max(0, j - 4):j].rstrip().endswith(b"for"):
            return   # a condition's declaration
        if data[j:j + 1] not in (b"\n", b";", b"{", b"}", b"(", b")", b":"):
            return   # the second declarator of a list, or something not at a declaration's start
        self.edits.wrap(f, start, start, "alignas(16) ", "", -2, "locals aligned to 16 bytes")

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
            if k in (CK.STRUCT_DECL, CK.CLASS_DECL) and ch.is_definition() and ch.spelling:
                sz = ch.type.get_size()
                if sz > 0:
                    self.record_sizes[ch.spelling] = sz
            if k == CK.FIELD_DECL:
                self.declaration(ch, True)
            elif k == CK.VAR_DECL:
                local = in_func and ch.storage_class not in (ci.StorageClass.STATIC, ci.StorageClass.EXTERN)
                if not local:
                    self.record_global(ch)
                addressed = (ch.location.file.name, ch.location.offset) in self.addressed
                if local:
                    self.align_local(ch)
                self.declaration(ch, not local or addressed)
                if not local or addressed:
                    self.initializer(ch, depth)
            elif k == CK.PARM_DECL:
                parent = ch.semantic_parent
                if parent is not None and parent.kind in (CK.FUNCTION_DECL, CK.CXX_METHOD, CK.CONSTRUCTOR,
                                                          CK.FUNCTION_TEMPLATE):
                    self.declaration(ch, False)
            elif k in (CK.FUNCTION_DECL, CK.CXX_METHOD, CK.FUNCTION_TEMPLATE):
                self.address_parameters(ch)
                self.record_function(ch)
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
            elif k == CK.UNARY_OPERATOR:
                self.roomy_address(ch)
            elif k in (CK.BINARY_OPERATOR, CK.COMPOUND_ASSIGNMENT_OPERATOR):
                self.float_division(ch, depth)
            elif k == CK.CONDITIONAL_OPERATOR:
                self.conditional(ch, depth)
            elif k == CK.CALL_EXPR:
                self.call(ch, depth)
                self.record_call_arguments(ch)
            elif k in (CK.WHILE_STMT, CK.DO_STMT, CK.FOR_STMT):
                self.wait_loop(ch)
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
        walker.top_cursors = cursors
        walker.collect_addressed(cursors)
        walker.walk_cursors(cursors)
        walker.flush_groups()
        walker.flush_rooms()
    return ({f: dict(r) for f, r in edits.replace.items()}, {f: set(i) for f, i in edits.insert.items()},
            edits.unsupported, diagnostics, edits.kinds, walker.global_defs, walker.global_decls,
            walker.global_inits, walker.global_kept, walker.functions, walker.result_kinds,
            walker.declarations, walker.room_plans, walker.record_sizes, walker.address_params,
            walker.param_sites, walker.call_args, walker.sizeof_locals)


def adapt_results(edits, result_kinds, declarations):
    """A declaration whose result is a pointer where the definition's is an integer, or the other
    way round. The two meet at link time (a C++ function's link name leaves its result out); the
    PS2 kept both in one 32-bit register, where here a pointer is 64 bits and a game address 32.
    The declaration is renamed to an adapter of this file's own, defined right after it: it calls
    the definition through a declaration of its kind of result, converting between game address
    and pointer, and a macro sends the file's uses of the name to it."""
    defined = {}
    for name, decls in result_kinds.items():
        for kind, is_def, _, _ in decls:
            if is_def:
                defined[name] = kind
    adapters, unadapted, seen = 0, [], set()
    # In file order: a file's first disagreeing declaration gets the adapter, and its macro renames
    # any later one.
    order = lambda d: (d[2], d[4][0] if d[4] else int(d[3].rsplit(":", 1)[1]) * 1000)
    for name, kind, f, where, details in sorted(declarations, key=order):
        want = defined.get(name)
        if want not in ("pointer", "integer") or want == kind or (f, name) in seen:
            continue
        seen.add((f, name))
        if details is None:
            unadapted.append("%s: %s declared %s, defined %s" % (where, name, kind, want))
            continue
        loc, length, after, result, params = details
        adapter = "port_as_%s_%s" % (name, re.sub(r"\W", "_", Path(f).stem))
        args = ", ".join("a%d" % i for i in range(len(params)))
        plist = ", ".join(params) or "void"
        if want == "integer":       # declared a pointer: the definition's word is a game address
            text = ("\nint %s(%s);\ninline %s %s(%s)\n{\n    return (%s)port::Native((std::uint32_t)%s(%s));\n}\n"
                    % (name, plist, result, adapter, plist, result, name, args))
        else:                       # declared an integer: the definition's pointer, as its game address
            text = ("\nvoid* %s(%s);\ninline %s %s(%s)\n{\n    return (%s)port::GameAddr(%s(%s));\n}\n"
                    % (name, plist, result, adapter, plist, result, name, args))
        text += "#define %s %s\n" % (name, adapter)
        edits.rep(f, loc, loc + length, adapter, "results that disagree")
        edits.wrap(f, after, after, text, "", 0, "results that disagree")
        adapters += 1
    return adapters, unadapted


def resolve_rooms(edits, plans, record_sizes):
    """Gives each local viewed as a larger type (Walker.roomy_cast) the room of its largest view,
    every record's size known now: a record local becomes port::Room<T, N>, an array gets more
    elements. Gives back the keys of the locals given room."""
    widened = set()
    for key, plan in plans.items():
        sizes = [n if isinstance(n, int) else record_sizes.get(n, 0) for n in plan["needs"]]
        size = max(sizes) if sizes else 0
        if size <= plan["size"]:
            continue
        f = plan["file"]
        if "error" in plan:
            edits.unsupported.append("%s:%d: %s" % (f, plan["line"], plan["error"]))
            continue
        a, b = plan["at"]
        widened.add(key)
        if "after" in plan:   # the audit build marks the added bytes (port/retail_data.h)
            edits.wrap(f, plan["after"], plan["after"], " PORT_ROOM_AUDIT(%s, %d);" % (plan["name"], plan["size"]),
                       "", 0, "room audit guards")
        if plan["kind"] == "array":
            count = -(-size // plan["elem"])
            edits.rep(f, a, b, "%d /* port: room for a larger view */" % count, "locals given a larger view's room")
        else:
            n = (size + 15) // 16 * 16
            edits.rep(f, a, b, "port::Room<%s, %d>" % (plan["type"], n), "locals given a larger view's room")
    return widened


def returned_in_place(t):
    """A class with its own destructor or copy constructor, returned by value: on the PS2 g++ 2.95
    passes the result's place as the hidden first argument, which callers through casts and the
    game's tables pass explicitly; here it goes in a register of its own (x8 on ARM64)."""
    t = t.get_canonical()
    if t.kind != TK.RECORD:
        return False
    d = t.get_declaration()
    for k in d.get_children():
        if k.kind == CK.DESTRUCTOR:
            return True
        if k.kind == CK.CONSTRUCTOR:
            args = list(k.get_arguments())
            if len(args) == 1 and args[0].type.get_canonical().kind == TK.LVALUEREFERENCE and \
                    args[0].type.get_canonical().get_pointee().get_canonical() == t:
                return True
    return False


def in_place_adapter(name, adapter, result, params, static=True):
    """`void* adapter(void* ret, params...)`: the function's result constructed in ret, ret given
    back, as the PS2's convention for a class returned by value has it."""
    decl = ", ".join(["void* port_ret"] + ["%s" % pr_named(p, "a%d" % i) for i, p in enumerate(params)])
    args = ", ".join("a%d" % i for i in range(len(params)))
    return ("%svoid* %s(%s)\n{\n    ::new (port_ret) %s(%s(%s));\n    return port_ret;\n}\n"
            % ("static " if static else "", adapter, decl, result, name, args))


def retail_macro(type_text, addr):
    """A game global's macro (port/retail_data.h): the object at its retail address. Below the
    program (0x100000) an unsized array is a label whose address is a number, a VU1
    microprogram's for an MSCAL: a pointer to that address, which address 0 may be."""
    m = re.fullmatch(r"(.+?)\s*\[\s*\]", type_text.strip())
    if addr < 0x100000 and m:
        return "(port::FromGameAddr< %s >(0x%08XU))" % (m.group(1), addr)
    return "(port::RetailRef< %s >(0x%08XU))" % (type_text, addr)


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
    global_defs, global_decls, global_inits, global_kept = {}, [], [], set()
    functions = collections.defaultdict(set)
    result_kinds = collections.defaultdict(set)
    declarations = []
    room_plans, record_sizes = {}, {}
    address_params, param_sites, call_args, sizeof_locals = set(), [], [], []
    for replace, insert, unsupported, diag, k, gdefs, gdecls, ginits, gkept, funcs, rkinds, fdecls, rplans, rsizes, \
            aparams, psites, cargs, slocals in results:
        sizeof_locals += slocals
        address_params |= aparams
        param_sites += psites
        call_args += cargs
        declarations += fdecls
        record_sizes.update(rsizes)
        for key, plan in rplans.items():
            if key in room_plans:
                room_plans[key]["needs"] += plan["needs"]
            else:
                room_plans[key] = plan
        for name, kind, is_def, f, line in rkinds:
            result_kinds[name].add((kind, is_def, f, line))
        for f, name, spelling, adapter in funcs:
            functions[f].add((name, spelling, adapter))
        global_inits += [g for g in ginits if g not in global_inits]
        global_kept |= gkept
        kinds.update(k)
        for name, size in gdefs.items():
            global_defs[name] = max(global_defs.get(name, 0), size)
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
        if all(n in global_kept for n in members):
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
            if name in global_kept:
                text += "\nextern %s;" % pr_named(type_text, name)
            else:
                if name not in global_defs:
                    undefined.setdefault(name, []).append((f, line))
                text += "\n#undef %s\n#define %s %s" % (name, name, retail_macro(type_text, int(name[2:], 16)))
        text += "\n"
        reps = edits.replace.get(f, {})
        for key in [k for k in reps if start <= k[0] < end]:
            del reps[key]
        edits.insert[f] = {i for i in edits.insert.get(f, set()) if not (start <= i[0] < end)}
        edits.rep(f, start, end, text, "globals nothing defines")
    report["undefined_globals"] = undefined
    report["global_defs"] = global_defs
    # Each file's definitions moved to retail addresses: a function that copies their initial values.
    by_file = collections.defaultdict(list)
    for f, name, new_name in global_inits:
        if name not in global_kept:
            by_file[f].append((name, new_name))
    report["global_init_files"] = []
    for f, items in sorted(by_file.items()):
        func = "port_data_init_" + re.sub(r"\W", "_", Path(f).stem)
        body = "".join("    port::RetailInit(0x%08XU, (const void*)&%s, sizeof(%s));\n" % (int(n[2:], 16), v, v)
                       for n, v in sorted(set(items)))
        size = Path(f).stat().st_size
        edits.wrap(f, size, size, "\n// Written by tools/gen.py: this file's globals to their retail "
                   "addresses (port/retail_data.h).\nvoid %s()\n{\n%s}\n" % (func, body), "", 0,
                   "globals at retail addresses")
        report["global_init_files"].append(func)
    report["global_kept"] = sorted(global_kept)
    # Each file's game functions: the table by retail address (port/ptr32.h).
    report["function_files"] = []
    count = 0
    for f, names in sorted(functions.items()):
        func = "port_functions_" + re.sub(r"\W", "_", Path(f).stem)
        # A function returning a class in place is entered by its adapter, which takes the result's
        # place first, as retail's tables call it (returned_in_place).
        adapters = "".join(a for _, _, a in sorted(names, key=lambda x: (x[0], x[1] or "")) if a)
        body = "".join("    {0x%sU, (void*)%s},\n" % (
            n[5:], ("&port_inplace_" + n) if a else
            ("static_cast<std::add_pointer_t<%s>>(&%s)" % (s, n)) if s else "&" + n)
            for n, s, a in sorted(names, key=lambda x: (x[0], x[1] or "")))
        size = Path(f).stat().st_size
        edits.wrap(f, size, size, "\n// Written by tools/gen.py: this file's functions by retail address "
                   "(port/ptr32.h).\n%sextern const port::FunctionEntry %s[] = {\n%s};\n" % (adapters, func, body),
                   "", 0, "functions by retail address")
        report["function_files"].append((func, len(names)))
        count += len(names)
    report["function_count"] = count
    # Results that disagree between pointer and integer: a 64-bit pointer meets a 32-bit word.
    clashes = {}
    for name, decls in sorted(result_kinds.items()):
        kinds = {k for k, _, _, _ in decls}
        if "pointer" in kinds and "integer" in kinds:
            clashes[name] = sorted(decls, key=lambda d: (not d[1], d[2], d[3]))
    report["result_clashes"] = clashes
    report["result_adapters"], report["result_unadapted"] = adapt_results(edits, result_kinds, declarations)
    widened = resolve_rooms(edits, room_plans, record_sizes)
    for key, f, a, b, size in set(sizeof_locals):
        if key in widened:
            edits.rep(f, a, b, "((size_t)%d)" % size, "sizeof of a local given room")
    # Parameters used only as addresses: void* in every declaration, so the link names agree.
    for key, f, a, b in set(param_sites):
        if key in address_params:
            edits.rep(f, a, b, "void*", "integer parameters used only as addresses")
    for key, f, a, b in set(call_args):
        if key in address_params:
            edits.wrap(f, a, b, "port::FromGameAddr<void>((u32)(", "))", 0, "address arguments made pointers")
    files = edits.apply(out)
    report["files_changed"] = len(files)
    report["counts"] = dict(edits.counts)
    report["unsupported"] = edits.unsupported
    report["parse_errors_before"] = diagnostics
    return edits
