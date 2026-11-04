/*
 * ps3recomp - WebGPU RSX backend (Emscripten / browser)
 *
 * See rsx_webgpu_backend.h for the scope decision, the bring-up model, and why
 * "present" here has no wgpuSurfacePresent in it.
 *
 * ===========================================================================
 * The Vulkan -> WebGPU mapping, in one place
 * ===========================================================================
 *
 * rsx_vulkan_backend.c is 4086 lines over 92 entry points. Most of that is
 * ceremony WebGPU does not have. Written down because "port the meaning, not
 * the code" is only actionable if the mapping is explicit:
 *
 *   VkRenderPass / VkFramebuffer      -> nothing persistent. A render pass is
 *                                        begun inside the command encoder
 *                                        (wgpuCommandEncoderBeginRenderPass)
 *                                        and its attachments are given as
 *                                        views for that pass only. There is no
 *                                        render-pass-compatibility question, so
 *                                        the whole render-pass cache -- and the
 *                                        "which PSO is this pass compatible
 *                                        with" bookkeeping -- has no
 *                                        counterpart.
 *
 *   VkDescriptorSetLayout/Pool/Set    -> WGPUBindGroupLayout + WGPUBindGroup.
 *                                        No pool, no per-descriptor
 *                                        allocation or free, no memory types:
 *                                        a bind group is created against a
 *                                        layout and is then immutable.
 *
 *   vkCmdPipelineBarrier + layouts    -> NOTHING. This is the largest single
 *                                        deletion. WebGPU derives a texture's
 *                                        usage from the pass or copy it
 *                                        appears in and inserts whatever
 *                                        transition is needed. Render targets
 *                                        stay in RENDER_ATTACHMENT, sampled
 *                                        textures stay in TEXTURE_BINDING, and
 *                                        a copy to a buffer is a
 *                                        CopyTextureToBuffer with no barrier
 *                                        before it. There is no image layout
 *                                        in the WebGPU API at all.
 *
 *   vkAllocateMemory / BindXMemory    -> nothing. A buffer or texture owns its
 *                                        storage from creation. There is no
 *                                        "which heap, which type, does it need
 *                                        a dedicated allocation" step, so the
 *                                        Vulkan backend's memory-type scoring
 *                                        function and its staging ring both
 *                                        disappear. Uploads are
 *                                        wgpuQueueWriteBuffer /
 *                                        wgpuQueueWriteTexture, which copy
 *                                        from CPU memory.
 *
 *   VkSwapchainKHR + vkAcquireNextImage
 *                                     -> wgpuSurfaceConfigure + a
 *                                        wgpuSurfaceGetCurrentTexture per
 *                                        frame. The browser owns the image
 *                                        count and the presentation engine.
 *                                        There is no acquire semaphore, no
 *                                        present queue, and no
 *                                        vkQueuePresentKHR -- see present()
 *                                        below.
 *
 *   vkCmdCopyImageToBuffer readback    -> wgpuCommandEncoderCopyTextureToBuffer
 *                                        + wgpuBufferMapAsync. Same two steps,
 *                                        no layout transition first.
 *
 *   VkShaderModule from SPIR-V         -> WGPUShaderModule from WGSL. The
 *                                        guest's shaders are NV40 microcode;
 *                                        rsx_shader_spirv.cpp and
 *                                        rsx_shader_msl.cpp lower HLSL for the
 *                                        other two backends, and neither
 *                                        target helps here (WGSL is neither
 *                                        SPIR-V nor MSL). This backend
 *                                        therefore has no guest shader path
 *                                        yet and says so rather than
 *                                        pretending otherwise.
 *
 * ===========================================================================
 * What this file actually does
 * ===========================================================================
 *
 * Bring-up: init creates the instance and requests an adapter; pump() -- called
 * once per requestAnimationFrame -- requests the device when the adapter
 * arrives, then creates the surface, configures the swapchain and builds the
 * pipelines. There is no ASYNCIFY and no JSPI anywhere in this path, which is
 * the decision that makes every vtable slot a plain synchronous function (see
 * the header for why that matters).
 *
 * clear: begins a render pass with loadOp=Clear, so the GPU clears the
 * attachment as part of beginning the pass. There is no separate clear command
 * in WebGPU, and using a load op rather than a full-screen quad is what makes
 * "the canvas is the guest's clear colour" one pass.
 *
 * draw_arrays / draw_indexed: fetch vertices out of GUEST memory on the CPU
 * with rsx_fetch_attrib (the same path D3D12 and Metal use -- guest vertex
 * decoding is RSX semantics, not host-API semantics), convert them to the host
 * vertex layout, upload, and draw. Fetching on the CPU rather than reading
 * straight from a GPU-visible window of guest memory is forced: under wasm64
 * the guest VM is hundreds of megabytes of linear memory that WebGPU cannot map
 * as a vertex buffer, and wgpuQueueWriteBuffer is the only road in. That is a
 * real cost and it is called out in the report.
 *
 * present: end the encoder, submit, count the frame.
 *
 * bind_texture / set_vertex_attribs: decode the guest texture through
 * rsx_texture_layout/rsx_texture_decode (again shared, again not host-API
 * business) into RGBA8, upload it, and rebuild the bind group.
 */
#include "rsx_webgpu_backend.h"

#ifdef __EMSCRIPTEN__

#include "rsx_vertex_fetch.h"
#include "rsx_texture_layout.h"
#include "rsx_primitives.h"

