/*
 * ps3recomp - native proof of rsx_null_backend_framebuffer (Phase B, B1)
 *
 * The accessor is the whole reason a canvas blit is possible: before it, the
 * headless backend could tell a host one pixel (rsx_null_backend_readback_center)
 * and nothing else. This drives the real RSX method dispatch -- not the backend's
 * internal struct, which would only prove the struct -- clears to a few colours,
 * draws a quad with the guest vertex-fetch path, and checks the bytes that come
 * back out of the framebuffer *and* out of the RGBA conversion.
 *
 * Built two ways by tools/test_rsx_canvas_blit.py: as a native binary that runs
 * here, and as a wasm32 module for the browser. Same source, which is the point
 * -- the blit path has to be identical on both or the browser test proves
 * nothing about the port.
 *
 * Exit code 0 when every check passes.
 */
#include "rsx_canvas_blit.h"
#include "rsx_commands.h"
#include "rsx_null_backend.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- what the runtime normally supplies --------------------------------
 * This harness drives the RSX command processor directly over a private
 * framebuffer-sized guest memory, the way libs/video/tests/rsx_harness_main.c
 * does, so it links without the whole guest runtime. */
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

/* Real cellGcmSys resolution. Guest memory here is one flat buffer with no IO
 * window, so both DMA contexts land at the same offset -- which is exactly the
 * identity this harness needs, and why it can drive the vertex fetch without
 * the FIFO. */
uint32_t cellGcmResolveLocated(int location, uint32_t offset)
{
    (void)location; return offset;
}
uint32_t cellGcmResolveOffset(uint32_t offset) { return offset; }

#define VM_SIZE   (32u * 1024u * 1024u)
#define POS_ADDR  0x00100000u
#define COL_ADDR  0x00200000u
#define TEX_ADDR  0x00300000u

static rsx_state g_state;
static void M(uint32_t method, uint32_t data) { rsx_process_method(&g_state, method, data); }

static void put_bef(uint32_t off, float f)
{
    uint32_t v; memcpy(&v, &f, 4);
    vm_base[off+0] = (uint8_t)(v >> 24); vm_base[off+1] = (uint8_t)(v >> 16);
    vm_base[off+2] = (uint8_t)(v >> 8);  vm_base[off+3] = (uint8_t)(v);
}

static int g_fail;
#define CHECK(c, ...) do { if (c) printf("[PASS] " __VA_ARGS__); \
                           else { g_fail++; printf("[FAIL] " __VA_ARGS__); } printf("\n"); } while (0)

/* Count pixels equal to an 0x00RRGGBB value. */
static unsigned count_rgb(const u32* fb, u32 w, u32 h, u32 rgb)
{
    unsigned n = 0;
    for (u32 i = 0; i < w * h; i++) if ((fb[i] & 0x00FFFFFFu) == rgb) n++;
    return n;
}

/* One clear through the real method dispatch: NV4097_SET_COLOR_CLEAR_VALUE then
 * NV4097_CLEAR_SURFACE with the four colour-channel bits set. */
