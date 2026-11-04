
/*
 * probe_placement.c -- can a 4 GB-aligned guest window be placed under wasm64?
 *
 * Answers, by measurement, the five questions the port depends on:
 *   1. Where does Emscripten put static data, the stack and the heap?
 *   2. Can a 4 GB-aligned window be placed, and by which mechanism?
 *   3. Does guest 0x10000 round-trip, and does the window avoid the statics?
 *   4. What does the window cost (mmap is a heap allocator, not a reservation)?
 *   5. Does mprotect do anything, or does it return success and lie?
 *
 * Build (wasm64, the only target that can work):
 *   emcc -m64 -O1 -sGLOBAL_BASE=8589934592 -sALLOW_MEMORY_GROWTH \
 *        -sMAXIMUM_MEMORY=17179869184 probe_placement.c -o probe_placement.js
 * Run: node probe_placement.js        (and the same .js in a browser)
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#include <emscripten/heap.h>

extern char __heap_base;

#define VM_TOTAL_SIZE 0x100000000ull    /* 4 GB: the guest address space */
#define VM_MAIN_MEM_BASE 0x00010000u    /* ps3recomp: first guest byte */
#define WINDOW_BASE  0x100000000ull     /* 4 GB, the only 4 GB-aligned base
                                         * that is not 0 (0 is program image) */

static char s_marker[64] = "EMSCRIPTEN-OWN-STATIC-DATA";
static int  s_zero[16];

static void rss(const char* tag)
{
    FILE* f = fopen("/proc/self/status", "r");
    char line[256];
    if (!f) { printf("  [rss %s] /proc unavailable\n", tag); return; }
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "VmRSS", 5) || !strncmp(line, "VmSize", 6))
            printf("  [rss %s] %s", tag, line);
    fclose(f);
    fflush(stdout);
}

int main(void)
{
    printf("== target ==\n");
    printf("sizeof(void*)      : %zu\n", sizeof(void*));
#ifdef __wasm64__
    printf("__wasm64__         : defined\n");
#endif
#ifdef __EMSCRIPTEN__
    printf("__EMSCRIPTEN__     : defined\n");
#endif
    printf("host page size     : %ld\n", sysconf(_SC_PAGESIZE));

    printf("\n== 1. where emscripten puts things ==\n");
    {
        int local = 0;
        void* h = malloc(16);
        printf("static s_marker    : %p\n", (void*)s_marker);
        printf("__heap_base        : %p\n", (void*)&__heap_base);
        printf("malloc(16)         : %p\n", h);
        printf("stack (local var)  : %p\n", (void*)&local);
        printf("linear mem size    : %zu (%.3f GiB)\n",
               emscripten_get_heap_size(), emscripten_get_heap_size() / 1073741824.0);
        printf("linear mem max     : %zu (%.3f GiB)\n",
               emscripten_get_heap_max(), emscripten_get_heap_max() / 1073741824.0);
        free(h);
    }

    printf("\n== 2. can a 4 GB-aligned window be placed? ==\n");
    {
        uint8_t* win = (uint8_t*)(uintptr_t)WINDOW_BASE;
        int aligned = ((uintptr_t)win & (uintptr_t)(VM_TOTAL_SIZE - 1u)) == 0;
        int inside  = (uint64_t)WINDOW_BASE + VM_TOTAL_SIZE <= emscripten_get_heap_size();
        int overlaps_static =
            ((uintptr_t)s_marker >= WINDOW_BASE &&
             (uintptr_t)s_marker < WINDOW_BASE + VM_TOTAL_SIZE);
        printf("window base        : %p\n", (void*)win);
        printf("4 GB aligned       : %s\n", aligned ? "YES" : "NO");
        printf("inside linear mem  : %s\n", inside ? "YES" : "NO");
        printf("overlaps statics   : %s\n", overlaps_static ? "YES -- COLLISION" : "no");
        printf("low 32 bits of base: 0x%08llx (must be 0 for GUEST_EA truncation)\n",
               (unsigned long long)((uintptr_t)win & 0xFFFFFFFFull));
    }

    printf("\n== 3. does guest 0x10000 round-trip? ==\n");
    {
        uint8_t* win = (uint8_t*)(uintptr_t)WINDOW_BASE;
        uint8_t* guest = win + VM_MAIN_MEM_BASE;
        const char* pat = "GUEST-PATTERN-42";
        memcpy(guest, pat, strlen(pat) + 1);
        int ok = strcmp((const char*)guest, pat) == 0;
        printf("guest 0x10000 host : %p\n", (void*)guest);
        printf("readback           : \"%s\" -> %s\n", (const char*)guest, ok ? "PASS" : "FAIL");
        /* the invariant the HLE relies on: truncate a host pointer to 32 bits */
        uint32_t recovered = (uint32_t)(uintptr_t)guest;
        printf("GUEST_EA(host ptr) : 0x%08x (want 0x%08x) %s\n",
               recovered, VM_MAIN_MEM_BASE,
               recovered == VM_MAIN_MEM_BASE ? "PASS" : "FAIL");
        /* zero a slice the way vm_init's boot memset does, then check statics */
        memset(win + VM_MAIN_MEM_BASE, 0, 0x100000);
        printf("statics after boot memset: \"%s\" -> %s\n",
               s_marker, strcmp(s_marker, "EMSCRIPTEN-OWN-STATIC-DATA") == 0 ? "intact" : "DESTROYED");
    }

    printf("\n== 4. what does an mmap over-reservation cost? ==\n");
    {
        /* vm.h's POSIX path: mmap 2x VM_TOTAL_SIZE then trim. Under Emscripten
         * mmap is emscripten_builtin_memalign + memset: it really allocates. */
        size_t want = (size_t)VM_TOTAL_SIZE * 2u;
        void* p = mmap(NULL, want, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED)
            printf("mmap(8 GiB)        : FAILED errno=%d (%s)\n", errno, strerror(errno));
        else
            printf("mmap(8 GiB)        : %p (a heap allocation, not a reservation)\n", p);
        /* a fixed-address hint, which is what placement-by-mmap would need */
        void* q = mmap((void*)(uintptr_t)WINDOW_BASE, 0x1000, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        printf("mmap(MAP_FIXED at %p) : %s\n", (void*)(uintptr_t)WINDOW_BASE,
               q == MAP_FAILED ? "FAILED (no fixed-address mapping in emscripten)" : "ok");
    }

    printf("\n== 5. does mprotect work or lie? ==\n");
    {
        uint8_t* win = (uint8_t*)(uintptr_t)WINDOW_BASE;
        void* page = win + 0x00200000u;
        memset(page, 0x11, 4096);
        errno = 0;
        int rc = mprotect(page, 4096, PROT_NONE);
        printf("mprotect(PROT_NONE) : rc=%d errno=%d\n", rc, errno);
        fflush(stdout);
        if (rc == 0) {
            /* if the page were really protected this store would trap */
            ((volatile uint8_t*)page)[0] = 0x22;
            printf("store into PROT_NONE page: SUCCEEDED -> mprotect is a no-op that lies\n");
        }
        errno = 0;
        rc = mprotect(page, 4096, PROT_READ | PROT_WRITE);
        printf("mprotect(PROT_RW)    : rc=%d errno=%d\n", rc, errno);
    }

    printf("\n== rss ==\n");
    rss("end");
    printf("\nPROBE COMPLETE\n");
    return 0;
}
