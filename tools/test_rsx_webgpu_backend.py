#!/usr/bin/env python3
"""C1 acceptance test: the RSX clear colour reaches a canvas through WebGPU.

Two halves, and the split is deliberate.

The native half always runs and needs no browser, no GPU and no wasm toolchain. It
links the real WebGPU backend translation unit for the HOST (where every symbol is
inert), asserts the backend refuses to come up, and asserts the header's contract:
init returns -1, is_ready() is 0, pump() is -1. That is what makes "new files are
inert off Emscripten" a checked claim rather than a comment.

The browser half runs when emcc and the bundled Chromium are both present. It builds
the SAME C for wasm64 through the emdawnwebgpu port, drives the real RSX method
dispatch into the real backend, and reads the canvas backing store back with
toDataURL. The assertions are the acceptance criteria of the task:

  * the clear colour the guest asked for (0x20406080) is the colour on the canvas,
    in R,G,B order -- so a channel swap fails;
  * four quadrants and the centre triangle are the colours the guest drew, so a
    present that showed only the clear (or only the last draw) fails;
  * the frame count is non-zero, so the canvas got the pixels from a submit and not
    from an unconfigured default.

Run: python3 tools/test_rsx_webgpu_backend.py [--screenshot DIR]
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
    ROOT / "libs/video/rsx_webgpu_backend.c",
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

# The flags that make WebGPU work in headless Chromium on a machine with no GPU.
# --use-angle=swiftshader alone is NOT enough: without --use-vulkan=swiftshader the
# adapter request returns null and the page fails with "no adapter". That was
# measured on this machine, not guessed.
CHROME_WEBGPU_FLAGS = [
    "--enable-unsafe-webgpu",
    "--use-angle=swiftshader",
    "--enable-features=Vulkan",
    "--use-vulkan=swiftshader",
]

# What the probe asks the guest to clear to, and what the canvas must show.
CLEAR_ARGB = 0x20406080
CLEAR_RGB = (0x40, 0x60, 0x80)


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd))
    return subprocess.run([str(c) for c in cmd], check=True, **kw)


def native_half(tmp):
    """The part CI can always run: the file is inert off Emscripten."""
    exe = tmp / "test_rsx_webgpu_inert"
    harness = tmp / "test_rsx_webgpu_inert.c"
    # The stubs rsx_commands.c and rsx_vertex_fetch.c expect from the runtime. Same
    # set probe_webgpu_main.c provides, and the same reason: this links the RSX
    # command processor and the backend, not the whole emulator.
    harness.write_text(r"""
#include "rsx_webgpu_backend.h"
#include <stdio.h>
#include <stdint.h>

uint8_t* vm_base = 0;
uint32_t ppu_hle_inject_base = 0;
uint32_t ppu_vm_size = 0;
void ps3_ms(const char* key) { (void)key; }
void ps3_msf(const char* fmt, ...) { (void)fmt; }
void ps3_ms_kv(const char* key, long long v) { (void)key; (void)v; }
void ps3_ms_dump(void) {}
long long ps3_ms_now(void) { return 0; }
void vm_write32(uint64_t a, uint32_t v) { (void)a; (void)v; }
uint32_t vm_read32(uint64_t a) { (void)a; return 0; }
uint32_t gcm_io_offset_to_ea(uint32_t o) { return o; }
uint32_t cellGcmResolveLocated(int location, uint32_t offset) { (void)location; return offset; }
uint32_t cellGcmResolveOffset(uint32_t offset) { return offset; }

