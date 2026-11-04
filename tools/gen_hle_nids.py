#!/usr/bin/env python3
"""
Generate the HLE NID -> handler registration unit for the HLE library sources.

What it emits, and why
----------------------
Every handler is registered behind a *trampoline* that really has the uniform
dispatcher signature

    uint64_t thunk(uint64_t a0, ..., uint64_t a7)

and which calls the handler through the handler's own ABI shape. That is a
correctness requirement, not a style choice: the dispatcher used to cast each
handler to one 8x-u64 signature and call it through that cast, and under wasm64
an indirect call is checked against the callee's function type, so the first
HLE import a title reaches traps:

    RuntimeError: null function or function signature mismatch

The trampoline moves the mismatched call out of the dispatcher (where the type
is whatever the table row was handed) into generated code (where the type is
derived from the handler's own prototype, so it matches by construction).

Two kinds of trampoline are emitted, because wasm64's type check forces a
distinction:

  * fixed-arity handlers -- one *shared* body per ABI shape, taking the
    handler's address as an extra first parameter. The 1196 that fit the
    dispatcher's 8 GPR slots collapse onto 98 shapes, so the unit stays small and
    each shape is auditable on its own. A shared body works because a function
    reference cast to the exact function-pointer type the callee has carries what
    the call needs; the shape came from that callee's prototype, so the cast is
    exact by construction.

  * variadic handlers (_sys_printf/_sys_sprintf/_sys_snprintf) -- one bespoke
    body each, calling the handler *by name* through a prototype that keeps the
    ellipsis. A cast to a fixed 8-argument type is a different wasm type from
    "(fixed..., ...)" and wasm-ld refuses to pair them, so the ellipsis has to
    survive into the declaration the compiler sees. The trailing slots are
    forwarded as VARARG_FORWARD_CTYPE; see hle_abi_shape for why that width.

Handlers this scheme cannot call are registered exactly as they are today and
reported on stderr rather than guessed at:

  * float/double arguments or returns -- the PS3 ABI passes those in f1..f13,
    which the dispatcher does not marshal at all. That is a separate, pre-
    existing bug (natively these handlers already receive garbage); building an
    integer trampoline for them would silently paper over it.
  * more parameters than the dispatcher has GPR slots for (r3..r10, i.e. 8).

Both groups keep today's behaviour bit for bit, so this change cannot regress
them on either target. They are listed in docs/HLE_DISPATCH_ABI.md.

Return values: an i32 result is zero-extended into r3, not sign-extended. That
is what the old cast produced natively (its return type was uint64_t, so only
the low half of the 32-bit result was meaningful), and a title reading CELL_OK
or a negative Cell error as (s32)r3 sees the same value either way.

See tools/hle_abi_shape.py for the prototype -> ABI-class mapping itself.

Usage:
    python gen_hle_nids.py --out gen/ppu_hle_nids.cpp cellGcmSys [cellSysutil ...]
    (lib names resolve to libs/**/<name>.c)
"""
import argparse
import glob
import os
import sys
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nid_database import compute_nid
import hle_abi_shape as shape

# Non-static, top-level definitions of exported-looking functions. The NID is
# COMPUTED from the name (the firmware-correct algorithm in nid_database.py),
# not read from a /* NID: 0x.. */ comment -- computation matches the real
# imports far better and needs no manual annotation.
DEF_RE = re.compile(
    r"^(?!static)[A-Za-z_][\w \t\*]*?\b(_?(?:cell|sys|sce|_sys)\w+)\s*\([^;{}]*?\)\s*\{",
    re.M)

#: C spelling of each ABI class, for the generated declarations and casts.
#: int32_t/int64_t rather than u32/s32/u64/s64 on purpose: the class is a width,
#: and using one spelling per class keeps the emitted casts visibly identical to
#: the declarations they were derived from.
def scan(c_path):
    txt = open(c_path, encoding="latin1").read()
    out = []
    for m in DEF_RE.finditer(txt):
        name = m.group(1)
        out.append((compute_nid(name), name))
    return out


