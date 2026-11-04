#!/usr/bin/env python3
"""Phase B: the RSX framebuffer reaches a canvas.

Two halves, and the split is deliberate.

The native half always runs: it drives the real RSX method dispatch into the
null backend's software path, then checks rsx_null_backend_framebuffer() returns
what was drawn and that the 0x00RRGGBB -> RGBA conversion puts R,G,B,A in the
bytes a canvas would read. That is the whole correctness question and it needs
no browser, no wasm toolchain and no GPU -- so it is the part CI can rely on.

The browser half runs when emcc and the bundled Chromium are both present: it
builds the same C for wasm32, renders a test pattern through the real backend,
blits it with putImageData, reads the canvas backing store back with
toDataURL, and asserts the corner pixels. It is skipped (loudly) otherwise,
because a runner without a browser should not claim the blit works.

Run: python3 tools/test_rsx_canvas_blit.py [--screenshot DIR]
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CC = os.environ.get("CC", "cc")

HOST_SRC = ROOT / "runtime/host/wasm"
BACKEND_SRCS = [
    ROOT / "libs/video/rsx_null_backend.c",
    ROOT / "libs/video/rsx_commands.c",
    ROOT / "libs/video/rsx_vertex_fetch.c",
    ROOT / "libs/video/rsx_texture_layout.c",
]
INCLUDES = [
    "-I" + str(ROOT / "include"),
    "-I" + str(ROOT / "libs/video"),
    "-I" + str(ROOT / "runtime"),
    "-I" + str(ROOT / "runtime/memory"),
    "-I" + str(ROOT / "runtime/platform"),
    "-I" + str(HOST_SRC),
]

CHROME = os.environ.get("CHROME",
    "/root/.cache/ms-playwright/chromium-1248/chrome-linux64/chrome")


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd))
    return subprocess.run([str(c) for c in cmd], check=True, **kw)


def native_half(tmp):
    """The part CI can always run: accessor + byte order, no browser."""
    exe = tmp / "test_rsx_canvas_blit"
    run([CC, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter",
         "-Wno-sign-compare", "-Wno-missing-field-initializers",
         *INCLUDES,
         HOST_SRC / "test_rsx_canvas_blit.c",
         HOST_SRC / "rsx_canvas_blit.c",
         *BACKEND_SRCS,
         "-o", exe])
    out = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
    sys.stdout.write(out.stdout)
    if out.returncode != 0:
        sys.stderr.write(out.stderr)
        raise SystemExit("native blit harness failed (exit %d)" % out.returncode)
    assert "[FAIL]" not in out.stdout, "native blit harness reported a failure"


def emcc():
    """emcc from PATH or from a local emsdk, or None."""
    for cand in (os.environ.get("EMCC"), shutil.which("emcc"),
                 "/root/emsdk/upstream/emscripten/emcc"):
        if cand and Path(cand).exists():
            return str(cand)
    return None


def wasm_half(tmp, shot_dir):
    """Build the wasm32 probe, blit it, and read the pixels back out."""
    compiler = emcc()
    if not compiler:
        return "skipped: no emcc (install emsdk or put emcc on PATH)"
    if not Path(CHROME).exists():
        return "skipped: no Chromium at %s" % CHROME

    web = tmp / "web"
    web.mkdir()
    exported = ",".join("_" + s for s in [
        "main", "rsx_probe_init", "rsx_probe_step", "rsx_canvas_blit_present",
        "rsx_null_backend_frame_count", "rsx_canvas_blit_frames",
        "rsx_canvas_blit_convert_ms", "rsx_null_backend_framebuffer",
        "rsx_canvas_blit_rgba_ptr", "malloc", "free",
    ])
    run([compiler, "-O2", "-w", *INCLUDES,
         "-sEXPORTED_FUNCTIONS=" + exported,
         "-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,HEAPU8",
         "-sALLOW_MEMORY_GROWTH=1", "-sINITIAL_MEMORY=64MB",
         "-sMODULARIZE=0",
         HOST_SRC / "probe_main.c",
         HOST_SRC / "rsx_canvas_blit.c",
         *BACKEND_SRCS,
         "-o", web / "probe.js"], timeout=600)
    for f in ("probe.html", "rsx_canvas_blit.js"):
        shutil.copy(HOST_SRC / f, web / f)

    shot = (shot_dir or tmp) / "blit-probe.png"
    shot.parent.mkdir(parents=True, exist_ok=True)
    out = subprocess.run(
        ["node", str(ROOT / "tools/run_canvas_blit_probe.js"),
         str(web), str(shot), CHROME],
        capture_output=True, text=True, timeout=300)
    sys.stdout.write(out.stdout)
    if out.returncode != 0:
        sys.stderr.write(out.stderr)
        raise SystemExit("wasm blit probe failed")

    native = shot.with_name(shot.stem + "-native.png")
    assert native.exists(), "no native-resolution screenshot at %s" % native

    # Read the canvas backing store back and check the pattern. This is the
    # assertion that matters: a channel swap, a row flip or an all-black blit
    # all produce a plausible-looking file and only the pixels catch them.
    w, h, px = read_png(native)
    assert (w, h) == (1280, 720), "screenshot is %dx%d, expected 1280x720" % (w, h)
    probes = {
        "top-left  red":    ((50, 50),   (255, 0, 0, 255)),
        "top-right green":  ((1200, 50), (0, 255, 0, 255)),
        "bottom-left blue": ((50, 650),  (0, 0, 255, 255)),
        "bottom-right white": ((1200, 650), (255, 255, 255, 255)),
        "gradient band left end":  ((10, 360),   (0, 0, 0, 255)),
        "gradient band right end": ((1270, 360), (255, 255, 255, 255)),
        "centre triangle magenta": ((640, 345),  (255, 0, 255, 255)),
    }
    for name, ((x, y), want) in probes.items():
        got = px(x, y)
        assert got == want, "%s at (%d,%d) is %s, expected %s" % (name, x, y, got, want)

    print("[PASS] canvas backing store is %dx%d and the pattern is pixel-correct" % (w, h))
    print("[PASS] screenshots: %s / %s" % (shot, native))
    return "ran"


def read_png(path):
    """Minimal PNG reader: 8-bit RGB/RGBA, all five filter types. Enough for
    what Chrome's toDataURL emits, and no Pillow on a CI runner."""
    data = Path(path).read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG: %s" % path
    pos, idat, w, h, ctype = 8, b"", None, None, None
    while pos < len(data):
        (ln,) = struct.unpack(">I", data[pos:pos + 4])
        typ = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h, _bd, ctype = struct.unpack(">IIBB", chunk[:10])
        elif typ == b"IDAT":
            idat += chunk
        pos += 12 + ln
    assert ctype in (2, 6), "unsupported PNG colour type %r" % ctype
    bpp = 4 if ctype == 6 else 3
    stride = w * bpp
    raw = zlib.decompress(idat)

    out = bytearray()
    prev = bytearray(stride)
    i = 0
    for _y in range(h):
        f = raw[i]; i += 1
        line = bytearray(raw[i:i + stride]); i += stride
        if f == 1:
            for x in range(bpp, stride):
                line[x] = (line[x] + line[x - bpp]) & 255
        elif f == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 255
        elif f == 3:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 255
        elif f == 4:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                b = prev[x]
                c = prev[x - bpp] if x >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        out += line
        prev = line

    def px(x, y):
        o = y * stride + x * bpp
        v = tuple(out[o:o + bpp])
        return v if bpp == 4 else v + (255,)
    return w, h, px


def main():
    shot_dir = None
    if "--screenshot" in sys.argv:
        shot_dir = Path(sys.argv[sys.argv.index("--screenshot") + 1])

    with tempfile.TemporaryDirectory(prefix="rsx-canvas-blit-") as tmp:
        tmp = Path(tmp)
        native_half(tmp)
        print("[PASS] native: accessor + 0x00RRGGBB -> RGBA conversion")
        status = wasm_half(tmp, shot_dir)
        if status.startswith("skipped"):
            print("[SKIP] browser blit probe: " + status.split(": ", 1)[1])
            if os.environ.get("CI"):
                print("[SKIP] (CI: the native half still covers the conversion "
                      "and the accessor; the browser half needs emcc + Chromium)")
    print("PASS")


if __name__ == "__main__":
    main()