int main(void)
{
    int fails = 0;
    /* Every entry point must be inert on a native build. This is the promise the
     * deliverable makes to the native build: a new libs/video/*.c costs it nothing. */
    if (rsx_webgpu_backend_init(1280, 720, "#screen") != -1) {
        printf("[FAIL] init returned non -1 on native\n"); fails++;
    }
    rsx_webgpu_backend_shutdown();     /* must not crash */
    if (rsx_webgpu_backend_pump() != -1) {
        printf("[FAIL] pump returned non -1 on native\n"); fails++;
    }
    if (rsx_webgpu_backend_is_ready() != 0) {
        printf("[FAIL] is_ready non-zero on native\n"); fails++;
    }
    if (rsx_webgpu_backend_frame_count() != 0 ||
        rsx_webgpu_backend_surface_format() != 0 ||
        rsx_webgpu_backend_guest_draws() != 0 ||
        rsx_webgpu_backend_debug_color() != 0 ||
        rsx_webgpu_backend_frame_ms() != 0.0) {
        printf("[FAIL] an accessor returned non-zero on native\n"); fails++;
    }
    if (!fails) printf("[PASS] WebGPU backend is inert on a native build\n");
    return fails ? 1 : 0;
}
""")
    run([CC, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter",
         "-Wno-sign-compare", "-Wno-missing-field-initializers",
         *INCLUDES,
         harness,
         *BACKEND_SRCS,
         "-o", exe])
    out = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
    sys.stdout.write(out.stdout)
    if out.returncode != 0:
        sys.stderr.write(out.stderr)
        raise SystemExit("native inert harness failed (exit %d)" % out.returncode)


def emcc():
    """emcc from PATH or from a local emsdk, or None."""
    for cand in (os.environ.get("EMCC"), shutil.which("emcc")):
        if cand and Path(cand).exists():
            return str(cand)
    return None


def wasm_half(tmp, shot_dir):
    compiler = emcc()
    if not compiler:
        return "skipped: no emcc (source the emsdk env first)"
    if not Path(CHROME).exists():
        return "skipped: no Chromium at %s" % CHROME

    web = tmp / "web"
    web.mkdir()
    exported = ",".join("_" + s for s in [
        "main", "rsx_probe_init", "rsx_probe_step",
        "rsx_webgpu_backend_pump", "rsx_webgpu_backend_is_ready",
        "rsx_webgpu_backend_frame_count", "rsx_webgpu_backend_debug_color",
        "rsx_webgpu_backend_surface_format", "rsx_webgpu_backend_frame_ms",
        "rsx_webgpu_backend_guest_draws",
    ])
    # -m64 and -pthread to match the real wasm64 toolchain (the guest VM needs a
    # 4 GB-aligned window, so the port builds wasm64). -sDEFAULT_TO_CXX because
    # emdawnwebgpu requires a C++ link: "emdawnwebgpu requires C++. Link using em++
    # (or emcc -sDEFAULT_TO_CXX), instead of emcc."
    run([compiler, "-O2", "-m64", "-pthread", "-sDEFAULT_TO_CXX", "-w",
         "--use-port=emdawnwebgpu", *INCLUDES,
         "-sEXPORTED_FUNCTIONS=" + exported,
         "-sEXPORTED_RUNTIME_METHODS=ccall",
         "-sALLOW_MEMORY_GROWTH=1", "-sINITIAL_MEMORY=64MB",
         "-sMODULARIZE=0",
         HOST_SRC / "probe_webgpu_main.c",
         *BACKEND_SRCS,
         "-o", web / "probe_webgpu.js"], timeout=900)
    shutil.copy(HOST_SRC / "probe_webgpu.html", web / "probe.html")

    shot = (shot_dir or tmp) / "webgpu-probe.png"
    shot.parent.mkdir(parents=True, exist_ok=True)
    out = subprocess.run(
        ["node", str(ROOT / "tools/run_canvas_blit_probe.js"),
         str(web), str(shot), CHROME] + CHROME_WEBGPU_FLAGS,
        capture_output=True, text=True, timeout=600)
    sys.stdout.write(out.stdout)
    if out.returncode != 0:
        sys.stderr.write(out.stderr)
        raise SystemExit("wasm WebGPU probe failed")

    native = shot.with_name(shot.stem + "-native.png")
    assert native.exists(), "no native-resolution screenshot at %s" % native

    w, h, px = read_png(native)
    assert (w, h) == (1280, 720), "screenshot is %dx%d, expected 1280x720" % (w, h)

    # The clear: the colour the guest asked for, at a pixel no geometry covers.
    got = px(5, 5)
    assert got[:3] == CLEAR_RGB, (
        "clear corner is %s, expected %s (RSX ARGB 0x%08X)"
        % (got[:3], CLEAR_RGB, CLEAR_ARGB))

    # The draws: four quadrants plus the centre triangle. If the arena fix were
    # missing, every one of these would read as the final draw's colour.
    probes = {
        "top-left red":       ((300, 150), (255, 0, 0)),
        "top-right green":    ((980, 150), (0, 255, 0)),
        "bottom-left blue":   ((300, 570), (0, 0, 255)),
        "bottom-right white": ((980, 570), (255, 255, 255)),
        "centre magenta":     ((640, 350), (255, 0, 255)),
    }
    for name, ((x, y), want) in probes.items():
        got = px(x, y)
        assert got[:3] == want, "%s at (%d,%d) is %s, expected %s" % (
            name, x, y, got[:3], want)

    print("[PASS] canvas is %dx%d; clear 0x%08X -> RGB%s and all 5 drawn regions correct"
          % (w, h, CLEAR_ARGB, CLEAR_RGB))
    print("[PASS] screenshots: %s / %s" % (shot, native))
    return "ran"


def read_png(path):
    """Minimal PNG reader: 8-bit RGB/RGBA, all five filter types."""
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

    with tempfile.TemporaryDirectory(prefix="rsx-webgpu-") as tmp:
        tmp = Path(tmp)
        native_half(tmp)
        print("[PASS] native: WebGPU backend is inert, header contract holds")
        status = wasm_half(tmp, shot_dir)
        if status.startswith("skipped"):
            print("[SKIP] browser WebGPU probe: " + status.split(": ", 1)[1])
    print("PASS")


if __name__ == "__main__":
    main()
