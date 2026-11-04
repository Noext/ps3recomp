/*
 * ps3recomp - WebGPU RSX backend for the browser (Emscripten)
 *
 * The third hardware backend, and the only one that runs in a web page. It
 * exists because the two that came first are each locked to an operating
 * system: D3D12 is Windows-only and Metal is Apple-only, so a wasm build
 * served to a browser had exactly one renderer available to it -- the null
 * backend's CPU rasteriser, which is a correctness probe, not a renderer.
 * WebGPU is the browser's only modern, non-deprecated graphics API, so this is
 * what "the port has a GPU path" has to mean there.
 *
 * What it is NOT is a port of rsx_vulkan_backend.c. The RSX is an NV47 and
 * WebGPU sits at a higher level than Vulkan: there are no image layouts, no
 * memory types, no descriptor pools, no explicit barriers and no render pass
 * objects to keep alive between frames. Every one of those is a thing the
 * Vulkan backend spends code on and this one simply does not have, which is
 * why it is a fraction of the length while covering the same vtable slots.
 *
 * Scope, stated plainly because it is the same scope the null backend claims:
 * the eight slots below are the ones wired up. The other thirty-five stay
 * null, and rsx_commands.c already guards every call site with a null check
 * (see the "if (s_backend->clear)" pattern), so a null slot means "this state
 * is tracked by rsx_state but not acted on", not a crash. That is a
 * deliberate, working renderer -- the same trade the null backend makes -- and
 * the report in docs/ lists what each missing slot would need.
 *
 * Build: only under __EMSCRIPTEN__. Every definition here is inside that
 * guard, so a native build never sees this file's contents at all and the
 * library it lands in is byte-identical to one built without it.
 */
#ifndef PS3RECOMP_RSX_WEBGPU_BACKEND_H
#define PS3RECOMP_RSX_WEBGPU_BACKEND_H

#include "rsx_commands.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- lifecycle -----------------------------------------------------------
 *
 * All of these are inert -- they return -1 and do nothing -- off Emscripten,
 * and so is the whole file. That is not politeness: CMake globs every C file in libs/video
 * into the runtime library on every platform, so this translation unit is
 * compiled by the native build too. Guarding the definitions is what keeps
 * "new files must be inert when not building for Emscripten" true without
 * needing a CMake condition.
 * ---------------------------------------------------------------------- */

/* Bring the backend up against the canvas selected by `canvas_selector` (a CSS
 * selector such as "#screen") at width x height.
 *
 * Returns 0 once the device, surface and swapchain are configured and the
 * backend is registered, -1 otherwise. Off Emscripten it always returns -1.
 *
 * SYNCHRONOUS BY DESIGN, and that is the single most important decision in
 * this file. wgpuInstanceRequestAdapter and wgpuAdapterRequestDevice are
 * asynchronous in the browser, and the two obvious ways to wait for them --
 * -sASYNCIFY and -sJSPI -- both change the shape of everything that calls
 * into this code: a vtable slot whose C signature is "void, returns now"
 * cannot suspend the wasm stack and resume it later unless the linker was
 * told to rewrite the whole program around that. rsx_backend's callers
 * (rsx_commands.c, the FIFO consumer, the PPU thread) are not written for
 * that, and making them so is a change to the emulator, not to a backend.
 *
 * So the page hands the device in through Module.preinitializedWebGPUDevice
 * instead: it does `await navigator.gpu.requestAdapter()` /
 * `requestDevice()` itself, from its own async context where awaiting is
 * free, and this function picks the result up synchronously with
 * emscripten_webgpu_get_device(). See docs/RSX_WEBGPU_BACKEND.md for the
 * three lines of page script that are the contract.
 *
 * If the page did not preinitialize a device we fall back to the instance
 * path, which can only get as far as an adapter request; the surface, the
 * pipeline and the clear are then created on the first rsx_webgpu_backend_pump
 * that sees the adapter arrive. That fallback is what makes the probe able to
 * test both shapes, and it is what a host without page cooperation gets. */
int  rsx_webgpu_backend_init(u32 width, u32 height, const char* canvas_selector);

/* Unregister, release every object, and leave the canvas unconfigured. Safe to
 * call when the backend was never initialised. */
void rsx_webgpu_backend_shutdown(void);

/* --- driving -------------------------------------------------------------
 *
 * WebGPU has no present call in a browser. wgpuSurfacePresent() is explicitly
 * unimplemented in emdawnwebgpu ("use requestAnimationFrame via html5.h
 * instead") because a canvas is composited when the frame ends, not when a
 * command is submitted. So "present" here means: end the guest's frame's
 * command encoder, submit it, and let the browser composite.
 *
 * The one piece of housekeeping that cannot happen inside a vtable slot is
 * advancing the device's event queue, because that has to run on a browser
 * task, not inside whatever guest thread called present. `pump` is that: call
 * it once per requestAnimationFrame. It processes events (completing pending
 * map/callback work), retires the swapchain texture for the frame that was
 * submitted, and -- when the page did not preinitialize a device -- advances
 * the fallback bring-up. Returns 0 normally.
 *
 * A host that never calls it still gets a correct first frame; it just will
 * not get a second one, because rAF is what makes the next
 * wgpuSurfaceGetCurrentTexture legal. */
int  rsx_webgpu_backend_pump(void);

/* --- introspection, for a host or a probe that wants to assert on the frame
 * ---------------------------------------------------------------------- */

/* Presents that reached wgpuQueueSubmit since init. */
u32 rsx_webgpu_backend_frame_count(void);

/* The surface format the canvas was configured with, as a
 * WGPUTextureFormat value (0 before init). Exposed as a u32 rather than the
 * WebGPU type so a caller that is not including webgpu.h can still print it. */
u32 rsx_webgpu_backend_surface_format(void);

/* Draws issued that ran the guest's own state: every draw_arrays call whose
 * vertex attributes were enabled. Mirrors rsx_null_backend_guest_draws() and
 * rsx_metal_backend_guest_draws() so one harness can name the same number on
 * every backend. Always 0: this backend has no guest shader path yet (the
 * one pipeline it builds is the fixed-function fallback), and reporting a
 * draw here would claim the guest's program ran when it did not. */
u32 rsx_webgpu_backend_guest_draws(void);

/* The clear colour of the most recent CLEAR_SURFACE, in RSX ARGB8888, so a
 * host can assert the command stream arrived without reading a pixel. */
u32 rsx_webgpu_backend_debug_color(void);

/* Milliseconds spent inside the last submitted frame, and an exponential
 * average over recent ones. The number that decides whether a title fits in
 * 16.67 ms; it covers encoder recording plus submit, not GPU execution, which
 * is the honest thing to report without a timestamp query. */
double rsx_webgpu_backend_frame_ms(void);

/* True once the device, surface and swapchain are up. A probe should wait for
 * this rather than for init's return, because the fallback path brings the
 * surface up later. */
int rsx_webgpu_backend_is_ready(void);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_WEBGPU_BACKEND_H */
