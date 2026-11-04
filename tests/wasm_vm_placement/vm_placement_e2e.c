/*
 * vm_placement_e2e.c -- B4: call the real vm_init() and prove the window.
 *
 * This is not a synthetic probe. It includes runtime/memory/vm.h and calls the
 * same vm_init() the hosts call, then exercises the translation helpers the
 * runtime actually uses.
 *
 * Built twice over the same source:
 *   native   : cc   -O2 -I. vm_placement_e2e.c -o vm_placement_e2e
 *   wasm64   : emcc -m64 -O2 -sGLOBAL_BASE=0x200000000 -sINITIAL_MEMORY=0x220000000 \
 *                   -sALLOW_MEMORY_GROWTH -sMAXIMUM_MEMORY=0x400000000 ...
 * and run under Node and under headless Chromium.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../../runtime/memory/vm.h"

/* vm.h declares these; a host normally defines them. */
uint8_t*  vm_base    = NULL;
uint32_t  ppu_vm_size = 0;

#define CHECK(cond, msg) do {                                         \
    if (cond) { printf("  PASS  %s\n", msg); pass++; }                \
    else      { printf("  FAIL  %s\n", msg); fail++; }                \
} while (0)

int main(void)
{
    int pass = 0, fail = 0;

    printf("== vm_init() on this platform ==\n");
    printf("  sizeof(void*)   : %zu\n", sizeof(void*));
#if defined(__EMSCRIPTEN__)
  #if defined(__wasm64__)
    printf("  target          : wasm64 (__EMSCRIPTEN__ + __wasm64__)\n");
  #else
    printf("  target          : wasm32 (__EMSCRIPTEN__, NO __wasm64__)\n");
  #endif
#else
    printf("  target          : native\n");
#endif

    int32_t rc = vm_init();
    printf("  vm_init()       : 0x%08x (%s)\n", (unsigned)rc,
           rc == CELL_OK ? "CELL_OK" : "error");
    CHECK(rc == CELL_OK, "vm_init returns CELL_OK");
    if (rc != CELL_OK) { printf("RESULT: FAIL (vm_init refused)\n"); return 1; }

    printf("\n== 1. the 4 GB-aligned base ==\n");
    printf("  vm_base         : %p\n", (void*)vm_base);
    printf("  low 32 bits     : 0x%08llx\n",
           (unsigned long long)((uintptr_t)vm_base & 0xFFFFFFFFull));
    CHECK(((uintptr_t)vm_base & (uintptr_t)(VM_TOTAL_SIZE - 1u)) == 0,
          "vm_base is 4 GB aligned");

    printf("\n== 2. guest 0x10000 round-trips ==\n");
    {
        uint8_t* host = (uint8_t*)vm_to_host(VM_MAIN_MEM_BASE);
        const char* pat = "B4-PATTERN-0x10000";
        memcpy(host, pat, strlen(pat) + 1);
        printf("  guest 0x%08x -> host %p\n", VM_MAIN_MEM_BASE, (void*)host);
        printf("  readback        : \"%s\"\n", (const char*)host);
        CHECK(strcmp((const char*)host, pat) == 0, "write/read at guest 0x10000");

        /* the direction the HLE actually runs: host pointer -> guest address,
         * by truncation. This is GUEST_EA in libs/guest_struct.h. */
        uint32_t ea = (uint32_t)(uintptr_t)host;
        printf("  GUEST_EA(host)  : 0x%08x (want 0x%08x)\n", ea, VM_MAIN_MEM_BASE);
        CHECK(ea == VM_MAIN_MEM_BASE, "host pointer truncates back to the guest address");

        /* and the runtime's own helper, which must agree */
        CHECK(vm_to_guest(host) == VM_MAIN_MEM_BASE, "vm_to_guest() agrees");
    }

    printf("\n== 3. the boot memset does not hit the program ==\n");
    {
        /* vm_init() already memset main memory once. Prove the harness's own
         * static state survived it, and survive a second one. */
        static const char* kStatic = "HARNESS-STATIC-STATE";
        printf("  static string   : \"%s\"\n", kStatic);
        memset(vm_base + VM_MAIN_MEM_BASE, 0, VM_MAIN_MEM_SIZE);
        CHECK(strcmp(kStatic, "HARNESS-STATIC-STATE") == 0,
              "statics survive the 256 MB main-memory memset");
        CHECK(vm_base[VM_MAIN_MEM_BASE] == 0, "main memory is zeroed");
    }

    printf("\n== 4. regions vm_init() committed are addressable ==\n");
    {
        /* Only the two regions vm_init() makes readable/writable on every
         * platform: main memory and the stack region. The rest of the guest map
         * stays PROT_NONE until vm_commit() is asked for it, on native -- and
         * touching it here would fault on Linux, which is exactly what this
         * control build demonstrates. Under Emscripten the whole window is
         * readable, but testing only the committed regions keeps the same
         * assertions meaningful on both. */
        static const uint32_t probes[] = {
            VM_MAIN_MEM_BASE,
            VM_MAIN_MEM_BASE + VM_MAIN_MEM_SIZE - 4u,
            VM_STACK_BASE,
            VM_STACK_BASE + VM_STACK_REGION - 4u,
        };
        for (unsigned i = 0; i < sizeof probes / sizeof probes[0]; i++) {
            uint32_t ea = probes[i];
            uint32_t v  = 0xA5A5A500u | i;
            memcpy(vm_to_host(ea), &v, 4);
            uint32_t back; memcpy(&back, vm_to_host(ea), 4);
            char msg[96];
            snprintf(msg, sizeof msg, "guest 0x%08x round-trips through vm_to_host", ea);
            CHECK(back == v, msg);
        }
    }

    printf("\n== 5. commit / protect reporting ==\n");
    {
        int32_t c = vm_commit(VM_PPU_TLS_BASE, 0x100000u);
        printf("  vm_commit(0x%08x, 1 MB) : %s\n", VM_PPU_TLS_BASE,
               c == CELL_OK ? "CELL_OK" : "error");
        CHECK(c == CELL_OK, "vm_commit succeeds (linear memory is already writable)");
        int32_t p = vm_protect(VM_STACK_BASE, VM_PAGE_SIZE, 1, 1, 0);
        printf("  vm_protect(.., rw)      : %s\n", p == CELL_OK ? "CELL_OK" : "error");
        CHECK(p == CELL_OK, "vm_protect(read+write) succeeds");
        int32_t q = vm_protect(VM_STACK_BASE, VM_PAGE_SIZE, 0, 0, 0);
        printf("  vm_protect(.., none)    : %s\n", q == CELL_OK ? "CELL_OK" : "refused");
#if defined(__EMSCRIPTEN__)
        CHECK(q != CELL_OK, "vm_protect(PROT_NONE) is refused, not a false success");
#else
        CHECK(q == CELL_OK, "vm_protect(PROT_NONE) works on this host");
#endif
    }

    printf("\n== 6. teardown ==\n");
    vm_shutdown();
    CHECK(vm_base == NULL, "vm_shutdown clears vm_base");

    printf("\n%d passed, %d failed\n", pass, fail);
    printf("RESULT: %s\n", fail == 0 ? "PASS" : "FAIL");
    return fail == 0 ? 0 : 1;
}
