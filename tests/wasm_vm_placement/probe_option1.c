/* probe_option1.c -- window placed by the LINKER, not by an allocator.
 *
 * Set GLOBAL_BASE to 8 GiB. The linker then puts the data segments, the stack
 * and the heap above 8 GiB, which leaves [4 GiB, 8 GiB) untouched -- exactly
 * the 4 GB-aligned window vm.h needs. No allocator, no alignment gamble: the
 * layout is a link-time constant and it is the same on every run.
 *
 * Peak declared linear memory is 8.5 GiB, but only touched pages are resident.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <emscripten/heap.h>
#include <emscripten/threading.h>

#define VM_TOTAL_SIZE    0x100000000ull
#define VM_MAIN_MEM_BASE 0x00010000u

static char s_marker[64] = "PROGRAM-STATIC-DATA";
static uint8_t* vm_base = (uint8_t*)(uintptr_t)VM_TOTAL_SIZE;
static atomic_int g_ok;

static void* worker(void* arg)
{
    (void)arg;
    /* another thread writes guest 0x10000 in the same window */
    vm_base[VM_MAIN_MEM_BASE + 4] = 0xC3;
    atomic_fetch_add(&g_ok, 1);
    return NULL;
}

int main(void)
{
    printf("statics=%p heap_base=%p heap=%zu max=%zu\n", (void*)s_marker,
           (void*)sbrk(0), emscripten_get_heap_size(), emscripten_get_heap_max());
    printf("vm_base=%p aligned=%s low32=0x%08llx\n", (void*)vm_base,
           ((uintptr_t)vm_base & (VM_TOTAL_SIZE - 1)) == 0 ? "YES" : "NO",
           (unsigned long long)((uintptr_t)vm_base & 0xFFFFFFFFull));
    printf("window inside linear memory: %s\n",
           emscripten_get_heap_size() >= VM_TOTAL_SIZE * 2 ? "YES" : "NO");
    printf("statics inside window: %s\n",
           ((uintptr_t)s_marker >= (uintptr_t)vm_base &&
            (uintptr_t)s_marker < (uintptr_t)vm_base + VM_TOTAL_SIZE) ? "YES -- COLLISION" : "no");

    /* the boot memset vm_init() performs */
    memset(vm_base + VM_MAIN_MEM_BASE, 0, 0x10000000u);
    printf("statics after boot memset: \"%s\" %s\n", s_marker,
           strcmp(s_marker, "PROGRAM-STATIC-DATA") == 0 ? "intact" : "DESTROYED");

    memcpy(vm_base + VM_MAIN_MEM_BASE, "GUEST-PATTERN-42", 17);
    printf("guest 0x10000 readback=\"%s\"\n", (char*)(vm_base + VM_MAIN_MEM_BASE));
    printf("GUEST_EA round-trip=0x%08x %s\n",
           (uint32_t)(uintptr_t)(vm_base + VM_MAIN_MEM_BASE),
           (uint32_t)(uintptr_t)(vm_base + VM_MAIN_MEM_BASE) == VM_MAIN_MEM_BASE ? "PASS" : "FAIL");

    pthread_t t; pthread_create(&t, NULL, worker, NULL); pthread_join(t, NULL);
    printf("other-thread write into window: %02x %s\n",
           vm_base[VM_MAIN_MEM_BASE + 4], vm_base[VM_MAIN_MEM_BASE + 4] == 0xC3 ? "PASS" : "FAIL");
    printf("statics at end: %s\n", strcmp(s_marker, "PROGRAM-STATIC-DATA") == 0 ? "intact" : "DESTROYED");

    int pass = ((uintptr_t)vm_base & (VM_TOTAL_SIZE - 1)) == 0
            && strcmp(s_marker, "PROGRAM-STATIC-DATA") == 0
            && vm_base[VM_MAIN_MEM_BASE + 4] == 0xC3;
    printf("RESULT: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