def discover_all():
    """Every PRX-style HLE module compiled into the runtime lib.

    The registration table must cover EVERY library the runtime implements:
    an unregistered import silently fakes CELL_OK (see the note below), and a
    hand-passed module list is the single point where a whole library gets
    dropped by accident -- exactly what happened when gen/ppu_hle_nids.cpp was
    regenerated with only cellFs/cellGcmSys/cellSysmodule and LBP then called
    cellSysutilCheckCallback unregistered, faked it, and diverged at boot. So
    `--all` is the canonical invocation; discover the same dirs CMakeLists.txt
    globs into ps3recomp_runtime, and take the cell*/sce*/sys* file names."""
    dirs = ["system", "filesystem", "input", "audio", "video", "network",
            "spurs", "sync", "codec", "font", "misc", "hardware"]
    names = set()
    for d in dirs:
        for p in glob.glob(os.path.join(ROOT, "libs", d, "*.c")):
            n = os.path.splitext(os.path.basename(p))[0]
            if re.match(r"^(cell|sce|sys)", n) and not re.match(r"^(test_|validate_)", n):
                names.add(n)
    return sorted(names)


_CTYPE = {
    shape.CLASS_I32: "int32_t",
    shape.CLASS_I64: "int64_t",
    shape.CLASS_VOID: "void",
}


def _class_params(params):
    """C parameter-type list for one handler's ABI-class declaration."""
    out = ["..." if p.cls == shape.CLASS_VARARG else _CTYPE[p.cls] for p in params]
    return ", ".join(out) if out else "void"


def _thunk_name(shape_key):
    """A stable C identifier for one ABI shape, e.g. hle_thunk_i32_i64__i32.

    Printable identity matters: this unit is read by a human when a dispatch
    question comes up, and hle_thunk_i64_void__void says what it does where
    hle_thunk_17 does not.
    """
    args, ret = shape_key
    stem = "_".join(args) if args else "void"
    return "hle_thunk_%s__%s" % (stem, ret)


def _emit_shared_thunks(out, shapes):
    """One body per distinct fixed-arity ABI shape.

    Each has the uniform 8-slot dispatcher signature; the first parameter is the
    handler's own address. The body casts that address to the signature this
    shape names -- exactly the type the handler's declaration below gives the
    symbol -- and calls through it. The trailing slots the shape does not use are
    left unnamed, which C++ allows.
    """
    out.append(
        "/* ---- shared trampolines, one per fixed-arity ABI shape ----\n"
        " *\n"
        " * Each is the uniform 8-slot dispatcher signature, so the dispatcher can\n"
        " * call any of them through one function-pointer type. The first parameter\n"
        " * is the handler's own address; the body casts it back to the signature\n"
        " * this shape names -- the same type the handler's declaration below gives\n"
        " * the symbol -- and calls through that. The two types matching is exactly\n"
        " * what wasm64's indirect-call check requires, and is why this unit exists.\n"
        " *\n"
        " * An i32 result is zero-extended into r3. The old cast did the same\n"
        " * natively: its return type was uint64_t, so only the low half of a\n"
        " * 32-bit result survived. A title reading CELL_OK, or a negative Cell\n"
        " * error code as (s32)r3, sees an identical value either way. */\n")
    for key in shapes:
        args, ret = key
        nargs = len(args)
        params = ["const void* fn"] + ["uint64_t a%d" % i for i in range(nargs)]
        params += ["uint64_t"] * (8 - nargs)
        cast_args = ", ".join("(%s)a%d" % (_CTYPE[a], i)
                              for i, a in enumerate(args))
        call = "((%s (*)(%s))fn)(%s)" % (
            _CTYPE[ret],
            ", ".join(_CTYPE[a] for a in args) if args else "void",
            cast_args)
        if ret == shape.CLASS_VOID:
            body = "    %s;\n    return 0;\n" % call
        else:
            body = "    return (uint64_t)(uint32_t)%s;\n" % call
        out.append("static uint64_t %s(%s)\n{\n%s}\n\n"
                   % (_thunk_name(key), ", ".join(params), body))