#include <webgpu/webgpu.h>
#include <emscripten.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Build protocol note
 *
 * emdawnwebgpu is the only supported WebGPU binding under Emscripten 6.0.11;
 * -sUSE_WEBGPU=1 is rejected by the port itself ("emdawnwebgpu may not be used
 * with -sUSE_WEBGPU=1"), so --use-port=emdawnwebgpu is the spelling that
 * applies here. The port also requires the final link to be a C++ link
 * (LINK_AS_CXX): a plain C link aborts with "emdawnwebgpu requires C++. Link
 * using em++ (or emcc -sDEFAULT_TO_CXX)". CMakeLists.txt passes
 * -sDEFAULT_TO_CXX for the wasm target, which keeps this file plain C and still
 * satisfies the port.
 *
 * Both flags live behind if(EMSCRIPTEN) in CMake, so a native configure never
 * passes them to the host compiler, and this file compiles to nothing there
 * anyway.
 * -----------------------------------------------------------------------*/

/* ---- configuration ----------------------------------------------------- */

/* How many guest vertices one draw may batch. 16384 host vertices is 512 KB of
 * staging -- big enough that a normal title's draws fit, small enough that a
 * pathological one does not try to allocate a second guest VM. */
#define WEBGPU_MAX_BATCH_VERTS 16384u

/* A draw's expanded triangle list is at most 3 indices per input vertex for a
 * strip (n-2 triangles from n vertices), so 3x is a safe ceiling for every
 * primitive the RSX has. */
#define WEBGPU_MAX_INDICES (WEBGPU_MAX_BATCH_VERTS * 3u)

/* The host vertex: clip-space position, diffuse colour as unorm8x4, and a
 * texture coordinate pair.
 *
 * 32 bytes, a multiple of 8: WebGPU requires a vertex buffer offset to be
 * 4-aligned, and a stride of 32 keeps every field naturally aligned for the
 * vector loads the shader compiler assumes. Packing to 28 would work and is not
 * worth the misalignment. */
typedef struct {
    float pos[4];   /* 0  : clip-space x, y, z, w            */
    u8    col[4];   /* 16 : R, G, B, A -- already host order */
    float uv[2];    /* 20 : u, v                             */
    u8    pad[4];   /* 28 : keeps the struct 32 bytes        */
} HostVertex;

/* WebGPU texture uploads require bytesPerRow to be a multiple of 256. */
#define WEBGPU_TEX_ROW_ALIGN 256u

/* The topologies this backend builds a pipeline for.
 *
 * WebGPU bakes the primitive topology into the pipeline object, unlike D3D12
 * (where it is a draw argument) and unlike Vulkan (where it is part of the
 * pipeline too, for the same reason). So "draw a line list" is a different
 * pipeline, not a different call. Five is the whole set WebGPU has that the RSX
 * can produce without expansion; a triangle fan has no WebGPU topology at all,
 * which is why rsx_primitives.h classifies it as needing expansion. */
enum {
    PIPE_TRIANGLES = 0,
    PIPE_TRI_STRIP,
    PIPE_LINES,
    PIPE_LINE_STRIP,
    PIPE_POINTS,
    PIPE_COUNT
};

/* ---- backend state ----------------------------------------------------- */

typedef struct {
    /* Core objects. Every one of these is released in shutdown; a field left
     * dangling after shutdown would only show up on a second init in the same
     * page, which is exactly the kind of bug a probe would never catch. */
    WGPUInstance       instance;
    WGPUAdapter        adapter;
    WGPUDevice         device;
    WGPUQueue          queue;
    WGPUSurface        surface;
    WGPUTextureFormat  surface_format;

    u32 width, height;

    /* Pipelines, one per topology above. All five share the vertex layout, the
     * bind group layout and the shader; only their topology differs, so
     * building them all at init costs five objects and saves a rebuild later. */
    WGPURenderPipeline pipeline[PIPE_COUNT];
    WGPUShaderModule   shader;
    WGPUPipelineLayout pipeline_layout;

    /* Bind group 0 is one uniform block, one sampled texture and one sampler,
     * rebuilt whenever a texture is bound. WebGPU has no "bind texture unit 0"
     * as separate from the group, so binding IS a group rebuild -- which is why
     * bind_texture is not a hot path here, and why the RSX's 16 units collapse
     * to the single one this pipeline declares. */
    WGPUBindGroupLayout bind_group_layout;
    WGPUBindGroup       bind_group;
    WGPUSampler         sampler;
    WGPUTexture         tex;         /* the currently bound guest texture */
    WGPUTextureView     tex_view;
    u32                 tex_w, tex_h;
    int                 tex_ready;

    /* Vertex and index staging, grown geometrically and never shrunk: a title
     * that draws one big batch should pay for it once, not every frame. */
    /* Per-frame upload arenas. Each draw appends at a fresh offset instead of
     * rewriting offset 0, because a command buffer executes at submit time: with
     * one shared buffer every draw in the frame would read whichever vertices were
     * uploaded LAST, and the frame would show only its final draw. The cursors are
     * reset when the frame's encoder is submitted. */
    WGPUBuffer          vbuf;
    u64                 vbuf_size;
    u64                 vbuf_used;
    WGPUBuffer          ibuf;
    u64                 ibuf_size;
    u64                 ibuf_used;

    /* Frame recorder. WebGPU has no global immediate-mode context: every
     * command belongs to an encoder that is finished and submitted together. A
     * frame's commands accumulate here between present() calls, which is the
     * direct analogue of the Vulkan backend's command buffer, except that its
     * lifetime is one submit rather than one swapchain image. */
    WGPUCommandEncoder  enc;
    WGPUTextureView     frame_view;  /* the swapchain view this frame draws to */
    WGPUTexture         frame_tex;
    int                 frame_open;
    int                 pass_open;   /* a render pass inside enc is recording */
    /* Passes begun into this frame's swapchain image. The first one must CLEAR and
     * every later one must LOAD, because a second pass in the same frame (or the
     * pass after a mid-frame flush_encoder) must not discard what is already
     * there. Keying this on acquire_frame's frame_open would be wrong: acquiring
     * the image is not painting it. */
    u32                 frame_passes;
    WGPURenderPassEncoder pass;

    /* Guest state, as last flushed by rsx_commands.c. This is the backend's only
     * view of the RSX: the vtable hands a const rsx_state* per slot and there is
     * no other channel. */
    const rsx_state*    state;

    /* The last CLEAR_SURFACE request, in RSX ARGB8888, so a host can assert the
     * command stream arrived without reading a pixel back. It is also the colour
     * a frame's first pass clears to when the guest draws without clearing. */
    u32 clear_argb;

    u32    frames;
    double frame_ms;
    int    ready;

    /* Bring-up state. adapter_pending/device_pending keep pump() from issuing
     * the same async request twice; bringup_failed latches a hard failure so a
     * probe sees -1 from pump rather than spinning forever. */
    int adapter_pending;
    int device_pending;
    int bringup_failed;

    /* The canvas selector, kept because the surface cannot be created until the
     * device exists, which is several browser tasks after init. */
    char selector[128];
} WebGpuBackend;

static WebGpuBackend s_gpu;

/* The pipeline needs a shader. WGSL, not HLSL or SPIR-V: it is the only shader
 * language WebGPU accepts, and there is no HLSL->WGSL lowering in this tree (the
 * HLSL the decompilers emit goes to D3D12 directly and to Metal via SPIR-V +
 * spirv-cross; neither waypoint is WGSL).
 *
 * The vertex stage reproduces what the D3D12 backend's fixed-function fallback
 * does when the guest has no transform program: apply the MVP from vertex
 * constants 0..3. That is why the matrix is a uniform rather than being folded
 * into the positions on the CPU -- it keeps the transform in one place and lets
 * a later guest-VP path reuse the same pipeline shape.
 *
 * The fragment stage is flat vertex colour, or vertex colour times one sampled
 * texture. Texturing is a branch on a uniform flag rather than a second
 * pipeline: one pipeline and one branch is cheaper to keep correct than two
 * pipelines that have to be kept in sync, and the branch is uniform across a
 * draw so it costs nothing on the GPU. */
static const char* const kShaderWGSL =
    "struct Globals {\n"
    "  mvp : mat4x4<f32>,\n"
    "  textured : f32,\n"
    "  pad0 : f32,\n"
    "  pad1 : f32,\n"
    "  pad2 : f32,\n"
    "};\n"
    "@group(0) @binding(0) var<uniform> g : Globals;\n"
    "@group(0) @binding(1) var tex : texture_2d<f32>;\n"
    "@group(0) @binding(2) var samp : sampler;\n"
    "\n"
    "struct VSIn {\n"
    "  @location(0) pos : vec4<f32>,\n"
    "  @location(1) col : vec4<f32>,\n"
    "  @location(2) uv  : vec2<f32>,\n"
    "};\n"
    "struct VSOut {\n"
    "  @builtin(position) clip : vec4<f32>,\n"
    "  @location(0) col : vec4<f32>,\n"
    "  @location(1) uv  : vec2<f32>,\n"
    "};\n"
    "\n"
    "@vertex fn vs(in : VSIn) -> VSOut {\n"
    "  var out : VSOut;\n"
    "  out.clip = g.mvp * in.pos;\n"
    "  out.col = in.col;\n"
    "  out.uv = in.uv;\n"
    "  return out;\n"
    "}\n"
    "\n"
    "@fragment fn fs(in : VSOut) -> @location(0) vec4<f32> {\n"
    "  if (g.textured > 0.5) {\n"
    "    return textureSample(tex, samp, in.uv) * in.col;\n"
    "  }\n"
    "  return in.col;\n"
    "}\n";

/* The uniform block the shader reads: one mat4 (64 bytes) plus the textured flag
 * and three words of padding, so the struct's size is a multiple of 16 -- WGSL
 * requires that of a uniform member, and a minBindingSize that disagrees with
 * the shader's is a validation error. */
typedef struct {
    float mvp[16];
    float textured, pad0, pad1, pad2;
} Globals;

static WGPUBuffer s_globals;

/* The draw path's scratch. Static rather than stack because 16384 host vertices
 * is 512 KB and 49152 indices is 192 KB, and the wasm stack is 64 KB by default;
 * static rather than heap because a draw is not reentrant (rsx_commands.c calls
 * the vtable from one FIFO consumer) and a per-draw malloc would be the single
 * most expensive thing in the path. */
static HostVertex s_hverts[WEBGPU_MAX_BATCH_VERTS];
static u32        s_indices[WEBGPU_MAX_INDICES];
static u32        s_map[WEBGPU_MAX_BATCH_VERTS];   /* guest index -> host slot */


/* ---- small helpers ----------------------------------------------------- */

/* WGPUStringView over a NUL-terminated literal. WGPU_STRLEN rather than a
 * hand-counted length, so editing a string above cannot desynchronise it. */
static WGPUStringView view(const char* s)
{
    WGPUStringView v;
    v.data = s;
    v.length = WGPU_STRLEN;
    return v;
}

/* Milliseconds from the page's monotonic clock. Kept local because the only
 * question is "did this frame blow the budget", and pulling in the runtime's
 * guest-tick-aware clock for that would be backwards (the same reasoning
 * rsx_canvas_blit.c gives for its copy). */
static double now_ms(void)
{
    return emscripten_get_now();
}

/* Device errors would otherwise be silent in the console. Uncaptured errors are
 * validation errors -- a bad descriptor, a bind group that does not match its
 * layout -- and they are exactly the class of bug this backend is most likely
 * to have, so they get printed rather than swallowed. The callback cannot throw
 * into wasm; printing and continuing is the only option. */
static void on_error(WGPUDevice const* device, WGPUErrorType type,
                     WGPUStringView msg, void* u1, void* u2)
{
    (void)device; (void)u1; (void)u2;
    const int n = (msg.length == WGPU_STRLEN)
                      ? (msg.data ? (int)strlen(msg.data) : 0)
                      : (int)msg.length;
    fprintf(stderr, "[RSX webgpu] device error %d: %.*s\n", (int)type, n,
            msg.data ? msg.data : "");
}

/* The guest's ARGB8888 clear word -> the four doubles a WebGPU clear wants.
 *
 * The layout is the one rsx_null_backend's software path documents: R in bits
 * 16..23, G in 8..15, B in 0..7, A in 24..31. Shifts rather than a union keep it
 * endian-neutral, the same reason rsx_canvas_blit.c uses shifts.
 *
 * A clear mask without the alpha bit means "leave alpha alone"; since this
 * backend's swapchain is opaque, the only sensible value is 1.0. */
static WGPUColor colour_from_argb(u32 color, u32 mask)
{
    WGPUColor c;
    c.r = (double)((color >> 16) & 0xFFu) / 255.0;
    c.g = (double)((color >>  8) & 0xFFu) / 255.0;
    c.b = (double)( color        & 0xFFu) / 255.0;
    c.a = (mask & 0x80u) ? (double)((color >> 24) & 0xFFu) / 255.0 : 1.0;
    return c;
}

/* ---- render pass management --------------------------------------------- */

/* Begin a render pass, opening a fresh encoder first if this frame has none.
 *
 * WebGPU has no standalone clear: a render pass begun with LoadOp_Clear clears
 * its attachments as part of beginning. That is the whole reason clear() is one
 * call, and it is why the pass is recorded lazily -- the first thing that needs
 * a pass (a clear or a draw) starts it, and present() ends it. A frame that only
 * clears therefore submits exactly one pass with no geometry at all.
 *
 * load_op is LoadOp_Clear for the frame's first pass and LoadOp_Load for any
 * later one, because a second pass in the same frame must not throw away what
 * the first wrote. The colour argument only matters for LoadOp_Clear. */
static void begin_pass(WGPULoadOp load_op, WGPUColor colour)
{
    if (s_gpu.pass_open) return;

    if (!s_gpu.enc) s_gpu.enc = wgpuDeviceCreateCommandEncoder(s_gpu.device, NULL);
    if (!s_gpu.enc) return;

    WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    ca.view = s_gpu.frame_view;
    ca.loadOp = load_op;
    ca.storeOp = WGPUStoreOp_Store;
    ca.clearValue = colour;

    WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &ca;
    /* No depth attachment. The pipelines have no depth-stencil state and the
     * backend keeps no depth texture, so declaring one here would be a
     * validation error, not a feature. See the report's "what WebGPU cannot
     * express" section: the guest's depth buffer lives in its own VRAM and this
     * backend does not shadow it. */

    s_gpu.pass = wgpuCommandEncoderBeginRenderPass(s_gpu.enc, &rp);
    s_gpu.pass_open = 1;
    s_gpu.frame_passes++;

    /* The viewport and scissor are per-pass in WebGPU; there is no global one.
     * Every pass therefore starts by covering the whole surface. The scissor
     * default after beginning a pass is empty, so a pass with no scissor set
     * draws nothing at all -- this call is load-bearing, not tidiness. */
    if (s_gpu.pass) {
        wgpuRenderPassEncoderSetViewport(s_gpu.pass, 0.0f, 0.0f,
                                         (float)s_gpu.width, (float)s_gpu.height,
                                         0.0f, 1.0f);
        wgpuRenderPassEncoderSetScissorRect(s_gpu.pass, 0, 0,
                                            s_gpu.width, s_gpu.height);
    }
}

static void end_pass(void)
{
    if (!s_gpu.pass_open) return;
    if (s_gpu.pass) {
        wgpuRenderPassEncoderEnd(s_gpu.pass);
        wgpuRenderPassEncoderRelease(s_gpu.pass);
        s_gpu.pass = NULL;
    }
    s_gpu.pass_open = 0;
}

/* Acquire this frame's swapchain texture and make its view current.
 *
 * Called from clear() and from the draw path, whichever comes first. The only
 * non-error statuses are the two Success ones; anything else means the browser
 * refused to hand over an image -- most often because the canvas was resized or
 * reconfigured since the last frame, which is what Outdated means. In that case
 * the surface is reconfigured and the frame is skipped rather than submitted
 * against a stale texture, which would be a validation error.
 *
 * Returns 0 if a view is current for this frame. */
static int acquire_frame(void)
{
    if (s_gpu.frame_view) return 0;
    if (!s_gpu.surface || !s_gpu.ready) return -1;

    WGPUSurfaceTexture st = WGPU_SURFACE_TEXTURE_INIT;
    wgpuSurfaceGetCurrentTexture(s_gpu.surface, &st);
    if (st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
        st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
        /* Outdated/Lost/Timeout: reconfigure and let the caller retry next
         * frame. Error is not recoverable by reconfiguring, so it is reported
         * rather than looping silently. */
        if (st.texture) wgpuTextureRelease(st.texture);
        if (st.status == WGPUSurfaceGetCurrentTextureStatus_Error)
            fprintf(stderr, "[RSX webgpu] getCurrentTexture error\n");
        WGPUSurfaceConfiguration cfg = WGPU_SURFACE_CONFIGURATION_INIT;
        cfg.device = s_gpu.device;
        cfg.format = s_gpu.surface_format;
        cfg.usage = WGPUTextureUsage_RenderAttachment;
        cfg.width = s_gpu.width;
        cfg.height = s_gpu.height;
        cfg.presentMode = WGPUPresentMode_Fifo;
        cfg.alphaMode = WGPUCompositeAlphaMode_Auto;
        wgpuSurfaceConfigure(s_gpu.surface, &cfg);
        return -1;
    }

    s_gpu.frame_tex = st.texture;
    s_gpu.frame_view = wgpuTextureCreateView(st.texture, NULL);
    s_gpu.frame_open = 1;
    return s_gpu.frame_view ? 0 : -1;
}

/* Retire the swapchain texture for the frame just submitted.
 *
 * In the browser the compositor presents the canvas when the current task
 * yields, so releasing the view is all a present does beyond the submit. This is
 * the piece a Vulkan-shaped backend spells vkQueuePresentKHR and which here is
 * simply "let go". */
static void release_frame(void)
{
    if (s_gpu.frame_view) {
        wgpuTextureViewRelease(s_gpu.frame_view);
        s_gpu.frame_view = NULL;
    }
    if (s_gpu.frame_tex) {
        wgpuTextureRelease(s_gpu.frame_tex);
        s_gpu.frame_tex = NULL;
    }
    s_gpu.frame_open = 0;
    s_gpu.frame_passes = 0;
}

/* ---- staging buffers ---------------------------------------------------- */

/* Grow a staging buffer geometrically, never shrinking. usage is the WebGPU
 * usage bit for the role the buffer plays (Vertex or Index); both also need
 * CopyDst because wgpuQueueWriteBuffer is the only way to fill them. */
/* Flush the frame's encoder to the GPU without ending the frame.
 *
 * This exists for one reason: a draw may need more upload space than the arena
 * has left, and the arena cannot be reallocated while already-recorded draws
 * still point into it -- destroying a buffer that a submitted-but-not-yet-executed
 * draw references is a validation error. Submitting what has been recorded frees
 * the arena to be recreated, and a frame is allowed any number of submits: the
 * canvas is still composited once, at the end of the task.
 *
 * The swapchain view is deliberately NOT released here. This is mid-frame, and the
 * next encoder keeps drawing into the same attachment. */
static void flush_encoder(void)
{
    end_pass();
    if (!s_gpu.enc) return;
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(s_gpu.enc, NULL);
    wgpuCommandEncoderRelease(s_gpu.enc);
    s_gpu.enc = NULL;
    if (cmd) {
        wgpuQueueSubmit(s_gpu.queue, 1, &cmd);
        wgpuCommandBufferRelease(cmd);
    }
}

/* Reserve bytes in an upload arena and hand back the offset to write at.
 *
 * Offsets are aligned to 16, which satisfies the 4-byte requirement of a vertex
 * buffer offset and of a uint32 index buffer offset with room to spare, and keeps
 * every upload on a vector boundary. Returns -1 only if the device refuses to make
 * a buffer big enough. */
static int arena_alloc(WGPUBuffer* buf, u64* size, u64* used, u64 bytes,
                       WGPUBufferUsage usage, u64* out_offset)
{
    const u64 aligned_bytes = (bytes + 15u) & ~15ull;
    if (*buf && *used + aligned_bytes <= *size) {
        *out_offset = *used;
        *used += aligned_bytes;
        return 0;
    }

    /* Not enough room. Whatever has been recorded references the current buffer,
     * so submit it before the buffer is replaced; then start again at zero with a
     * buffer big enough for this draw (at least double the old one, so a growing
     * frame settles on a size instead of flushing every draw). */
    if (*buf) {
        flush_encoder();
        wgpuBufferDestroy(*buf);
        *buf = NULL;
    }
    u64 want = *size ? *size : aligned_bytes;
    while (want < aligned_bytes) want *= 2u;
    WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
    bd.usage = usage | WGPUBufferUsage_CopyDst;
    bd.size = want;
    *buf = wgpuDeviceCreateBuffer(s_gpu.device, &bd);
    if (!*buf) { *size = 0; *used = 0; return -1; }
    *size = want;
    *used = aligned_bytes;
    *out_offset = 0;
    return 0;
}

/* Drop the arenas' contents. Called when the frame's encoder is submitted:
 * everything the GPU needed has been copied in by then, and the next frame starts
 * at offset 0 again. */
static void arena_reset(void)
{
    s_gpu.vbuf_used = 0;
    s_gpu.ibuf_used = 0;
}

/* ---- vertex conversion -------------------------------------------------- */

/* One guest vertex -> one HostVertex, position and colour.
 *
 * The attributes are the ones the null backend, D3D12 and Metal all agree on: 0
 * is POSITION and 3 is DIFFUSE. Texture coordinates come from attribute 8 when
 * a texture is bound; the null backend makes the identical decision (see nullsw
 * draw_prim), so a probe that draws the same geometry through both backends gets
 * the same picture.
 *
 * The colour conversion is RSX -> host, not host -> host: the guest stores
 * floats in the attribute array and rsx_fetch_attrib has already un-byte-swapped
 * them, so this is a clamp-and-scale to unorm8 and nothing more. */
static void guest_vertex(const rsx_state* st, u32 vi, int textured, HostVertex* out)
{
    float p[4], c[4], uv[4];
    rsx_fetch_attrib(st, 0, vi, p);
    rsx_fetch_attrib(st, 3, vi, c);
    out->pos[0] = p[0]; out->pos[1] = p[1];
    out->pos[2] = p[2]; out->pos[3] = p[3] != 0.0f ? p[3] : 1.0f;

    for (int i = 0; i < 4; i++) {
        float v = c[i];
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        /* +0.5 before truncation is round-to-nearest, so 0.5 becomes 128 rather
         * than the 127 a bare truncating cast would give. */
        out->col[i] = (u8)(v * 255.0f + 0.5f);
    }

    if (textured) {
        rsx_fetch_attrib(st, 8, vi, uv);
        out->uv[0] = uv[0];
        out->uv[1] = uv[1];
    } else {
        /* Zero, and the shader does not sample it: the uniform's textured flag
         * is what gates the texture read, not the uv. */
        out->uv[0] = 0.0f;
        out->uv[1] = 0.0f;
    }
    memset(out->pad, 0, sizeof out->pad);
}

/* Push the MVP and the textured flag into the uniform buffer and bind group 0.
 *
 * The matrix slot is not arbitrary: the D3D12 backend takes the fixed-function
 * fallback's matrix from vertex constants 0..3 (see its "Push the MVP matrix
 * from RSX vertex constants slots 0..3"), and titles that keep their MVP
 * elsewhere -- gcm/cube uses c[256..259] -- are exactly the ones whose guest VP
 * the fallback cannot honour anyway. Matching D3D12 means the same geometry
 * lands the same way through both backends, which is what makes a cross-backend
 * probe meaningful.
 *
 * An all-zero constant bank falls back to identity rather than a zero matrix: a
 * zero matrix collapses every vertex to the origin and draws nothing, which
 * looks exactly like a broken backend. The guest writes its constants before it
 * draws in every title observed, so this is a first-frame guard. */
static void push_globals(int textured)
{
    Globals g;
    memset(&g, 0, sizeof g);
    int have = 0;
    if (s_gpu.state) {
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) {
                const float v = s_gpu.state->vertex_constants[r][c];
                g.mvp[r * 4 + c] = v;
                if (v != 0.0f) have = 1;
            }
    }
    if (!have) {
        memset(g.mvp, 0, sizeof g.mvp);
        g.mvp[0] = g.mvp[5] = g.mvp[10] = g.mvp[15] = 1.0f;
    }
    g.textured = textured ? 1.0f : 0.0f;
    wgpuQueueWriteBuffer(s_gpu.queue, s_globals, 0, &g, sizeof g);
    if (s_gpu.bind_group && s_gpu.pass)
        wgpuRenderPassEncoderSetBindGroup(s_gpu.pass, 0, s_gpu.bind_group, 0, NULL);
}

