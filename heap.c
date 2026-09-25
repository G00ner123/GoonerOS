#include "kernel.h"

unsigned int heap_ptr = HEAP_START;

void* kmalloc(unsigned int size) {
    if(heap_ptr + size > HEAP_START + 0x100000) return 0;
    void* p = (void*)heap_ptr;
    heap_ptr += (size + 3) & ~3;
    return p;
}