def _emit_variadic_thunks(out, variadic):
    """One body per variadic handler, calling it *by name*.

    A shared body is impossible here: the handler's wasm type is
    "(fixed..., ...)", and a cast to a fixed 8-argument type produces a
    different type that wasm-ld refuses to pair with the definition (measured:
    "warning: function signature mismatch"). Naming the handler directly keeps
    the ellipsis in the prototype the compiler sees, so the call carries the
    callee's own type.

    Only _sys_printf/_sys_sprintf/_sys_snprintf are in this set.
    """
    out.append(
        "/* ---- variadic trampolines ----\n"
        " *\n"
        " * A cast to a fixed-arity type is not the same wasm type as\n"
        " * \"(fixed..., ...)\", so these call the handler by name through a\n"
        " * prototype that keeps the ellipsis. The tail is forwarded as %s --\n"
        " * the width the PS3 ABI widens varargs into, and the one the reader\n"
        " * (yz_format in libs/system/sysPrxForUser.c) always fetches. */\n"
        % shape.VARARG_FORWARD_CTYPE)
    for sig in variadic:
        nfixed = len([p for p in sig.params if p.cls != shape.CLASS_VARARG])
        params = ", ".join(["const void* fn"] + ["uint64_t a%d" % i for i in range(8)])
        fixed = ", ".join("(%s)a%d" % (_CTYPE[p.cls], i)
                          for i, p in enumerate(sig.params)
                          if p.cls != shape.CLASS_VARARG)
        tail = ", ".join("(%s)a%d" % (shape.VARARG_FORWARD_CTYPE, i)
                         for i in range(nfixed, 8))
        call_args = ", ".join(x for x in (fixed, tail) if x)
        call = "%s(%s)" % (sig.name, call_args)
        if sig.ret_cls == shape.CLASS_VOID:
            body = "    %s;\n    return 0;\n" % call
        else:
            body = "    return (uint64_t)(uint32_t)%s;\n" % call
        out.append("static uint64_t hle_thunk_va_%s(%s)\n{\n    (void)fn;\n%s}\n\n"
                   % (sig.name, params, body))


def _emit_declarations(out, trampolined, legacy):
    """Declare every handler with its ABI-class prototype.

    Deliberately NOT the true prototype from the library header: this unit would
    then have to include libs headers, and several of them do not compile as C++
    (cellSync.h uses C11 _Atomic; sceNpBasic.h and sceNpCommerce2.h disagree with
    their siblings about the same struct and the same function). The ABI class is
    all a trampoline needs, and it is all that can be declared without pulling
    that in.

    Handlers that get no trampoline keep the old self-contained spelling, because
    they are still called through the cast in the dispatcher and that is all the
    old code ever relied on.
    """
    out.append("extern \"C\" {\n")
    for sig in trampolined:
        out.append("    %s %s(%s);\n"
                   % (_CTYPE[sig.ret_cls], sig.name, _class_params(sig.params)))
    for name in legacy:
        out.append("    void %s(void);\n" % name)
    out.append("}\n\n")


