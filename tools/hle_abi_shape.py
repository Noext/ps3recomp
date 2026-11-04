#!/usr/bin/env python3
"""hle_abi_shape.py -- the one place that decides what an HLE handler's call
ABI *is*, so the NID table generator and the ABI audit cannot disagree about it.

Why this exists
---------------
The HLE dispatcher used to reach every table handler through a single cast:

    typedef uint64_t (*hle_generic)(uint64_t x8);
    hle_generic fn = (hle_generic)e->handler;      /* ppu_hle.cpp, old */
    ctx->gpr[3] = fn(gpr[3], ..., gpr[10]);

That is an *indirect* call through a signature the handler does not have. On
x86-64 SysV it is benign: six register args are spilled, the callee reads the
ones its prototype declares and ignores the rest, and caller and callee agree
on which register carries the return value because everything fits in RAX.
Under wasm64 an indirect call is checked against the callee's own function type
at the instruction, so it traps:

    RuntimeError: null function or function signature mismatch

and the boot smoke dies at the first HLE import. Measured on this repo before
the fix: cellFsClose reached through the old cast traps under wasm64 with
exactly that message while linking and running fine natively.

The fix is to stop making an indirect call with a signature that is not the
callee's. Every handler is registered behind a *trampoline* that really does
have the uniform 8x u64 signature, and which calls the handler through its own
declared prototype. Under wasm64 that call is a direct one, so the type matches
by construction.

What this module supplies
-------------------------
The trampoline a handler gets is derived from the handler's C prototype, which
this module reads out of the HLE library sources. A C prototype maps onto a set
of *ABI classes* -- how each argument is actually passed -- not onto the
spelling of its type. u32, s32, int, b32, an enum and float all travel in a
32-bit slot; u64, s64, a pointer and size_t share a 64-bit slot. Two handlers
with the same class list produce identical machine code, so they can share one
trampoline shape.

Three things are NOT expressible this way and are reported, never guessed:

  * a float/double *argument*. The PPC ABI passes those in f1..f13, which the
    dispatcher does not marshal at all -- that is a separate bug (see
    find_float_handlers), and building an integer-class trampoline for such a
    handler would silently feed it a register the guest never wrote;
  * a float/double *return value*, same reason in the other direction;
  * a struct/union passed or returned *by value* -- on PPC64 ELFv1 these are
    indirected by the caller and would need per-struct marshaling rules.

A variadic handler (only the _sys_* printf family) is expressible, but its
trailing arguments must be forwarded as unsigned long long. That is what the
one consumer of those varargs (yz_format) always reads, on both the PS3 ABI
(where varargs travel in r3..r10, 8 bytes each) and the host ABI (where the
first four travel in registers and the rest on the stack) -- see the note on
VARARG_FORWARD_CTYPE. Forwarding them as int would truncate to 4 bytes and
misalign every argument after the first.

Stdlib-only, py -3, LF line endings, 4-space indent.
"""

from __future__ import annotations

import glob
import os
import re

# ---------------------------------------------------------------------------
# What a handler's arguments and return value look like at the ABI level
# ---------------------------------------------------------------------------

#: How one argument or return value is passed. These are *widths and classes*,
#: not spellings: i32 covers u8/u16/u32/s32/int/bool/enums, i64 covers
#: u64/s64/long/pointers/size_t. wasm64 is LP64 like the native POSIX builds
#: (verified: sizeof(long)==8, sizeof(void*)==8 under both), so a single map
#: serves native and wasm.
CLASS_I32 = "i32"
CLASS_I64 = "i64"
CLASS_F32 = "f32"
CLASS_F64 = "f64"
CLASS_AGG = "AGG"        # struct/union by value: not expressible here
CLASS_VARARG = "..."     # C ellipsis in the trailing position
CLASS_VOID = "void"      # return position only
CLASS_UNKNOWN = "?"

#: The dispatcher has exactly eight GPR argument slots (r3..r10). A handler
#: declaring more than this cannot be called from the table at all -- those are
#: the cellGcmSetZcull / cellSaveDataListAutoLoad / cellSpursWorkloadAttribute
#: family, which go through ps3_hle_register_ctx instead. The generator reports
#: one rather than emitting a trampoline that silently drops arguments.
DISPATCH_ARG_SLOTS = 8


