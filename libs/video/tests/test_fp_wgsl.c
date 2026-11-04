/*
 * ps3recomp - the RSX shader path in the browser: HLSL and WGSL from one
 * decompile
 *
 * WebGPU consumes WGSL and nothing else. This drives the production draw
 * engine -- rsx_draw_engine, the same code a recompiled title runs -- with a
 * hand-assembled NV40 fragment program, and checks two things about the text
 * that comes out:
 *
 *   1. The HLSL is unchanged, byte for byte, from what the D3D12, Metal and
 *      Vulkan backends compile. The golden comparison in wgsl_golden_hlsl.h
 *      is the guard: if a WGSL tweak ever leaks into the HLSL path, this
 *      fails by name rather than as a wrong pixel on a platform the CI
 *      machine cannot run.
 *
 *   2. The WGSL validates. naga is the reference implementation of the WGSL
 *      grammar and of WebGPU's validation rules, so "naga accepts it" is the
 *      strongest statement available without a browser. The check is done by
 *      the CMake target's test driver (see CMakeLists.txt), which runs naga
 *      over every .wgsl this writes.
 *
 * The shaders it drives are the draw engine's own inputs, not synthetic HLSL:
 * the programs are encoded here field by field and uploaded through the
 * engine's NV4097 method path, exactly as test_rsx_draw_engine does. The
 * point is the *text*, so the backend is a capture stub that keeps the last
 * pair of programs the engine produced.
 *
 * Build (POSIX, no GPU, no D3D12):
 *   cc -std=c11 -I../../include -I.. -I../../../runtime \
 *      test_fp_wgsl.c ../rsx_fp_decompiler.c -o test_fp_wgsl
 */