/* ---- vtable: lifecycle -------------------------------------------------- */

static int webgpu_init(void* ud, u32 width, u32 height)
{
    (void)ud;
    /* The vtable's init has no selector argument; the public entry point took
     * the canvas already and stored the size it was configured for. Reaching
     * init through the vtable is therefore a resize, not a second bring-up, and
     * rsx_commands.c never calls it -- the slot is populated so that a host
     * which does call it gets a truthful answer instead of a null dereference. */
    if (width) s_gpu.width = width;
    if (height) s_gpu.height = height;
    return s_gpu.ready ? 0 : -1;
}

static void webgpu_shutdown(void* ud)
{
    (void)ud;
    /* There is deliberately no "drain the device" call here, and the reason is a
     * genuine WebGPU gap rather than an oversight: wgpuDevicePoll is a Dawn/native
     * extension that emdawnwebgpu does not provide (a browser cannot block on GPU
     * work from the page thread), and wgpuQueueOnSubmittedWorkDone is asynchronous
     * with no synchronous wait. So the releases below may race in-flight work.
     *
     * That is safe in practice for exactly the reason the WebGPU spec gives: a
     * released object stays alive until the GPU has finished with it. The JS
     * bindings hold a reference to the underlying GPUBuffer/GPUTexture, and the
     * browser's implementation keeps it valid for as long as a submitted command
     * buffer refers to it. What this avoids is only the (legal) case of a driver
     * seeing the object dropped early; correctness does not depend on it.
     *
     * The honest consequence: a probe that shuts the backend down immediately
     * after a submit and then reads the canvas may read the frame before the last
     * submit lands. The probe in runtime/host/wasm waits for a rAF after present
     * for precisely this reason. */
    if (s_gpu.pass_open) end_pass();
    if (s_gpu.enc) { wgpuCommandEncoderRelease(s_gpu.enc); s_gpu.enc = NULL; }
    release_frame();

    for (int i = 0; i < PIPE_COUNT; i++)
        if (s_gpu.pipeline[i]) wgpuRenderPipelineRelease(s_gpu.pipeline[i]);
    if (s_gpu.bind_group)        wgpuBindGroupRelease(s_gpu.bind_group);
    if (s_gpu.bind_group_layout) wgpuBindGroupLayoutRelease(s_gpu.bind_group_layout);
    if (s_gpu.pipeline_layout)   wgpuPipelineLayoutRelease(s_gpu.pipeline_layout);
    if (s_gpu.shader)            wgpuShaderModuleRelease(s_gpu.shader);
    if (s_gpu.sampler)           wgpuSamplerRelease(s_gpu.sampler);
    if (s_gpu.tex_view)          wgpuTextureViewRelease(s_gpu.tex_view);
    if (s_gpu.tex)               wgpuTextureRelease(s_gpu.tex);
    if (s_globals)               wgpuBufferRelease(s_globals);
    if (s_gpu.vbuf)              wgpuBufferRelease(s_gpu.vbuf);
    if (s_gpu.ibuf)              wgpuBufferRelease(s_gpu.ibuf);
    if (s_gpu.surface)           wgpuSurfaceRelease(s_gpu.surface);
    if (s_gpu.queue)             wgpuQueueRelease(s_gpu.queue);
    if (s_gpu.device)            wgpuDeviceRelease(s_gpu.device);
    if (s_gpu.adapter)           wgpuAdapterRelease(s_gpu.adapter);
    if (s_gpu.instance)          wgpuInstanceRelease(s_gpu.instance);

    memset(&s_gpu, 0, sizeof s_gpu);
    s_globals = NULL;
    rsx_set_backend(NULL);
}

