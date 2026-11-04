/*
 * ps3recomp - wasm32 blit probe (Phase B, B2)
 *
 * Renders a test pattern through the REAL null backend and the REAL RSX method
 * dispatch -- not a hand-filled buffer -- and blits it to a canvas with the
 * same rsx_canvas_blit_present() the port will call. If the pattern is right in
 * the screenshot, then the accessor, the byte-order conversion and the
 * putImageData path are all right together; a hand-filled buffer would only
 * prove the last two.
 *
 * wasm32 is enough for this (and the point): the framebuffer, the depth buffer
 * and the RGBA staging buffer are ~11 MB, nowhere near the 4 GB the guest's
 * flat VM needs, so the blit path can be validated today without waiting for
 * the wasm64 placement work.
 *
 * The pattern is chosen so a channel swap or a row flip cannot pass:
 *   - four quadrants in fixed order red / green / blue / white around the
 *     centre, so R and B swapped is instantly visible;
 *   - a horizontal gradient band whose left end is dark and right end bright,
 *     so a flipped or transposed row is visible;
 *   - a magenta triangle in the middle, which only appears if the guest vertex
 *     fetch path ran.
 */
#include "rsx_canvas_blit.h"
#include "rsx_commands.h"
#include "rsx_null_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

/* The runtime pieces this probe stubs out. Same set the native harness uses;
 * see test_rsx_canvas_blit.c for why each is identity here. */
uint8_t* vm_base = NULL;
uint32_t ppu_hle_inject_base = 0;
uint32_t ppu_vm_size = 0;
void ps3_ms(const char* key) { (void)key; }
void ps3_msf(const char* fmt, ...) { (void)fmt; }
void ps3_ms_kv(const char* key, long long v) { (void)key; (void)v; }
void ps3_ms_dump(void) {}
long long ps3_ms_now(void) { return 0; }
void vm_write32(uint32_t a, uint32_t v) { (void)a; (void)v; }
uint32_t vm_read32(uint32_t a) { (void)a; return 0; }
uint32_t gcm_io_offset_to_ea(uint32_t o) { return o; }
uint32_t cellGcmResolveLocated(int location, uint32_t offset) { (void)location; return offset; }
uint32_t cellGcmResolveOffset(uint32_t offset) { return offset; }

#define W 1280u
#define H 720u

#define POS_ADDR  0x00010000u   /* float3 per vertex, stride 12 */
#define COL_ADDR  0x00040000u   /* ubyte4 per vertex, stride 4 */
#define VM_SIZE   0x00100000u   /* 1 MB: this probe has no guest image       */

static rsx_state g_state;
static unsigned  g_vert;

static void M(uint32_t method, uint32_t data) { rsx_process_method(&g_state, method, data); }

static void put_bef(uint32_t off, float f)
{
    uint32_t v; memcpy(&v, &f, 4);
    vm_base[off+0] = (uint8_t)(v >> 24); vm_base[off+1] = (uint8_t)(v >> 16);
    vm_base[off+2] = (uint8_t)(v >> 8);  vm_base[off+3] = (uint8_t)(v);
}

/* Append one vertex: clip-space x,y in [-1,1] and an 0xRRGGBB diffuse colour. */
static void vertex(float x, float y, uint32_t rgb)
{
    put_bef(POS_ADDR + g_vert * 12u + 0u, x);
    put_bef(POS_ADDR + g_vert * 12u + 4u, y);
    put_bef(POS_ADDR + g_vert * 12u + 8u, 0.0f);

    uint8_t* c = vm_base + COL_ADDR + g_vert * 4u;
    c[0] = (uint8_t)(rgb >> 16); c[1] = (uint8_t)(rgb >> 8); c[2] = (uint8_t)rgb; c[3] = 0xFF;
    g_vert++;
}

static void setup_attribs(void)
{
    /* attrib 0 = position: float32, size 3, stride 12 */
    M(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 0 * 4, 0x2u | (3u << 4) | (12u << 8));
    M(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 0 * 4, POS_ADDR);
    /* attrib 3 = diffuse colour: ubyte, size 4, stride 4 */
    M(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 3 * 4, 0x4u | (4u << 4) | (4u << 8));
    M(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 3 * 4, COL_ADDR);
    /* No depth test: the pattern is drawn back to front and must not be
     * rejected by a depth buffer this probe never clears. */
    M(NV4097_SET_DEPTH_TEST_ENABLE, 0);
}

/* A solid axis-aligned quad, as two triangles, in clip space.
 *
 * DRAW_ARRAYS' encoding is count-1 in the high byte and first in the low 24
 * bits, so six vertices from `first` is a count byte of 5. It is written out
 * here rather than hidden behind a helper because getting it wrong draws
 * nothing at all, silently -- the screenshot would just be the clear colour,
 * with no error anywhere to say why. */
