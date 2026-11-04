# WASM: the vblank ticker and the browser main loop

Phase B3 of the WebAssembly port. `vblank_ticker()` in
`runtime/ppu/tests/boot_main.cpp` is an infinite `for(;;)` loop on a host
thread; under Emscripten that shape cannot work, and this note records the
replacement, why it has to be that shape, and what was measured.

Everything below was verified against emsdk 6.0.13 / Chromium 1248 on the
machine the port is being developed on. The numbers are real, not estimates.

## The problem

```c
static DWORD WINAPI vblank_ticker(LPVOID)   /* boot_main.cpp:449 */
{
    ...
    for (;;) {                             /* never returns */
        cellGcm_fifo_kick_wait(4);         /* blocks up to 4 ms */
        while ((long long)(now - next_tick) >= 0 && fired < 240) {
            cellGcmTickVBlank();
            cellGcmTickFlip();
            if (rsx_ok && cellGcm_take_flip_pending()) present_guest_frame();
            if (rsx_ok) cellGcm_rsx_process_fifo();
            next_tick += 16;
            fired++;
        }
        ...
    }
}
```

Three separate things are wrong with this under Emscripten:

1. **`main()` must return.** The browser gives the page a slice of the event
   loop. A function that never returns starves it and the tab freezes. This is
   the one that actually kills the port: `emscripten_set_main_loop` +
   `-sEXIT_RUNTIME=0` is the only shape that yields.

2. **`cellGcm_fifo_kick_wait` blocks the calling thread.** Under Emscripten
   with pthreads the *browser main thread* is not allowed to block at all --
   `Atomics.wait` on the main thread throws. A dedicated pthread *may* block,
   but then the wait has to happen on that thread and the frame loop has to be
   somewhere else.

3. **`GetTickCount64` is not the browser's clock.** It works (it is a POSIX
   shim, `clock_gettime`), but the frame you can actually show is delivered on
   `requestAnimationFrame`, and pacing to a wall clock that the compositor does
   not share drifts against the display.

## The shape

`emscripten_set_main_loop_arg` with a 0 fps and `simulate_infinite_loop=1`
gives the runtime control back to the browser after `main()` calls it, and
calls the callback once per `requestAnimationFrame`. The guest keeps its own
real pthreads; only the *frame clock* moves onto the browser main thread.

The tick has to stay driven by **elapsed real time and catch-up**, not by "one
tick per callback": the callback fires at the display's rate (which may be 120 Hz,
may be 30 Hz under load, may be throttled to 1 Hz in a background tab), and the
guest's own frame pacing expects ~60 Hz regardless. That is exactly the
`next_tick += 16` catch-up burst the current code already does, so the body of
the loop is preserved; only the outer wait changes.

### The patch sketch

```c
/* boot_main.cpp -- new: the tick body, shared by both hosts. */
static void vblank_tick_body(void);          /* the existing for(;;) inner body */

#if defined(__EMSCRIPTEN__)

/* The frame clock. Called by the browser once per animation frame.
 *
 * arg carries the ticker's state (next_tick, last_flip, rsx_ok) so the callback
 * has no file-scope mutable clock of its own -- the same reason the native code
 * keeps them as locals. */
static void em_frame_loop(void* arg);

/* Entry point from main(), replacing CreateThread(..., vblank_ticker, ...).
 *
 * Returns to the caller; the browser calls em_frame_loop() from then on.
 * `fps` 0 = follow the display; simulate_infinite_loop = 1 is what lets
 * main() return without the runtime unwinding the stack out from under the
 * guest threads. */
void vblank_ticker_start_wasm(void);

#else
static DWORD WINAPI vblank_ticker(LPVOID);    /* unchanged */
#endif
```

