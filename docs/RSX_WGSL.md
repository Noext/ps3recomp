# RSX shaders on WebGPU: WGSL

WebGPU consumes **WGSL** and nothing else. It has no HLSL front end and no
SPIR-V front end either, so the chain the Vulkan backend uses
(`rsx_fp_decompiler.c` -> HLSL -> glslang -> SPIR-V) does not reach a browser
without also shipping glslang and naga as WebAssembly. This document records
the decision, the reasoning, and the emit contract a WebGPU backend binds to.

## The decision

**`rsx_fp_decompiler.c` emits WGSL directly, as a second output dialect of
the existing decode loop.** It does not compile anything at run time.

The three candidates, and what decided against the other two:

**(a) Build-time translation, baked into the binary.** Needs the shader set
ahead of time. The decompiler's whole design is the opposite: it is handed
NV40 ucode at run time, from whatever the title happens to draw, and
`rsx_fp_decompile_buffered_shadow_color_expand_vol_ex` decides the shader's
text from the program *and* the draw's texture metadata (gamma lanes, sign
expansion, depth comparison, cube/volume dimensionality). Every one of those
inputs is per-draw state. A baked set would miss any program a title reaches
after the point the capture stopped, and would have to be regenerated per
game. That is not a shader path; it is an offline test fixture.

**(b) glslang + naga both compiled to WebAssembly, run in the page.** This
works in principle, and it is what the question suggested. It costs,
measured on this machine with the published builds and a from-source naga:

| module | what it does | raw | gzip |
|---|---|---|---|
| `@webgpu/glslang` 0.0.15 web-min-nocompute | GLSL -> SPIR-V | 454 KB | 172 KB |
| naga-cli 30.0.1 (spv-in + wgsl-out, `-Oz` + LTO) | SPIR-V -> WGSL | 509 KB | 161 KB |
| **total** | | **963 KB** | **333 KB** |

and the glslang build is the problem beyond the size: the published
`@webgpu/glslang` export only exposes `compileGLSL`, i.e. a *GLSL* entry
point. Its wasm carries no HLSL front end -- there is not one occurrence of
`SV_Target`, `Texture2D`, `SamplerState` or `cbuffer` in the binary, and the
`HLSL` string that does appear is in the parser's error text. Feeding it HLSL
returns a compile error, and the module never finishes initialising under
Node in this environment. Getting an HLSL-capable glslang into a page means
building glslang's HLSL front end with Emscripten yourself -- a much larger
artifact than the GLSL-only one, plus a Rust toolchain for naga, plus JS
glue, in exchange for re-deriving at run time what this repository already
has as C.

Then the recursive part: this tree *already contains* the HLSL emitter.
Option (b) ships a compiler to avoid writing a second emitter; option (c)
writes the second emitter and ships no compiler. The emitter is 1862 lines of
`snprintf`; the decode loop, the register model, the flow-control
reconstruction and every semantic decision are already written and tested,
and only the *text* differs.

## What the HLSL actually uses

Measured by decompiling a corpus of hand-assembled NV40 programs covering
every opcode the emitter handles -- the corpus `tests/test_fp_wgsl.c` walks.
The constructs, and how each lands in WGSL:

| HLSL | where | WGSL | notes |
|---|---|---|---|
| `float4(x)` / `(float4)x` | everywhere | `vec4<f32>(x)` | pure spelling |
| `saturate(v)` | colour inputs, `OUT_SAT`, srgb, depth | `_rsx_sat(v)` | `clamp(v, 0, 1)` |
| vector `?:` | predicated writes, `DIVSQ`, `LIF`, `FACING`, NaN guards | `select(f, t, c)` | WGSL's `?:` takes a scalar condition |
| `(float4)(a < b)` | SLT/SGE/SLE/SGT/SNE/SEQ | `select(vec4(0), vec4(1), a < b)` | comparisons yield `vec4<bool>` |
| `dst.xyzw = v.xyzw` | every destination write | one statement per lane | **WGSL has no swizzle assignment** |
| `lerp` / `frac` / `rsqrt` | LRP, FRC, RSQ | `mix` / `fract` / `inverseSqrt` | renamed intrinsics |
| `asfloat` / `asuint` | PK*/UP* | `bitcast<f32>` / `bitcast<u32>` | |
| `f32tof16` / `f16tof32` | PK2H/UP2H | `pack2x16float` / `unpack2x16float` | core WGSL |
| `Sample` / `SampleBias` / `SampleLevel` | TEX/TXP/TXB/TXL | `textureSample*` | sampler is the 2nd argument |
| `[unroll] for` zero init | once per shader | *nothing* | a WGSL `var` starts zeroed |
| `Texture2D rsx_tex[16]`, `SamplerState rsx_samp[16]` | once per shader | one binding per **used** unit | **the one structural change** |
| `register(tN/sN/bN)` | once per shader | `@group(0) @binding(N)` | the same numbers as the Vulkan path |
| `cbuffer PSConstants` | buffered mode | `struct PSConstants` + `var<uniform>` | members via `ps.` |
| `[loop]` / `discard` / `break` | LOOP/REP/BRK, KIL | the same; `discard` needs a block | |
| `SV_POSITION` / `SV_TARGETn` / `SV_Depth` / `SV_IsFrontFace` | entry point | `@builtin(position)` / `@location(n)` / `@builtin(frag_depth)` / `@builtin(front_facing)` | |

