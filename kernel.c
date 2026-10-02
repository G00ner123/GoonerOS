#include "kernel.h"
#include "io.h"

static const char* kernel_panic_reason;
<<<<<<< HEAD
extern const unsigned char _binary_user_program_elf_start[];
extern const unsigned char _binary_user_program_elf_end[];

static void kernel_install_sample_elf(void) {
    struct fs_entry* existing = fs_find("hello.elf");
    if(existing && (fs_is_directory(existing) || existing->size)) return;
    unsigned int length = (unsigned int)_binary_user_program_elf_end -
                          (unsigned int)_binary_user_program_elf_start;
    if(!length || length > FS_MAX_FILE_BYTES ||
       !fs_write("hello.elf", (const char*)_binary_user_program_elf_start,
                 (int)length)) {
        vga_print(ui_text("Warning: could not install hello.elf\n",
                          "Warnung: hello.elf konnte nicht installiert werden\n"));
    }
}
=======
>>>>>>> 409f10ca7eb9b89ecee2ad93002bb7b6b9e15e54

void kernel_main(void) {
    unsigned int fb_addr, pitch, bpp;
    // Lese Framebuffer-Adresse, Pitch und BPP aus den Registern, die der Bootloader übergeben hat
    asm volatile("movl %%ebx, %0\n\t movl %%ecx, %1\n\t movl %%edx, %2"
                 : "=r"(fb_addr), "=r"(pitch), "=r"(bpp) : : "ebx", "ecx", "edx");
    gdt_install();
    vga_init(fb_addr, pitch, bpp);
    idt_install();
    int paging_ready = paging_init(fb_addr, pitch, VESA_HEIGHT);
    if(!paging_ready) {
        vga_print("Paging initialization failed\n");
        for(;;) asm volatile("hlt");
    }
    vga_clear();
    remap_pic();
    pit_init();
    ps2_init();
    scheduler_init();
    int filesystem_ready = fs_load();
    keyboard_load_layout();
    int boot_logo = desktop_boot_logo_enabled();
    int mouse_ready = mouse_init();
    outb(0x21, 0xF8);
    outb(0xA1, 0xEF);
    page_allocator_init();
    asm volatile("sti");
<<<<<<< HEAD
    if(filesystem_ready) kernel_install_sample_elf();

    if(desktop_boot_animation_enabled()) play_gooneros_animation();
=======

    play_gooneros_animation();
>>>>>>> 409f10ca7eb9b89ecee2ad93002bb7b6b9e15e54

    vga_print(ui_text("GOonerOS v0.10 Full 780L\n", "GOonerOS v0.10 Vollversion 780L\n"));
    if(boot_logo) draw_arch_logo(900, 20, 330, 100, 0x1793D1);
    if(filesystem_ready && mouse_ready)
        vga_print(ui_text("Paging, VESA, filesystem, and PS/2 mouse ready\n> ",
                          "Paging, VESA, Dateisystem und PS/2-Maus bereit\n> "));
    else if(filesystem_ready)
        vga_print(ui_text("VESA and filesystem ready; PS/2 mouse unavailable\n> ",
                          "VESA und Dateisystem bereit; PS/2-Maus nicht verfuegbar\n> "));
    else if(mouse_ready)
        vga_print(ui_text("VESA and mouse ready; disk persistence unavailable\n> ",
                          "VESA und Maus bereit; Datentraeger nicht verfuegbar\n> "));
    else
        vga_print(ui_text("VESA ready; disk persistence and PS/2 mouse unavailable\n> ",
                          "VESA bereit; Datentraeger und PS/2-Maus nicht verfuegbar\n> "));
    prompt_x = text_x; prompt_y = text_y;
    draw_cursor_bar(1);

    static unsigned int last_desktop_tick = 0;
    for(;;) {
        asm volatile("hlt");

        // Zeichenarbeit fuer Mausereignisse laeuft bewusst HIER (mit
        // aktivierten Interrupts), nicht im Maus-Interrupt selbst - siehe
        // Kommentar in mouse.c/desktop.c.
        int mx, my, mleft;
        if(mouse_poll_event(&mx, &my, &mleft))
            desktop_handle_mouse(mx, my, mleft);
        keyboard_poll();
<<<<<<< HEAD
=======
        scheduler_run();
>>>>>>> 409f10ca7eb9b89ecee2ad93002bb7b6b9e15e54
        desktop_editor_update();

        if(ticks - last_blink_tick >= 300) {
            last_blink_tick = ticks;
            blink_visible = !blink_visible;
            // Nur zeichnen, wenn das Terminal gerade das fokussierte
            // Fenster ist - sonst blinkt der Cursor auf dem nackten
            // Desktop oder hinter einem anderen Fenster weiter.
<<<<<<< HEAD
            if(desktop_terminal_focused())
                draw_cursor_bar(desktop_terminal_cursor_blink_enabled() ? blink_visible : 1);
=======
            if(desktop_terminal_focused()) draw_cursor_bar(blink_visible);
>>>>>>> 409f10ca7eb9b89ecee2ad93002bb7b6b9e15e54
        }
        if(ticks - last_desktop_tick >= 1000) {
            last_desktop_tick = ticks;
            desktop_tick();
        }
    }

}