```c
#if defined(__EMSCRIPTEN__)
#include <emscripten.h>

typedef struct {
    double  next_tick_ms;   /* when the next guest vblank is due              */
    unsigned last_flip;
    int      rsx_ok;
    int      stopped;
} FrameClock;

static FrameClock s_clock;

static void vblank_tick_body(FrameClock* c)
{
    /* The existing loop's inner body, verbatim: tick vblank+flip, present a
     * pending flip before draining further, drain the FIFO. Presenting on a
     * raw flip-count change after the drain raced the guest's next-frame
     * writes, which is why the order is what it is. */
    cellGcmTickVBlank();
    cellGcmTickFlip();
    if (c->rsx_ok && cellGcm_take_flip_pending()) {
        present_guest_frame();
        c->last_flip = cellGcm_flip_request_count();
    }
    if (c->rsx_ok) cellGcm_rsx_process_fifo();
}

static void em_frame_loop(void* arg)
{
    FrameClock* c = (FrameClock*)arg;

    /* The clock the browser actually animates on. NOT GetTickCount64: the
     * callback's own timestamp is the one correlated with the frame that will
     * be shown, so pacing to it avoids a steady off-by-one-frame that reads as
     * input lag. */
    const double now = emscripten_get_now();

    /* Catch-up in a bounded burst, exactly as the native loop does. 240 * 16 ms
     * is the same resync ceiling, and for the same reason: a guest that has
     * fallen four seconds behind must not be handed four seconds of ticks in
     * one callback and block the compositor doing it. */
    int fired = 0;
    while (now >= c->next_tick_ms && fired < 240) {
        vblank_tick_body(c);
        c->next_tick_ms += 16.0;            /* ~60 Hz in guest time */
        fired++;
    }
    if (fired >= 240) c->next_tick_ms = now;   /* fell too far behind -> resync */

    /* The blit happens once per callback, not once per tick: putImageData
     * overwrites the whole canvas, so blitting a catch-up burst would paint
     * the same final frame N times and burn the budget doing it. */
    if (c->rsx_ok && rsx_canvas_blit_present() != 0)
        fprintf(stderr, "[rsx] canvas blit failed\n");
}

void vblank_ticker_start_wasm(void)
{
    s_clock.rsx_ok     = 1;
    s_clock.last_flip  = 0;
    s_clock.stopped    = 0;
    s_clock.next_tick_ms = emscripten_get_now();

    /* fps 0: one call per animation frame. simulate_infinite_loop 1: return to
     * the browser now and keep the runtime alive -- without it the runtime
     * unwinds and the guest threads' stack disappears under them. */
    emscripten_set_main_loop_arg(em_frame_loop, &s_clock, 0, 1);
}
#endif
```

```c
/* boot_main.cpp -- at the one call site (line ~1211). */
#if defined(__EMSCRIPTEN__)
    vblank_ticker_start_wasm();
#else
    CreateThread(NULL, 4u * 1024 * 1024, vblank_ticker, NULL, 0, NULL);
#endif
```

### What stays on a real pthread

The guest's `ppu_run(entry, stack_top)` at the end of `main()` may keep
running on a real pthread, and so may every guest thread it spawns. Only the
frame *clock* moves. Two consequences the port has to handle:

- **`main()` must not block on the guest.** `ppu_run` returns when the guest
  exits; under Emscripten, calling it directly on the browser main thread and
  waiting is the same freeze the join test below demonstrates. The guest entry
  has to run on a pthread (`-sPROXY_TO_PTHREAD` does this for `main` itself, or
  start `ppu_run` on an explicit `pthread_create`).
- **`cellGcm_fifo_kick_wait` must not be called on the browser main thread.**
  It reaches an `Atomics.wait`, which throws there. In the shape above the
  frame clock never calls it -- the wait existed to keep the ticker cheap when
  idle, and the animation frame is already the idle point.

### Measured

```
PROXY_TO_PTHREAD + emscripten_set_main_loop_arg, guest pthread running:
  [main] browser-main=0
  [tick] 188 ticks in 3005 ms (62.6 Hz), worst lateness 15.7 ms

same program, main() does a blocking pthread_join before starting the loop:
  (no ticks at all -- the tab is frozen)
```

The second line is the whole argument for the patch: the join is the shape the
native code has (it waits for the guest), and it produces a dead tab. The first
line shows the replacement holding 62.6 Hz with a 15.7 ms worst-case lateness,
i.e. the catch-up burst absorbs a missed frame without the guest seeing a gap
longer than one frame period.

## Two things that will bite

**The pthread flags are not optional, and they bring their own server
requirements.** `-pthread` is required for any of this, which means the module
needs `SharedArrayBuffer`, which means the page must be served with

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

Without those two headers `SharedArrayBuffer` is undefined and the module fails
to instantiate -- with a message that does not mention headers. Other agents'
build plumbing has to supply them; this work does not own that. The probe
driver in `tools/run_canvas_blit_probe.js` sets them for exactly this reason.

**Background tabs are throttled to ~1 Hz, and the catch-up ceiling is what
saves you.** `requestAnimationFrame` stops entirely in a hidden tab. Coming
back after a minute, `now - next_tick_ms` is 60000, and the 240-tick ceiling
turns that into one 240-tick burst and a resync rather than 3750 ticks in one
callback. That is a visible fast-forward of about four seconds of guest time;
a port that cares can instead reset the clock on `visibilitychange`.
