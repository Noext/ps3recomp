/* probe_zero.c -- is a freshly memalign'd 4 GiB window zeroed?
 *
 * Native gets zeroed pages for free (mmap MAP_ANONYMOUS, calloc). The wasm
 * window comes out of dlmalloc, so "zeroed" has to be checked rather than
 * assumed: wasm linear memory is specified zero on creation and on grow, but
 * dlmalloc may hand back pages it has used before. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <emscripten/heap.h>

#define VM_TOTAL_SIZE 0x100000000ull
static char s_marker[64] = "PROGRAM-STATIC-DATA";

static size_t scan(uint8_t* w, size_t from, size_t to, size_t step)
{
    size_t nz = 0;
    for (size_t o = from; o < to; o += step) if (w[o]) nz++;
    return nz;
}

int main(void)
{
    /* dirty the heap first, the pessimistic case: a title that allocates
     * before the arena is carved out of whatever dlmalloc has recycled. */
    for (int i = 0; i < 64; i++) {
        void* p = malloc(1 << 20);
        if (p) memset(p, 0xA5, 1 << 20);
    }
    uint8_t* w = (uint8_t*)memalign((size_t)VM_TOTAL_SIZE, (size_t)VM_TOTAL_SIZE);
    if (!w) { printf("memalign FAILED after dirtying the heap\n"); return 1; }
    printf("window=%p low32=0x%08llx\n", (void*)w,
           (unsigned long long)((uintptr_t)w & 0xFFFFFFFFull));
    size_t nz = scan(w, 0, (size_t)VM_TOTAL_SIZE, 4093);
    printf("nonzero bytes in a strided scan of the window: %zu\n", nz);
    printf("main memory (first 256 MB) nonzero: %zu\n", scan(w, 0, 0x10000000u, 4093));
    printf("statics intact: %s\n", strcmp(s_marker, "PROGRAM-STATIC-DATA") == 0 ? "YES" : "NO");
    return 0;
}