#: Integer widths. The 32-bit set is deliberately generous: every spelling the
#: Cell SDK headers use for something that occupies one 32-bit slot belongs here,
#: including the b8/b16/b32 wrappers from ps3types.h and the fixed-width names.
_I32 = {
    "u8", "s8", "char", "int8_t", "uint8_t", "b8", "bool",
    "u16", "s16", "short", "int16_t", "uint16_t", "b16",
    "u32", "s32", "int", "unsigned", "unsigned int", "int32_t", "uint32_t",
    "b32", "unsigned char", "signed char", "unsigned short", "signed short",
}
_I64 = {
    "u64", "s64", "long long", "unsigned long long", "int64_t", "uint64_t",
    "b64", "unsigned long", "long", "signed long",
}
_F32 = {"float", "f32"}
_F64 = {"double", "f64", "float64_t"}
_PTR_SIZED = {"size_t", "ssize_t", "ptrdiff_t", "intptr_t", "uintptr_t",
              "intmax_t", "uintmax_t", "off_t"}

#: The C type a trampoline forwards a variadic handler's trailing arguments as.
#: Not int: see the module docstring. yz_format -- the only reader of these
#: varargs in the tree -- fetches every conversion with
#: va_arg(ap, unsigned long long), and the PS3 ABI widens varargs into an
#: 8-byte register slot, so 8 bytes is both what the guest wrote and what the
#: reader expects. A %d-style conversion then reads the low 32 bits of that,
#: which is where the PS3 ABI put the value.
VARARG_FORWARD_CTYPE = "unsigned long long"


class Param:
    """One declared parameter, with its ABI class resolved."""

    __slots__ = ("raw_type", "name", "cls", "detail")

    def __init__(self, raw_type, name, cls, detail=None):
        self.raw_type = raw_type
        self.name = name
        self.cls = cls
        #: For an unexpressible class: the type spelling that defeated us, so
        #: the report can name it instead of saying "unknown".
        self.detail = detail

    def __repr__(self):
        return "Param(%r -> %s)" % (self.raw_type, self.cls)


class Signature:
    """A handler's prototype, as an ABI shape plus enough detail to report."""

    __slots__ = ("name", "ret_raw", "ret_cls", "params", "file", "line", "why")

    def __init__(self, name, ret_raw, ret_cls, params, file, line, why=None):
        self.name = name
        self.ret_raw = ret_raw
        self.ret_cls = ret_cls
        self.params = params
        self.file = file
        self.line = line
        #: Non-None when the shape cannot be expressed as an integer trampoline;
        #: a human-readable reason naming the offending type.
        self.why = why

    @property
    def expressible(self):
        return self.why is None

    def arg_classes(self):
        return tuple(p.cls for p in self.params)

    def shape(self):
        """The identity of the trampoline this handler needs: (args, ret).

        Two handlers with equal shape() can -- and do -- share one generated
        trampoline body. The ellipsis participates in the key, because a
        variadic trampoline forwards its tail differently from a fixed one.
        """
        return (self.arg_classes(), self.ret_cls)

    def pretty(self):
        args = ", ".join(("%s %s" % (p.raw_type, p.name)).strip()
                         for p in self.params)
        return "%s %s(%s)" % (self.ret_raw, self.name, args or "void")


# ---------------------------------------------------------------------------
# Typedef table: a prototype's spelling has to be reduced to a base type, and
# the Cell SDK expresses most widths through typedefs (CellFsFd, CellSpurs*,
# CellVoicePortId, sys_heap_t, ...). Built from the headers the compiler sees.
# ---------------------------------------------------------------------------

#: Marker for "this typedef names a struct/union (possibly an opaque one)".
_AGG = "<<AGG>>"
#: Marker for "this typedef names a function pointer".
_FNPTR = "<<FNPTR>>"

