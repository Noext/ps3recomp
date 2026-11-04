# HLE dispatch: the trampoline, and why wasm64 forces it

Every firmware import a recompiled title calls arrives in the HLE dispatcher as
a NID plus the eight PPC64 integer argument registers. Until this change the
dispatcher reached the handler behind that NID through a single cast:

```cpp
typedef uint64_t (*hle_generic)(uint64_t x8);
hle_generic fn = (hle_generic)e->handler;      /* runtime/ppu/ppu_hle.cpp */
ctx->gpr[3] = fn(gpr[3], ..., gpr[10]);
```

That is an *indirect* call through a signature the handler does not have.
Handlers in `libs/` declare what they really take -- `s32 cellFsClose(CellFsFd)`,
`void* cellMemalign(u32, u32)`, `s32 cellSpuThreadGroupCreate(u32, u32, u32,
u64)` -- and the cast lies about every one of them.

On x86-64 SysV the lie is harmless, which is why it survived: six register
arguments are spilled, a callee that reads two of them never touches the other
six, and caller and callee agree that the return value is in RAX because every
result the Cell ABI returns fits there. Under wasm64 **an indirect call is
checked against the callee's own function type at the instruction**, so the
first HLE import a title reaches traps:

```
RuntimeError: null function or function signature mismatch
```

Measured on this branch before the fix: with the `cellFiber` blocker stubbed
out, the boot smoke reached 5 of 11 markers and died at the first HLE import
through `_cellGcmInitBody`. Reproduce that by generating the table with the
trampolines removed (`ps3_hle_register` rows instead of
`ps3_hle_register_trampolined`) and running the wasm smoke: it stops at marker
6 with the message above.

## The fix

`tools/gen_hle_nids.py` no longer registers handlers directly. It reads each
handler's prototype out of the library source and emits a **trampoline** with
the uniform dispatcher signature:

```cpp
uint64_t hle_thunk_i32_i64_i64__void(const void* fn, uint64_t a0, ..., uint64_t a7)
{
    ((void (*)(int32_t, int64_t, int64_t))fn)((int32_t)a0, (int64_t)a1, (int64_t)a2);
    return 0;
}
```

The dispatcher calls the trampoline (one function-pointer type for the whole
table, so that call always type-checks), and the trampoline calls the handler
through a function-pointer type derived from the handler's own prototype. The
two types match by construction, which is what wasm64 checks.

Trampolines are shared: the handler's address is the trampoline's first
argument, so the 1196 fixed-arity handlers among the 1215 registered collapse
onto **98 distinct bodies**, plus one bespoke body per variadic handler. Each is
named after its ABI shape (`hle_thunk_i32_i64__i32`), which is what makes a
generated 1200-line table reviewable.

### ABI classes, not type spellings

A prototype maps onto *how each argument is passed*, not what it is called.
`u32`, `s32`, `int`, `b32`, `CellMsgDialogType` and an enum all travel in a
32-bit slot; `u64`, `s64`, a pointer, `size_t` and a function-pointer typedef
share a 64-bit slot. `tools/hle_abi_shape.py` owns that mapping (reduced through
the typedef table the compiler sees) and is the only place it is decided, so the
generator and the audit cannot drift apart.

This is why the generated declarations say `int32_t`/`int64_t` rather than
repeating the library prototypes: the ABI class is all a trampoline needs, and
declaring the real prototypes would force the generated unit to include the
`libs/` headers -- several of which do not compile as C++ (`cellSync.h` uses C11
`_Atomic`; `sceNpBasic.h` and `sceNpCommerce2.h` disagree with their siblings
about the same struct and the same function). Class-only declarations keep the
unit self-contained AND correct, because a class is exactly what the callee
reads out of the register.

### Return values

An `i32` result is zero-extended into r3, not sign-extended. That is what the
old cast produced on every native build -- its return type was `uint64_t`, so
only the low half of a 32-bit result was ever meaningful. A title reading
`CELL_OK` or a negative Cell error as `(s32)r3` sees an identical value either
way; `runtime/ppu/tests/test_hle_trampoline.cpp` pins it at
`0x0000000080010006`. Nothing in the tree compared all 64 bits, so this is a
preservation rather than a behaviour change.

## What a trampoline cannot express

Three prototype shapes are reported by the generator on stderr and registered
through the old path, unchanged:

| shape | why | count |
|---|---|---|
| `float`/`double` argument | the PS3 ABI passes those in `f1..f13`, which the dispatcher never marshalled | 12 |
| `float`/`double` return | same, in the other direction (`f1`) | 1 |
| more than 8 parameters | the dispatcher has exactly 8 GPR slots | 3 |

**The float cases are a pre-existing bug, not something this change
introduced.** `cellVideoOutSetGamma(u32, float)` has always received whatever
happened to be in r4 as its `gamma`; on x86-64 that is garbage rather than a
trap. `libs/misc/cellAvconfExt.c` documents this and works around it with a
context handler (`avconf_register_ctx`) that reads `ctx->fpr[1]`. Building an
integer trampoline for these would silently paper over the bug, so they keep
today's behaviour bit for bit and stay listed here. They are the one place where
'fix the call ABI' is not the same problem as 'marshal the PS3 ABI'.

