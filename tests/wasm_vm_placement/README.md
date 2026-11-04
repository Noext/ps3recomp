# wasm64 VM-placement probes

These answer one question, by measurement rather than argument: can the 4 GB
aligned guest window \`runtime/memory/vm.h\` needs be placed under WebAssembly?
The findings are written up in \`../docs/WASM_VM.md\`.

All of them build with Emscripten 6.0.11 and were run under both Node 24 and
headless Chromium 156.

## Sources

| File | Question it answers |
|---|---|
| \`probe_placement.c\` | Where does Emscripten put statics/heap/stack under wasm64, can a 4 GB-aligned window sit above them, does guest 0x10000 round-trip, what does an over-map cost, and does \`mprotect\` work or lie? |
| \`probe_realalloc.c\` | Reproduces the **real** host allocation (\`calloc(1, 0xE0000000)\`, \`mmap(NULL, ...)\`) and shows the base is not aligned -- the silent-corruption case. |
| \`probe_fullwindow.c\` | The first working mechanism: \`memalign(4 GiB, 4 GiB)\` at run time, in Node and in Chromium. Superseded by the linker placement but kept as evidence. |
| \`probe_option1.c\` | The mechanism that shipped: \`-sGLOBAL_BASE=8gb\` leaves \`[4 GiB, 8 GiB)\` empty, so \`vm_base\` is the constant \`0x100000000\`. Includes a pthread writing into the window. |
| \`probe_zero.c\` | Is a freshly placed window actually zero, even after heap churn? (Yes.) |
| \`host_arena_e2e.c\` | **B4.** Reproduces the host's own sequence -- allocate, write an ELF image at guest 0x10000, boot memset, truncate host pointers back across the whole 4 GB space. |
| \`vm_placement_e2e.c\` | The same through \`vm_init()\` itself, 15 assertions, passing on native, wasm64/Node and wasm64/Chromium. |
| \`mt.c\` | wasm64 + pthreads + shared memory with the window in place. |
| \`browser/cdp.py\` | Minimal headless-Chromium DevTools driver that serves the directory with COOP/COEP. |
| \`browser/measure_*.py\` | Sample the kernel's \`VmRSS\`/\`VmSize\` over the whole Chromium process tree, so the memory cost is measured rather than estimated. |

## Building

\`\`\`sh
source <emsdk>/emsdk_env.sh
cd tests/wasm_vm_placement

# the shipped layout
emcc -m64 -O2 -sGLOBAL_BASE=8gb -sINITIAL_MEMORY=8704mb \
     -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=16gb  -I../.. \
     host_arena_e2e.c -o host_wasm64.js
node host_wasm64.js

# wasm32 must refuse rather than corrupt itself
emcc -O2  -I../.. vm_placement_e2e.c -o e2e_wasm32.js && node e2e_wasm32.js
\`\`\`

## Running in a browser

\`browser/cdp.py\` serves a directory with the cross-origin-isolation headers
shared memory needs, launches headless Chromium and evaluates an expression:

\`\`\`sh
cd browser
cp ../host_wasm64.js ../host_wasm64.wasm .
printf '<!doctype html><meta charset=utf-8><script>window.__o=[];(function(){const o=console.log;console.log=function(...a){window.__o.push(a.join(" "));o.apply(console,a);};})();</script><script src="host_wasm64.js"></script>' > host.html
python3 cdp.py --dir . --url 'http://127.0.0.1:{port}/host.html' --timeout 150 \
  --eval 'window.__o && window.__o.join("\\n")'
\`\`\`