def _nid_of(regs, name):
    for nid, n in regs:
        if n == name:
            return nid
    raise KeyError(name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("libs", nargs="*",
                    help="Module (file) names to register; omit with --all.")
    ap.add_argument("--all", action="store_true",
                    help="Register EVERY PRX module compiled into the runtime "
                         "(the canonical invocation -- can't silently drop a "
                         "library the way a hand-typed list can).")
    ap.add_argument("--out", default="ppu_hle_nids.cpp")
    args = ap.parse_args()

    if args.all:
        args.libs = discover_all() + list(args.libs)
    if not args.libs:
        ap.error("no modules given; pass module names or --all")

    regs, seen, nlibs, missing = [], set(), 0, []
    for lib in args.libs:
        cs = glob.glob(os.path.join(ROOT, "libs", "**", lib + ".c"), recursive=True)
        if not cs:
            missing.append(lib); continue
        nlibs += 1
        for nid, name in scan(cs[0]):
            if nid in seen:
                continue
            seen.add(nid)
            regs.append((nid, name))

    # A name that resolves to no .c file used to print a warning and carry on.
    # That is the worst possible outcome: every NID in that module stays
    # unregistered, so the dispatcher's unresolved path answers CELL_OK with
    # untouched out-params and the title runs on fabricated success. It is also
    # silent -- the warning scrolls past in the build noise. The trap is that
    # these are PRX names, while the files are named after the LIBRARY:
    # sys_io is cellMouse.c + cellKb.c, sys_fs is cellFs.c, and so on. LBP
    # shipped six such names (sys_io, sys_fs, sys_net, sceNp2,
    # cellSysutilAvconfExt, cellDiscGame) and silently faked all of them --
    # cellMouseInit/cellMouseGetInfo/cellKbInit were fully implemented and never
    # registered. Fail loudly instead, and suggest the file that probably meant.
    if missing:
        have = sorted(os.path.splitext(os.path.basename(p))[0]
                      for p in glob.glob(os.path.join(ROOT, "libs", "**", "*.c"),
                                         recursive=True))
        print("error: these module names match no libs/**/<name>.c, so every NID "
              "they export would go unregistered and silently fake CELL_OK:",
              file=sys.stderr)
        for lib in missing:
            near = [h for h in have if lib.lower() in h.lower()
                    or h.lower() in lib.lower()]
            hint = f"  did you mean: {', '.join(near)}" if near else \
                   "  no similar file -- implement it or drop it from the list"
            print(f"  {lib}.c NOT FOUND\n{hint}", file=sys.stderr)
        sys.exit(1)

    # Resolve every handler's ABI shape from its prototype, then split the table
    # three ways: trampolined by shape, trampolined individually (variadic), and
    # left on the old path. The third bucket is reported, never silently dropped.
    sigs = shape.analyse(ROOT)
    by_shape, variadic, legacy, unshaped = {}, [], [], []
    for nid, name in regs:
        sig = sigs.get(name)
        if sig is None:
            unshaped.append((name, "no prototype found in libs/"))
            legacy.append(name)
            continue
        if not sig.expressible:
            unshaped.append((name, sig.why))
            legacy.append(name)
            continue
        nfixed = len([p for p in sig.params if p.cls != shape.CLASS_VARARG])
        if nfixed > shape.DISPATCH_ARG_SLOTS:
            unshaped.append((name, "%d parameters, dispatcher has %d GPR slots"
                             % (nfixed, shape.DISPATCH_ARG_SLOTS)))
            legacy.append(name)
            continue
        if nfixed != len(sig.params):
            variadic.append(sig)
        else:
            by_shape.setdefault(sig.shape(), []).append(sig)

    trampolined = [s for sh in by_shape.values() for s in sh] + variadic
    shapes = sorted(by_shape)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    out = []
    out.append("/* Auto-generated by gen_hle_nids.py -- do not edit.\n"
               " *\n"
               " * One trampoline per HLE dispatch ABI shape, and the NID table that\n"
               " * points at them. See docs/HLE_DISPATCH_ABI.md for why the dispatcher\n"
               " * cannot call a handler directly on wasm64. */\n")
    out.append("#include <stdint.h>\n\n")
    out.append('extern "C" void ps3_hle_register_trampolined(\n'
               "    unsigned int nid, const char* name, void* fn,\n"
               "    uint64_t (*trampoline)(const void*, uint64_t, uint64_t, uint64_t,\n"
               "                           uint64_t, uint64_t, uint64_t, uint64_t,\n"
               "                           uint64_t));\n")
    out.append('extern "C" void ps3_hle_register(unsigned int nid, const char* name,'
               ' void* handler);\n\n')
    _emit_declarations(out, trampolined, legacy)
    if any(name == 'cellSaveDataListAutoLoad' for _, name in regs):
        out.append('struct ppu_context;\n')
        out.append('extern "C" void ps3_savedata_list_auto_load(ppu_context*);\n')
        out.append('extern "C" void ps3_hle_register_ctx(unsigned, const char*,'
                   ' void (*)(ppu_context*));\n\n')
    _emit_shared_thunks(out, shapes)
    _emit_variadic_thunks(out, variadic)

    out.append('extern "C" void ppu_hle_register_all(void) {\n')
    for key in shapes:
        thunk = _thunk_name(key)
        for sig in by_shape[key]:
            out.append('    ps3_hle_register_trampolined(0x%08Xu, "%s", '
                       '(void*)%s, %s);\n'
                       % (_nid_of(regs, sig.name), sig.name, sig.name, thunk))
    for sig in variadic:
        out.append('    ps3_hle_register_trampolined(0x%08Xu, "%s", '
                   '(void*)%s, hle_thunk_va_%s);\n'
                   % (_nid_of(regs, sig.name), sig.name, sig.name, sig.name))
    for name in legacy:
        out.append('    ps3_hle_register(0x%08Xu, "%s", (void*)%s);  '
                   '/* no trampoline: see docs/HLE_DISPATCH_ABI.md */\n'
                   % (_nid_of(regs, name), name, name))
    if any(name == 'cellSaveDataListAutoLoad' for _, name in regs):
        out.append('    ps3_hle_register_ctx(0x21425307u, "cellSaveDataListAutoLoad",'
                   ' ps3_savedata_list_auto_load);\n')
    out.append("}\n")
    with open(args.out, "w") as f:
        f.writelines(out)

    print(f"wrote {args.out}: {len(regs)} NID handlers from {nlibs} module(s), "
          f"{len(shapes)} trampoline shapes, {len(variadic)} variadic, "
          f"{len(legacy)} on the legacy path")
    if unshaped:
        print("", file=sys.stderr)
        print("gen_hle_nids: %d handler(s) cannot be called through a trampoline "
              "and keep the pre-existing dispatch behaviour:" % len(unshaped),
              file=sys.stderr)
        for name, why in unshaped:
            print("    %-42s %s" % (name, why), file=sys.stderr)


if __name__ == "__main__":
    main()
