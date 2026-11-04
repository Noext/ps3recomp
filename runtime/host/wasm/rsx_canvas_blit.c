/*
 * ps3recomp - RSX framebuffer -> browser canvas blit (see rsx_canvas_blit.h)
 *
 * The byte-order answer, spelled out because it is the whole point:
 *
 *   The null backend stores pixels as u32 in 0x00RRGGBB -- R in bits 16..23,
 *   G in 8..15, B in 0..7. On a little-endian host that word's bytes in memory
 *   are B,G,R,0. A 2D canvas ImageData wants R,G,B,A in that order. So the
 *   conversion is a byte swap of the colour channels plus an opaque alpha, not
 *   a widen -- writing the u32 straight into a Uint8ClampedArray gives blue and
 *   red swapped, which is the classic "everything is orange" bug.
 *
 * Done through shifts rather than a struct or a union so it is endian-neutral:
 * the WASM guest, x86-64 and aarch64 all get RGBA out of the same expression.
 * The compiler turns each iteration into a load, three masks and three stores;
 * the measured cost is in the report.
 */
#include "rsx_canvas_blit.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

/* Declared rather than included: rsx_null_backend.h drags in rsx_commands.h for
 * one u32 typedef, and this file has no business pulling the whole RSX state
 * machinery into a canvas blit. */
extern const u32* rsx_null_backend_framebuffer(u32* out_w, u32* out_h);
extern u32        rsx_null_backend_frame_count(void);

#ifdef __EMSCRIPTEN__
/* rsx_canvas_blit.js installs this on globalThis. Going through globalThis
 * rather than Module keeps the load order free: the page must load the glue
 * before the module (the module calls into it), which means it runs before
 * emscripten has defined Module. Looking the object up at call time means
 * either order works, and a missing glue fails one visible call rather than a
 * parse-time TypeError. */
EM_JS(void, rsx_js_blit_rgba, (const unsigned char* rgba, int w, int h), {
    if (typeof RSXCanvasBlit === 'undefined') {
        console.error('rsx_canvas_blit.js was not loaded: no RSXCanvasBlit on globalThis');
        return;
    }
    /* A view, not a copy: the only copy is the one blit() makes into the
     * ImageData. HEAPU8 is re-read each call because heap growth invalidates
     * every previous view of the buffer. */
    RSXCanvasBlit.blit(new Uint8Array(Module.HEAPU8.buffer, rgba, w * h * 4), w, h);
});
#endif

static struct {
    u8*     rgba;
    u32     width, height;
    u32     frames;
    double  convert_ms;
} s_blit;

/* The conversion itself. Separate from present() so the native harness can
 * call it with a framebuffer it built by hand -- no backend, no canvas. */
void rsx_canvas_blit_convert(const u32* fb, u32 width, u32 height, u8* rgba)
{
    if (!fb || !rgba) return;
    const u32 n = width * height;
    for (u32 i = 0; i < n; i++) {
        const u32 p = fb[i];
        u8* o = rgba + (size_t)i * 4u;
        o[0] = (u8)(p >> 16);   /* R */
        o[1] = (u8)(p >>  8);   /* G */
        o[2] = (u8)(p      );   /* B */
        o[3] = 0xFF;            /* A: the RSX target is opaque here; the null
                                 * backend has no destination alpha to carry */
    }
}

/* Milliseconds from the platform's monotonic clock. Kept local and tiny: the
 * only thing wanted is "did this frame blow the budget", and pulling in the
 * runtime's clock (which is guest-tick aware) for that would be backwards. */
static double blit_now_ms(void)
{
#ifdef __EMSCRIPTEN__
    return emscripten_get_now();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
#endif
}

int rsx_canvas_blit_init(u32 width, u32 height)
{
    if (!width || !height) return -1;
    if (s_blit.rgba && s_blit.width == width && s_blit.height == height)
        return 0;

    const size_t bytes = (size_t)width * height * 4u;
    u8* buf = (u8*)realloc(s_blit.rgba, bytes);
    if (!buf) return -1;
    s_blit.rgba   = buf;
    s_blit.width  = width;
    s_blit.height = height;
    s_blit.frames = 0;
    s_blit.convert_ms = 0.0;
    return 0;
}

void rsx_canvas_blit_shutdown(void)
{
    free(s_blit.rgba);
    memset(&s_blit, 0, sizeof(s_blit));
}

int rsx_canvas_blit_present(void)
{
    u32 w = 0, h = 0;
    const u32* fb = rsx_null_backend_framebuffer(&w, &h);
    if (!fb || !w || !h) return -1;

    /* A guest that resizes its target (or a first call before init) is not an
     * error: resize the staging buffer and carry on. */
    if (!s_blit.rgba || s_blit.width != w || s_blit.height != h) {
        if (rsx_canvas_blit_init(w, h) != 0) return -1;
    }

    const double t0 = blit_now_ms();
    rsx_canvas_blit_convert(fb, w, h, s_blit.rgba);
    const double dt = blit_now_ms() - t0;

    /* Exponential average, so one unlucky frame (a GC pause in the browser)
     * does not read as the steady-state cost. */
    s_blit.convert_ms = s_blit.frames ? s_blit.convert_ms * 0.9 + dt * 0.1 : dt;

#ifdef __EMSCRIPTEN__
    rsx_js_blit_rgba(s_blit.rgba, (int)w, (int)h);
#else
    /* Native: the conversion is the thing under test. The pixels are in
     * s_blit.rgba and rsx_canvas_blit_rgba_ptr() hands them out. */
#endif

    s_blit.frames++;
    return 0;
}

u32    rsx_canvas_blit_frames(void)      { return s_blit.frames; }
double rsx_canvas_blit_convert_ms(void)  { return s_blit.convert_ms; }
u8*    rsx_canvas_blit_rgba_ptr(void)    { return s_blit.rgba; }
