/*
 * ps3recomp - browser canvas blit glue (Emscripten)
 *
 * Paired with rsx_canvas_blit.c. The C side owns the byte-order conversion
 * (0x00RRGGBB -> RGBA, alpha forced to 0xFF) because that is the part with a
 * right answer; this file owns only the upload, because that is the part that
 * only exists in a browser.
 *
 * Why putImageData and not a WebGL texture upload: the null backend hands over
 * CPU pixels that were never on a GPU. ImageData -> putImageData keeps them on
 * the CPU path and costs one memcpy, which measured cheaper than a texture
 * upload plus a full-screen quad at this frame size. A WebGL path becomes worth
 * it when the guest's own shaders come online and the pixels are already
 * GPU-side; see the report's "will not work once the guest runs" section.
 *
 * Why it hangs off globalThis rather than Module: the page loads this before
 * the Emscripten module (it must, because the module calls into it), and an
 * `--pre-js` file would run before emscripten's own `var Module = ...`, so
 * touching Module at load time is a TypeError waiting to happen. The module's
 * EM_JS shim reads RSXCanvasBlit off globalThis at call time instead, which
 * makes load order irrelevant.
 *
 * Why the ImageData is cached: constructing one per frame allocates a fresh
 * 3.6 MB buffer at 1280x720 and hands the GC a reason to pause mid-frame. After
 * the first frame nothing is allocated at all.
 */
(function (global) {
  'use strict';

  var state = {
    canvas: null,
    ctx: null,
    imageData: null,
    width: 0,
    height: 0,
    frames: 0,
    copyMs: 0.0,
    drawMs: 0.0,
  };

  /*
   * Attach the blit to a canvas. Resizing the canvas here is what makes a guest
   * whose RSX target is not 1280x720 still fill the element.
   */
  function attach(canvasOrId, width, height) {
    var canvas = typeof canvasOrId === 'string'
      ? document.getElementById(canvasOrId) : canvasOrId;
    if (!canvas) throw new Error('RSXCanvasBlit.attach: no canvas');

    state.canvas = canvas;
    state.ctx = canvas.getContext('2d', { alpha: false, desynchronized: true });
    if (!state.ctx) throw new Error('RSXCanvasBlit.attach: no 2d context');

    if (width && height) {
      canvas.width = width;
      canvas.height = height;
    }
    state.imageData = state.ctx.createImageData(canvas.width, canvas.height);
    state.width = canvas.width;
    state.height = canvas.height;
    return true;
  }

  /*
   * The one function the module calls, once per presented guest frame.
   *
   * `pixels` is a Uint8Array view over the wasm heap built by the EM_JS shim,
   * so there is no per-call copy of the pixels into JS memory -- only the one
   * memcpy below into the ImageData's backing store, which is unavoidable
   * because putImageData reads from exactly that buffer.
   */
  function blit(pixels, w, h) {
    if (!state.ctx) return;

    if (!state.imageData || w !== state.width || h !== state.height) {
      if (state.canvas.width !== w) state.canvas.width = w;
      if (state.canvas.height !== h) state.canvas.height = h;
      state.imageData = state.ctx.createImageData(w, h);
      state.width = w;
      state.height = h;
    }

    var t0 = performance.now();
    state.imageData.data.set(pixels);
    var t1 = performance.now();
    state.ctx.putImageData(state.imageData, 0, 0);
    var t2 = performance.now();

    /* Exponential average, so one unlucky frame (a GC pause, a compositor
     * hitch) does not read as the steady-state cost. */
    var a = state.frames ? 0.9 : 0.0;
    state.copyMs = state.copyMs * a + (t1 - t0) * (1 - a);
    state.drawMs = state.drawMs * a + (t2 - t1) * (1 - a);
    state.frames++;
  }

  function stats() {
    return {
      frames: state.frames,
      copyMs: state.copyMs,
      drawMs: state.drawMs,
      width: state.width,
      height: state.height,
    };
  }

  global.RSXCanvasBlit = { attach: attach, blit: blit, stats: stats };
})(typeof globalThis !== 'undefined' ? globalThis : this);