#: A single regex, applied with finditer, that picks up all four typedef forms
#: in one pass over a line, left to right -- so a later looser match can never
#: run before an earlier precise one for the same name. The alternation is
#: ordered: function pointer, aggregate/enum body, opaque alias, plain alias.
_TYPEDEF_RE = re.compile(
    r"typedef\s+(?P<fp>[A-Za-z_][\w\s\*]*?\(\s*\*\s*(?P<fpname>[A-Za-z_]\w*)\s*\)"
    r"\s*\([^;]*\))\s*;"
    r"|typedef\s+(?P<aggtag>struct|union|enum)\b[^;{]*\{[^}]*\}\s*"
    r"(?P<aggname>[A-Za-z_]\w*)\s*;"
    r"|typedef\s+(?P<opqtag>struct|union)\s+[A-Za-z_]\w*\s+"
    r"(?P<opqname>[A-Za-z_]\w*)\s*;"
    r"|typedef\s+(?P<plain>[A-Za-z_][\w\s\*]*?)\s+(?P<plainname>[A-Za-z_]\w*)\s*;",
    re.S)


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/",
                  lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def build_typedef_map(paths):
    """name -> underlying spelling, or one of the _AGG / _FNPTR markers.

    The regex alternation is ordered so that for any single typedef the most
    specific form matches first; a name is still recorded only once, because
    the Cell headers define each typedef exactly once in the translation unit
    the compiler sees (duplicates across headers are the same definition).
    """
    tdefs = {}
    for path in paths:
        try:
            with open(path, "r", encoding="latin1") as f:
                text = _strip_comments(f.read())
        except OSError:
            continue
        for m in _TYPEDEF_RE.finditer(text):
            if m.group("fpname"):
                tdefs.setdefault(m.group("fpname"), _FNPTR)
            elif m.group("aggname"):
                # An enum is an int, not an aggregate: CellMsgDialogType is a
                # plain u32 by the time it reaches the ABI.
                tdefs.setdefault(m.group("aggname"),
                                 _AGG if m.group("aggtag") in ("struct", "union")
                                 else "int")
            elif m.group("opqname"):
                tdefs.setdefault(m.group("opqname"), _AGG)
            elif m.group("plainname"):
                tdefs.setdefault(m.group("plainname"),
                                 re.sub(r"\s+", " ", m.group("plain")).strip())
    return tdefs


def _base_spelling(raw):
    """Drop cv-qualifiers/storage classes and collapse whitespace.

    Does NOT drop the star: whether a pointer is present is the whole question
    for pointer-vs-value, and it is answered before this is called.
    """
    t = re.sub(r"\b(const|volatile|register|restrict|__restrict)\b", " ", raw)
    return re.sub(r"\s+", " ", t).strip()


def classify_type(raw, tdefs, depth=0):
    """Reduce one parameter/return spelling to (class, detail).

    detail is only meaningful for CLASS_UNKNOWN and names the spelling that
    could not be resolved, so a report can say "unresolved: Foo" rather than
    "unknown".
    """
    if depth > 12:                       # typedef cycles are not a real risk
        return CLASS_UNKNOWN, raw
    t = _base_spelling(raw)
    if not t:
        return CLASS_UNKNOWN, raw

    # An array parameter decays to a pointer. u64 args[8] is the one site.
    if re.search(r"\[[^\]]*\]\s*$", t):
        return CLASS_I64, None
    # A pointer is a guest address or a host pointer, 8 bytes either way.
    if t.endswith("*") or "(*" in t:
        return CLASS_I64, None

    if t in tdefs:
        under = tdefs[t]
        if under == _FNPTR:
            return CLASS_I64, None
        if under == _AGG:
            return CLASS_AGG, t
        if "(*" in under:
            return CLASS_I64, None
        return classify_type(under, tdefs, depth + 1)

    b = t.lower()
    if b in _F32:
        return CLASS_F32, None
    if b in _F64:
        return CLASS_F64, None
    if b in _PTR_SIZED:
        # LP64 on every target this runtime builds for, wasm64 included.
        return CLASS_I64, None
    if b in _I32:
        return CLASS_I32, None
    if b in _I64:
        return CLASS_I64, None
    if b == "void":
        return CLASS_VOID, None
    return CLASS_UNKNOWN, b


# ---------------------------------------------------------------------------
# Prototype extraction from the HLE library sources
# ---------------------------------------------------------------------------

#: A non-static top-level function definition. Mirrors gen_hle_nids.DEF_RE, and
#: additionally captures the return type and the parameter list. The return
#: type is taken from the START of the line, which is how every definition in
#: libs/ is written; a definition whose return type wraps onto its own line
#: would be missed, and the audit reports that as a missing signature rather
#: than guessing.
_DEF_RE = re.compile(
    r"^(?!static)(?P<ret>[A-Za-z_][\w \t\*]*?)\b"
    r"(?P<name>_?(?:cell|sys|sce|_sys)\w+)\s*"
    r"\((?P<params>[^;{}]*?)\)\s*\n?\s*\{",
    re.M)


def _split_top_level_commas(s):
    out, cur, depth = [], [], 0
    for ch in s:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    if cur:
        out.append("".join(cur))
    return [p.strip() for p in out if p.strip()]


_IDENT = r"[A-Za-z_][A-Za-z0-9_]*"


def _split_param(part):
    """('Type', 'name') from one parameter declaration; name may be empty."""
    # RetType (*name)(args)  -- a raw function-pointer parameter
    m = re.match(r"^(.+?\(\s*\*\s*)(" + _IDENT + r")(\s*\).*)$", part)
    if m:
        return (m.group(1) + m.group(3)).strip(), m.group(2)
    m = re.match(r"^(.*?[\s\*])(" + _IDENT + r")$", part)
    if m:
        return m.group(1).strip(), m.group(2)
    return part.strip(), ""


