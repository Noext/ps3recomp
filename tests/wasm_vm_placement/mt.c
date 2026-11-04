#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <emscripten/heap.h>
#include <emscripten/threading.h>
static char s_data[64]="STATIC-MARKER";
extern char __heap_base;
static atomic_int counter;
static void* worker(void* a){ (void)a; for(int i=0;i<1000;i++) atomic_fetch_add(&counter,1);
  /* touch guest window from another thread */
  volatile uint8_t* p=(volatile uint8_t*)0x100020000ull; *p=0x5A; return NULL; }
int main(void){
  printf("static=%p heap_base=%p heap=%zu max=%zu shared=%d\n",(void*)s_data,(void*)&__heap_base,
     emscripten_get_heap_size(), emscripten_get_heap_max(), emscripten_is_main_runtime_thread());
  pthread_t t[2];
  for(int i=0;i<2;i++) printf("create[%d]=%d\n", i, pthread_create(&t[i],NULL,worker,NULL));
  for(int i=0;i<2;i++) pthread_join(t[i],NULL);
  printf("counter=%d (expect 2000)\n", atomic_load(&counter));
  printf("other-thread guest write readback=%02x\n", *(volatile uint8_t*)0x100020000ull);
  printf("static after: %s\n", s_data);
  return atomic_load(&counter)==2000?0:1;
}
