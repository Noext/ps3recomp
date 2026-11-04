#!/usr/bin/env python3
"""Validate the fragment decompiler's WGSL with naga.

The decompiler emits WGSL for the WebGPU backend (rsx_fp_decompiler.c, the
WGSL dialect), and there is no WGSL parser in this tree. naga is the reference
implementation -- it is the front end wgpu and therefore every WebGPU browser
implementation is built on -- so "naga accepts the module" is the strongest
statement about validity available without a GPU.

The test binary does the decompiling; this script runs it with
TEST_FP_WGSL_DUMP=<dir>, then feeds every module it wrote to naga. A module
naga rejects is a bug in the emitter, not in this script, and the diagnostic
naga prints names the line.

Usage:
    tools/validate_fp_wgsl.py --test build/test_fp_wgsl --naga /usr/bin/naga

Exit code 0 means every module validated. The script is also happy to be run
by hand against an already-built test binary.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--test", required=True, help="the test_fp_wgsl binary")
    ap.add_argument("--naga", default="naga", help="the naga CLI")
    ap.add_argument("--keep", metavar="DIR",
                    help="write the dumped shaders here instead of a temp dir")
    args = ap.parse_args()

    if not (os.path.isfile(args.naga) and os.access(args.naga, os.X_OK)):
        found = shutil.which(args.naga)
        if not found:
            print("naga not found: %s" % args.naga, file=sys.stderr)
            return 2
        args.naga = found

    outdir = args.keep
    if outdir:
        os.makedirs(outdir, exist_ok=True)
    else:
        outdir = tempfile.mkdtemp(prefix="fp_wgsl_")

    env = dict(os.environ)
    env["TEST_FP_WGSL_DUMP"] = outdir
    run = subprocess.run([args.test], env=env)
    if run.returncode != 0:
        print("the test's own checks failed; not validating", file=sys.stderr)
        return run.returncode

    modules = sorted(f for f in os.listdir(outdir) if f.endswith(".wgsl"))
    if not modules:
        print("the test wrote no WGSL modules", file=sys.stderr)
        return 1

    failed = 0
    for name in modules:
        path = os.path.join(outdir, name)
        naga = subprocess.run([args.naga, path], capture_output=True, text=True)
        if naga.returncode != 0:
            failed += 1
            print("[FAIL] naga rejected %s" % name)
            print((naga.stderr or naga.stdout).rstrip())
    print("naga: %d module(s) validated, %d rejected" % (len(modules) - failed, failed))
    if not args.keep:
        shutil.rmtree(outdir, ignore_errors=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