def parse_signature(name, ret_raw, params_raw, path, line, tdefs):
    """Build a Signature, resolving every parameter's ABI class."""
    ret_cls, ret_detail = classify_type(ret_raw, tdefs)
    params, why = [], None

    stripped = params_raw.strip()
    parts = [] if stripped in ("", "void") else _split_top_level_commas(stripped)
    for part in parts:
        if part.strip() == "...":
            params.append(Param(part, "", CLASS_VARARG))
            continue
        raw_type, pname = _split_param(part)
        cls, detail = classify_type(raw_type, tdefs)
        params.append(Param(raw_type, pname, cls, detail))

    if ret_cls in (CLASS_F32, CLASS_F64):
        why = ("returns %s -- a floating-point return travels in f1 on the PS3 "
               "ABI, which the dispatcher does not marshal"
               % ret_raw.strip())
    elif ret_cls == CLASS_AGG:
        why = ("returns %s by value -- PPC64 ELFv1 indirects a by-value "
               "aggregate return through a hidden pointer"
               % ret_raw.strip())
    elif ret_cls == CLASS_UNKNOWN:
        why = "unresolved return type %r" % (ret_detail,)
    else:
        for p in params:
            if p.cls in (CLASS_F32, CLASS_F64):
                why = ("takes %s %s -- a floating-point argument travels in "
                       "f1..f13 on the PS3 ABI, which the dispatcher does not "
                       "marshal" % (p.raw_type.strip(), p.name))
                break
            if p.cls == CLASS_AGG:
                why = ("takes %s %s by value -- a by-value aggregate needs "
                       "per-struct marshaling rules"
                       % (p.raw_type.strip(), p.name))
                break
            if p.cls == CLASS_UNKNOWN:
                why = "unresolved parameter type %r" % (p.detail,)
                break

    return Signature(name, ret_raw.strip(), ret_cls, params, path, line, why)


def find_definitions(paths):
    """name -> (ret_raw, params_raw, path, line) for every module-prefixed,
    non-static function definition in paths (first definition wins)."""
    out = {}
    for path in paths:
        try:
            with open(path, "r", encoding="latin1") as f:
                text = _strip_comments(f.read())
        except OSError:
            continue
        for m in _DEF_RE.finditer(text):
            name = m.group("name")
            if name in out:
                continue
            line = text.count("\n", 0, m.start()) + 1
            out[name] = (m.group("ret").strip(), m.group("params"), path, line)
    return out


def library_sources(root):
    """Every libs/**/*.c, which is where the HLE handlers are defined."""
    return sorted(glob.glob(os.path.join(root, "libs", "**", "*.c"),
                            recursive=True))


def library_headers(root):
    """Every libs/**/*.h, which is where their prototypes' typedefs live.

    include/**/*.h and runtime/**/*.h are added too: ps3types.h defines the
    b8/b16/b32 wrappers and ppu_memory.h the vm_* helpers, and a handler
    parameter may legitimately name either.
    """
    pats = [os.path.join(root, "libs", "**", "*.h"),
            os.path.join(root, "include", "**", "*.h"),
            os.path.join(root, "runtime", "**", "*.h")]
    seen, out = set(), []
    for pat in pats:
        for p in sorted(glob.glob(pat, recursive=True)):
            rp = os.path.realpath(p)
            if rp not in seen:
                seen.add(rp)
                out.append(p)
    return out


def analyse(root):
    """Every handler prototype in the tree, as name -> Signature.

    root is the repo root (the directory holding libs/, include/, runtime/).
    """
    tdefs = build_typedef_map(library_headers(root))
    defs = find_definitions(library_sources(root))
    sigs = {}
    for name, (ret, params, path, line) in defs.items():
        sigs[name] = parse_signature(name, ret, params, path, line, tdefs)
    return sigs


def find_float_handlers(sigs, names=None):
    """Handlers whose prototype involves a float/double in an argument or the
    return value, i.e. the ones this trampoline scheme cannot call correctly.

    Kept separate from the generator so the audit and the report can enumerate
    them without regenerating anything.
    """
    out = []
    for name in sorted(names if names is not None else sigs):
        sig = sigs.get(name)
        if sig is None:
            continue
        if any(p.cls in (CLASS_F32, CLASS_F64) for p in sig.params) or \
           sig.ret_cls in (CLASS_F32, CLASS_F64):
            out.append(sig)
    return out