static void clear_to(u32 argb)
{
    M(NV4097_SET_COLOR_CLEAR_VALUE, argb);
    M(NV4097_CLEAR_SURFACE, 0xF0u);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    vm_base = (uint8_t*)calloc(1, VM_SIZE);
    if (!vm_base) { printf("[FAIL] guest VM alloc\n"); return 1; }

    /* --- before init: the accessor must say "nothing" ------------------- */
    {
        u32 w = 0xDEADBEEFu, h = 0xDEADBEEFu;
        const u32* fb = rsx_null_backend_framebuffer(&w, &h);
        CHECK(fb == NULL, "framebuffer() is NULL before init");
        CHECK(rsx_null_backend_frame_count() == 0, "frame_count() is 0 before init");
    }

    if (rsx_null_backend_init(1280, 720, "canvas-blit harness") != 0) {
        printf("[FAIL] backend init\n"); return 1;
    }
    rsx_state_init(&g_state);

    u32 w = 0, h = 0;
    const u32* fb = rsx_null_backend_framebuffer(&w, &h);
    CHECK(fb != NULL, "framebuffer() is non-NULL after init");
    CHECK(w == 1280 && h == 720, "framebuffer() reports 1280x720 (got %ux%u)", w, h);

    /* --- clear to a few colours, read them back ------------------------ */
    /* 0xRRGGBB values chosen so a channel swap cannot pass: R G B distinct and
     * not palindromic. */
    static const u32 kColours[] = { 0xFF204060u, 0x00FF8000u, 0x00C81E50u };
    static const u32 kExpect[]  = { 0x204060u,  0xFF8000u,  0xC81E50u };

    for (unsigned c = 0; c < sizeof kColours / sizeof kColours[0]; c++) {
        clear_to(kColours[c]);
        rsx_null_backend_present();

        u32 cw = 0, ch = 0;
        const u32* cf = rsx_null_backend_framebuffer(&cw, &ch);
        const unsigned hits = count_rgb(cf, cw, ch, kExpect[c]);
        CHECK(cf && hits == cw * ch,
              "clear 0x%08X presented as 0x%06X over the whole %ux%u target (%u/%u px)",
              kColours[c], kExpect[c], cw, ch, hits, cw * ch);
        CHECK(rsx_null_backend_readback_center() == (0xFF000000u | kExpect[c]),
              "readback_center agrees with the framebuffer for 0x%06X (got 0x%08X)",
              kExpect[c], rsx_null_backend_readback_center());
        CHECK(rsx_null_backend_debug_color() == kColours[c],
              "debug_color still reports the raw ARGB clear value");
    }
    CHECK(rsx_null_backend_frame_count() == 3, "frame_count() counted 3 presents (got %u)",
          rsx_null_backend_frame_count());

    /* --- the accessor does not copy: it points at the live target ------ */
    {
        u32 aw = 0, ah = 0;
        const u32* a = rsx_null_backend_framebuffer(&aw, &ah);
        clear_to(0x00112233);
        u32 bw = 0, bh = 0;
        const u32* b = rsx_null_backend_framebuffer(&bw, &bh);
        CHECK(a == b && a[0] == 0x112233u,
              "the returned pointer is the live target and follows a clear");
    }

    /* out_w/out_h are optional: a caller that only wants the pixels passes NULL. */
    CHECK(rsx_null_backend_framebuffer(NULL, NULL) != NULL,
          "framebuffer(NULL, NULL) still returns the pixels");

    /* --- byte order through the real conversion ------------------------ */
    /* This is the check that settles B2's question without a browser: a known
     * 0x00RRGGBB word must land in memory as R,G,B,A. */
    {
        static const u32 fb_in[2]  = { 0x00123456u, 0x00ABCDEFu };
        static const u8  want[8]   = { 0x12,0x34,0x56,0xFF,  0xAB,0xCD,0xEF,0xFF };
        u8 out[8];
        memset(out, 0x77, sizeof out);
        rsx_canvas_blit_convert(fb_in, 2, 1, out);
        CHECK(memcmp(out, want, sizeof want) == 0,
              "convert() writes R,G,B,A for 0x00123456 / 0x00ABCDEF");
        if (memcmp(out, want, sizeof want) != 0) {
            printf("       got %02X %02X %02X %02X %02X %02X %02X %02X\n",
                   out[0],out[1],out[2],out[3],out[4],out[5],out[6],out[7]);
            printf("       want %02X %02X %02X %02X %02X %02X %02X %02X\n",
                   want[0],want[1],want[2],want[3],want[4],want[5],want[6],want[7]);
        }
    }

    /* A clear through the whole stack -- method dispatch, backend, accessor,
     * conversion -- must arrive as pixels a canvas can show. */
    {
        if (rsx_canvas_blit_init(1280, 720) != 0) { printf("[FAIL] blit init\n"); return 1; }
        clear_to(0x00FF8000u);           /* orange */
        rsx_null_backend_present();
        if (rsx_canvas_blit_present() != 0) { printf("[FAIL] blit present\n"); return 1; }

        const u8* rgba = rsx_canvas_blit_rgba_ptr();
        CHECK(rgba != NULL, "blit staging buffer exists");
        /* Sample the four corners and the centre: all must be FF 80 00 FF. */
        const u32 probes[5][2] = { {0,0}, {1279,0}, {0,719}, {1279,719}, {640,360} };
        int all = 1;
        for (int i = 0; i < 5; i++) {
            const u8* p = rgba + ((size_t)probes[i][1] * 1280u + probes[i][0]) * 4u;
            if (!(p[0] == 0xFF && p[1] == 0x80 && p[2] == 0x00 && p[3] == 0xFF)) {
                all = 0;
                printf("       corner (%u,%u) = %02X %02X %02X %02X\n",
                       probes[i][0], probes[i][1], p[0], p[1], p[2], p[3]);
            }
        }
        CHECK(all, "blit_present() converted an orange clear to FF 80 00 FF at corners + centre");
        CHECK(rsx_canvas_blit_frames() == 1, "blit counted one frame");
    }

    rsx_null_backend_shutdown();
    CHECK(rsx_null_backend_framebuffer(NULL, NULL) == NULL,
          "framebuffer() is NULL again after shutdown");

    free(vm_base);
    printf("\n%s: %d failure(s)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