static void quad(float x0, float y0, float x1, float y1, uint32_t rgb)
{
    const unsigned first = g_vert;
    vertex(x0, y1, rgb); vertex(x1, y1, rgb); vertex(x1, y0, rgb);
    vertex(x0, y1, rgb); vertex(x1, y0, rgb); vertex(x0, y0, rgb);

    M(NV4097_SET_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    M(NV4097_DRAW_ARRAYS, (5u << 24) | (first & 0xFFFFFFu));
    M(NV4097_SET_BEGIN_END, 0);
}

static void build_pattern(void)
{
    /* Restart the vertex stream each frame: the buffers are rebuilt wholesale,
     * and leaving g_vert climbing would redraw every earlier frame's geometry
     * on top of this one. */
    g_vert = 0;

    /* Background: a dark grey, so an untextured/undrawn region is obvious. */
    M(NV4097_SET_COLOR_CLEAR_VALUE, 0x00101820u);
    M(NV4097_CLEAR_SURFACE, 0xF0u);

    /* Four quadrants. Red top-left, green top-right, blue bottom-left, white
     * bottom-right. A red/blue swap turns the top-left blue, which is the
     * single most common blit bug. */
    quad(-1.0f,  0.0f,  0.0f,  1.0f, 0x00FF0000u);   /* top-left     red   */
    quad( 0.0f,  0.0f,  1.0f,  1.0f, 0x0000FF00u);   /* top-right    green */
    quad(-1.0f, -1.0f,  0.0f,  0.0f, 0x000000FFu);   /* bottom-left  blue  */
    quad( 0.0f, -1.0f,  1.0f,  0.0f, 0x00FFFFFFu);   /* bottom-right white */

    /* Gradient band across the middle: 16 vertical strips, dark -> bright. A
     * transposed or flipped row is visible as running the wrong way. */
    for (int i = 0; i < 16; i++) {
        const float x0 = -1.0f + (float)i * (2.0f / 16.0f);
        const float x1 = -1.0f + (float)(i + 1) * (2.0f / 16.0f);
        const uint32_t v = (uint32_t)(i * 17);           /* 0 .. 255 */
        quad(x0, -0.10f, x1, 0.10f, (v << 16) | (v << 8) | v);
    }

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
}

static int g_inited;

/* Explicit init, called from the page rather than from main().
 *
 * Emscripten runs Module.onRuntimeInitialized BEFORE callMain(), so a page that
 * waits for that hook to call into the module finds the backend uninitialised
 * and every call returns -1 -- which is exactly the silent nothing this probe
 * produced the first time. An explicit init trades that implicit ordering for a
 * function whose name says when to call it. The real port does not have this
 * problem: there, main() runs the guest and the frame loop is started from
 * within it (see the B3 sketch). */
EMSCRIPTEN_KEEPALIVE int rsx_probe_init(void)
{
    if (g_inited) return 0;
    setvbuf(stdout, NULL, _IONBF, 0);

    vm_base = (uint8_t*)calloc(1, VM_SIZE);
    if (!vm_base) { printf("[probe] VM alloc failed\n"); return -1; }

    if (rsx_null_backend_init(W, H, "wasm blit probe") != 0) {
        printf("[probe] backend init failed\n"); return -1;
    }
    rsx_state_init(&g_state);
    setup_attribs();

    if (rsx_canvas_blit_init(W, H) != 0) { printf("[probe] blit init failed\n"); return -1; }

    g_inited = 1;
    printf("[probe] up: %ux%u, framebuffer=%p\n", W, H,
           (void*)rsx_null_backend_framebuffer(NULL, NULL));
    return 0;
}

/* One presented guest frame plus its blit. Driving the tick from the page
 * rather than a host thread is the shape B3 proposes for the real port, so this
 * doubles as a check that the shape works. */
EMSCRIPTEN_KEEPALIVE int rsx_probe_step(void)
{
    if (!g_inited) return -2;

    build_pattern();
    rsx_null_backend_present();
    rsx_canvas_blit_present();

    printf("[probe] frame %d: guest presents=%u blit frames=%u convert=%.3f ms\n",
           (int)rsx_canvas_blit_frames(), rsx_null_backend_frame_count(),
           rsx_canvas_blit_frames(), rsx_canvas_blit_convert_ms());
    return 0;
}

int main(void)
{
    /* Nothing here: the page owns the order (see rsx_probe_init). Under plain
     * wasm32 without a page -- "node probe.js" -- that is fine, main returning
     * immediately is not an error condition. */
    return 0;
}
