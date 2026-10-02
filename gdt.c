#include "kernel.h"

struct gdt_entry {
    unsigned short limit_low;
    unsigned short base_low;
    unsigned char base_middle;
    unsigned char access;
    unsigned char granularity;
    unsigned char base_high;
} __attribute__((packed));

struct gdt_pointer {
    unsigned short limit;
    unsigned int base;
} __attribute__((packed));

struct tss_entry {
    unsigned int previous;
    unsigned int esp0;
    unsigned int ss0;
    unsigned int esp1;
    unsigned int ss1;
    unsigned int esp2;
    unsigned int ss2;
    unsigned int cr3;
    unsigned int eip;
    unsigned int eflags;
    unsigned int eax;
    unsigned int ecx;
    unsigned int edx;
    unsigned int ebx;
    unsigned int esp;
    unsigned int ebp;
    unsigned int esi;
    unsigned int edi;
    unsigned int es;
    unsigned int cs;
    unsigned int ss;
    unsigned int ds;
    unsigned int fs;
    unsigned int gs;
    unsigned int ldt;
    unsigned short trap;
    unsigned short iomap_base;
} __attribute__((packed));

static struct gdt_entry gdt[6];
static struct gdt_pointer gdtr;
static struct tss_entry tss;
extern void gdt_flush(const struct gdt_pointer* pointer);
extern char stack_top;

static void gdt_set_entry(unsigned int index, unsigned int base, unsigned int limit,
                          unsigned char access, unsigned char granularity) {
    gdt[index].base_low = base & 0xFFFF;
    gdt[index].base_middle = (base >> 16) & 0xFF;
    gdt[index].base_high = (base >> 24) & 0xFF;
    gdt[index].limit_low = limit & 0xFFFF;
    gdt[index].granularity = (limit >> 16) & 0x0F;
    gdt[index].granularity |= granularity & 0xF0;
    gdt[index].access = access;
}

void gdt_install(void) {
    gdt_set_entry(0, 0, 0, 0, 0);
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xC0);
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xC0);
    gdt_set_entry(3, 0, 0xFFFFF, 0xFA, 0xC0);
    gdt_set_entry(4, 0, 0xFFFFF, 0xF2, 0xC0);
    for(unsigned int i = 0; i < sizeof(tss); i++) ((unsigned char*)&tss)[i] = 0;
    tss.ss0 = 0x10;
    tss.esp0 = (unsigned int)&stack_top;
    tss.iomap_base = sizeof(tss);
    gdt_set_entry(5, (unsigned int)&tss, sizeof(tss)-1, 0x89, 0x00);
    gdtr.limit = sizeof(gdt)-1;
    gdtr.base = (unsigned int)gdt;
    gdt_flush(&gdtr);
    unsigned short selector = 0x28;
    asm volatile("ltr %0" :: "r"(selector));
}

void gdt_set_kernel_stack(unsigned int stack_top) {
    tss.esp0 = stack_top;
}