/* ---- vtable: state ------------------------------------------------------ */

/* set_render_target is where the backend learns about the guest's surfaces. It
 * stores the pointer and nothing else: this backend renders to the canvas, not
 * to the guest's VRAM surfaces, so a guest that renders to an offscreen shadow
 * (which most do before a post-process) is not reproducible here. That gap is
 * the single largest thing this backend does not do and it leads the report's
 * "what WebGPU cannot express" section. */
static void webgpu_set_render_target(void* ud, const rsx_state* st)
{
    (void)ud;
    s_gpu.state = st;
}

/* set_vertex_attribs is where a host vertex layout would be derived. There is
 * nothing to derive: the host layout is fixed by HostVertex and the guest's
 * per-attribute formats are consumed by rsx_fetch_attrib at fetch time. So this
 * only caches the state pointer the fetch needs. */
static void webgpu_set_vertex_attribs(void* ud, const rsx_state* st)
{
    (void)ud;
    s_gpu.state = st;
}

/* ---- vtable: clear ------------------------------------------------------ */

/* The clear, and the piece that proves the whole path.
 *
 * NV4097_CLEAR_SURFACE's argument is a mask: 0x01 depth, 0x02 stencil, and 0xF0
 * the four colour channels (RSX_CLEAR_COLOR_R/G/B/A). A colour clear is taken as
 * a whole -- a per-channel mask is not something a render pass load op can
 * express, and no title observed clears only some channels -- while a depth-only
 * clear has nowhere to go because this backend has no depth attachment. So the
 * mask is honoured as "colour if any colour bit, else nothing", and the depth and
 * stencil arguments are recorded but unused.
 *
 * clear_argb is kept for two reasons: a host can assert the command stream
 * arrived without reading a pixel back, and a frame whose first event is a draw
 * (no clear at all) still needs a defined background. */
static void webgpu_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud; (void)depth; (void)stencil;
    s_gpu.clear_argb = color;

    if (!s_gpu.ready || !s_gpu.device) return;
    if (acquire_frame() != 0) return;

    if ((flags & 0xF0u) == 0) {
        /* Depth and/or stencil only. Nothing to clear here, but the frame must
         * still exist or the draws that follow would open a pass with an
         * undefined load. */
        begin_pass(s_gpu.frame_passes == 0 ? WGPULoadOp_Clear : WGPULoadOp_Load,
                   colour_from_argb(color, flags));
        return;
    }

    /* LoadOp_Clear, not LoadOp_Load: this IS the clear. Beginning the pass with it
     * means the GPU clears the attachment as part of the pass, with no draw and no
     * full-screen quad.
     *
     * If a pass is ALREADY recording into this frame's image, a clear has to start
     * a new one: a load op belongs to beginning a pass, and extending an open pass
     * cannot retroactively clear it. Ending the current pass first is the correct
     * and cheap answer -- the GPU's clear at the start of the next pass overwrites
     * everything anyway. This is the shape a guest that draws, then clears, then
     * draws again produces. */
    if (s_gpu.pass_open) end_pass();
    begin_pass(WGPULoadOp_Clear, colour_from_argb(color, flags));
}

/* ---- vtable: textures --------------------------------------------------- */

/* Decode one guest texture unit into an RGBA8 image and make it current.
 *
 * Everything about "what shape is this guest texture" is in
 * rsx_texture_layout/rsx_texture_decode, which the D3D12 backend and the null
 * backend's software path both use; this only adds the upload. That split is
 * deliberate and predates this backend -- see rsx_texture_layout.h's header --
 * and it is why this function is fifty lines rather than five hundred.
 *
 * The register conventions are the ones the null backend documents at length in
 * nullsw_bind_texture: CONTROL0 bit 31 is the unit enable, the format register is
 * (format byte << 8) | location, and image_rect is (width << 16) | height.
 * Getting any of those wrong reads guest memory that is not the texture. */
