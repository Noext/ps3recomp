/*
 * probe_realalloc.c -- reproduce the REAL guest-arena allocation the hosts do,
 * not a synthetic mmap, and show what it yields under wasm64.
 *
 * Every host in this tree allocates the guest arena itself and assigns vm_base;
 * runtime/memory/vm.h's vm_init() is only reached from two unit tests. The
 * sites, found by grep, are:
 *
 *   runtime/ppu/tests/boot_main.cpp:1142   calloc(1, 0xE0000000u)   (POSIX)
 *   runtime/host/host_posix.c:203          calloc(1, VM_SIZE)
 *   templates/project/main.cpp:306         mmap(NULL, VM_SIZE, ...)
 *   wave/main.cpp:388                      calloc(1, VM_SIZE)
 *   runtime/prx/prx_loader.c:75            VirtualAlloc (Windows only)
 *
 * So the question "does wasm64 give a 4 GB-aligned window" has to be asked of
 * calloc(1, 0xE0000000) and of mmap(NULL, VM_SIZE), not of a chosen address.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <emscripten/heap.h>

#define VM_SIZE 0xE0000000ull          /* boot_main.cpp's arena size */
#define VM_TOTAL_SIZE 0x100000000ull

static char s_marker[64] = "PROGRAM-STATIC-DATA";

static int aligned4g(const void* p)
{
    return ((uintptr_t)p & (uintptr_t)(VM_TOTAL_SIZE - 1u)) == 0;
}

int main(void)
{
    printf("sizeof(void*)=%zu  static=%p  heap_max=%zu (%.2f GiB)\n",
           sizeof(void*), (void*)s_marker, emscripten_get_heap_max(),
           emscripten_get_heap_max() / 1073741824.0);

    printf("\n-- A. calloc(1, 0xE0000000) -- the boot_main / host_posix path --\n");
    {
        uint8_t* a = (uint8_t*)calloc(1, (size_t)VM_SIZE);
        if (!a) printf("calloc FAILED\n");
        else {
            printf("base            : %p\n", (void*)a);
            printf("4 GB aligned    : %s\n", aligned4g(a) ? "YES" : "NO");
            printf("low 32 bits     : 0x%08llx\n", (unsigned long long)((uintptr_t)a & 0xFFFFFFFFull));
            /* The corruption the vm.h comment warns about, made concrete:
             * a guest pointer to guest 0x00100000 truncated back to 32 bits. */
            uint8_t* host = a + 0x00100000u;
            uint32_t recovered = (uint32_t)(uintptr_t)host;
            printf("guest 0x00100000 -> host %p -> GUEST_EA 0x%08x  %s\n",
                   (void*)host, recovered,
                   recovered == 0x00100000u ? "round-trips" : "MISMATCH -- silent corruption");
            free(a);
        }
    }

    printf("\n-- B. mmap(NULL, 0xE0000000) -- the templates/project path --\n");
    {
        void* p = mmap(NULL, (size_t)VM_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED) printf("mmap FAILED errno=%d (%s)\n", errno, strerror(errno));
        else {
            printf("base            : %p\n", p);
            printf("4 GB aligned    : %s\n", aligned4g(p) ? "YES" : "NO");
            printf("low 32 bits     : 0x%08llx\n", (unsigned long long)((uintptr_t)p & 0xFFFFFFFFull));
            munmap(p, (size_t)VM_SIZE);
        }
    }

    printf("\n-- C. the fixed 4 GB window the linker reserved --\n");
    {
        uint8_t* w = (uint8_t*)(uintptr_t)VM_TOTAL_SIZE;
        printf("base            : %p\n", (void*)w);
        printf("4 GB aligned    : %s\n", aligned4g(w) ? "YES" : "NO");
        printf("inside memory   : %s\n",
               (uint64_t)VM_TOTAL_SIZE + VM_SIZE <= emscripten_get_heap_size() ? "YES" : "NO");
        printf("overlaps statics: %s\n",
               ((uintptr_t)s_marker >= VM_TOTAL_SIZE &&
                (uintptr_t)s_marker < VM_TOTAL_SIZE + VM_SIZE) ? "YES -- COLLISION" : "no");
        uint8_t* host = w + 0x00100000u;
        printf("guest 0x00100000 -> host %p -> GUEST_EA 0x%08x  %s\n",
               (void*)host, (uint32_t)(uintptr_t)host,
               (uint32_t)(uintptr_t)host == 0x00100000u ? "round-trips" : "MISMATCH");
    }
    return 0;
}
