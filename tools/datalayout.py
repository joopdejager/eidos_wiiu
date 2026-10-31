"""datalayout.py -- the DS layout of the struct types a C file declares data
with, for a big-endian host's swap runs (tools/armdata.py, layout_runs).

aletheia's layout/data_objects.json types a data object by what showed:
a numeric guess (u8, u16, u32) when nothing better did.  A matched source's
declaration says what the code reads: `extern const struct BgTemplate
d_021BD470;` is u32, u32, u32, u32, four u8, u32, four u8, u32, and the
metadata's 32 u8 (or 16 u16) left a big-endian host reading its u32 fields
in the wrong order.  This module lays such a struct out the way mwcc does
for the ARM9 (each scalar aligned to its size, an 8-byte one to 4; a
struct to its widest member; padded to that) and lists the 16-, 32- and
64-bit fields as (offset, size, width) runs.

Only what it can lay out for certain gets a layout.  A union, a bit-field, a
packed or aligned attribute, an array whose length is not a number, or a
member type it does not know makes the whole type unknown (None), and an
unknown type gives no runs: the metadata's typing stands.

The definitions come from the file and from the headers it includes with
quotes, found beside it, and from theirs (each header parsed once).
"""

import os
import re

_INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]+"([^"]+)"', re.M)

# Scalar type names and their sizes (armdata.py's _WIDTH_OF, plus the C
# spellings and the SDK's handful of fixed-width typedefs).
SCALARS = {
    "u8": 1, "s8": 1, "vu8": 1, "vs8": 1, "char": 1, "BOOL8": 1,
    "u16": 2, "s16": 2, "vu16": 2, "vs16": 2, "short": 2, "fx16": 2, "GXRgb": 2,
    "u32": 4, "s32": 4, "vu32": 4, "vs32": 4, "int": 4, "long": 4, "fx32": 4,
    "BOOL": 4, "uptr": 4, "OSIntrMode": 4, "size_t": 4, "float": 4,
    "u64": 8, "s64": 8, "vu64": 8, "fx64": 8, "double": 8,
}
_QUALIFIERS = {"const", "volatile", "static", "extern", "register", "signed", "unsigned"}


class Layout:
    """A type's size, alignment and swap runs [(offset, size, width)], and
    its pointer fields whose pointee is laid out: [(offset, Layout)]
    (tools/armdata.py follows them to the objects they point at)."""

    __slots__ = ("size", "align", "runs", "ptrs")

    def __init__(self, size, align, runs, ptrs=None):
        self.size = size
        self.align = align
        self.runs = runs
        self.ptrs = ptrs or []

    def key(self):
        return (self.size, tuple(self.runs))


def _scalar(width):
    return Layout(width, min(width, 4), [(0, width, width)] if width > 1 else [])


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def _match_brace(text, i):
    """The index just past the brace that closes the one at text[i]."""
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return j + 1
    return -1


def _split_top(text, sep):
    out, depth, cur = [], 0, []
    for ch in text:
        if ch in "({[":
            depth += 1
        elif ch in ")}]":
            depth -= 1
        if ch == sep and depth == 0:
            out.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    out.append("".join(cur))
    return out


def _number(text):
    text = text.strip()
    if not re.fullmatch(r"[0-9xXa-fA-FuUlL+\-*/() \t]+", text):
        return None
    try:
        v = eval(re.sub(r"(?<=[0-9a-fA-F])[uUlL]+", "", text), {"__builtins__": {}}, {})
    except Exception:                       # not a constant expression
        return None
    return v if isinstance(v, int) and v >= 0 else None


