/*
 * host_arena_e2e.c -- B4, done the way the hosts actually do it.
 *
 * This reproduces the boot scaffold's own sequence rather than calling
 * vm_init():
 *
 *   1. the host allocates the arena and assigns vm_base   (boot_main.cpp:1142)
 *   2. the loader writes the ELF image through vm_base
 *   3. the boot memset clears guest main memory
 *   4. the guest's static state and the HLE's truncation both still work
 *
 * It is the path that matters, and the one where a plain calloc is wrong.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../runtime/memory/vm.h"

/* A host defines these; so does this probe. */
uint8_t*  vm_base     = NULL;
uint32_t  ppu_vm_size = 0;

/* boot_main.cpp's arena size, verbatim. */
#define VM_SIZE 0x100010000ull

static char s_guest_static[64] = "GUEST-STATIC-STATE";
static int  s_fail = 0;

#define CHECK(c, m) do { if (c) printf("  PASS  %s\n", (m)); \
                         else { printf("  FAIL  %s\n", (m)); s_fail++; } } while (0)

int main(void)
{
    int pass = 0;

    printf("== the host's own arena allocation ==\n");
#if defined(__EMSCRIPTEN__)
    printf("  target          : Emscripten\n");
#else
    printf("  target          : native\n");
#endif

    /* Step 1: exactly what boot_main.cpp:1142 does, now routed through the
     * helper. Under Emscripten this returns the linker-reserved window; on
     * native it returns NULL and calloc is still the right answer. */
    vm_base = vm_place_guest_window();
    if (vm_base) {
        printf("  vm_place_guest_window : %p (linker-reserved)\n", (void*)vm_base);
        ppu_vm_size = 0;
    } else {
        vm_base = (uint8_t*)calloc(1, (size_t)VM_SIZE);
        ppu_vm_size = 0xE0000000u;
        printf("  calloc(0xE0000000)    : %p (host allocator)\n", (void*)vm_base);
    }
    CHECK(vm_base != NULL, "arena obtained");
    if (!vm_base) { printf("RESULT: FAIL\n"); return 1; }

    printf("\n== the invariant the HLE truncation rests on ==\n");
    printf("  vm_base         : %p\n", (void*)vm_base);
    printf("  low 32 bits     : 0x%08llx\n",
           (unsigned long long)((uintptr_t)vm_base & 0xFFFFFFFFull));
#if defined(__EMSCRIPTEN__)
    CHECK(((uintptr_t)vm_base & (uintptr_t)(VM_TOTAL_SIZE - 1u)) == 0,
          "vm_base is 4 GB aligned");
#else
    /* Native hosts bypass vm_init() and allocate the arena themselves, so the
     * 4 GB alignment vm.h documents is NOT in force here -- calloc gives
     * whatever the allocator has. That is a pre-existing property of the native
     * hosts, not something this probe introduces, and it is reported rather
     * than asserted so the measurement is not mistaken for a regression.
     * Hosts whose HLE actually hands host pointers back through GUEST_EA would
     * need the same placement the wasm64 path now gets. */
    printf("  (native: the host's own calloc, not vm_init -- alignment not guaranteed)\n");
    printf("  4 GB aligned    : %s (pre-existing native gap, see docs/WASM_VM.md)\n",
           ((uintptr_t)vm_base & (uintptr_t)(VM_TOTAL_SIZE - 1u)) == 0 ? "YES" : "NO");
#endif

    printf("\n== the loader writing an ELF image through vm_base ==\n");
    {
        /* A PT_LOAD-shaped write at guest 0x10000, the way ppu_load_elf does. */
        static const uint8_t image[32] = {
            0x7C,0x08,0x02,0xA6, 0x60,0x00,0x00,0x00,
            0x38,0x60,0x00,0x2A, 0x4E,0x80,0x00,0x20,
            0xDE,0xAD,0xBE,0xEF, 0xCA,0xFE,0xBA,0xBE,
            0x00,0x11,0x22,0x33, 0x44,0x55,0x66,0x77
        };
        memcpy(vm_base + VM_MAIN_MEM_BASE, image, sizeof image);
        int same = memcmp(vm_base + VM_MAIN_MEM_BASE, image, sizeof image) == 0;
        printf("  wrote %zu bytes at guest 0x%08x -> host %p\n",
               sizeof image, VM_MAIN_MEM_BASE, (void*)(vm_base + VM_MAIN_MEM_BASE));
        CHECK(same, "the image reads back byte for byte");
        /* the HLE's direction: host pointer -> guest address by truncation */
        uint32_t ea = (uint32_t)(uintptr_t)(vm_base + VM_MAIN_MEM_BASE);
        printf("  truncate(host)  : 0x%08x\n", ea);
        CHECK(ea == VM_MAIN_MEM_BASE, "truncating the host pointer recovers the guest address");
        /* and an interior pointer, which is what a bridge usually holds */
        uint32_t ea2 = (uint32_t)(uintptr_t)(vm_base + VM_MAIN_MEM_BASE + 16);
        CHECK(ea2 == VM_MAIN_MEM_BASE + 16, "interior host pointers truncate correctly too");
    }

    printf("\n== the boot memset clears guest memory, not the program ==\n");
    {
        memset(vm_base + VM_MAIN_MEM_BASE, 0, VM_MAIN_MEM_SIZE);
        CHECK(vm_base[VM_MAIN_MEM_BASE] == 0, "guest main memory is cleared");
        CHECK(strcmp(s_guest_static, "GUEST-STATIC-STATE") == 0,
              "the program's own statics are untouched");
    }

    printf("\n== a guest pointer handed to an HLE bridge, and back ==\n");
    {
        /* The round trip the whole design exists for: the guest computes an
         * address, the HLE gets a host pointer, and the guest address has to
         * come back out unchanged. */
        int ok = 1;
        for (uint32_t ea = VM_MAIN_MEM_BASE; ea < VM_MAIN_MEM_BASE + 0x10000u; ea += 4096u) {
            void* host = vm_to_host(ea);
            if ((uint32_t)(uintptr_t)host != ea) { ok = 0; break; }
            if (vm_to_guest(host) != ea) { ok = 0; break; }
        }
        CHECK(ok, "vm_to_host/vm_to_guest round-trip over 16 pages");
        /* the whole guest space, sampled: 256 addresses across all 4 GB */
        ok = 1;
        for (uint64_t ea = 0; ea < VM_TOTAL_SIZE; ea += VM_TOTAL_SIZE / 256u) {
            void* host = vm_to_host((uint32_t)ea);
            if ((uint32_t)(uintptr_t)host != (uint32_t)ea) { ok = 0; break; }
        }
        CHECK(ok, "GUEST_EA truncation holds across the whole 4 GB space");
    }

    printf("\n== ppu_vm_size stays consistent ==\n");
    printf("  ppu_vm_size     : 0x%08x\n", ppu_vm_size);
    CHECK(VM_READY, "VM_READY is true");

    printf("\nRESULT: %s\n", s_fail == 0 ? "PASS" : "FAIL");
    return s_fail == 0 ? 0 : 1;
}
