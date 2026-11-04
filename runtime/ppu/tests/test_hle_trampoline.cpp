/*
 * ps3recomp - HLE trampoline dispatch test (self-contained).
 *
 * Proves the generated trampoline contract that ppu_hle.cpp's dispatcher relies
 * on: a trampoline has the uniform (const void*, u64 x8) -> u64 signature, calls
 * the handler it is handed through the handler's own prototype, and returns the
 * result in r3 with the same extension the old single-cast dispatch produced.
 *
 * Why this needs its own test: on x86-64 SysV the old cast worked by accident,
 * so nothing here can be caught by "does the title boot" on native alone. The
 * property under test is the one wasm64 checks at the instruction -- the call's
 * type is the callee's type -- and these shapes are exactly the ones the
 * generator emits (tools/gen_hle_nids.py, tools/hle_abi_shape.py).
 *
 * Build: g++ -std=c++17 -I <lift_out> -x c++ test_hle_trampoline.cpp
 *            ../ppu_hle.cpp -o test_hle_trampoline
 *
 * ppu_hle.cpp references the VM store, the guest-call bridge, the PRX export
 * resolver and the milestone log. None of them are exercised here, so they get
 * the smallest stubs that link; that is what keeps this test one translation
 * unit against the dispatcher instead of a whole boot harness. The boot smoke
 * (runtime/ppu/tests/boot_main.cpp) is where those get driven for real.
 */
#include "ppu_recomp.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- link-only stubs for the pieces of ppu_hle.cpp not under test ----
 * The vm_read and vm_write helpers are inline in the generated ppu_recomp.h, so
 * only their vm_base has to be defined here. */
extern "C" {
uint8_t* vm_base = nullptr;
/* ppu_recomp.h #defines these onto inline *_inl wrappers, so the out-of-line
 * definitions its declarations ask for have to be written with the macro out of
 * the way. Nothing here reads or writes the VM, so they are never reached. */
#undef vm_read32
#undef vm_read64
#undef vm_write32
#undef vm_write64
uint32_t vm_read32(uint64_t) { return 0; }
uint64_t vm_read64(uint64_t) { return 0; }
void     vm_write32(uint64_t, uint32_t) {}
void     vm_write64(uint64_t, uint64_t) {}
void     ps3_msf(const char*, ...) {}
uint32_t prx_resolve_export(uint32_t) { return 0; }
void     ps3_indirect_call(ppu_context*) {}
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t) { return 0; }
void     ppu_dump_guest_stack(void) {}
void     ppu_dump_bctrl_ring(void) {}
const char* ppu_guest_callstack(void) { return ""; }
const char* ppu_prof_resolve_host(const void*) { return nullptr; }
void     ppu_prof_stamp(unsigned, unsigned) {}
uint32_t g_ydkj_real_spurs_ea = 0;
uint32_t g_ydkj_real_taskset_ea = 0;
}

extern "C" {
void     ps3_hle_register(uint32_t nid, const char* name, void* handler);
void     ps3_hle_register_trampolined(uint32_t nid, const char* name, void* fn,
                                      uint64_t (*trampoline)(const void*,
                                                             uint64_t, uint64_t,
                                                             uint64_t, uint64_t,
                                                             uint64_t, uint64_t,
                                                             uint64_t, uint64_t));
void     ps3_hle_call(uint32_t nid, ppu_context* ctx);
uint32_t ps3_hle_count(void);
}

/* ---- Handlers with the shapes the generator classifies ---- */

/* (i32, i32) -> i32 : the common Cell status-code shape. */
static int32_t h_add(int32_t a, int32_t b) { return a + b; }

/* (i64) -> i64 : a pointer-in, pointer-out shape. */
static int64_t h_scale(int64_t v) { return v * 3; }

/* (i32, i64, i64) -> void : a handler that reports through an out-parameter. */
static void h_store(int32_t tag, int64_t* out, int64_t v) { if (out) *out = tag + v; }

/* () -> i32 : a no-argument query. */
static int32_t h_answer(void) { return 42; }

/* A negative Cell error code, to pin the zero-extension behaviour: the old
 * dispatcher's cast had a uint64_t return type, so a 32-bit result arrived in
 * r3 with only its low half meaningful. Every title that reads (s32)r3 still
 * sees the right value; a title that compared all 64 bits never worked. */
static int32_t h_error(void) { return (int32_t)0x80010006u; }

/* ---- Trampolines, spelled exactly as the generator emits them ---- */

