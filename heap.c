#include "kernel.h"

#define HEAP_LIMIT 0x00300000u
#define PAGE_FRAME_BASE 0x00300000u
#define PAGE_FRAME_COUNT ((PAGING_IDENTITY_LIMIT-PAGE_FRAME_BASE)/4096u)

unsigned int heap_ptr = HEAP_START;
static unsigned char page_frames_used[PAGE_FRAME_COUNT/8u];

static unsigned int interrupts_save_disable(void) {
    unsigned int flags;
    asm volatile("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void interrupts_restore(unsigned int flags) {
    if(flags & 0x200u) asm volatile("sti" ::: "memory");
}

void page_allocator_init(void) {
    unsigned int flags = interrupts_save_disable();
    for(unsigned int i = 0; i < sizeof(page_frames_used); i++) page_frames_used[i] = 0;
    interrupts_restore(flags);
}

void* page_alloc(void) {
    unsigned int flags = interrupts_save_disable();
    void* page = 0;
    for(unsigned int i = 0; i < PAGE_FRAME_COUNT; i++) {
        unsigned char mask = (unsigned char)(1u << (i & 7u));
        if(page_frames_used[i >> 3] & mask) continue;
        page_frames_used[i >> 3] |= mask;
        page = (void*)(PAGE_FRAME_BASE + i*4096u);
        unsigned char* bytes = (unsigned char*)page;
        for(unsigned int j = 0; j < 4096u; j++) bytes[j] = 0;
        break;
    }
    interrupts_restore(flags);
    return page;
}

int page_free(void* page) {
    unsigned int address = (unsigned int)page;
    if(address < PAGE_FRAME_BASE || address >= PAGING_IDENTITY_LIMIT ||
       (address & 4095u)) return 0;
    unsigned int index = (address-PAGE_FRAME_BASE)/4096u;
    unsigned int flags = interrupts_save_disable();
    unsigned char mask = (unsigned char)(1u << (index & 7u));
    int released = (page_frames_used[index >> 3] & mask) != 0;
    if(released) page_frames_used[index >> 3] &= (unsigned char)~mask;
    interrupts_restore(flags);
    return released;
}

unsigned int page_free_count(void) {
    unsigned int flags = interrupts_save_disable();
    unsigned int free_pages = 0;
    for(unsigned int i = 0; i < PAGE_FRAME_COUNT; i++)
        if(!(page_frames_used[i >> 3] & (1u << (i & 7u)))) free_pages++;
    interrupts_restore(flags);
    return free_pages;
}

void* kmalloc(unsigned int size) {
    if(!size || heap_ptr < HEAP_START || heap_ptr > HEAP_LIMIT) return 0;
    unsigned int aligned = (size + 3u) & ~3u;
    if(aligned < size || aligned > HEAP_LIMIT-heap_ptr) return 0;
    void* p = (void*)heap_ptr;
    heap_ptr += aligned;
    return p;
}
