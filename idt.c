#include "kernel.h"
#include "io.h"

volatile unsigned int ticks = 0;

void pit_init(void) {
    outb(0x43, 0x36);
    outb(0x40, 0xA9);
    outb(0x40, 0x04);
}

struct idt_entry { unsigned short base_lo; unsigned short sel; unsigned char always0; unsigned char flags; unsigned short base_hi; } __attribute__((packed));
struct idt_ptr { unsigned short limit; unsigned int base; } __attribute__((packed));
static struct idt_entry idt[256];
static struct idt_ptr idtp;
extern void isr0(); extern void isr1(); extern void irq0(); extern void irq1(); extern void irq12();

void idt_set_gate(unsigned char num, unsigned int base, unsigned short sel, unsigned char flags) {
    idt[num].base_lo = base & 0xFFFF;
    idt[num].base_hi = (base >> 16) & 0xFFFF;
    idt[num].sel = sel; idt[num].always0 = 0; idt[num].flags = flags;
}
void idt_install(void) {
    idtp.limit = (sizeof(struct idt_entry) * 256) - 1;
    idtp.base = (unsigned int)&idt;
    for(int i=0; i<256; i++) idt_set_gate(i, 0, 0, 0);
    idt_set_gate(0, (unsigned int)isr0, 0x08, 0x8E);
    idt_set_gate(1, (unsigned int)isr1, 0x08, 0x8E);
    idt_set_gate(32, (unsigned int)irq0, 0x08, 0x8E);
    idt_set_gate(33, (unsigned int)irq1, 0x08, 0x8E);
    idt_set_gate(44, (unsigned int)irq12, 0x08, 0x8E);
    asm volatile("lidt %0" :: "m"(idtp));
}
void remap_pic(void) {
    // Mask all interrupts initially
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    outb(0x20, 0x11); outb(0xA0, 0x11); // Start initialization sequence
    outb(0x21, 0x20); outb(0xA1, 0x28); // Set PIC offsets (Master 0x20, Slave 0x28)
    outb(0x21, 0x04); outb(0xA1, 0x02); // Tell Master about Slave at IRQ2, Tell Slave its cascade identity
    outb(0x21, 0x01); outb(0xA1, 0x01); // Set 8086 mode
    
    // IRQs will be unmasked specifically in kernel_main
}
void irq_ack(unsigned char irq) {
    if(irq >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}
void irq0_handler(void) {
    ticks++;
    irq_ack(0);
}

// Echte Wartefunktion, unabhaengig von Interrupts (die sind waehrend eines
// Shell-Befehls sowieso gesperrt - ticks zaehlt dann NICHT mit, das war der
// Kern des "kein Zeitgefuehl"-Bugs). Liest stattdessen den PIT-Zaehler
// direkt per Port aus (Latch-Kommando 0x00 an Port 0x43, dann Port 0x40
// zweimal lesen = aktueller 16-Bit-Countdown-Wert). Der PIT laeuft mit
// Divisor 1193 (siehe pit_init) -> ca. 1193182/1193 = 1000 Hz, also ein
// Ueberlauf (Countdown startet wieder von vorn) pro Millisekunde. Wir
// zaehlen diese Ueberlaeufe direkt, statt uns auf den Interrupt zu
// verlassen, der waehrend eines Befehls ja gar nicht feuern kann.
void pit_wait_ms(unsigned int ms) {
    unsigned int elapsed = 0;
    unsigned short last = 0xFFFF;
    int first = 1;
    while(elapsed < ms) {
        outb(0x43, 0x00);
        unsigned char lo = inb(0x40);
        unsigned char hi = inb(0x40);
        unsigned short cur = (unsigned short)((hi << 8) | lo);
        if(!first && cur > last) elapsed++;
        last = cur;
        first = 0;
    }
}