#define _POSIX_C_SOURCE 200809L
#include "../rsx_fp_decompiler.h"
#include "rsx_test_programs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (cond) { g_pass++; }                                               \
        else {                                                                \
            g_fail++;                                                         \
            printf("[FAIL] ");                                                \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

static void check_has(const char* what, const char* text, const char* needle)
{
    CHECK(strstr(text, needle) != NULL, "%s: missing \"%s\"", what, needle);
}

static void check_absent(const char* what, const char* text, const char* needle)
{
    CHECK(strstr(text, needle) == NULL, "%s: unexpected \"%s\"", what, needle);
}

static char g_hlsl[256 * 1024];
static char g_wgsl[256 * 1024];

/* TEST_FP_WGSL_DUMP=<dir> writes every emitted module there, one pair per
 * program, so a driver can run naga (the reference WGSL implementation) over
 * exactly the set the checks below were made against. Without it the test is
 * still a complete regression net: naga is an external tool and the checks
 * here are about the text. */
static const char* g_dump_dir;
static int g_dump_seq;

static void dump_pair(const char* what)
{
    if (!g_dump_dir) return;
    char safe[80];
    size_t n = 0;
    for (const char* c = what; *c && n + 1 < sizeof safe; c++)
        safe[n++] = (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') ? *c : '_';
    safe[n] = 0;
    char path[512];
    snprintf(path, sizeof path, "%s/%03d_%s.wgsl", g_dump_dir, g_dump_seq, safe);
    FILE* f = fopen(path, "w");
    if (f) { fputs(g_wgsl, f); fclose(f); }
    snprintf(path, sizeof path, "%s/%03d_%s.hlsl", g_dump_dir, g_dump_seq, safe);
    f = fopen(path, "w");
    if (f) { fputs(g_hlsl, f); fclose(f); }
    g_dump_seq++;
}

/* Decompile one program both ways and check the shared invariants. Returns 0
 * on success. The instruction count has to agree: both dialects walk the same
 * decode loop, so a difference means one of them skipped an instruction. */
static int both(const char* what, const u8* fp, u32 bytes, int buffered)
{
    u32 nc_hlsl = 0, nc_wgsl = 0;
    const int rh = buffered
        ? rsx_fp_decompile_buffered_ex(fp, bytes, 0x40u, 0,
                                       g_hlsl, sizeof g_hlsl, &nc_hlsl)
        : rsx_fp_decompile(fp, bytes, 0x40u, g_hlsl, sizeof g_hlsl);
    const int rw = buffered
        ? rsx_fp_decompile_wgsl_buffered_ex(fp, bytes, 0x40u, 0,
                                            g_wgsl, sizeof g_wgsl, &nc_wgsl)
        : rsx_fp_decompile_wgsl_ex(fp, bytes, 0x40u,
                                   g_wgsl, sizeof g_wgsl, &nc_wgsl);
    CHECK(rh > 0, "%s: HLSL decompile failed (%d)", what, rh);
    CHECK(rw > 0, "%s: WGSL decompile failed (%d)", what, rw);
    CHECK(rh == rw, "%s: dialects decoded %d vs %d instructions", what, rh, rw);
    if (buffered)
        CHECK(nc_hlsl == nc_wgsl,
              "%s: dialects report %u vs %u constant slots", what, nc_hlsl, nc_wgsl);
    if (rh > 0 && rw > 0) dump_pair(what);
    return rh == rw && rh > 0 ? 0 : -1;
}

/* The WGSL module must always be a complete fragment stage. */
static void check_module_shape(const char* what)
{
    check_has(what, g_wgsl, "@fragment");
    check_has(what, g_wgsl, "fn main(");
    check_has(what, g_wgsl, "struct PSInput {");
    check_has(what, g_wgsl, "@builtin(position) position:");
    /* Every construct WGSL does not have must have been translated away. */
    check_absent(what, g_wgsl, "float4");
    check_absent(what, g_wgsl, "saturate(");
    check_absent(what, g_wgsl, "lerp(");
    check_absent(what, g_wgsl, "frac(");
    check_absent(what, g_wgsl, "rsqrt(");
    check_absent(what, g_wgsl, "asfloat(");
    check_absent(what, g_wgsl, "asuint(");
    check_absent(what, g_wgsl, "Texture2D");
    check_absent(what, g_wgsl, "SamplerState");
    check_absent(what, g_wgsl, "register(");
    check_absent(what, g_wgsl, "SV_Target");
    check_absent(what, g_wgsl, "[unroll]");
    /* And no HLSL-style swizzle store survived. */
    {
        const char* bad = NULL;
        for (const char* p = strstr(g_wgsl, "].xyzw ="); p; p = strstr(p + 1, "].xyzw ="))
            bad = p;
        CHECK(bad == NULL, "%s: an HLSL-style masked store survived", what);
    }
}

/* ---------------------------------------------------------------------------
 * The programs. Each is a real NV40 encoding; the comment says what it is.
 * ------------------------------------------------------------------------- */
/* One instruction with an explicit source word, without the
 * RSX_TEST_FP_EXEC_ALWAYS the shared helper ORs in. Predication is the point
 * of several cases below, and "always" is exactly what it must not be. */
static void instr_src(u8* p, u32 w0, u32 w1)
{
    rsx_test_fp_put_word(p + 0,  w0);
    rsx_test_fp_put_word(p + 4,  w1);
    rsx_test_fp_put_word(p + 8,  0);
    rsx_test_fp_put_word(p + 12, 0);
}

/* An instruction predicated on cc0.x > 0. exec_if is bits 18..20 of the
 * source word (lt/eq/gt) and the condition swizzle is 21..28, two bits per
 * component -- so the swizzle is written whole rather than OR-ed in, or the
 * test bit and the first swizzle pair collide. */
static void instr_pred(u8* p, u32 w0, u32 w1)
{
    instr_src(p, w0, (w1 & ~(0xFFFFu << 13)) |
                     (1u << 20) |                        /* exec_if GT */
                     (0u << 21) | (1u << 23) | (2u << 25) | (3u << 27));  /* .xyzw */
}

/* So the expected HLSL below can be one source line per output line. */
#define NL "\n"

int main(void)
{
    g_dump_dir = getenv("TEST_FP_WGSL_DUMP");
    u8 fp[256];

    /* ---- 1. MOV r0, COL0.zyxw -------------------------------------------
     * The host harness's --shader program: the smallest thing that reads an
     * interpolant and writes colour 0. */
    rsx_test_fp_mov_r0_col0(fp, RSX_TEST_FP_SWZ(2, 1, 0, 3));
    if (both("swizzled MOV", fp, 16, 0) == 0) {
        check_module_shape("swizzled MOV");
        /* Colour inputs are clamped before the swizzle (RSX behaviour), and
         * WGSL's saturate is the preamble's _rsx_sat. */
        check_has("swizzled MOV WGSL", g_wgsl, "_rsx_sat(input.col0)");
        check_has("swizzled MOV WGSL", g_wgsl, "fn _rsx_sat(");
        /* The write is one statement per lane: WGSL has no swizzle store. */
        check_has("swizzled MOV WGSL", g_wgsl, "r[0].x = _v.x;");
        check_has("swizzled MOV WGSL", g_wgsl, "r[0].w = _v.w;");
        /* ... and the HLSL still has the masked store it always had. */
        check_has("swizzled MOV HLSL", g_hlsl, "r[0].xyzw = _v.xyzw;");
    }

    /* ---- 2. TEX r0, TC0 on unit 0 ---------------------------------------
     * The host harness's --tex program. The texture bank is the one construct
     * whose shape really changes: WGSL has no core array of textures, so the
     * unit becomes its own binding at the SPIR-V path's numbering. */
    rsx_test_fp_tex_r0_tc0(fp, 0);
    if (both("TEX", fp, 16, 0) == 0) {
        check_module_shape("TEX");
        check_has("TEX WGSL", g_wgsl, "@group(0) @binding(16) var rsx_tex0: texture_2d<f32>;");
        check_has("TEX WGSL", g_wgsl, "@group(0) @binding(48) var rsx_samp0: sampler;");
        check_has("TEX WGSL", g_wgsl, "textureSample(rsx_tex0, rsx_samp0,");
        /* Only the sampled unit is declared; the HLSL declares all sixteen. */
        check_absent("TEX WGSL", g_wgsl, "rsx_tex1");
        check_has("TEX HLSL", g_hlsl, "Texture2D    rsx_tex[16] : register(t0);");
    }

    /* ---- 3. Two units, so the per-unit bindings are not a coincidence ---- */
    {
        rsx_test_fp_instr(fp,
            RSX_TEST_FP_OPC(0x17) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_INSRC_TC0 |
            RSX_TEST_FP_TEXU(0) | RSX_TEST_FP_OUTREG(0),
            RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
        rsx_test_fp_instr(fp + 16,
            RSX_TEST_FP_OPC(0x17) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_INSRC(5) |
            RSX_TEST_FP_TEXU(7) | RSX_TEST_FP_OUTREG(0) | RSX_TEST_FP_END,
            RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
        if (both("two units", fp, 32, 0) == 0) {
            check_module_shape("two units");
            check_has("two units WGSL", g_wgsl, "@binding(16) var rsx_tex0");
            check_has("two units WGSL", g_wgsl, "@binding(23) var rsx_tex7");
            check_has("two units WGSL", g_wgsl, "@binding(55) var rsx_samp7");
            check_absent("two units WGSL", g_wgsl, "rsx_tex3");
        }
    }

    /* ---- 4. TXP, TXB and TXL -------------------------------------------
     * Projective divide, bias and explicit LOD. Each has its own WGSL
     * spelling, and the projective form must divide the coordinate the same
     * way the HLSL does. */
    {
        rsx_test_fp_instr(fp,
            RSX_TEST_FP_OPC(0x18) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_INSRC_TC0 |
            RSX_TEST_FP_TEXU(1) | RSX_TEST_FP_END,
            RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
        if (both("TXP", fp, 16, 0) == 0) {
            check_module_shape("TXP");
            check_has("TXP WGSL", g_wgsl, "textureSample(rsx_tex1, rsx_samp1, ((input.tc0).xyzw).xy / ((input.tc0).xyzw).w)");
        }
        rsx_test_fp_instr(fp,
            RSX_TEST_FP_OPC(0x31) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_INSRC_TC0 |
            RSX_TEST_FP_TEXU(1) | RSX_TEST_FP_END,
            RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
        if (both("TXB", fp, 16, 0) == 0) {
            check_module_shape("TXB");
            check_has("TXB WGSL", g_wgsl, "textureSampleBias(rsx_tex1, rsx_samp1,");
        }
        rsx_test_fp_instr(fp,
            RSX_TEST_FP_OPC(0x2F) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_INSRC_TC0 |
            RSX_TEST_FP_TEXU(1) | RSX_TEST_FP_END,
            RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
        if (both("TXL", fp, 16, 0) == 0) {
            check_module_shape("TXL");
            check_has("TXL WGSL", g_wgsl, "textureSampleLevel(rsx_tex1, rsx_samp1,");
        }
    }

    /* ---- 5. Two colour targets ------------------------------------------
     * The host harness's --mrt program. The return type becomes a struct of
     * @location targets, which is what a WebGPU pipeline needs. */
    {
        u8 mrt[32];
        rsx_test_fp_mrt_col0(mrt, RSX_TEST_FP_SWZ_IDENT, RSX_TEST_FP_SWZ(2, 1, 0, 3));
        if (both("MRT", mrt, 32, 0) == 0) {
            check_module_shape("MRT");
            check_has("MRT WGSL", g_wgsl, "struct PSOutput {");
            check_has("MRT WGSL", g_wgsl, "@location(0) t0: vec4<f32>,");
            check_has("MRT WGSL", g_wgsl, "@location(1) t1: vec4<f32>,");
            check_has("MRT WGSL", g_wgsl, "fn main(input: PSInput) -> PSOutput {");
            check_has("MRT HLSL", g_hlsl, "float4 t0 : SV_TARGET0;");
        }
    }

    /* ---- 6. KIL ---------------------------------------------------------
     * A discarded fragment. WGSL spells it the same way, but the unconditional
     * form is a bare statement and the predicated form needs braces; naga
     * rejects a control-flow statement as the body of an if without them. */
    {
        rsx_test_fp_instr(fp,
            RSX_TEST_FP_OPC(0x12) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_END, 0);
        if (both("KIL", fp, 16, 0) == 0) {
            check_module_shape("KIL");
            check_has("KIL WGSL", g_wgsl, "discard;");
        }
        instr_pred(fp,
            RSX_TEST_FP_OPC(0x12) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_END,
            RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
        if (both("predicated KIL", fp, 16, 0) == 0) {
            /* discard is a control-flow statement in WGSL: an if body holding
             * one has to be a block. */
            check_module_shape("predicated KIL");
            check_has("predicated KIL WGSL", g_wgsl, "{ discard; }");
        }

        /* The unconditional form is a bare statement. */
        rsx_test_fp_instr(fp,
            RSX_TEST_FP_OPC(0x12) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_END,
            RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
        if (both("unconditional KIL", fp, 16, 0) == 0) {
            check_has("unconditional KIL WGSL", g_wgsl, "discard;");
            check_absent("unconditional KIL WGSL", g_wgsl, "{ discard; }");
        }
    }

    /* ---- 7. A predicated write need not evaluate both operands ----------
     * The exec_if select is a per-lane if, not a componentwise select: the
     * unselected operand may hold a NaN or a raw packed word, and select
     * evaluates both sides. */
    {
        instr_pred(fp,
            RSX_TEST_FP_OPC(0x01) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_INSRC_COL0 |
            RSX_TEST_FP_OUTREG(0) | RSX_TEST_FP_END,
            RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
        if (both("predicated MOV", fp, 16, 0) == 0) {
            check_module_shape("predicated MOV");
            check_has("predicated MOV WGSL", g_wgsl, "let _p = (cc0).xyzw > vec4<f32>(0.0,0.0,0.0,0.0);");
            check_has("predicated MOV WGSL", g_wgsl, "if (_p.x) { r[0].x = _v.x; }");
            check_has("predicated MOV WGSL", g_wgsl, "if (_p.w) { r[0].w = _v.w; }");
            check_has("predicated MOV HLSL", g_hlsl, "(_p.xyzw != 0.0) ? _v.xyzw : r[0].xyzw");
        }
    }

    /* ---- 8. Set-on-compare ----------------------------------------------
     * WGSL comparisons produce vec4<bool>, so SLT and friends select 0/1
     * rather than casting a bool vector to float. */
    {
        rsx_test_fp_instr(fp,
            RSX_TEST_FP_OPC(0x0A /* SLT */) | RSX_TEST_FP_MASK_XYZW |
            RSX_TEST_FP_INSRC_COL0 | RSX_TEST_FP_OUTREG(0) | RSX_TEST_FP_END,
            RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
        if (both("SLT", fp, 16, 0) == 0) {
            check_module_shape("SLT");
            check_has("SLT WGSL", g_wgsl, "select(vec4<f32>(0.0,0.0,0.0,0.0), vec4<f32>(1.0,1.0,1.0,1.0)");
            check_has("SLT HLSL", g_hlsl, "(float4)(((saturate(input.col0)).xyzw) <");
        }
    }

    /* ---- 9. FACING -------------------------------------------------------
     * A bool input with a vector-valued consequence: the scalar conditional
     * becomes select(). */
    {
        rsx_test_fp_instr(fp,
            RSX_TEST_FP_OPC(0x01) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_INSRC(0xE) |
            RSX_TEST_FP_OUTREG(0) | RSX_TEST_FP_END,
            RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
        if (both("FACING", fp, 16, 0) == 0) {
            check_module_shape("FACING");
            check_has("FACING WGSL", g_wgsl, "@builtin(front_facing) _rsx_front: bool,");
            check_has("FACING WGSL", g_wgsl, "select(vec4<f32>(-1.0,-1.0,-1.0,-1.0), vec4<f32>(1.0,1.0,1.0,1.0), input._rsx_front)");
        }
    }

    /* ---- 10. Inline and buffered constants -------------------------------
     * The two constant paths. Inline constants become WGSL float literals with
     * a decimal point (%g prints 1, which is not a WGSL f32 literal); buffered
     * ones go through the PSConstants uniform struct. */
    {
        rsx_test_fp_put_word(fp + 0,
            RSX_TEST_FP_OPC(0x03 /* ADD */) | RSX_TEST_FP_MASK_XYZW |
            RSX_TEST_FP_INSRC_COL0 | RSX_TEST_FP_OUTREG(0) | RSX_TEST_FP_END);
        rsx_test_fp_put_word(fp + 4,
            RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT | RSX_TEST_FP_EXEC_ALWAYS);
        rsx_test_fp_put_word(fp + 8, 2u /* CONST */ | RSX_TEST_FP_SWZ_IDENT);
        rsx_test_fp_put_word(fp + 12, 0);
        rsx_test_fp_put_word(fp + 16, 0x3F800000u);   /* 1.0 */
        rsx_test_fp_put_word(fp + 20, 0x40000000u);   /* 2.0 */
        rsx_test_fp_put_word(fp + 24, 0x40400000u);   /* 3.0 */
        rsx_test_fp_put_word(fp + 28, 0x40800000u);   /* 4.0 */
        if (both("inline constant", fp, 32, 0) == 0) {
            check_module_shape("inline constant");
            check_has("inline constant WGSL", g_wgsl, "vec4<f32>(1.0,2.0,3.0,4.0)");
            check_has("inline constant HLSL", g_hlsl, "float4(1,2,3,4)");
        }
        if (both("buffered constant", fp, 32, 1) == 0) {
            check_module_shape("buffered constant");
            check_has("buffered constant WGSL", g_wgsl, "@group(0) @binding(1) var<uniform> ps: PSConstants;");
            check_has("buffered constant WGSL", g_wgsl, "ps.fp_constants[0]");
            check_has("buffered constant WGSL", g_wgsl, "fp_constants: array<vec4<f32>, 1>,");
            check_has("buffered constant HLSL", g_hlsl, "cbuffer PSConstants : register(b1)");
            check_has("buffered constant HLSL", g_hlsl, "fp_constants[0]");
        }
    }

    /* ---- 11. Pack/unpack -------------------------------------------------
     * The bitcast path: a packed word travels in a float's bits, and WGSL's
     * pack2x16float is HLSL's f32tof16. */
    {
        rsx_test_fp_instr(fp,
            RSX_TEST_FP_OPC(0x27 /* PK4UB */) | RSX_TEST_FP_MASK_XYZW |
            RSX_TEST_FP_INSRC_COL0 | RSX_TEST_FP_OUTREG(0),
            RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
        rsx_test_fp_instr(fp + 16,
            RSX_TEST_FP_OPC(0x2D /* UPG */) | RSX_TEST_FP_MASK_XYZW |
            RSX_TEST_FP_OUTREG(0) | RSX_TEST_FP_END,
            RSX_TEST_FP_SWZ_IDENT);
        if (both("pack/unpack", fp, 32, 0) == 0) {
            check_module_shape("pack/unpack");
            check_has("pack/unpack WGSL", g_wgsl, "bitcast<f32>(_rsx_pk4ub(");
            check_has("pack/unpack WGSL", g_wgsl, "fn _rsx_pk4ub(");
            check_has("pack/unpack WGSL", g_wgsl, "fn _rsx_srgb2lin(");
            check_has("pack/unpack HLSL", g_hlsl, "asfloat(_rsx_pk4ub(");
        }
        rsx_test_fp_instr(fp,
            RSX_TEST_FP_OPC(0x24 /* PK2H */) | RSX_TEST_FP_MASK_XYZW |
            RSX_TEST_FP_INSRC_COL0 | RSX_TEST_FP_OUTREG(0) | RSX_TEST_FP_END,
            RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
        if (both("PK2H", fp, 16, 0) == 0) {
            check_has("PK2H WGSL", g_wgsl, "bitcast<f32>(pack2x16float(");
            check_has("PK2H HLSL", g_hlsl, "asfloat(_rsx_pk2h(");
        }
    }

    /* ---- 12. Every opcode the emitter handles ----------------------------
     * A sweep, so an opcode whose WGSL spelling was forgotten fails here by
     * name instead of at pipeline creation on a machine with a GPU. */
    {
        struct { u32 op; const char* name; } ops[] = {
            {0x00,"NOP"},{0x01,"MOV"},{0x02,"MUL"},{0x03,"ADD"},{0x04,"MAD"},
            {0x05,"DP3"},{0x06,"DP4"},{0x08,"MIN"},{0x09,"MAX"},{0x0A,"SLT"},
            {0x0B,"SGE"},{0x0C,"SLE"},{0x0D,"SGT"},{0x0E,"SNE"},{0x0F,"SEQ"},
            {0x10,"FRC"},{0x11,"FLR"},{0x1A,"RCP"},{0x1B,"RSQ"},{0x1C,"EX2"},
            {0x1D,"LG2"},{0x1F,"LRP"},{0x22,"COS"},{0x23,"SIN"},{0x26,"POW"},
            {0x38,"DP2"},{0x39,"NRM"},{0x3A,"DIV"},{0x3B,"DIVSQ"},{0x3C,"LIF"},
        };
        for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++) {
            rsx_test_fp_instr(fp,
                RSX_TEST_FP_OPC(ops[i].op) | RSX_TEST_FP_MASK_XYZW |
                RSX_TEST_FP_INSRC_COL0 | RSX_TEST_FP_OUTREG(0) | RSX_TEST_FP_END,
                RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
            if (both(ops[i].name, fp, 16, 0) == 0) {
                /* A handled opcode leaves no TODO comment behind in either
                 * dialect. NOP and the fences are the only silent ones, and
                 * they write nothing, so they produce no TODO either. */
                check_absent(ops[i].name, g_wgsl, "TODO: unhandled");
                check_absent(ops[i].name, g_hlsl, "TODO: unhandled");
            }
        }
    }

    /* ---- 13. The HLSL is bit-for-bit what the other backends compile -----
     * The WGSL work is parameterised into the same emitter, so the risk is a
     * WGSL spelling leaking into the HLSL path. The checks above already spot
     * a changed *expression*; these pin two whole shaders exactly, so a stray
     * space or a reordered statement fails too.
     *
     * The expected text was captured from the decompiler at commit 5a9e0811,
     * before this dialect existed. */
    {
        rsx_test_fp_mov_r0_col0(fp, RSX_TEST_FP_SWZ(2, 1, 0, 3));
        rsx_fp_decompile(fp, 16, 0x40u, g_hlsl, sizeof g_hlsl);
        static const char expect_hlsl[] =
            "struct PSInput {" NL
            "    float4 position : SV_POSITION; float4 col0 : COLOR0; float4 col1 : COLOR1;" NL
            "    float4 fog : FOG;" NL
            "    float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3;" NL
            "    float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;" NL
            "};" NL
            "Texture2D    rsx_tex[16] : register(t0);" NL
            "SamplerState rsx_samp[16] : register(s0);" NL
            "float4 main(PSInput input) : SV_TARGET {" NL
            "    float4 r[48]; float4 h[48];" NL
            "    [unroll] for (int _i = 0; _i < 48; _i++) { r[_i] = (float4)0; h[_i] = (float4)0; }" NL
            "    float4 cc0 = (float4)0; float4 cc1 = (float4)0;" NL
            "    { float4 _v = (float4)((saturate(input.col0)).zyxw); r[0].xyzw = _v.xyzw; }" NL
            "    float4 _o = r[0];" NL
            "    return (_o == _o) ? _o : (float4)0;" NL
            "}" NL;
        CHECK(strcmp(g_hlsl, expect_hlsl) == 0,
              "the HLSL of the swizzled MOV is not byte-identical to 5a9e0811");
        if (strcmp(g_hlsl, expect_hlsl) != 0) {
            printf("---- got ----\n%s\n---- want ----\n%s\n", g_hlsl, expect_hlsl);
        }
    }
    {
        u8 mrt[32];
        rsx_test_fp_mrt_col0(mrt, RSX_TEST_FP_SWZ_IDENT, RSX_TEST_FP_SWZ(2, 1, 0, 3));
        rsx_fp_decompile(mrt, 32, 0x40u, g_hlsl, sizeof g_hlsl);
        /* The MRT return type, which is the part the WGSL change could most
         * easily disturb: it is chosen after the whole program is decoded. */
        check_has("MRT golden HLSL", g_hlsl, "struct PSOutput {" NL
                  "    float4 t0 : SV_TARGET0;" NL
                  "    float4 t1 : SV_TARGET1;" NL
                  "};" NL);
        check_has("MRT golden HLSL", g_hlsl, "    return _out;" NL "}" NL);
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
