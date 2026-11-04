/*
 * ps3recomp - WebGPU backend probe (Emscripten, wasm64)
 *
 * Renders through the REAL WebGPU backend and the REAL RSX method dispatch, then
 * lets the page screenshot the canvas. Not a hand-written WebGPU program: every
 * pixel this produces travelled guest method -> rsx_commands.c -> the
 * rsx_backend vtable -> WebGPU, which is the whole claim being tested. A
 * hand-rolled clear would prove only that the browser has a GPU.
 *
 * The scene is the same four-quadrant pattern the canvas blit probe uses, in the
 * same order around a distinctive clear colour. That is deliberate: the two
 * probes then answer the same question -- does the guest's command stream reach
 * the screen with the colours it asked for -- through two completely different
 * presentation paths (CPU putImageData vs. a WebGPU render pass), so a
 * disagreement between the screenshots localises the bug to one of them.
 *
 * The clear colour is 0x20406080, chosen because no quadrant, gradient step or
 * triangle in the pattern uses it and because a channel swap turns it into a
 * different, also-unused colour. A probe whose clear colour happens to match its
 * geometry can pass while presenting nothing.
 */
#include "rsx_commands.h"
#include "rsx_webgpu_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten.h>

/* The runtime pieces this probe stubs out. Same set, and the same reasoning, as
 * probe_main.c for the canvas blit probe: this links the RSX command processor
 * and the backend, not the whole emulator. */
uint8_t* vm_base = NULL;
uint32_t ppu_hle_inject_base = 0;
uint32_t ppu_vm_size = 0;
void ps3_ms(const char* key) { (void)key; }
void ps3_msf(const char* fmt, ...) { (void)fmt; }
void ps3_ms_kv(const char* key, long long v) { (void)key; (void)v; }
void ps3_ms_dump(void) {}
long long ps3_ms_now(void) { return 0; }
/* Signature must match runtime/ppu/ppu_loader.cpp: vm_write32(uint64_t, uint32_t).
 * Declaring it uint32_t is invisible on x86-64 SysV and a link warning plus a
 * runtime trap under wasm64 -- see probe_main.c's note, and the commit that
 * fixed the same mistake there. */
void vm_write32(uint64_t a, uint32_t v) { (void)a; (void)v; }
uint32_t vm_read32(uint64_t a) { (void)a; return 0; }
uint32_t gcm_io_offset_to_ea(uint32_t o) { return o; }
uint32_t cellGcmResolveLocated(int location, uint32_t offset) { (void)location; return offset; }
uint32_t cellGcmResolveOffset(uint32_t offset) { return offset; }

#define W 1280u
#define H 720u

#define POS_ADDR  0x00010000u   /* float4 per vertex, stride 16 */
#define COL_ADDR  0x00040000u   /* ubyte4 per vertex, stride 4 */
#define VM_SIZE   0x00100000u   /* 1 MB: this probe has no guest image */

static rsx_state g_state;
static unsigned  g_vert;

static void M(uint32_t method, uint32_t data) { rsx_process_method(&g_state, method, data); }

/* Big-endian float store: the PS3 is a big-endian machine and every guest value
 * this backend reads is byteswapped by rsx_fetch_attrib. Writing the probe's
 * vertices little-endian would make the fetch read garbage, so the probe has to
 * store them the way a guest would. */
static void put_bef(uint32_t off, float f)
{
    uint32_t v; memcpy(&v, &f, 4);
    vm_base[off+0] = (uint8_t)(v >> 24); vm_base[off+1] = (uint8_t)(v >> 16);
    vm_base[off+2] = (uint8_t)(v >> 8);  vm_base[off+3] = (uint8_t)(v);
}

/* Append one vertex: clip-space x,y in [-1,1] and an 0xRRGGBB diffuse colour. */
static void vertex(float x, float y, uint32_t rgb)
{
    put_bef(POS_ADDR + g_vert * 16u + 0u, x);
    put_bef(POS_ADDR + g_vert * 16u + 4u, y);
    put_bef(POS_ADDR + g_vert * 16u + 8u, 0.0f);
    put_bef(POS_ADDR + g_vert * 16u + 12u, 1.0f);

    uint8_t* c = vm_base + COL_ADDR + g_vert * 4u;
    /* UNORM8 attribute arrays are read as four bytes per texel in R,G,B,A order.
     * The colour word is 0xRRGGBB, so the byte order here is R then G then B. */
    c[0] = (uint8_t)(rgb >> 16); c[1] = (uint8_t)(rgb >> 8); c[2] = (uint8_t)rgb; c[3] = 0xFF;
    g_vert++;
}

static void setup_attribs(void)
{
    /* attrib 0 = position: float32, size 4, stride 16 */
    M(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 0 * 4, 0x2u | (4u << 4) | (16u << 8));
    M(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 0 * 4, POS_ADDR);
    /* attrib 3 = diffuse colour: unorm8, size 4, stride 4 */
    M(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 3 * 4, 0x4u | (4u << 4) | (4u << 8));
    M(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 3 * 4, COL_ADDR);
    /* No depth test: the pattern is drawn back to front and this backend has no
     * depth attachment, so leaving it enabled would be claiming a test it cannot
     * perform. */
    M(NV4097_SET_DEPTH_TEST_ENABLE, 0);
}