class Defs:
    """The struct, union, enum and typedef definitions one file sees."""

    def __init__(self):
        self.structs = {}       # tag -> body text, or None for a union
        self.typedefs = {}      # name -> ("body", text) | ("ubody", text) | ("union",) | ("type", text)
        self.unions = {}        # tag -> body text, or None (packed, aligned)
        self.protos = {}        # function name -> [parameter type texts]
        self._cache = {}

    def add_text(self, text):
        code = strip_comments(text)
        i = 0
        pat = re.compile(r"\b(typedef\s+)?(struct|union|enum)\s*([A-Za-z_]\w*)?\s*\{")
        while True:
            m = pat.search(code, i)
            if m is None:
                break
            end = _match_brace(code, m.end() - 1)
            if end < 0:
                break
            body = code[m.end():end - 1]
            kind, tag = m.group(2), m.group(3)
            semi = code.find(";", end)
            tail = code[end:semi if semi >= 0 else len(code)]
            attr = "__attribute__" in tail      # packed, aligned: unknown
            if tag and kind == "struct":
                self.structs.setdefault(tag, None if attr else body)
            elif tag and kind == "union":
                self.structs.setdefault(tag, None)
                self.unions.setdefault(tag, None if attr else body)
            if m.group(1):
                tail = re.sub(r"__attribute__\s*\(\(.*?\)\)", " ", tail)
                for d in _split_top(tail, ","):
                    dm = re.match(r"\s*(\**)\s*([A-Za-z_]\w*)\s*$", d)
                    if dm is None:
                        continue
                    name = dm.group(2)
                    if dm.group(1):
                        self.typedefs.setdefault(name, ("type", "void *"))
                    elif kind == "enum":
                        self.typedefs.setdefault(name, ("type", "int"))
                    elif kind == "struct" and not attr:
                        self.typedefs.setdefault(name, ("body", body))
                    elif kind == "union" and not attr:
                        self.typedefs.setdefault(name, ("ubody", body))
                    else:
                        self.typedefs.setdefault(name, ("union",))
            i = end
        for m in re.finditer(r"\btypedef\s+([^;{}()]*?)\b([A-Za-z_]\w*)\s*;", code):
            self.typedefs.setdefault(m.group(2), ("type", m.group(1)))
        for m in _PROTO_RE.finditer(code):
            if m.group(1) not in self.protos:
                self.protos[m.group(1)] = [_param_type(p) for p in _split_top(m.group(2), ",")]

    # -- layouts ----------------------------------------------------------

    def type_layout(self, text, depth=0):
        """The layout of a type spelling ("const struct X", "u16", "Name")."""
        if depth > 16:
            return None
        if "*" in text:
            return _scalar(4)
        words = [w for w in re.findall(r"[A-Za-z_]\w*", text) if w not in _QUALIFIERS]
        if not words:
            return _scalar(4) if re.search(r"\b(un)?signed\b", text) else None
        if words[0] in ("struct", "union", "enum"):
            if len(words) < 2:
                return None
            if words[0] == "enum":
                return _scalar(4)
            if words[0] == "union":
                body = self.unions.get(words[1])
                return None if body is None else self._union_layout(("union", words[1]), body, depth + 1)
            return self.struct_layout(words[1], depth + 1)
        if words == ["long", "long"]:
            return _scalar(8)
        name = words[-1]
        if name in SCALARS:
            return _scalar(SCALARS[name])
        td = self.typedefs.get(name)
        if td is None:
            return None
        if td[0] == "union":
            return None
        if td[0] == "ubody":
            return self._union_layout(("typedef", name), td[1], depth + 1)
        if td[0] == "body":
            return self._body_layout(("typedef", name), td[1], depth + 1)
        return self.type_layout(td[1], depth + 1)

    def struct_layout(self, tag, depth=0):
        body = self.structs.get(tag)
        if body is None:
            return None
        return self._body_layout(("struct", tag), body, depth)

    def _union_layout(self, key, body, depth):
        """A union laid out when every member has one layout (MtxFx33: nine
        fx32 by element, by row and column, and flat)."""
        if key not in self._cache:
            self._cache[key] = None         # a cycle is unknown
            self._cache[key] = self._union(body, depth)
        return self._cache[key]

    def _body_layout(self, key, body, depth):
        if key in self._cache:
            return self._cache[key]
        self._cache[key] = None             # a cycle is unknown
        # A block stand-in says nothing of the bytes' layout, here or as a
        # member of another struct.
        lay = None if _is_block_body(body) else self._lay_out(body, depth)
        self._cache[key] = lay
        return lay

    def _members(self, member, depth):
        """[(Layout or None, count)] for one member declaration."""
        dims_re = r"((?:\[[^\]]*\]\s*)*)"
        if "{" in member:                   # struct { ... } a, b[2];
            um = re.match(r"(struct|union)\s*\{", member)
            if um is None:
                return [(None, 1)]
            i = member.index("{")
            end = _match_brace(member, i)
            body = member[i + 1:end - 1]
            inner = (self._lay_out(body, depth + 1) if um.group(1) == "struct"
                     else self._union(body, depth + 1))
            if not member[end:].strip():
                return [(inner, 1)]         # an anonymous member: its fields are the outer's
            return [(inner, _count(re.match(r"\s*\**\s*[A-Za-z_]\w*\s*" + dims_re, d).group(1)))
                    if re.match(r"\s*[A-Za-z_]\w*", d) else (None, 1)
                    for d in _split_top(member[end:], ",") if d.strip()]
        if "(" in member:                   # void (*fn[2])(int)
            m = re.search(r"\(\s*\*\s*[A-Za-z_]\w*\s*" + dims_re + r"\)", member)
            return [(_scalar(4), _count(m.group(1)) if m else None)]
        parts = _split_top(member, ",")
        m = re.match(r"(.*?)(\**)\s*\b([A-Za-z_]\w*)\s*" + dims_re + r"$", parts[0].strip(), re.S)
        if m is None:
            return [(None, 1)]
        base = None if m.group(2) else self.type_layout(m.group(1), depth + 1)
        out = [(self._pointer(m.group(1), len(m.group(2)), depth) if m.group(2) else base,
                _count(m.group(4)))]
        for d in parts[1:]:
            dm = re.match(r"\s*(\**)\s*([A-Za-z_]\w*)\s*" + dims_re + r"$", d)
            if dm is None:
                return [(None, 1)]
            if base is None and not dm.group(1):
                base = self.type_layout(m.group(1), depth + 1)
            out.append((self._pointer(m.group(1), len(dm.group(1)), depth) if dm.group(1)
                        else base, _count(dm.group(3))))
        return out

    def _pointer(self, pointee_text, stars, depth):
        """A pointer member: a word, and the layout of what it points at when
        it is a single pointer to a struct or a multi-byte scalar the module
        lays out."""
        word = _scalar(4)
        if depth < 8 and stars == 1 and "*" not in pointee_text:
            pl = self.type_layout(pointee_text, depth + 1)
            if pl is not None and pl.runs:
                word = Layout(4, 4, [(0, 4, 4)], [(0, pl)])
        return word

    def _union(self, body, depth):
        """A union laid out only when every member has the same layout (the
        G3D setup's `union { u32 id; const char *path; }` is one word)."""
        lays = [self._lay_out(m.strip() + ";", depth) for m in _split_top(body, ";") if m.strip()]
        if not lays or any(l is None for l in lays):
            return None
        return lays[0] if len({l.key() for l in lays}) == 1 else None

    def _lay_out(self, body, depth):
        if "__attribute__" in body or ":" in body:
            return None                     # an attribute or a bit-field
        off, align, runs, ptrs = 0, 1, [], []
        for member in _split_top(body, ";"):
            member = member.strip()
            if not member:
                continue
            for lay, count in self._members(member, depth):
                if lay is None or count is None:
                    return None
                off = (off + lay.align - 1) // lay.align * lay.align
                for i in range(count):
                    base = off + i * lay.size
                    runs.extend((base + o, s, w) for o, s, w in lay.runs)
                    ptrs.extend((base + o, pl) for o, pl in lay.ptrs)
                off += lay.size * count
                align = max(align, lay.align)
        size = (off + align - 1) // align * align
        return Layout(size, align, _merge(runs), ptrs)


