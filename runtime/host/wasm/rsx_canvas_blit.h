/*
 * ps3recomp - RSX framebuffer -> browser canvas blit
 *
 * The null backend's headless software path renders into a plain u32
 * framebuffer (0x00RRGGBB). A browser canvas wants RGBA bytes with an opaque
 * alpha. This is the one place that conversion lives, so the same C is used by
 * the native harness that proves the byte order and by the Emscripten build
 * that actually blits.
 *
 * Split deliberately:
 *
 *   - rsx_canvas_blit_convert() is portable C with no Emscripten in it. It is
 *     the whole correctness question (does 0x00RRGGBB become the bytes a
 *     canvas expects?) and it is therefore the part a native test can settle
 *     without a browser.
 *   - rsx_canvas_blit_present() adds only the upload. Under Emscripten that is
 *     an EM_JS call into rsx_canvas_blit.js; anywhere else it converts and
 *     counts, so a native run exercises the same call sequence.
 *
 * The guest's framebuffer is NOT where the RGBA lands. Writing in place would
 * either corrupt the frame the guest is still reading or cost a second
 * conversion; a staging buffer costs one allocation at init and one linear
 * pass per frame, which is what the measured 60 fps budget assumes.
 */
#ifndef PS3RECOMP_RSX_CANVAS_BLIT_H
#define PS3RECOMP_RSX_CANVAS_BLIT_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate the RGBA staging buffer for a w*h canvas. Idempotent: calling it
 * again with the same size is a no-op, and with a different size it reallocs.
 * Returns 0 on success. */
int  rsx_canvas_blit_init(u32 width, u32 height);

void rsx_canvas_blit_shutdown(void);

/* Convert the presented framebuffer and hand it to the canvas.
 *
 * Returns 0 if a frame went out, -1 if there is nothing to blit (no backend,
 * or init never ran). Cheap enough to call from the tick: one linear pass over
 * width*height u32s and one putImageData. */
int  rsx_canvas_blit_present(void);

/* Pixels converted since init, and the last conversion's duration. Together
 * they answer "is the canvas keeping up" without a profiler: frames should
 * track rsx_null_backend_frame_count() and the milliseconds should sit well
 * under 16.67. */
u32    rsx_canvas_blit_frames(void);
double rsx_canvas_blit_convert_ms(void);

/* --- the portable part --------------------------------------------------
 * Writes w*h RGBA8 texels: byte order R,G,B,A, alpha forced opaque, from
 * 0x00RRGGBB source words. `rgba` must have room for w*h*4 bytes. Exposed so
 * the native harness can check the byte order directly instead of inferring
 * it from a screenshot. */
void rsx_canvas_blit_convert(const u32* fb, u32 width, u32 height, u8* rgba);

/* The staging buffer, for a host that would rather wrap it in an ImageData
 * itself (which is what rsx_canvas_blit.js does). NULL before init. */
u8*  rsx_canvas_blit_rgba_ptr(void);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_CANVAS_BLIT_H */