static uint64_t t_i32_i32__i32(const void* fn, uint64_t a0, uint64_t a1,
                               uint64_t, uint64_t, uint64_t, uint64_t,
                               uint64_t, uint64_t)
{
    return (uint64_t)(uint32_t)((int32_t (*)(int32_t, int32_t))fn)((int32_t)a0, (int32_t)a1);
}
static uint64_t t_i64__i64(const void* fn, uint64_t a0, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    return (uint64_t)((int64_t (*)(int64_t))fn)((int64_t)a0);
}
static uint64_t t_i32_i64_i64__void(const void* fn, uint64_t a0, uint64_t a1,
                                    uint64_t a2, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t)
{
    ((void (*)(int32_t, int64_t, int64_t))fn)((int32_t)a0, (int64_t)a1, (int64_t)a2);
    return 0;
}
static uint64_t t_void__i32(const void* fn, uint64_t, uint64_t, uint64_t,
                            uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    return (uint64_t)(uint32_t)((int32_t (*)(void))fn)();
}

static int fails = 0;
#define CHECK(c, m) do { if (c) printf("[PASS] %s\n", m); \
                         else { printf("[FAIL] %s\n", m); fails++; } } while (0)

/* One dispatch through the NID table with the args a PPC caller would leave in
 * r3..r10, returning whatever landed in r3. */
static uint64_t dispatch(uint32_t nid, uint64_t a3, uint64_t a4, uint64_t a5,
                         uint64_t a6, uint64_t a7, uint64_t a8, uint64_t a9,
                         uint64_t a10)
{
    ppu_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.gpr[3] = a3; ctx.gpr[4] = a4; ctx.gpr[5] = a5; ctx.gpr[6] = a6;
    ctx.gpr[7] = a7; ctx.gpr[8] = a8; ctx.gpr[9] = a9; ctx.gpr[10] = a10;
    ps3_hle_call(nid, &ctx);
    return ctx.gpr[3];
}

int main()
{
    ps3_hle_register_trampolined(0x00000001u, "h_add",   (void*)h_add,   t_i32_i32__i32);
    ps3_hle_register_trampolined(0x00000002u, "h_scale", (void*)h_scale, t_i64__i64);
    ps3_hle_register_trampolined(0x00000003u, "h_store", (void*)h_store, t_i32_i64_i64__void);
    ps3_hle_register_trampolined(0x00000004u, "h_answer",(void*)h_answer,t_void__i32);
    ps3_hle_register_trampolined(0x00000005u, "h_error", (void*)h_error, t_void__i32);
    CHECK(ps3_hle_count() == 5, "five trampolined NIDs registered");

    CHECK(dispatch(0x00000001u, 7, 35, 0, 0, 0, 0, 0, 0) == 42,
          "(i32,i32)->i32: r3/r4 marshalled and the result returned in r3");

    CHECK(dispatch(0x00000002u, (uint64_t)-5, 0, 0, 0, 0, 0, 0, 0) == (uint64_t)-15,
          "(i64)->i64: a negative argument and result survive the 64-bit slot");

    /* (i32, i64, i64)->void, with the middle argument an out-pointer: proves
     * trampolines pass arguments through untouched, including the pointer. */
    int64_t slot = 0;
    dispatch(0x00000003u, 5, (uint64_t)(uintptr_t)&slot, 9, 0, 0, 0, 0, 0);
    CHECK(slot == 14, "h_store wrote through its out-pointer (5 + 9)");
    CHECK(dispatch(0x00000003u, 5, (uint64_t)(uintptr_t)&slot, 9, 0, 0, 0, 0, 0) == 0,
          "(i32,i64,i64)->void returns 0 in r3");

    CHECK(dispatch(0x00000004u, 0, 0, 0, 0, 0, 0, 0, 0) == 42,
          "()->i32 with no arguments");

    /* The extension contract: a 32-bit result occupies the low half of r3 and
     * the high half is zero, matching what the old uint64_t-returning cast did
     * on every native build. */
    CHECK(dispatch(0x00000005u, 0, 0, 0, 0, 0, 0, 0, 0) == 0x0000000080010006ull,
          "(s32)0x80010006 arrives as 0x0000000080010006 in r3 (zero-extended)");

    /* A NID with no trampoline still dispatches through the legacy cast, which
     * is what keeps float-argument and >8-parameter handlers working natively. */
    ps3_hle_register(0x00000006u, "h_add_legacy", (void*)h_add);
    CHECK(ps3_hle_count() == 6, "a plain-registered NID still counts");

    printf("\nResults: %s (%d failure(s))\n", fails ? "FAIL" : "OK", fails);
    return fails ? 1 : 0;
}