static void webgpu_bind_texture(void* ud, u32 unit, const rsx_texture_state* t)
{
    (void)ud;
    /* Only unit 0 is honoured. The bind group has one texture binding and the
     * shader samples one texture; the RSX's 16 units would need a descriptor
     * array and a multi-unit shader, which is guest-program territory this
     * backend has not reached. Binding a unit other than 0 must not clobber unit
     * 0's texture, hence the guard. */
    if (unit != 0 || !t) return;

    if (!s_gpu.ready || !s_gpu.device) return;
    if (!(t->control0 & 0x80000000u) || !t->offset) { s_gpu.tex_ready = 0; return; }

    const u32 w = (t->image_rect >> 16) & 0xFFFFu;
    const u32 h =  t->image_rect        & 0xFFFFu;
    if (!w || !h || w > 4096u || h > 4096u) { s_gpu.tex_ready = 0; return; }

    const u32 ea = cellGcmResolveLocated((t->format & 3u) == 1u, t->offset);
    if (!vm_base || ea == 0xFFFFFFFFu) { s_gpu.tex_ready = 0; return; }
    const u32 fmt = (t->format >> 8) & 0xFFu;

    rsx_tex_layout tl;
    rsx_texture_layout(fmt, w, h, &tl);
    /* Block-compressed guest formats are left unbindable rather than decoded into
     * a wrong shape: rsx_texture_decode copies BC payloads verbatim, and WebGPU
     * would need a matching compressed format on the texture to accept them. The
     * formats this backend does take are the uncompressed ones. */
    if (tl.compressed) { s_gpu.tex_ready = 0; return; }

    /* Decode into a tightly packed RGBA8 image. Narrow formats are splayed to
     * four channels here, once, instead of in the shader: a single texture format
     * on the GPU keeps the bind group layout fixed, and the conversion is a
     * linear pass over the image either way. */
    const u32 pitch = w * 4u;
    u8* rgba = (u8*)malloc((size_t)pitch * h);
    if (!rgba) { s_gpu.tex_ready = 0; return; }

    if (tl.fmt == RSX_TEXFMT_R8G8B8A8) {
        rsx_texture_decode(rgba, pitch, vm_base + ea, w, h, &tl,
                           rsx_texture_argb_is_rgba());
    } else {
        const u32 srcp = tl.row_bytes;
        u8* tmp = (u8*)malloc((size_t)srcp * h);
        if (!tmp) { free(rgba); s_gpu.tex_ready = 0; return; }
        rsx_texture_decode(tmp, srcp, vm_base + ea, w, h, &tl, 0);
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++) {
                const u8* sp = tmp + (size_t)y * srcp + (size_t)x * tl.bytes_per_texel;
                u8* dp = rgba + (size_t)y * pitch + (size_t)x * 4u;
                dp[0] = sp[0];
                dp[1] = tl.bytes_per_texel > 1 ? sp[1] : sp[0];
                dp[2] = tl.bytes_per_texel > 2 ? sp[2] : sp[0];
                dp[3] = 255;
            }
        free(tmp);
    }

    /* Recreate rather than reuse. wgpuQueueWriteTexture could overwrite in place
     * when the size did not change, and that is the obvious next optimisation --
     * but a different guest texture of the same size would then silently reuse
     * the previous one if anything above early-returned, and correctness first is
     * the rule this file follows elsewhere too. */
    if (s_gpu.tex_view) { wgpuTextureViewRelease(s_gpu.tex_view); s_gpu.tex_view = NULL; }
    if (s_gpu.tex)      { wgpuTextureRelease(s_gpu.tex); s_gpu.tex = NULL; }

    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.dimension = WGPUTextureDimension_2D;
    td.size.width = w;
    td.size.height = h;
    td.size.depthOrArrayLayers = 1;
    td.format = WGPUTextureFormat_RGBA8Unorm;
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    s_gpu.tex = wgpuDeviceCreateTexture(s_gpu.device, &td);
    if (!s_gpu.tex) { free(rgba); s_gpu.tex_ready = 0; return; }

    /* bytesPerRow must be a multiple of 256 for a texture write. A width that is
     * already a multiple of 64 texels needs no repacking, which is the common
     * case at RSX sizes; otherwise the rows are copied into a padded staging
     * buffer. */
    const u32 aligned = (pitch + WEBGPU_TEX_ROW_ALIGN - 1u) & ~(WEBGPU_TEX_ROW_ALIGN - 1u);
    const u8* src_pixels = rgba;
    u8* padded = NULL;
    if (aligned != pitch) {
        padded = (u8*)calloc((size_t)aligned * h, 1);
        if (!padded) { free(rgba); s_gpu.tex_ready = 0; return; }
        for (u32 y = 0; y < h; y++)
            memcpy(padded + (size_t)y * aligned, rgba + (size_t)y * pitch, pitch);
        src_pixels = padded;
    }

    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    dst.texture = s_gpu.tex;
    dst.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    layout.offset = 0;
    layout.bytesPerRow = aligned;
    layout.rowsPerImage = h;
    WGPUExtent3D ext = WGPU_EXTENT_3D_INIT;
    ext.width = w;
    ext.height = h;
    ext.depthOrArrayLayers = 1;
    wgpuQueueWriteTexture(s_gpu.queue, &dst, src_pixels,
                          (size_t)aligned * h, &layout, &ext);
    free(padded);
    free(rgba);

    s_gpu.tex_view = wgpuTextureCreateView(s_gpu.tex, NULL);
    if (!s_gpu.tex_view) { s_gpu.tex_ready = 0; return; }
    s_gpu.tex_w = w;
    s_gpu.tex_h = h;
    s_gpu.tex_ready = 1;

    /* Rebind. A bind group holds concrete objects, so replacing the texture means
     * replacing the group; there is no "same slot, new resource" in WebGPU short
     * of a texture array. */
    if (s_gpu.bind_group) { wgpuBindGroupRelease(s_gpu.bind_group); s_gpu.bind_group = NULL; }
    WGPUBindGroupEntry e[3] = {
        WGPU_BIND_GROUP_ENTRY_INIT,
        WGPU_BIND_GROUP_ENTRY_INIT,
        WGPU_BIND_GROUP_ENTRY_INIT,
    };
    e[0].binding = 0;
    e[0].buffer = s_globals;
    e[0].size = sizeof(Globals);
    e[1].binding = 1;
    e[1].textureView = s_gpu.tex_view;
    e[2].binding = 2;
    e[2].sampler = s_gpu.sampler;
    WGPUBindGroupDescriptor bg = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bg.layout = s_gpu.bind_group_layout;
    bg.entryCount = 3;
    bg.entries = e;
    s_gpu.bind_group = wgpuDeviceCreateBindGroup(s_gpu.device, &bg);
}

/* ---- vtable: draws ------------------------------------------------------ */

/* Pick the pipeline for a primitive, and say whether the CPU must expand it.
 *
 * Two separate questions that look like one. Expansion is decided by
 * rsx_primitives.h, which owns it and which the other two backends also obey (see
 * that header's note about three disagreeing copies). The pipeline is chosen by
 * what the primitive becomes, and WebGPU bakes the topology into the pipeline
 * object, so "draw a line list" is a different pipeline rather than a different
 * call argument. A triangle fan has no WebGPU topology at all, which is exactly
 * why rsx_primitives.h classifies it as needing expansion. */
static int pipeline_for(u32 prim, int* needs_expand)
{
    *needs_expand = rsx_primitive_needs_expansion(prim);
    switch (prim) {
    case RSX_PRIMITIVE_TRIANGLE_STRIP: return PIPE_TRI_STRIP;
    case RSX_PRIMITIVE_LINES:          return PIPE_LINES;
    case RSX_PRIMITIVE_LINE_STRIP:     return PIPE_LINE_STRIP;
    case RSX_PRIMITIVE_LINE_LOOP:      return PIPE_LINE_STRIP;
    case RSX_PRIMITIVE_POINTS:         return PIPE_POINTS;
    default:                           return PIPE_TRIANGLES;
    }
}

/* Expand a primitive's vertex sequence into a triangle list of guest indices.
 *
 * This is the same case analysis rsx_null_backend.c does in draw_prim, which is
 * the reference for what each RSX primitive means; writing it as "emit three
 * indices" rather than "fill three pixels" is the only difference. The strip
 * winding flip (i+1 + (i&1)) is the same expression, because a triangle strip
 * that does not alternate winding renders half its triangles back-facing.
 *
 * Returns the number of indices written. */
static u32 expand_indices(u32 prim, u32 first, u32 count, u32* out, u32 cap)
{
    u32 n = 0;

    /* The macro cannot be written any shorter without losing the bound check, and
     * a hand-unrolled version would be nine near-identical copies of it. */
    #define EMIT(a, b, c) do {                                  \
        if (n + 3 > cap) return n;                              \
        out[n++] = (a); out[n++] = (b); out[n++] = (c);         \
    } while (0)

    switch (prim) {
    case RSX_PRIMITIVE_TRIANGLES:
        for (u32 i = 0; i + 2 < count; i += 3)
            EMIT(first + i, first + i + 1, first + i + 2);
        break;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
        for (u32 i = 0; i + 2 < count; i++)
            EMIT(first + i, first + i + 1 + (i & 1), first + i + 2 - (i & 1));
        break;
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_POLYGON:
        for (u32 i = 1; i + 1 < count; i++)
            EMIT(first, first + i, first + i + 1);
        break;
    case RSX_PRIMITIVE_QUADS:
        for (u32 i = 0; i + 3 < count; i += 4) {
            EMIT(first + i, first + i + 1, first + i + 2);
            EMIT(first + i, first + i + 2, first + i + 3);
        }
        break;
    case RSX_PRIMITIVE_QUAD_STRIP:
        for (u32 i = 0; i + 3 < count; i += 2) {
            EMIT(first + i, first + i + 1, first + i + 3);
            EMIT(first + i, first + i + 3, first + i + 2);
        }
        break;
    default:
        break;
    }
    #undef EMIT
    return n;
}

/* The shared body of draw_arrays and draw_indexed.
 *
 * idx is NULL for a non-indexed draw, in which case the guest vertex range
 * [first, first+count) is fetched and drawn directly; for an indexed draw the
 * caller has already resolved the guest's indices and passes them, so first is
 * unused and every index is absolute.
 *
 * Two phases, because expansion makes the host vertex count unknown up front: the
 * host vertex table is built first (deduplicated by guest index, since a strip or
 * a fan references one vertex several times), then the index list is rebased onto
 * that table and drawn. Building it the other way round would need a worst-case
 * allocation three times the size for a strip.
 *
 * The scratch is the file-static arrays declared above, not the stack: 16384 host
 * vertices is 512 KB and the wasm stack is 64 KB by default. */