/* A solid axis-aligned quad as two triangles. DRAW_ARRAYS' encoding is count-1 in
 * the high byte and first in the low 24 bits, so six vertices from first is a
 * count byte of 5. Written out rather than hidden behind a helper because getting
 * it wrong draws nothing at all, silently. */
static void quad(float x0, float y0, float x1, float y1, uint32_t rgb)
{
    const unsigned first = g_vert;
    vertex(x0, y1, rgb); vertex(x1, y1, rgb); vertex(x1, y0, rgb);
    vertex(x0, y1, rgb); vertex(x1, y0, rgb); vertex(x0, y0, rgb);

    M(NV4097_SET_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    M(NV4097_DRAW_ARRAYS, (5u << 24) | (first & 0xFFFFFFu));
    M(NV4097_SET_BEGIN_END, 0);
}

/* One frame of the pattern. The geometry is in clip space and the vertex
 * constants are left at zero, so the backend's identity fallback applies and the
 * quadrants land where the coordinates say. */
static void build_pattern(void)
{
    /* Restart the vertex stream each frame: the buffers are rebuilt wholesale, and
     * leaving g_vert climbing would redraw every earlier frame's geometry. */
    g_vert = 0;

    /* The clear. This is the slot the acceptance test turns on: a distinctive
     * colour that no geometry uses, so a canvas showing it proves the render pass
     * ran and presented. */
    M(NV4097_SET_COLOR_CLEAR_VALUE, 0x20406080u);
    M(NV4097_CLEAR_SURFACE, 0xF0u);

    /* Four quadrants: red top-left, green top-right, blue bottom-left, white
     * bottom-right. A red/blue swap turns the top-left blue, which is the single
     * most common channel-order bug. The quadrants are inset from the frame edge
     * so the clear colour stays readable at the corners. */
    quad(-0.95f,  0.05f, -0.05f,  0.95f, 0x00FF0000u);   /* top-left     red   */
    quad( 0.05f,  0.05f,  0.95f,  0.95f, 0x0000FF00u);   /* top-right    green */
    quad(-0.95f, -0.95f, -0.05f, -0.05f, 0x000000FFu);   /* bottom-left  blue  */
    quad( 0.05f, -0.95f,  0.95f, -0.05f, 0x00FFFFFFu);   /* bottom-right white */

    /* A magenta triangle dead centre: proves the vertex fetch path ran, since
     * nothing else in the pattern uses three-vertex geometry. */
    {
        const unsigned first = g_vert;
        vertex(-0.15f, -0.06f, 0x00FF00FFu);
        vertex( 0.15f, -0.06f, 0x00FF00FFu);
        vertex( 0.00f,  0.12f, 0x00FF00FFu);
        M(NV4097_SET_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
        M(NV4097_DRAW_ARRAYS, (2u << 24) | (first & 0xFFFFFFu));
        M(NV4097_SET_BEGIN_END, 0);
    }

    /* And a present, which is what submits the encoder. */
    if (rsx_get_backend() && rsx_get_backend()->present)
        rsx_get_backend()->present(rsx_get_backend()->userdata, 0);
}

static int g_inited;

/* Explicit init, called from the page after it has created the device (see
 * probe_webgpu.html: the page's contract is three lines of script). Emscripten
 * runs Module.onRuntimeInitialized before callMain, so a page that waits for that
 * hook to call into the module would find the backend uninitialised -- the same
 * trap probe_main.c documents. An explicit init trades that implicit ordering for
 * a function whose name says when to call it. */
EMSCRIPTEN_KEEPALIVE int rsx_probe_init(void)
{
    if (g_inited) return 0;
    setvbuf(stdout, NULL, _IONBF, 0);

    vm_base = (uint8_t*)calloc(1, VM_SIZE);
    if (!vm_base) { printf("[probe] VM alloc failed\n"); return -1; }

    rsx_state_init(&g_state);
    setup_attribs();

    /* Init the backend. Under the preinitialized-device path this brings the
     * surface and pipelines up synchronously and is ready on return; the page
     * still polls is_ready()/pump() because the fallback path would not be. */
    if (rsx_webgpu_backend_init(W, H, "#screen") != 0) {
        printf("[probe] backend init failed\n");
        return -1;
    }

    g_inited = 1;
    printf("[probe] init: ready=%d\n", rsx_webgpu_backend_is_ready());
    return 0;
}

/* One frame plus its present, driven from the page. Returns 0 on success, -2 if
 * init never ran, -3 if the backend is not ready yet (the page retries). */
EMSCRIPTEN_KEEPALIVE int rsx_probe_step(void)
{
    if (!g_inited) return -2;
    if (!rsx_webgpu_backend_is_ready()) return -3;

    build_pattern();

    printf("[probe] frame %u: clear=0x%08X fmt=%u ms=%.3f\n",
           rsx_webgpu_backend_frame_count(),
           rsx_webgpu_backend_debug_color(),
           rsx_webgpu_backend_surface_format(),
           rsx_webgpu_backend_frame_ms());
    return 0;
}

int main(void)
{
    /* Nothing: the page owns the order (see rsx_probe_init). */
    return 0;
}
