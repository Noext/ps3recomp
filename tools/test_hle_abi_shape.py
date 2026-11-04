#!/usr/bin/env python3
"""The generator's ABI-shape analysis over the real libs/ tree.

pins three properties the HLE trampoline scheme depends on, against the
actual library sources rather than a fixture:

  1. every handler gen_hle_nids.py registers has a prototype it can analyse --
     a handler with no prototype would silently fall back to the legacy cast,
  2. the shape analysis agrees with hle_abi_audit.py's independent prototype
     parser on arity, so a parser regression shows up as a count mismatch
     rather than as a silently mis-called handler,
  3. the set of handlers no trampoline can express is exactly the accounted-for
     one (float arguments/returns, >8 parameters), so a new one appearing is a
     test failure and not a quiet return to the wasm64 trap.

Stdlib-only, py -3.
"""
import glob
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import gen_hle_nids as G          # noqa: E402
import hle_abi_audit as A         # noqa: E402
import hle_abi_shape as H         # noqa: E402


def registered():
    """name -> nid for every handler the generator's --all table registers."""
    regs = {}
    for lib in G.discover_all():
        for nid, name in G.scan(glob.glob(os.path.join(
                ROOT, "libs", "**", lib + ".c"), recursive=True)[0]):
            regs.setdefault(name, nid)
    return regs


def main():
    regs = registered()
    assert regs, "gen_hle_nids.py discovered no handlers at all"
    sigs = H.analyse(ROOT)

    # 1. Every registered handler is analysable.
    no_proto = sorted(n for n in regs if n not in sigs)
    assert not no_proto, (
        "these registered handlers have no prototype in libs/, so they would "
        "silently keep the legacy single-cast dispatch:\n  "
        + "\n  ".join(no_proto))

    # 2. Arity agrees with the audit tool's independent parser.
    audit_sigs = A.extract_ours(os.path.join(ROOT, "libs"))
    common = [n for n in regs if n in audit_sigs]
    assert len(common) > 1000, (
        "the audit parser only recognised %d of %d handlers; one of the two "
        "parsers has regressed" % (len(common), len(regs)))
    bad = [(n, len(audit_sigs[n].params), len(sigs[n].params))
           for n in common
           if len(audit_sigs[n].params) != len(sigs[n].params)]
    assert not bad, "arity disagreement between the two parsers:\n  " + \
        "\n  ".join("%s: audit %d vs shape %d" % (n, a, s) for n, a, s in bad)

    # 3. The unexpressible set is exactly the accounted-for one.
    unexpressible = {n: sigs[n].why for n in regs
                     if n in sigs and not sigs[n].expressible}
    too_many = {n: len(sigs[n].params) for n in regs
                if n in sigs and sigs[n].expressible
                and len(sigs[n].params) > H.DISPATCH_ARG_SLOTS}

    unexpected = sorted(n for n in unexpressible
                        if "floating-point" not in unexpressible[n])
    assert not unexpected, (
        "handlers became unexpressible for a NEW reason -- trampolines cannot "
        "call these, and they would keep the wasm64 trap:\n  " + "\n  ".join(
            "%s: %s" % (n, unexpressible[n]) for n in unexpected))

    # The counts are documentation (docs/HLE_DISPATCH_ABI.md). Pin them so the
    # doc cannot drift silently.
    assert len(unexpressible) == 13, (
        "expected 13 float-argument/return handlers, found %d: %s"
        % (len(unexpressible), sorted(unexpressible)))
    assert len(too_many) == 3, (
        "expected 3 handlers over the 8 GPR slots, found %d: %s"
        % (len(too_many), sorted(too_many)))

    # Every expressible handler yields a shape the generator can name.
    shapes = {sigs[n].shape() for n in regs if n in sigs and sigs[n].expressible}
    for args, ret in shapes:
        assert all(a in ("i32", "i64", "...") for a in args), args
        assert ret in ("i32", "i64", "void"), ret

    legacy = len(unexpressible) + len(too_many)
    variadic = sum(1 for n in regs if n in sigs and sigs[n].expressible
                   and any(p.cls == H.CLASS_VARARG for p in sigs[n].params))
    # Shapes the generator actually emits: fixed-arity handlers that fit the
    # dispatcher's 8 GPR slots. The over-8 handlers are expressible in principle
    # but go down the legacy path, so they contribute no body.
    fixed_shapes = {sigs[n].shape() for n in regs
                    if n in sigs and sigs[n].expressible
                    and n not in too_many
                    and not any(p.cls == H.CLASS_VARARG for p in sigs[n].params)}
    print("PASS: %d handlers, %d shared trampoline bodies, %d variadic "
          "trampolines, %d legacy" % (len(regs), len(fixed_shapes), variadic, legacy))
    return 0


if __name__ == "__main__":
    sys.exit(main())