static void draw_batch(u32 prim, u32 first, u32 count, const u32* idx, u32 nidx)
{
    if (!s_gpu.ready || !s_gpu.device || !s_gpu.state) return;
    if (!count && !nidx) return;
    if (acquire_frame() != 0) return;

    int expand = 0;
    const int pipe = pipeline_for(prim, &expand);
    if (!s_gpu.pipeline[pipe]) return;

    /* The guest index list for this draw. When the primitive needs expansion this
     * becomes a triangle list of guest indices; when it does not, a strip or fan
     * is drawn natively and the list is simply the guest sequence. */
    u32 n_out = 0;
    int indexed = 0;
    if (expand) {
        if (idx) {
            /* An expanded indexed draw: expand the ORDINALS into triangles, then
             * translate each ordinal through the caller's index array so the
             * result is a list of guest vertex indices like the unindexed case. */
            if (nidx > WEBGPU_MAX_BATCH_VERTS) return;
            static u32 ordinals[WEBGPU_MAX_BATCH_VERTS];
            memcpy(ordinals, idx, (size_t)nidx * sizeof(u32));
            const u32 n = expand_indices(prim, 0, nidx, s_indices, WEBGPU_MAX_INDICES);
            for (u32 i = 0; i < n; i++) {
                const u32 o = s_indices[i];
                s_indices[i] = ordinals[o < nidx ? o : 0];
            }
            n_out = n;
        } else {
            n_out = expand_indices(prim, first, count, s_indices, WEBGPU_MAX_INDICES);
        }
        if (!n_out) return;
        indexed = 1;
    } else if (idx) {
        if (nidx > WEBGPU_MAX_INDICES) return;
        memcpy(s_indices, idx, (size_t)nidx * sizeof(u32));
        n_out = nidx;
        indexed = 1;
    }

    const u32 n_given = idx ? nidx : count;
    if (n_given == 0 || n_given > WEBGPU_MAX_BATCH_VERTS) return;

    /* The guest index range, so the dedup table can be offset-indexed rather than
     * sized to the whole 32-bit index space. */
    u32 lo = 0xFFFFFFFFu, hi = 0;
    if (idx) {
        for (u32 i = 0; i < nidx; i++) {
            if (idx[i] < lo) lo = idx[i];
            if (idx[i] > hi) hi = idx[i];
        }
    } else {
        lo = first;
        hi = first + count - 1u;
    }
    if (hi < lo) return;
    const u32 span = hi - lo + 1u;
    if (span > WEBGPU_MAX_BATCH_VERTS) return;

    for (u32 i = 0; i < span; i++) s_map[i] = 0xFFFFFFFFu;
    u32 nvert = 0;

    /* Texture coordinates are fetched only when a texture is actually bound. An
     * untextured draw must not pay for a fetch of attribute 8 the guest may never
     * have supplied, and the uniform flag is what tells the shader not to sample. */
    const int textured = s_gpu.tex_ready;

    if (idx) {
        for (u32 i = 0; i < nidx; i++) {
            const u32 gv = idx[i];
            if (gv < lo || gv > hi) continue;
            if (s_map[gv - lo] != 0xFFFFFFFFu) continue;
            if (nvert >= WEBGPU_MAX_BATCH_VERTS) break;
            s_map[gv - lo] = nvert;
            guest_vertex(s_gpu.state, gv, textured, &s_hverts[nvert]);
            nvert++;
        }
    } else {
        for (u32 gv = first; gv <= hi && nvert < WEBGPU_MAX_BATCH_VERTS; gv++) {
            s_map[gv - lo] = nvert;
            guest_vertex(s_gpu.state, gv, textured, &s_hverts[nvert]);
            nvert++;
        }
    }
    if (nvert == 0) return;

    /* Rebase the index list onto the deduplicated host table. A guest index the
     * table skipped (because the cap was hit) becomes slot 0, which draws a
     * degenerate triangle rather than reading past the buffer. */
    if (indexed) {
        for (u32 i = 0; i < n_out; i++) {
            const u32 gv = s_indices[i];
            s_indices[i] = (gv >= lo && gv <= hi && s_map[gv - lo] != 0xFFFFFFFFu)
                               ? s_map[gv - lo] : 0u;
        }
    }

    /* Reserve this draw's slice of the arena, at a new offset. See arena_alloc for
     * why offset 0 must not be reused within a frame. */
    u64 vofs = 0;
    if (arena_alloc(&s_gpu.vbuf, &s_gpu.vbuf_size, &s_gpu.vbuf_used,
                    (u64)nvert * sizeof(HostVertex),
                    WGPUBufferUsage_Vertex, &vofs) != 0) return;
    wgpuQueueWriteBuffer(s_gpu.queue, s_gpu.vbuf, vofs, s_hverts,
                         (u64)nvert * sizeof(HostVertex));

    /* The frame's first pass clears -- to the guest's clear colour, or to black if
     * the guest never set one -- because a frame whose first event is a draw must
     * still present defined pixels rather than whatever the swapchain image
     * happened to contain. Every later pass loads, so draws after a clear (and
     * draws after a mid-frame flush) keep what is already there. A pass that is
     * already open is simply extended; begin_pass is a no-op then. */
    begin_pass(s_gpu.frame_passes == 0 ? WGPULoadOp_Clear : WGPULoadOp_Load,
               colour_from_argb(s_gpu.clear_argb, 0xF0u));
    if (!s_gpu.pass) return;

    push_globals(textured);
    wgpuRenderPassEncoderSetPipeline(s_gpu.pass, s_gpu.pipeline[pipe]);
    wgpuRenderPassEncoderSetVertexBuffer(s_gpu.pass, 0, s_gpu.vbuf, vofs,
                                         WGPU_WHOLE_SIZE);
    if (indexed) {
        u64 iofs = 0;
        if (arena_alloc(&s_gpu.ibuf, &s_gpu.ibuf_size, &s_gpu.ibuf_used,
                        (u64)n_out * sizeof(u32),
                        WGPUBufferUsage_Index, &iofs) != 0) return;
        wgpuQueueWriteBuffer(s_gpu.queue, s_gpu.ibuf, iofs, s_indices,
                             (u64)n_out * sizeof(u32));
        wgpuRenderPassEncoderSetIndexBuffer(s_gpu.pass, s_gpu.ibuf,
                                            WGPUIndexFormat_Uint32, iofs,
                                            WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderDrawIndexed(s_gpu.pass, n_out, 1, 0, 0, 0);
    } else {
        wgpuRenderPassEncoderDraw(s_gpu.pass, count, 1, 0, 0);
    }
}

static void webgpu_draw_arrays(void* ud, u32 primitive, u32 first, u32 count)
{
    (void)ud;
    draw_batch(primitive, first, count, NULL, 0);
}

/* Resolve the guest's index array and draw through it.
 *
 * The index array is guest memory: u32 or u16 elements in the guest's byte order
 * like every other word the PS3 writes, with the type in the DMA register's bits
 * [7:4] (0 = u32, 1 = u16) and the context DMA in bits [3:0] -- the encoding
 * rsx_commands.h documents for SET_INDEX_ARRAY_ADDRESS. The null backend does not
 * read indices at all ("index buffers are not read"); this one does, because a
 * backend that silently drops every indexed draw is a backend that draws nothing
 * for most titles.
 *
 * index_offset arrives as DRAW_INDEX_ARRAY's low 24 bits, which is the FIRST
 * INDEX, not a byte offset. It is added to the array base before the stride is
 * applied.
 *
 * The staging array is static for the same reason the draw scratch is: a
 * 16384-element u32 list is 64 KB, which is the whole default wasm stack. */
static void webgpu_draw_indexed(void* ud, u32 primitive, u32 index_offset, u32 count)
{
    (void)ud;
    static u32 idx[WEBGPU_MAX_BATCH_VERTS];

    if (!s_gpu.state || !vm_base || !count) return;
    if (count > WEBGPU_MAX_BATCH_VERTS) return;

    const rsx_state* st = s_gpu.state;
    const u32 ea = cellGcmResolveLocated((st->index_array_dma & 0x0Fu) == 1u,
                                         st->index_array_offset);
    if (ea == 0xFFFFFFFFu) return;

    const int wide = ((st->index_array_dma >> 4) & 0x0Fu) == 0;   /* 0 = u32 */
    const u8* base = vm_base + ea;
    if (wide) {
        for (u32 i = 0; i < count; i++) {
            const u32 at = (index_offset + i) * 4u;
            idx[i] = ((u32)base[at] << 24) | ((u32)base[at + 1] << 16) |
                     ((u32)base[at + 2] << 8) | (u32)base[at + 3];
        }
    } else {
        for (u32 i = 0; i < count; i++) {
            const u32 at = (index_offset + i) * 2u;
            idx[i] = ((u32)base[at] << 8) | (u32)base[at + 1];
        }
    }

    draw_batch(primitive, 0, count, idx, count);
}

/* ---- vtable: present ---------------------------------------------------- */

/* End the frame's pass and encoder, submit, and let go of the swapchain
 * texture. See the header for why there is no wgpuSurfacePresent here.
 *
 * buffer_id is the guest's display buffer (which of its VRAM surfaces it is
 * flipping to). This backend does not shadow guest surfaces, so the id is not
 * acted on -- rendering always went to the canvas. */
static void webgpu_present(void* ud, u32 buffer_id)
{
    (void)ud; (void)buffer_id;
    if (!s_gpu.ready || !s_gpu.device) return;
    if (!s_gpu.frame_view) {
        /* A flip with nothing rendered: the guest neither cleared nor drew this
         * frame. Submitting an empty encoder would still count a frame, which
         * would make "the canvas updates" indistinguishable from "the guest is
         * drawing", so it is skipped. */
        return;
    }

    const double t0 = now_ms();

    end_pass();
    if (!s_gpu.enc) return;
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(s_gpu.enc, NULL);
    wgpuCommandEncoderRelease(s_gpu.enc);
    s_gpu.enc = NULL;
    if (cmd) {
        wgpuQueueSubmit(s_gpu.queue, 1, &cmd);
        wgpuCommandBufferRelease(cmd);
    }
    release_frame();
    arena_reset();

    /* Exponential average, so one unlucky frame (a GC pause in the browser) does
     * not read as the steady-state cost. */
    const double dt = now_ms() - t0;
    s_gpu.frame_ms = s_gpu.frames ? s_gpu.frame_ms * 0.9 + dt * 0.1 : dt;
    s_gpu.frames++;
}

/* ---- the vtable --------------------------------------------------------- */

/* Eight slots populated, thirty-five left null -- the same bargain
 * rsx_null_backend.c's software path makes, and for the same reason: every call
 * site in rsx_commands.c null-checks the slot it calls, so a null means "the
 * state is tracked but not acted on", not a crash. What each omitted slot would
 * need is in the report; the short version is that they all want guest shaders,
 * which need an NV40 -> WGSL translation that does not exist. */
static rsx_backend s_webgpu_backend = {
    .userdata           = &s_gpu,
    .init               = webgpu_init,
    .shutdown           = webgpu_shutdown,
    .present            = webgpu_present,
    .set_render_target  = webgpu_set_render_target,
    .set_vertex_attribs = webgpu_set_vertex_attribs,
    .clear              = webgpu_clear,
    .bind_texture       = webgpu_bind_texture,
    .draw_arrays        = webgpu_draw_arrays,
    .draw_indexed       = webgpu_draw_indexed,
};

/* ---- bring-up ----------------------------------------------------------- */

/* Build everything that does not depend on the swapchain: the uniform buffer, the
 * shader, the bind-group layout, the pipeline layout, the sampler, the default
 * white texture, the bind group and one pipeline per topology.
 *
 * Pipeline creation is synchronous here (wgpuDeviceCreateRenderPipeline returns
 * the object; only the *async* create variant takes a callback), which is what
 * makes this callable from init without an event-loop turn.
 *
 * The pipelines' colour target format must match the surface's, because a pipeline
 * is only usable with render passes whose attachments have the formats it
 * declares. That is why the surface format is chosen before this runs, and why a
 * surface reconfigure with a different format would need the pipelines rebuilt --
 * something this backend never does, because the canvas format does not change
 * under it. */
static int build_pipeline(void)
{
    WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
    bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    bd.size = sizeof(Globals);
    s_globals = wgpuDeviceCreateBuffer(s_gpu.device, &bd);
    if (!s_globals) return -1;

    WGPUShaderSourceWGSL src = WGPU_SHADER_SOURCE_WGSL_INIT;
    src.code = view(kShaderWGSL);
    WGPUShaderModuleDescriptor smd = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    smd.nextInChain = &src.chain;
    s_gpu.shader = wgpuDeviceCreateShaderModule(s_gpu.device, &smd);
    if (!s_gpu.shader) return -1;

    /* Bind group layout: uniform, sampled texture, sampler. Written explicitly
     * rather than relying on "layout: auto" because an explicit layout is what
     * lets the same bind group be reused across draws without re-deriving it, and
     * because a validation error against an explicit layout names the binding it
     * objected to. */
    WGPUBindGroupLayoutEntry ble[3] = {
        WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
        WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
        WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
    };
    ble[0].binding = 0;
    ble[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    ble[0].buffer.type = WGPUBufferBindingType_Uniform;
    ble[0].buffer.minBindingSize = sizeof(Globals);
    ble[1].binding = 1;
    ble[1].visibility = WGPUShaderStage_Fragment;
    ble[1].texture.sampleType = WGPUTextureSampleType_Float;
    ble[1].texture.viewDimension = WGPUTextureViewDimension_2D;
    ble[2].binding = 2;
    ble[2].visibility = WGPUShaderStage_Fragment;
    ble[2].sampler.type = WGPUSamplerBindingType_Filtering;

    WGPUBindGroupLayoutDescriptor bld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bld.entryCount = 3;
    bld.entries = ble;
    s_gpu.bind_group_layout = wgpuDeviceCreateBindGroupLayout(s_gpu.device, &bld);
    if (!s_gpu.bind_group_layout) return -1;

    WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pld.bindGroupLayoutCount = 1;
    pld.bindGroupLayouts = &s_gpu.bind_group_layout;
    s_gpu.pipeline_layout = wgpuDeviceCreatePipelineLayout(s_gpu.device, &pld);
    if (!s_gpu.pipeline_layout) return -1;

    WGPUSamplerDescriptor sd = WGPU_SAMPLER_DESCRIPTOR_INIT;
    sd.addressModeU = WGPUAddressMode_Repeat;
    sd.addressModeV = WGPUAddressMode_Repeat;
    sd.addressModeW = WGPUAddressMode_Repeat;
    sd.magFilter = WGPUFilterMode_Nearest;
    sd.minFilter = WGPUFilterMode_Nearest;
    sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    sd.lodMinClamp = 0.0f;
    sd.lodMaxClamp = 1.0f;
    sd.maxAnisotropy = 1;
    s_gpu.sampler = wgpuDeviceCreateSampler(s_gpu.device, &sd);
    if (!s_gpu.sampler) return -1;

    /* A 1x1 white default texture. The bind group must hold a valid texture view
     * before the first draw even when the guest has bound no texture, because a
     * bind group entry may not be null. A 1x1 white texel multiplied into the
     * vertex colour is the identity, so an untextured draw through the sampling
     * branch still gets the right pixels. */
    {
        const u32 white = 0xFFFFFFFFu;
        WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
        td.dimension = WGPUTextureDimension_2D;
        td.size.width = 1;
        td.size.height = 1;
        td.size.depthOrArrayLayers = 1;
        td.format = WGPUTextureFormat_RGBA8Unorm;
        td.mipLevelCount = 1;
        td.sampleCount = 1;
        td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
        s_gpu.tex = wgpuDeviceCreateTexture(s_gpu.device, &td);
        if (!s_gpu.tex) return -1;
        WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        dst.texture = s_gpu.tex;
        dst.aspect = WGPUTextureAspect_All;
        WGPUTexelCopyBufferLayout lay = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        lay.offset = 0;
        lay.bytesPerRow = 4;
        lay.rowsPerImage = 1;
        WGPUExtent3D ext = WGPU_EXTENT_3D_INIT;
        ext.width = 1; ext.height = 1; ext.depthOrArrayLayers = 1;
        wgpuQueueWriteTexture(s_gpu.queue, &dst, &white, sizeof white, &lay, &ext);
        s_gpu.tex_view = wgpuTextureCreateView(s_gpu.tex, NULL);
        if (!s_gpu.tex_view) return -1;
        s_gpu.tex_ready = 0;      /* white, but no guest texture bound */
    }

    /* The bind group. Rebuilt by bind_texture when the guest binds something. */
    WGPUBindGroupEntry be[3] = {
        WGPU_BIND_GROUP_ENTRY_INIT,
        WGPU_BIND_GROUP_ENTRY_INIT,
        WGPU_BIND_GROUP_ENTRY_INIT,
    };
    be[0].binding = 0; be[0].buffer = s_globals; be[0].size = sizeof(Globals);
    be[1].binding = 1; be[1].textureView = s_gpu.tex_view;
    be[2].binding = 2; be[2].sampler = s_gpu.sampler;
    WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bgd.layout = s_gpu.bind_group_layout;
    bgd.entryCount = 3;
    bgd.entries = be;
    s_gpu.bind_group = wgpuDeviceCreateBindGroup(s_gpu.device, &bgd);
    if (!s_gpu.bind_group) return -1;

    /* Vertex layout. Locations 0/1/2 are position/colour/uv, matching the shader's
     * VSIn and the HostVertex field order. */
    WGPUVertexAttribute attrs[3] = {
        WGPU_VERTEX_ATTRIBUTE_INIT,
        WGPU_VERTEX_ATTRIBUTE_INIT,
        WGPU_VERTEX_ATTRIBUTE_INIT,
    };
    attrs[0].format = WGPUVertexFormat_Float32x4;
    attrs[0].offset = offsetof(HostVertex, pos);
    attrs[0].shaderLocation = 0;
    /* unorm8x4, not float32x4: the host vertex stores colour as bytes, and unorm8
     * is the format that makes the GPU divide by 255 for free. Storing them as
     * floats would double the vertex size for no gain. */
    attrs[1].format = WGPUVertexFormat_Unorm8x4;
    attrs[1].offset = offsetof(HostVertex, col);
    attrs[1].shaderLocation = 1;
    attrs[2].format = WGPUVertexFormat_Float32x2;
    attrs[2].offset = offsetof(HostVertex, uv);
    attrs[2].shaderLocation = 2;

    WGPUVertexBufferLayout vbl = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    vbl.stepMode = WGPUVertexStepMode_Vertex;
    vbl.arrayStride = sizeof(HostVertex);
    vbl.attributeCount = 3;
    vbl.attributes = attrs;

    WGPUVertexState vs = WGPU_VERTEX_STATE_INIT;
    vs.module = s_gpu.shader;
    vs.entryPoint = view("vs");
    vs.bufferCount = 1;
    vs.buffers = &vbl;

    WGPUMultisampleState ms = WGPU_MULTISAMPLE_STATE_INIT;
    ms.count = 1;
    ms.mask = 0xFFFFFFFFu;

    WGPUColorTargetState ct = WGPU_COLOR_TARGET_STATE_INIT;
    ct.format = s_gpu.surface_format;
    ct.writeMask = WGPUColorWriteMask_All;
    /* No blending, deliberately, and it is the same call the null backend's
     * software path makes ("no blending"): the guest's blend state lives in
     * rsx_state, but a blend pipeline is a distinct pipeline object in WebGPU, so
     * honouring a runtime blend mode means rebuilding the pipeline when the guest
     * changes it. That is a real piece of work -- a small pipeline cache keyed on
     * blend+depth+cull -- and is listed in the report as such. */
    WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
    fs.module = s_gpu.shader;
    fs.entryPoint = view("fs");
    fs.targetCount = 1;
    fs.targets = &ct;

    /* One pipeline per topology. Everything but the topology is shared, so this
     * loop is the whole difference between drawing a triangle list and a line
     * list in this backend -- WebGPU has no per-draw topology argument. */
    static const WGPUPrimitiveTopology tops[PIPE_COUNT] = {
        WGPUPrimitiveTopology_TriangleList,
        WGPUPrimitiveTopology_TriangleStrip,
        WGPUPrimitiveTopology_LineList,
        WGPUPrimitiveTopology_LineStrip,
        WGPUPrimitiveTopology_PointList,
    };
    for (int i = 0; i < PIPE_COUNT; i++) {
        WGPUPrimitiveState ps = WGPU_PRIMITIVE_STATE_INIT;
        ps.topology = tops[i];
        /* A strip topology needs a strip index format, because an indexed strip's
         * primitive-restart behaviour depends on it. Undefined is only legal for
         * the list topologies. */
        if (tops[i] == WGPUPrimitiveTopology_TriangleStrip ||
            tops[i] == WGPUPrimitiveTopology_LineStrip)
            ps.stripIndexFormat = WGPUIndexFormat_Uint32;
        else
            ps.stripIndexFormat = WGPUIndexFormat_Undefined;
        /* Culling is off. The guest's own cull state is in rsx_state but honouring
         * it needs a pipeline per cull mode, and a winding-order mistake here would
         * make half a title's geometry vanish in a way that looks like a depth bug.
         * Off is the honest default until the guest's state is threaded through. */
        ps.frontFace = WGPUFrontFace_CCW;
        ps.cullMode = WGPUCullMode_None;

        WGPURenderPipelineDescriptor rpd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        rpd.layout = s_gpu.pipeline_layout;
        rpd.vertex = vs;
        rpd.primitive = ps;
        rpd.multisample = ms;
        rpd.fragment = &fs;
        /* depthStencil left NULL: no depth attachment exists, so declaring depth
         * state would be a validation error rather than a feature. */
        s_gpu.pipeline[i] = wgpuDeviceCreateRenderPipeline(s_gpu.device, &rpd);
        if (!s_gpu.pipeline[i]) return -1;
    }
    return 0;
}

/* Configure the swapchain for the current size.
 *
 * Fifo rather than Immediate/Mailbox: the browser only guarantees Fifo, an
 * Immediate request is downgraded silently where it is unsupported, and a headless
 * probe's screenshot is of the composited canvas either way. AlphaMode Auto lets
 * the browser pick opaque composition, which is what the guest's framebuffer is. */
static void configure_surface(void)
{
    WGPUSurfaceConfiguration cfg = WGPU_SURFACE_CONFIGURATION_INIT;
    cfg.device = s_gpu.device;
    cfg.format = s_gpu.surface_format;
    cfg.usage = WGPUTextureUsage_RenderAttachment;
    cfg.width = s_gpu.width;
    cfg.height = s_gpu.height;
    cfg.presentMode = WGPUPresentMode_Fifo;
    cfg.alphaMode = WGPUCompositeAlphaMode_Auto;
    wgpuSurfaceConfigure(s_gpu.surface, &cfg);
}

/* Create the surface for the selector and finish the bring-up.
 *
 * Split from init because the fallback path reaches it on a later pump, once the
 * adapter and device callbacks have fired. Everything here needs a device, and the
 * surface needs a selector, so this is the earliest point either can be used.
 *
 * The canvas selector is a chained struct, not a descriptor field:
 * WGPUEmscriptenSurfaceSourceCanvasHTMLSelector is chained onto the
 * WGPUSurfaceDescriptor's nextInChain. That is the Emscripten-specific idiom in
 * this file and the one thing there is no non-Emscripten equivalent of -- every
 * other platform's surface source (HWND, Xlib, Metal layer) is chained the same
 * way, so the SHAPE is portable and only the struct differs. */
static int finish_bringup(const char* selector)
{
    if (!s_gpu.instance || !s_gpu.adapter || !s_gpu.device) return -1;
    s_gpu.queue = wgpuDeviceGetQueue(s_gpu.device);
    if (!s_gpu.queue) return -1;

    WGPUSurfaceDescriptor sd = WGPU_SURFACE_DESCRIPTOR_INIT;
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas =
        WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
    canvas.selector = view(selector ? selector : "#screen");
    sd.nextInChain = &canvas.chain;
    s_gpu.surface = wgpuInstanceCreateSurface(s_gpu.instance, &sd);
    if (!s_gpu.surface) return -1;

    /* Pick the format from the surface's capabilities rather than from
     * navigator.gpu.getPreferredCanvasFormat(). They normally agree, but the
     * capabilities list is what this surface will accept for rendering, and a
     * pipeline built against a format the surface does not support is rejected at
     * pass time with a validation error. Preferring an 8-bit unorm format keeps
     * the guest's 8888 framebuffer a direct copy. */
    WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
    const WGPUStatus cap_status =
        wgpuSurfaceGetCapabilities(s_gpu.surface, s_gpu.adapter, &caps);
    if (cap_status != WGPUStatus_Success || !caps.formatCount) return -1;
    s_gpu.surface_format = caps.formats[0];
    for (size_t i = 0; i < caps.formatCount; i++) {
        if (caps.formats[i] == WGPUTextureFormat_BGRA8Unorm ||
            caps.formats[i] == WGPUTextureFormat_RGBA8Unorm) {
            s_gpu.surface_format = caps.formats[i];
            break;
        }
    }
    wgpuSurfaceCapabilitiesFreeMembers(caps);

    configure_surface();
    if (build_pipeline() != 0) return -1;

    s_gpu.ready = 1;
    printf("[RSX webgpu] up: %ux%u, surface format %d\n", s_gpu.width,
           s_gpu.height, (int)s_gpu.surface_format);
    return 0;
}

/* ---- adapter / device callbacks ----------------------------------------- */

/* Both callbacks only record the result. The work that consumes it happens in
 * pump(), on the next browser task, which is what keeps this file free of
 * ASYNCIFY: a callback that runs from JS-land must not call into code that could
 * block, and a "call finish_bringup from here" shape does exactly that (it writes
 * backend state that a guest thread may be reading, and it can printf). Recording
 * and returning is the shape that works under every Emscripten threading mode. */
static void on_adapter(WGPURequestAdapterStatus status, WGPUAdapter adapter,
                       WGPUStringView msg, void* u1, void* u2)
{
    (void)msg; (void)u1; (void)u2;
    s_gpu.adapter_pending = 0;
    if (status == WGPURequestAdapterStatus_Success) s_gpu.adapter = adapter;
    else s_gpu.bringup_failed = 1;
}

static void on_device(WGPURequestDeviceStatus status, WGPUDevice device,
                      WGPUStringView msg, void* u1, void* u2)
{
    (void)u1; (void)u2;
    s_gpu.device_pending = 0;
    if (status == WGPURequestDeviceStatus_Success) {
        s_gpu.device = device;
    } else {
        const int n = (msg.length == WGPU_STRLEN)
                          ? (msg.data ? (int)strlen(msg.data) : 0)
                          : (int)msg.length;
        fprintf(stderr, "[RSX webgpu] device request failed: %.*s\n", n,
                msg.data ? msg.data : "");
        s_gpu.bringup_failed = 1;
    }
}

/* ---- public API --------------------------------------------------------- */

int rsx_webgpu_backend_init(u32 width, u32 height, const char* canvas_selector)
{
    if (s_gpu.ready) return 0;
    memset(&s_gpu, 0, sizeof s_gpu);
    s_globals = NULL;
    s_gpu.width = width ? width : 1280;
    s_gpu.height = height ? height : 720;
    /* Stored rather than used here: the surface cannot be created until there is
     * a device, which under the fallback path is several browser tasks away. */
    snprintf(s_gpu.selector, sizeof s_gpu.selector, "%s",
             canvas_selector ? canvas_selector : "#screen");

    s_gpu.instance = wgpuCreateInstance(NULL);
    if (!s_gpu.instance) {
        fprintf(stderr, "[RSX webgpu] no instance\n");
        return -1;
    }

    /* Adapter first. Under the preinitialized-device path the page's device came
     * from an adapter we never saw, and wgpuSurfaceGetCapabilities needs one --
     * so this request happens on both paths and is what pump() waits for. */
    WGPURequestAdapterOptions o = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    o.powerPreference = WGPUPowerPreference_HighPerformance;
    WGPURequestAdapterCallbackInfo acb = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    /* AllowProcessEvents, not AllowSpontaneous: the callback must run from a pump()
     * we control, not from wherever the browser feels like calling it, because it
     * writes backend state that a guest thread may be reading. */
    acb.mode = WGPUCallbackMode_AllowProcessEvents;
    acb.callback = on_adapter;
    wgpuInstanceRequestAdapter(s_gpu.instance, &o, acb);
    s_gpu.adapter_pending = 1;

    /* Registered now, not at the end of bring-up: the guest's command stream
     * should be recorded from the first frame, and every slot early-returns on
     * !ready until the device exists. A host that wants to know when the canvas is
     * live polls rsx_webgpu_backend_is_ready(). */
    rsx_set_backend(&s_webgpu_backend);
    printf("[RSX webgpu] init %ux%u, canvas '%s': requesting an adapter\n",
           s_gpu.width, s_gpu.height, s_gpu.selector);
    return 0;
}

void rsx_webgpu_backend_shutdown(void)
{
    webgpu_shutdown(NULL);
}

/* One browser task's worth of device housekeeping.
 *
 * wgpuInstanceProcessEvents is what completes AllowProcessEvents callbacks. It must
 * be called from a task the browser controls (a requestAnimationFrame or a timer),
 * not spun on inside wasm -- and the whole point of the callback-mode choice above
 * is that this function is the only place that happens.
 *
 * The chain is: adapter arrives -> request device; device arrives -> finish
 * bring-up. Each transition happens at most once. */
int rsx_webgpu_backend_pump(void)
{
    if (s_gpu.bringup_failed) return -1;
    if (s_gpu.instance) wgpuInstanceProcessEvents(s_gpu.instance);

    if (!s_gpu.ready) {
        if (s_gpu.adapter && !s_gpu.device && !s_gpu.device_pending) {
            s_gpu.device_pending = 1;
            WGPUDeviceDescriptor dd = WGPU_DEVICE_DESCRIPTOR_INIT;
            dd.uncapturedErrorCallbackInfo.callback = on_error;
            WGPURequestDeviceCallbackInfo dcb = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
            dcb.mode = WGPUCallbackMode_AllowProcessEvents;
            dcb.callback = on_device;
            wgpuAdapterRequestDevice(s_gpu.adapter, &dd, dcb);
        } else if (s_gpu.device && !s_gpu.ready) {
            if (finish_bringup(s_gpu.selector) != 0) {
                s_gpu.bringup_failed = 1;
                return -1;
            }
        }
    }
    return 0;
}

u32 rsx_webgpu_backend_frame_count(void) { return s_gpu.frames; }
u32 rsx_webgpu_backend_surface_format(void) { return (u32)s_gpu.surface_format; }
u32 rsx_webgpu_backend_guest_draws(void) { return 0; }
u32 rsx_webgpu_backend_debug_color(void) { return s_gpu.clear_argb; }
double rsx_webgpu_backend_frame_ms(void) { return s_gpu.frame_ms; }
int rsx_webgpu_backend_is_ready(void) { return s_gpu.ready; }

#else /* !__EMSCRIPTEN__ */

/* ---- native build -------------------------------------------------------
 *
 * Nothing here. CMake globs every C file in libs/video into the runtime library on every
 * platform, so this translation unit is compiled natively too, and the whole point
 * of the deliverable is that it costs the native build nothing: no webgpu.h
 * include, no Emscripten flags, no objects. The entry points are still defined so a
 * host that links against the header (a test that wants to assert the backend is
 * unavailable on native, say) resolves, and they fail honestly rather than
 * pretending.
 * ---------------------------------------------------------------------- */

int  rsx_webgpu_backend_init(u32 width, u32 height, const char* canvas_selector)
{
    (void)width; (void)height; (void)canvas_selector;
    return -1;      /* no WebGPU outside a browser */
}

void rsx_webgpu_backend_shutdown(void) {}

int  rsx_webgpu_backend_pump(void) { return -1; }

u32 rsx_webgpu_backend_frame_count(void) { return 0; }
u32 rsx_webgpu_backend_surface_format(void) { return 0; }
u32 rsx_webgpu_backend_guest_draws(void) { return 0; }
u32 rsx_webgpu_backend_debug_color(void) { return 0; }
double rsx_webgpu_backend_frame_ms(void) { return 0.0; }
int rsx_webgpu_backend_is_ready(void) { return 0; }

#endif /* __EMSCRIPTEN__ */