def _count(dims):
    """The element count of `[a][b]...`, 1 for none, None if not numbers
    (or a flexible `[]`)."""
    n = 1
    for dim in re.findall(r"\[([^\]]*)\]", dims or ""):
        v = _number(dim)
        if v is None:
            return None
        n *= v
    return n


def _merge(runs):
    out = []
    for o, s, w in sorted(runs):
        if out and out[-1][2] == w and out[-1][0] + out[-1][1] == o:
            out[-1] = (out[-1][0], out[-1][1] + s, w)
        else:
            out.append((o, s, w))
    return out


_HEADER_DEFS = {}


def _header_defs(path, seen):
    key = os.path.normcase(os.path.abspath(path))
    if key in _HEADER_DEFS:
        return _HEADER_DEFS[key]
    texts = []
    _HEADER_DEFS[key] = texts               # a cycle stops here
    if key in seen or not os.path.isfile(path):
        return texts
    seen.add(key)
    with open(path, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    texts.append(text)
    for inc in _INCLUDE_RE.findall(text):
        texts.extend(_header_defs(os.path.join(os.path.dirname(path), inc), seen))
    return texts


def defs_for(path, text):
    """The definitions `text` (the file at `path`) sees: its own first, then
    its headers', in include order."""
    d = Defs()
    d.add_text(text)
    for inc in _INCLUDE_RE.findall(text):
        for t in _header_defs(os.path.join(os.path.dirname(path), inc), set()):
            d.add_text(t)
    return d


_PROTO_RE = re.compile(
    r"^[ \t]*(?:extern[ \t]+)?(?:const[ \t]+)?(?:struct[ \t]+)?[A-Za-z_]\w*[\s*]+"
    r"([A-Za-z_]\w*)\s*\(([^;{}()]*)\)\s*;", re.M)
_CALL_RE = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
_NOT_CALLS = {"if", "while", "for", "switch", "return", "sizeof", "defined", "__attribute__"}
_CAST_PREFIX = re.compile(
    r"\(\s*(?:const\s+|volatile\s+)*(?:struct\s+|union\s+)?[A-Za-z_]\w*[\s*]*\)\s*")


def _param_type(param):
    """A parameter's type, its name dropped ("const struct X *p" ->
    "const struct X *")."""
    param = param.strip()
    m = re.match(r"(.*?[\s*])([A-Za-z_]\w*)\s*(\[[^\]]*\])?$", param)
    if m and m.group(1).strip() and m.group(2) not in _QUALIFIERS:
        return m.group(1).strip() + ("*" if m.group(3) is not None else "")
    return param


def _match_paren(text, i):
    """The index just past the parenthesis that closes the one at text[i]."""
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "(":
            depth += 1
        elif text[j] == ")":
            depth -= 1
            if depth == 0:
                return j + 1
    return -1


def _arg_target(arg):
    """What one call argument names: ("name", n) for `n`, `&n` or a cast of
    either; ("addr", a) for a cast of a literal 0x02xxxxxx address; else
    None."""
    a = arg.strip()
    for _ in range(8):
        before = a
        a = a.lstrip("&").strip()
        if _CAST_PREFIX.match(a):
            a = _CAST_PREFIX.sub("", a, count=1)
        if a.startswith("(") and _match_paren(a, 0) == len(a):
            a = a[1:-1].strip()
        if a == before:
            break
    if re.fullmatch(r"[A-Za-z_]\w*", a):
        return ("name", a)
    m = re.fullmatch(r"0x(0?2[0-9A-Fa-f]{6})[uUlL]*", a)
    return ("addr", int(m.group(1), 16)) if m else None


def call_args(code):
    """[(callee, argument index, target)] for the arguments of every call in
    `code` (comments stripped) that name data: a name or a literal address
    (_arg_target)."""
    out = []
    for m in _CALL_RE.finditer(code):
        if m.group(1) in _NOT_CALLS:
            continue
        end = _match_paren(code, m.end() - 1)
        if end < 0:
            continue
        for i, arg in enumerate(_split_top(code[m.end():end - 1], ",")):
            t = _arg_target(arg)
            if t is not None:
                out.append((m.group(1), i, t))
    return out


def _arg_types(d, sf):
    """{target: set of struct type texts} from the parameters the calls in
    `sf` pass data to: a parameter `const struct DispVram *` says what the
    object it is given is."""
    out = {}
    for callee, i, target in getattr(sf, "call_args", ()):
        params = d.protos.get(callee)
        if params is None or i >= len(params):
            continue
        t = params[i]
        if t.count("*") != 1:
            continue
        out.setdefault(target, set()).add(t.replace("*", " ").strip())
    return out


def _scalar_text(type_text):
    """Whether a declaration's type is a plain scalar (u32 d_X[] is how the
    decomp often spells a struct it casts the name to)."""
    words = [w for w in re.findall(r"[A-Za-z_]\w*", type_text) if w not in _QUALIFIERS]
    return bool(words) and words[-1] in SCALARS or words == ["long", "long"]


def _one_layout(d, type_texts):
    """The one struct layout a set of type spellings agrees on, or None."""
    found = {}
    for t in type_texts:
        cl = real_struct(d, t)
        if cl is not None:
            found[cl.key()] = cl
    return next(iter(found.values())) if len(found) == 1 else None


def _is_block_body(body):
    """A struct whose one member is an array of one scalar type: how the
    decomp spells "these bytes, copied whole" (BgTemplateBlock's u32 w[8]),
    not what the code reads them as."""
    members = [m.strip() for m in _split_top(strip_comments(body), ";") if m.strip()]
    if len(members) != 1:
        return False
    m = re.match(r"([A-Za-z_][\w\s]*?)\s+[A-Za-z_]\w*\s*\[[^\]]*\]$", members[0])
    if m is None:
        return False
    words = [w for w in m.group(1).split() if w not in _QUALIFIERS]
    return len(words) <= 1 and (not words or words[0] in SCALARS)


def opaque(d, type_text):
    """Whether a declaration's type says nothing about the layout: bytes
    (char, u8, unsigned char) or a block stand-in."""
    words = [w for w in re.findall(r"[A-Za-z_]\w*", type_text) if w not in _QUALIFIERS]
    if "*" in type_text:
        return False
    if not words or words == ["char"] or words[-1] in ("u8", "s8", "char"):
        return True
    if words[0] == "struct" and len(words) > 1:
        body = d.structs.get(words[1])
        return body is not None and _is_block_body(body)
    td = d.typedefs.get(words[-1])
    while td is not None and td[0] == "type":
        inner = [w for w in re.findall(r"[A-Za-z_]\w*", td[1]) if w not in _QUALIFIERS]
        if not inner:
            return False
        if inner[0] == "struct" and len(inner) > 1:
            body = d.structs.get(inner[1])
            return body is not None and _is_block_body(body)
        td = d.typedefs.get(inner[-1])
    return td is not None and td[0] == "body" and _is_block_body(td[1])


def real_struct(d, type_text):
    """The layout of a declaration's type when it is a struct the module lays
    out for certain and is not a block stand-in.  A struct of byte fields
    counts too: its runs are none, and the metadata's guess of words over it
    goes (gStartMenuWindowTemplates, {u8 bgId, x, y, width, height,
    palette}[14], which data_objects.json has as 21 u32)."""
    words = [w for w in re.findall(r"[A-Za-z_]\w*", type_text) if w not in _QUALIFIERS]
    if "*" in type_text or not words or words[-1] in SCALARS or opaque(d, type_text):
        return None
    lay = d.type_layout(type_text)
    return lay if lay is not None and lay.size > 0 else None


_FILE_DEFS = {}


def file_defs(path):
    """defs_for the file at `path`, read and parsed once per run."""
    key = os.path.normcase(os.path.abspath(path))
    if key not in _FILE_DEFS:
        try:
            with open(path, encoding="utf-8", errors="replace") as fh:
                text = fh.read()
        except OSError:
            text = ""
        _FILE_DEFS[key] = defs_for(path, text)
    return _FILE_DEFS[key]


def declared_layouts(sf, data_names):
    """{name: Layout} for one source file's data names: its declared struct
    type where that is a real struct, else, where the declared type is
    opaque, the one struct type the file casts the name (or a local copied
    from it) to, or passes it to a parameter of.  `sf` has .path,
    .decl_types {name: type text}, .cast_types {name: set of type texts},
    .copies {local: set of names} and .call_args (call_args)
    (tools/armdata.py, SourceFile)."""
    decls = getattr(sf, "decl_types", None)
    if not decls:
        return {}
    d = file_defs(sf.path)
    casts_of = {}
    for name, types in sf.cast_types.items():
        casts_of.setdefault(name, set()).update(types)
    for local, srcs in sf.copies.items():
        for src in srcs:
            casts_of.setdefault(src, set()).update(sf.cast_types.get(local, ()))
    args = _arg_types(d, sf)
    out = {}
    for name, type_text in decls.items():
        if name not in data_names:
            continue
        lay = real_struct(d, type_text)
        if lay is None and "*" not in type_text and (opaque(d, type_text)
                                                     or _scalar_text(type_text)):
            lay = _one_layout(d, casts_of.get(name, set()) | args.get(("name", name), set()))
        if lay is not None:
            out[name] = lay
    return out


def address_layouts(sf):
    """{address: Layout} for the literal addresses one source file passes to
    a function whose parameter is a struct pointer
    (`BG_SYS_SetVramBanks((void *)0x021B68F0)`, a const struct DispVram *),
    when the calls agree on one struct.  The caller keeps only addresses
    where its region has a data object (tools/armdata.py, layout_runs)."""
    if not getattr(sf, "call_args", None):
        return {}
    d = file_defs(sf.path)
    out = {}
    for target, types in _arg_types(d, sf).items():
        if target[0] == "addr":
            lay = _one_layout(d, types)
            if lay is not None:
                out[target[1]] = lay
    return out
