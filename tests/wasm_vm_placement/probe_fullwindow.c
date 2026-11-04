/* probe_fullwindow.c -- the exact candidate fix, at full window size.
 *
 * vm.h needs vm_base 4 GB aligned with 4 GB of guest space behind it.
 * Under wasm64 the linear memory is the whole address space, so the window has
 * to be carved out of the heap at an explicit 4 GB alignment. This is that
 * allocation at full size, on the real toolchain flags, with pthreads. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <emscripten/heap.h>

#define VM_TOTAL_SIZE    0x100000000ull
#define VM_MAIN_MEM_BASE 0x00010000u

static char s_marker[64] = "PROGRAM-STATIC-DATA";

int main(void)
{
    printf("statics=%p heap=%zu max=%zu\n", (void*)s_marker,
           emscripten_get_heap_size(), emscripten_get_heap_max());
    uint8_t* w = (uint8_t*)memalign((size_t)VM_TOTAL_SIZE, (size_t)VM_TOTAL_SIZE);
    if (!w) { printf("memalign(4G,4G) FAILED\n"); return 1; }
    printf("window=%p aligned=%s low32=0x%08llx heap_now=%zu (%.2f GiB)\n", (void*)w,
           ((uintptr_t)w & (VM_TOTAL_SIZE-1)) == 0 ? "YES" : "NO",
           (unsigned long long)((uintptr_t)w & 0xFFFFFFFFull),
           emscripten_get_heap_size(), emscripten_get_heap_size()/1073741824.0);
    /* the boot memset vm_init does, against the program's own statics */
    memset(w + VM_MAIN_MEM_BASE, 0, 0x10000000u);
    printf("statics intact=%s\n",
           strcmp(s_marker, "PROGRAM-STATIC-DATA")==0 ? "YES" : "NO -- DESTROYED");
    memcpy(w + VM_MAIN_MEM_BASE, "GUEST-PATTERN-42", 17);
    printf("guest 0x10000 readback=\"%s\"\n", (char*)(w + VM_MAIN_MEM_BASE));
    printf("GUEST_EA round-trip=0x%08x %s\n",
           (uint32_t)(uintptr_t)(w+VM_MAIN_MEM_BASE),
           (uint32_t)(uintptr_t)(w+VM_MAIN_MEM_BASE)==VM_MAIN_MEM_BASE ? "PASS":"FAIL");
    printf("RESULT: %s\n", ((uintptr_t)w & (VM_TOTAL_SIZE-1))==0 ? "PASS" : "FAIL");
    return 0;
}
