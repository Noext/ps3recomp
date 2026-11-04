# The guest VM on WebAssembly (wasm64)

Measured on this machine (Emscripten 6.0.11, Node 24.14.0, Chromium 156.0.8078.4
"Chrome for Testing" headless on Ubuntu 24.04), against
\`sp00nznet/ps3recomp\` @ \`4e8f3ca\`.

This document answers one question: **can the guest address space that
\`runtime/memory/vm.h\` requires actually be placed under WebAssembly, in a real
browser?** The short answer is yes, but only under wasm64, and only by asking the
linker for it rather than an allocator.

---

## 1. The requirement

\`vm.h\` maps the PS3's 32-bit address space as one contiguous window:

\`\`\`
host_ptr = vm_base + guest_addr
\`\`\`

and \`vm_base\` must be **4 GB aligned**. The reason is not layout taste. Most of
the runtime translates one way, but the HLE also runs the other way: a bridge
handed a host pointer recovers the guest address by truncating it to 32 bits
(\`GUEST_EA\` in \`libs/guest_struct.h\`). That only yields the right answer while
the low 32 bits of \`vm_base\` are zero. If they are not, the truncated pointer is
a plausible-looking guest address that moves with every run, and a write lands
somewhere in the guest arena leaving the guest's own variable untouched. There
are 58 direct \`GUEST_EA(...)\` call sites and 337 \`GUEST_PTR/GUEST_EA\` uses in
total, so this is load-bearing, not theoretical.

Two more properties matter: the window must be 4 GB *tall*, and it must not
overlap the program's own data, because \`vm_init()\` zeroes 256 MB of main memory
at boot.

## 2. What does not work

### 2.1 wasm32 cannot express it

Under wasm32 the linear memory *is* the whole address space and begins at 0, so
the only 4 GB-aligned base is **0** -- exactly where Emscripten's own data,
stack and heap live. Measured with \`probe_collide.c\`:

\`\`\`
static data lives at : 0x112e0
guest vm_base        : 0
guest main mem at    : 0x10000
collision?           : YES - guest memory overlaps the program
\`\`\`

The boot \`memset(\`vm_base + VM_MAIN_MEM_BASE, 0, 256MB)\` then wipes the program.
wasm32 is not difficult here, it is impossible.

### 2.2 The native paths do not carry over

Each host allocates the arena itself and assigns \`vm_base\` directly, because
each wants a different size and a different \`ppu_vm_size\`:

| Site | Allocation |
|---|---|
| \`runtime/ppu/tests/boot_main.cpp:1142\` | \`calloc(1, 0xE0000000u)\` (POSIX) |
| \`wave/main.cpp:392\` | \`calloc(1, 0xE0000000u)\` (POSIX) |
| \`runtime/host/host_posix.c:203\` | \`calloc(1, VM_SIZE)\` |
| \`runtime/host/host_main.c:137\` | \`calloc(1, VM_SIZE)\` |
| \`templates/project/main.cpp:331\` | \`mmap(NULL, VM_SIZE, ...)\` (POSIX) |

\`vm_init()\` is reached only from two unit tests, **not** from any host. So
"change vm_init" is not by itself sufficient, and it is worth being explicit
about that: the placement has to reach the host's own allocation.

Under Emscripten both of those calls return whatever the allocator has, and it
is not 4 GB aligned. Measured (\`tests/wasm_vm_placement/probe_realalloc.c\`, wasm64, Node and
Chromium agree):

\`\`\`
A. what the hosts do today: calloc(1, 0xE0000000)
  calloc                       0x2000116c0  aligned4G=NO  low32=0x000116c0
  guest 0x00100000 -> GUEST_EA 0x001116c0  MISMATCH (silent corruption)

B. mmap(NULL, 0xE0000000)
  mmap                         0x200020000  aligned4G=NO  low32=0x00020000
\`\`\`

The other obvious mechanisms also fail, each for its own reason:

* **\`mmap\` + align** -- Emscripten's \`mmap\` ignores address hints and returns
  \`EINVAL\` for a non-zero \`addr\`. Even \`MAP_FIXED\` fails.
* **\`mmap\` over-reserve and trim** (what \`vm_init\`'s POSIX path does) -- Emscripten's
  \`mmap\` is not a lazy kernel reservation, it is \`emscripten_builtin_memalign\`
  plus a \`memset\`. Asking for 8 GB really allocates 8 GB and fails:
  \`mmap(8 GiB) FAILED errno=48 (Out of memory)\`.
* **A 4 GB-aligned static array** -- does not compile. A wasm object file caps a
  single section at 4 GB: \`fatal error: section size does not fit in a uint32_t\`.
* **\`memalign(4 GiB, 4 GiB)\` at run time** -- this *does* work, and was the first
  mechanism found (see \`tests/wasm_vm_placement/probe_fullwindow.c\`), but it is a runtime
  allocation out of a heap that has to grow to 8 GB to satisfy it, and it leaves
  the layout dependent on allocator behaviour. The linker-based placement below
  is strictly better and is what shipped.

## 3. What does work: place the window by construction

wasm64 pointers are 64-bit, so linear memory has room above 4 GB. Tell the
linker to keep everything it places above 8 GB:

\`\`\`
-sGLOBAL_BASE=8gb
-sINITIAL_MEMORY=8704mb      # 8.5 GiB: 4 GiB window + 4 GiB for the segments
-sALLOW_MEMORY_GROWTH=1
-sMAXIMUM_MEMORY=16gb
-m64                          # current spelling of -sMEMORY64=1
-pthread
\`\`\`

Now \`[4 GiB, 8 GiB)\` is permanently empty, and \`vm_base\` can simply be the
constant **\`0x100000000\`**. That is stronger than either native path: it is a
link-time constant, not an allocation that might land anywhere, so the invariant
holds on the first attempt and on every run, with no retry loop and no trim.

Measured layout (\`tests/wasm_vm_placement/probe_option1.c\`, Node and Chromium):

\`\`\`
statics=0x200000370 heap_base=0x200013000 heap=9126805504 max=17179869184
vm_base=0x100000000 aligned=YES low32=0x00000000
window inside linear memory: YES
statics inside window: no
statics after boot memset: "PROGRAM-STATIC-DATA" intact
guest 0x10000 readback="GUEST-PATTERN-42"
GUEST_EA round-trip=0x00010000 PASS
other-thread write into window: c3 PASS
RESULT: PASS
\`\`\`

### 3.1 A guard for the flag

\`-sGLOBAL_BASE\` is easy to lose, and losing it fails *silently* -- the segments
land at 0x770 instead of 0x200000370. So \`vm_init()\` checks that its own
\`vm_base\` symbol (a global, so it moves with the segments) sits above the
window, and returns \`CELL_ENOMEM\` rather than proceeding. Negative test:

\`\`\`
a) WITH -sGLOBAL_BASE=8gb : statics at 0x200000370    vm_init -> CELL_OK
b) WITHOUT it             : statics at 0x770          vm_init -> ENOMEM (refused)
\`\`\`

Refusing is the point. A wrong base is not a crash; it is a plausible guest
address written somewhere else, discovered thousands of instructions later.

## 4. The browser

Chromium **156.0.8078.4** (Chrome for Testing, headless). Driven over the
DevTools protocol; the server sets \`COOP: same-origin\` and
\`COEP: require-corp\`.

### 4.1 No flag is needed

This is the deployment-critical question, and the answer is good:

* \`WebAssembly.validate()\` on a memory64 module: **true**
* An 8.5 GiB shared memory64 instance: **instantiates**, no \`--enable-features\`,
  no \`--js-flags\`, nothing but the stock binary.
* \`v8_flags\` in the binary do contain \`kWasmMemory64\`, and
  \`enable-experimental-webassembly-features\` exists, but **neither was required**.

The engine's implementation limit is **16384 MiB (16 GiB)**: a 16 GiB memory
instantiates, a 17 GiB one is rejected with
\`initial memory size (278528 pages) is larger than implementation limit (262144 pages)\`.

### 4.2 Instantiation time

| Memory | Time |
|---|---|
| 4 GiB, non-shared | 1.4 ms |
| 8.5 GiB, non-shared | 1.8 ms |
| 8.5 GiB, shared, max 16 GiB | 1.6-3.1 ms |
| 16 GiB, shared | 0.8-2.4 ms |

Instantiation is not the cost. A 16 GiB memory is created lazily; the pages cost
only when touched.

### 4.3 SharedArrayBuffer and pthreads

Shared memory requires cross-origin isolation, which is a deployment
requirement rather than a wasm64 one:

| | \`crossOriginIsolated\` | \`SharedArrayBuffer\` | shared memory64 |
|---|---|---|---|
| with COOP/COEP | true | defined | works; \`buffer instanceof SharedArrayBuffer\` is **true** |
| without | false | **undefined** | reference error |

Emscripten's shared-memory path refuses to start without \`SharedArrayBuffer\`,
so **the page must be served cross-origin isolated** (\`COOP: same-origin\` +
\`COEP: require-corp\`). Non-shared (no-pthread) wasm64 works without isolation,
which is a useful fallback for a single-threaded demo but not for this runtime.

pthreads work end to end with the window in place -- two threads incrementing an
atomic counter 1000 times each, and a worker thread writing into the guest window
while the main thread reads it back (\`tests/wasm_vm_placement/mt.c\`, both Node and Chromium).

## 5. What it costs

Emscripten has no lazy backing for anonymous memory, so the module *declares*
8.5 GiB initial and 16 GiB maximum. The engine commits only touched pages. This
was measured at the kernel, by summing \`VmRSS\`/\`VmSize\` over the whole Chromium
process tree at checkpoints the page announces:

\`\`\`
[kern] module-loaded              procs=12 VmRSS=1083 MiB VmSize=9229844 MiB
[page] window=0x100000000 aligned=YES low32=0x00000000 heap_now=8590065664
[page] RESULT: PASS
[kern] after-arena                procs=12 VmRSS=1347 MiB VmSize=9229848 MiB
\`\`\`

**+264 MiB RSS** for placing the 4 GB window and memsetting 256 MB of main
memory, not 4 GB. Separately, a 16 GiB shared memory touched sparsely (every
64 MiB) adds ~2 MiB; touching 512 MiB densely adds ~517 MiB. So the rule is
plain: *declared* memory is cheap, *touched* memory costs its size.

Two honest caveats:

* \`VmSize\` is ~9.2 GB per browser and does not move, because the reservation is
  made once. On a machine where overcommit is disabled or the tab is under a
  tight memory budget, declaring 16 GB maximum can still be refused at
  instantiation.
* Growth past the initial 8.5 GiB is real allocation, and with \`-pthread\` it is
  also why Emscripten warns about slow non-wasm code.

## 6. The change

\`runtime/memory/vm.h\` gains a WebAssembly branch:

* \`vm_place_guest_window()\` -- returns \`0x100000000\` on wasm64 when linear
  memory is large enough, and \`NULL\` everywhere else (so native hosts keep
  their own allocator). This is the piece the hosts call.
* \`vm_init()\` -- uses it, then verifies its own data sits above the window and
  returns \`CELL_ENOMEM\` otherwise. wasm32 returns \`CELL_ENOMEM\` unconditionally.
* \`vm__mprotect_pages()\` -- **refuses** under Emscripten (\`errno = ENOSYS\`,
  returns \`-1\`) instead of calling a stub that returns success and does nothing.
* \`vm_commit()\` -- succeeds without calling mprotect, which is the truth: linear
  memory is already readable and writable. Deliberately *not* routed through the
  refusing helper, because \`sys_memory_allocate\` and
  \`sys_mmapper_search_and_map\` depend on it and answering them ENOMEM would be
  a lie in the other direction.
* \`vm_protect()\` -- read/write succeeds (already true); anything narrower is
  refused. The caller that matters is \`vm_stack_allocate()\`'s guard page, which
  cannot exist under Emscripten; it now learns that instead of believing it is
  armed.
* \`vm_shutdown()\` -- clears the pointer and does not \`munmap\` a window it does
  not own.

The hosts that assign \`vm_base\` themselves (`boot_main.cpp`, `wave/main.cpp`,
`host_posix.c`) now call \`vm_place_guest_window()\` first and fall back to their
existing allocation. On native the helper returns \`NULL\` and the fallback runs,
so native behaviour is bit-for-bit unchanged.

\`cmake/toolchains/emscripten-wasm64.cmake\` carries the flags.

## 7. Proof

\`tests/wasm_vm_placement/host_arena_e2e.c\` reproduces the real host sequence -- allocate as the
host does, write an ELF-shaped image at guest \`0x10000\`, run the boot memset,
then round-trip host pointers back to guest addresses across the whole 4 GB
space. Built for wasm64 and run under **both** Node and headless Chromium:

\`\`\`
  target          : Emscripten
  vm_place_guest_window : 0x100000000 (linker-reserved)
  vm_base         : 0x100000000   low 32 bits: 0x00000000
  PASS  vm_base is 4 GB aligned
  PASS  the image reads back byte for byte
  PASS  truncating the host pointer recovers the guest address
  PASS  interior host pointers truncate correctly too
  PASS  guest main memory is cleared
  PASS  the program's own statics are untouched
  PASS  GUEST_EA truncation holds across the whole 4 GB space
RESULT: PASS
\`\`\`

\`tests/wasm_vm_placement/vm_placement_e2e.c\` does the same through \`vm_init()\` itself and passes
15/15 on native, on wasm64 in Node, and on wasm64 in Chromium.

## 8. A pre-existing native finding

The same probe run **natively** fails, and that is worth recording because it is
not caused by this work:

\`\`\`
== the host's own arena allocation ==
  calloc(0xE0000000)    : 0x7de064f1c010 (host allocator)
  low 32 bits     : 0x64f1c010
  4 GB aligned    : NO (pre-existing native gap)
  FAIL  truncating the host pointer recovers the guest address
\`\`\`

The native hosts bypass \`vm_init()\` and \`calloc\` the arena, which is not 4 GB
aligned. Whatever HLE path hands a **host pointer** back through \`GUEST_EA\`
would therefore be wrong on native too. Paths that take a guest address and
translate forward are unaffected. \`vm_init()\`'s alignment guarantee is real, but
the hosts do not use it -- which is precisely why the fix has to touch the hosts
and not just \`vm.h\`.

This is reported, not fixed: it is out of scope here and fixing it natively
means changing the arena allocation in five hosts, which should be its own
change with its own testing.

## 9. Verdict

**G2 (VM placement proven): PASS.**

Placement works, in a real browser, with no flag, in ~2 ms, at a measured cost of
~264 MiB RSS for a boot. The mechanism is a link-time constant, not an
allocator gamble, and there is a guard that refuses to run if the flag is lost.

Shippable is a different question, and the honest answer is **"demo, not
product"** -- but for reasons outside this probe:

* wasm64 itself is fine in Chromium 156 and needed no flag.
* The 16 GiB ceiling is real but the guest needs 4 GB of it plus ~256 MB touched.
* Cross-origin isolation is required, which is a hosting constraint.
* What still blocks a real boot is elsewhere: \`cellFiber.c\` needs \`ucontext\`,
  which Emscripten declares but does not implement, and the guard pages above
  no longer exist.

None of that is a memory-placement problem. The 4 GB-aligned window is solved.