The full legacy set, as of this branch:

```
cellFontFTSetFontSize                   float argument
cellFontRenderCharGlyphImage            float arguments
cellFontSetEffectSlant                  float argument
cellFontSetEffectWeight                 float argument
cellFontSetScalePixel                   float arguments
cellFontSetupRenderScalePixel           float arguments
cellMsgDialogClose                      float argument
cellRescAdjustAspectRatio               float arguments
cellRescCreateInterlaceTable            float argument
cellRescSetPalInterpolateDropFlexRatio  float argument
cellVideoOutSetGamma                    float argument   (has a ctx handler already)
cellVoiceSetVolume                      float argument
cellAvconfExt_display_gamma             float return
cellGcmSetZcull                         12 parameters
cellSaveDataListAutoLoad                9 parameters
cellSpursWorkloadAttributeInitialize    9 parameters
```

Under wasm64 these 16 still trap if a title reaches them through the table.
Native is unaffected. Fixing them means teaching the dispatcher the PPC
floating-point argument registers (f1..f13) and, for the >8 case, the stack
argument convention -- a separate change with its own tests, deliberately
scoped out of this one.

## Variadic handlers

`_sys_printf`, `_sys_sprintf` and `_sys_snprintf` are variadic, and get one
bespoke trampoline each rather than a shared body. A cast to a fixed 8-argument
type is a *different* wasm type from `(i64, ...)`, and wasm-ld refuses to pair
them (measured: 'warning: function signature mismatch'), so the declaration has
to keep the ellipsis and the call has to name the handler directly.

Their trailing slots are forwarded as `unsigned long long`, which is what the
PS3 ABI widens varargs into and what the only reader in the tree
(`yz_format`, `libs/system/sysPrxForUser.c`) always fetches with
`va_arg(ap, unsigned long long)`. Forwarding them as `int` would truncate to
four bytes and misalign every argument after the first -- measured, not assumed.

## Verifying it

```sh
# Native: the dispatcher's contract, in isolation.
cmake -B build -DPS3RECOMP_BUILD_TESTS=ON && cmake --build build
./build/test_hle_trampoline                 # 8/8

# Native: the whole boot path still passes.
./build/ps3recomp_boot_smoke build/smoke/smoke.elf
#   [smoke] OK: all 11 markers, in order
#   [smoke] PASS
```

Under wasm64 the boot smoke is still blocked on a *different* problem: R1's
`ucontext` gap in `libs/spurs/cellFiber.c` (`getcontext`, `makecontext`,
`setcontext`, `swapcontext` have declarations in Emscripten's sysroot and no
implementation). That is unrelated to this change and is being fixed in parallel.

To measure this change end to end before R1 lands, four abort-on-use stubs for
those symbols can be linked in by hand. **That stub is a measurement scaffold,
not part of this change** -- it is not in the tree and not referenced by
`CMakeLists.txt`. Reproduce the 11/11 result with:

```sh
cat > /tmp/uc.c <<'EOF'
#include <stdlib.h>
typedef struct { long o[16]; } ucontext_t;
int getcontext(ucontext_t* u) { (void)u; abort(); }
int setcontext(const ucontext_t* u) { (void)u; abort(); }
int swapcontext(ucontext_t* o, const ucontext_t* n) { (void)o; (void)n; abort(); }
void makecontext(ucontext_t* u, void (*f)(void), int c, ...) { (void)u; (void)f; (void)c; abort(); }
EOF
emcc -m64 -pthread -O3 -c /tmp/uc.c -o /tmp/uc.o

# Relink the boot smoke's objects with /tmp/uc.o, and preload the ELF (the wasm
# sandbox cannot open a host file, so the image has to be in its virtual FS):
cd build-wasm
em++ -m64 -pthread -O3 -DNDEBUG -sINITIAL_MEMORY=4GB -sALLOW_MEMORY_GROWTH=1 \
     -sMAXIMUM_MEMORY=16GB -sPTHREAD_POOL_SIZE=8 -sEXIT_RUNTIME=1 \
     --preload-file smoke/smoke.elf@/smoke/smoke.elf -sFORCE_FILESYSTEM=1 \
     ${ps3recomp_boot_smoke objects...} -o wasm_smoke.js \
     libps3recomp_runtime.a /tmp/uc.o -sUSE_SDL=2

node wasm_smoke.js /smoke/smoke.elf
#   [smoke] OK: all 11 markers, in order
#   [smoke] PASS
```

Swapping the generated `ppu_hle_nids.cpp` for one whose rows all use
`ps3_hle_register` instead of `ps3_hle_register_trampolined` reproduces the
original failure from the same binary: it reaches marker 6 and stops with
`RuntimeError: null function or function signature mismatch`.

## Files

| file | role |
|---|---|
| `tools/hle_abi_shape.py` | prototype -> ABI class; the single source of truth |
| `tools/gen_hle_nids.py` | emits the trampolines and the registration table |
| `runtime/ppu/ppu_hle.cpp` | `ps3_hle_register_trampolined`, per-slot trampoline table, dispatch |
| `runtime/ppu/tests/test_hle_trampoline.cpp` | pins the contract, incl. the r3 extension |