static void panic_draw_text(int x, int y, const char* text, unsigned int color,
                            unsigned int background, int limit) {
    for(int i = 0; text && text[i] && i < limit; i++)
        draw_char(x+i*8, y, text[i], color, background);
}

static void panic_draw_hex(int x, int y, const char* label, unsigned int value) {
    static const char digits[] = "0123456789ABCDEF";
    panic_draw_text(x, y, label, 0xB8C4CF, 0x080B10, 12);
    x += (int)strlen(label) * 8;
    draw_char(x, y, '0', 0xFFFFFF, 0x080B10);
    draw_char(x+8, y, 'x', 0xFFFFFF, 0x080B10);
    for(int i = 0; i < 8; i++)
        draw_char(x+16+i*8, y, digits[(value >> (28-i*4)) & 0xF], 0xFFFFFF, 0x080B10);
}

static void panic_draw_large_text(int x, int y, const char* text,
                                  unsigned int color, unsigned int background) {
    for(int i = 0; text && text[i] && x+i*24 < VESA_WIDTH-24; i++)
        draw_char_scaled(x+i*24, y, text[i], 3, color, background);
}

static void kernel_panic_render(const char* message, unsigned int vector,
                                unsigned int error, unsigned int eip,
                                unsigned int cs, unsigned int address,
                                int exception) {
    asm volatile("cli" ::: "memory");
    draw_rect(0, 0, VESA_WIDTH, VESA_HEIGHT, 0x07090C);
    draw_rect(0, 0, VESA_WIDTH, 8, 0xE32636);
    draw_rect(0, 8, VESA_WIDTH, 78, 0x26090D);
    draw_rect(0, 86, VESA_WIDTH, 3, 0x8B1E2D);
    panic_draw_large_text(32, 20, "FATAL KERNEL PANIC", 0xFFFFFF, 0x26090D);
<<<<<<< HEAD
    panic_draw_text(32, 64, "Kernel panic - not syncing",
=======
    panic_draw_text(32, 64, "SYSTEM HALTED - REBOOT REQUIRED",
>>>>>>> 409f10ca7eb9b89ecee2ad93002bb7b6b9e15e54
                    0xFF737D, 0x26090D, 48);
    if(exception) {
        static const char* names[32] = {
            "Divide Error", "Debug", "Non-maskable interrupt", "Breakpoint",
            "Overflow", "Bounds Range", "Invalid Opcode", "Device Not Available",
            "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS",
            "Segment Not Present", "Stack-Segment Fault", "General Protection",
            "Page Fault", "Reserved Exception", "x87 Floating-Point Exception",
            "Alignment Check", "Machine Check", "SIMD Floating-Point Exception",
            "Virtualization Exception", "Control Protection Exception",
            "Reserved Exception", "Reserved Exception", "Reserved Exception",
            "Reserved Exception", "Reserved Exception", "Reserved Exception",
            "Hypervisor Injection Exception", "VMM Communication Exception",
            "Security Exception", "Reserved Exception"
        };
        static const char* reasons[32] = {
            "Integer divide by zero or quotient overflow.",
            "CPU debug trap; inspect the faulting instruction.",
            "Non-maskable hardware interrupt reached the kernel.",
            "Breakpoint exception was not handled by the kernel.",
            "Overflow trap was not handled by the kernel.",
            "Instruction accessed outside its declared bounds.",
            "CPU rejected an invalid or unsupported instruction.",
            "Kernel used a processor feature that is unavailable.",
            "CPU could not deliver an exception without corrupting state.",
            "Legacy coprocessor reported an invalid segment.",
            "Invalid task-state segment or task-state selector.",
            "Required code, data, or stack segment is not present.",
            "Invalid stack segment or stack access.",
            "Kernel violated a segment or privilege protection rule.",
            "Kernel accessed memory without a valid page mapping.",
            "Reserved processor exception vector was raised.",
            "Unrecoverable x87 floating-point exception.",
            "Unaligned memory access violated processor protection.",
            "CPU detected an unrecoverable hardware machine-check error.",
            "Unrecoverable SIMD floating-point exception.",
            "Unrecoverable virtualization exception.",
            "Kernel violated control-flow protection.",
            "Reserved processor exception vector was raised.",
            "Reserved processor exception vector was raised.",
            "Reserved processor exception vector was raised.",
            "Reserved processor exception vector was raised.",
            "Reserved processor exception vector was raised.",
            "Reserved processor exception vector was raised.",
            "Hypervisor injected an exception into the kernel.",
            "Virtual machine communication protocol failed.",
            "Processor security mechanism raised an exception.",
            "Reserved processor exception vector was raised."
        };
        const char* name = vector < 32 ? names[vector] : "Unknown CPU exception";
        const char* reason = vector < 32 ? reasons[vector] : "No handler exists for this exception vector.";
        draw_rect(24, 108, VESA_WIDTH-48, 98, 0x12171E);
        panic_draw_text(40, 120, "STOP CODE:", 0xFF737D, 0x12171E, 24);
        panic_draw_text(40, 148, name, 0xFFFFFF, 0x12171E, 72);
        panic_draw_text(40, 176, message ? message : reason,
                        0xB8C4CF, 0x12171E, 112);
        panic_draw_text(32, 224, "CPU EXCEPTION FRAME", 0xFF737D, 0x07090C, 40);
        panic_draw_hex(32, 252, "Vector: ", vector);
        panic_draw_hex(320, 252, "Error: ", error);
        panic_draw_hex(32, 280, "EIP: ", eip);
        panic_draw_hex(320, 280, "CS: ", cs);
        if(vector == 6) {
            panic_draw_text(32, 316,
                message ? "Trigger: intentional UD2 panic trap; vector 6 has no CPU error code."
                        : "Cause: processor rejected an invalid or unsupported instruction.",
                0xB8C4CF, 0x07090C, 112);
        } else if(vector == 14) {
            panic_draw_hex(32, 308, "CR2: ", address);
            panic_draw_text(32, 344,
                error & 1u ? "Fault: protection violation; page is present."
                           : "Fault: page is not present in the address space.",
                0xFFFFFF, 0x07090C, 112);
            panic_draw_text(32, 368,
                error & 2u ? "Access: write." : "Access: read.",
                0xB8C4CF, 0x07090C, 112);
            panic_draw_text(32, 392,
                error & 4u ? "Origin: user-mode access." : "Origin: supervisor/kernel access.",
                0xB8C4CF, 0x07090C, 112);
            if(error & 8u)
                panic_draw_text(32, 416, "Cause: reserved page-table bit was set.",
                                0xFF737D, 0x07090C, 112);
            if(error & 16u)
                panic_draw_text(32, 440, "Access originated from instruction fetch.",
                                0xFF737D, 0x07090C, 112);
        } else if(vector == 13) {
            panic_draw_text(32, 316,
                error & 2u ? "Selector source: IDT." :
                error & 4u ? "Selector source: LDT." : "Selector source: GDT.",
                0xB8C4CF, 0x07090C, 112);
            panic_draw_hex(32, 344, "Selector index: ", error >> 3);
        }
        panic_draw_text(32, 476,
            "No recovery was attempted: kernel state is unsafe to continue.",
            0xFFFFFF, 0x07090C, 112);
<<<<<<< HEAD
        panic_draw_text(32, 500,
            "No safe return path; the kernel is looping here until you reboot.",
            0xB8C4CF, 0x07090C, 112);
=======
>>>>>>> 409f10ca7eb9b89ecee2ad93002bb7b6b9e15e54
    } else {
        draw_rect(24, 108, VESA_WIDTH-48, 170, 0x12171E);
        panic_draw_text(40, 124, "FATAL KERNEL INTEGRITY FAILURE",
                        0xFF737D, 0x12171E, 80);
        panic_draw_text(40, 156, "PANIC REASON:", 0xFFFFFF, 0x12171E, 24);
        panic_draw_text(40, 184, message, 0xB8C4CF, 0x12171E, 112);
        panic_draw_text(40, 220,
            "Protected kernel state cannot be safely recovered.",
            0xFFFFFF, 0x12171E, 112);
        panic_draw_text(32, 308,
            "No recovery was attempted. Reboot the machine to continue.",
            0xFF737D, 0x07090C, 112);
<<<<<<< HEAD
        panic_draw_text(32, 336,
            "No safe return path; the kernel is looping here until you reboot.",
            0xB8C4CF, 0x07090C, 112);
=======
>>>>>>> 409f10ca7eb9b89ecee2ad93002bb7b6b9e15e54
    }
    for(;;) asm volatile("hlt");
}

void kernel_panic(const char* message) {
    kernel_panic_reason = message;
    asm volatile("ud2" ::: "memory");
    for(;;) asm volatile("hlt");
}

unsigned int kernel_exception_dispatch(unsigned int vector, unsigned int error,
                                       unsigned int eip, unsigned int cs,
                                       unsigned int address) {
    if((cs & 3u) == 3u) {
<<<<<<< HEAD
        return (unsigned int)user_fault_dispatch(error, eip, cs,
                                                  vector == 14 ? address : 0);
=======
        user_fault_dispatch(error, eip, cs, vector == 14 ? address : 0);
        return 1;
>>>>>>> 409f10ca7eb9b89ecee2ad93002bb7b6b9e15e54
    }
    kernel_panic_render(vector == 6 ? kernel_panic_reason : 0,
                        vector, error, eip, cs, address, 1);
    return 0;
}