Nothing in the list is awkward, and nothing needs a WGSL extension. The two
items that are not a rename are the swizzle store and the texture bank, and
both are handled by the emitter rather than by a compiler:

- **Swizzle store.** WGSL rejects swizzle assignment outright ("WGSL does
  not support assignments to swizzles"), so the emitter writes one
  statement per masked lane. The value is bound to a `var _v` first, so a
  texture fetch on the right-hand side still runs exactly once.
- **Texture bank.** WGSL has no core array-of-textures; the SPIR-V path gets
  `binding_array<texture_2d<f32>>` only through the `wgpu_binding_array` extension,
  which naga enables for SPIR-V input but which a WebGPU pipeline cannot bind
  through a default layout. So each *used* unit is its own binding. That is
  a backend-visible contract, recorded in `rsx_fp_decompiler.h`:
  `tN -> binding 16+N`, `sN -> binding 48+N`, `bN -> binding N` -- the same
  numbers `rsx_shader_spirv.h` uses, so one table describes a Vulkan
  descriptor set and a WebGPU bind group.

HLSL features the emitter uses that WGSL has no answer for: none were found
in the emitted subset. `asuint` on a scalar and the `f32tof16` pair were the
closest; both have core WGSL equivalents.

## The API

`libs/video/rsx_fp_decompiler.h`:

```c
int rsx_fp_decompile_wgsl_buffered_ex(   /* the one a backend uses */
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask,
    char* out, u32 out_size, u32* out_constant_count);

int rsx_fp_decompile_wgsl_ex(            /* inline constants as literals */
    const u8* ucode, u32 max_bytes, u32 ctrl,
    char* out, u32 out_size, u32* out_constant_count);

int rsx_fp_decompile_wgsl(               /* full metadata variant */
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask, u32 tex_vol_mask,
    const rsx_fp_shadow_desc shadow[16], const u8 gamma_masks[16],
    const u8 expand_masks[16],
    char* out, u32 out_size, u32* out_constant_count);
```

The output is a complete, self-contained WGSL module: the `PSInput` struct,
whatever helper functions the program needs (`_rsx_sat`, `_rsx_inv_w`,
`_rsx_wpos`, the shadow comparison, gamma, sign-expansion and pack helpers),
the bindings, and `@fragment fn main`. The entry point returns either
`vec4<f32>` (one colour target) or a `PSOutput` struct of `@location`
targets plus `@builtin(frag_depth)`.

## How the HLSL stays unchanged

The dialect is a parameter of `rsx_fp_decompile_internal`, and every WGSL
spelling lives in its own `switch` arm. The HLSL arm is textually the same
code it was. `tests/test_fp_wgsl.c` guards that two ways:

- It compares the HLSL of the swizzled-MOV program against the exact text the
  decompiler produced at commit `5a9e0811`, byte for byte -- so even a
  space change fails by name.
- It asserts the MRT return type is unchanged, which is the part whose shape
  is decided after the whole program is decoded.

Verified out of band as well: 98 shader programs covering every handled opcode
decompile to HLSL byte-identical to the pre-change tree.

## Validation

`naga` is the reference WGSL implementation and the front end wgpu (and
therefore every browser WebGPU implementation) is built on, so "naga accepts
it" is the strongest statement available without a GPU:

```
$ python3 tools/validate_fp_wgsl.py --test build/test_fp_wgsl --naga /root/.cargo/bin/naga
507 passed, 0 failed
naga: 47 module(s) validated, 0 rejected
```

The test binary decompiles 47 distinct programs (every opcode, plus texture,
MRT, depth, predication, pack/unpack, FACING and both constant paths) and
writes their WGSL; the script runs naga over every one. It is registered as
the `fp_wgsl_naga` CMake test when naga is present and simply does not exist
when it is not -- the C-level checks are the part that always runs.

## Binary size for the browser

The decompiler itself is the whole cost, and it is already in the build:

| | raw | gzip |
|---|---|---|
| `rsx_fp_decompiler.o` `.text` (`-O2`, x86-64) | 48.3 KB | -- |

Its wasm64 size is the same order. Against option (b)'s 963 KB raw / 333 KB
gzip of *added* compiler, plus the Rust and Emscripten build dependencies, the
chosen shape adds no download at all: WGSL emission is more format strings in
a translation unit the runtime already links.

## Not done here

The vertex decompiler (`rsx_vp_decompiler.c`) still emits HLSL only. It is
741 lines and its HLSL uses the same constructs, so the same treatment
applies; it is a separate, smaller piece of work. The WebGPU *backend* itself
is another agent's scope and consumes the WGSL this emits, needing the binding
numbers above plus the canvas configuration.
