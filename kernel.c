#include "kernel.h"
#include "io.h"

void kernel_main(void) {
    unsigned int fb_addr, pitch, bpp;
    // Lese Framebuffer-Adresse, Pitch und BPP aus den Registern, die der Bootloader übergeben hat
    asm volatile("movl %%ebx, %0\n\t movl %%ecx, %1\n\t movl %%edx, %2"
                 : "=r"(fb_addr), "=r"(pitch), "=r"(bpp) : : "ebx", "ecx", "edx");
    vga_init(fb_addr, pitch, bpp);
    vga_clear();
    remap_pic();
    idt_install();
    pit_init();
    ps2_init();
    scheduler_init();
    fs_load();
    keyboard_load_layout();
    mouse_init();
    outb(0x21, 0xF8);
    outb(0xA1, 0xEF);
    asm volatile("sti");

    play_gooneros_animation();

    vga_print("GOonerOS v0.10 Full 780L\n");
    draw_arch_logo(900, 20, 330, 100, 0x1793D1);
    vga_print("VESA + ATA + Maus OK\n> ");
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
        scheduler_run();
        desktop_editor_update();

        if(ticks - last_blink_tick >= 300) {
            last_blink_tick = ticks;
            blink_visible = !blink_visible;
            // Nur zeichnen, wenn das Terminal gerade das fokussierte
            // Fenster ist - sonst blinkt der Cursor auf dem nackten
            // Desktop oder hinter einem anderen Fenster weiter.
            if(desktop_terminal_focused()) draw_cursor_bar(blink_visible);
        }
        if(ticks - last_desktop_tick >= 1000) {
            last_desktop_tick = ticks;
            desktop_tick();
        }
    }
}
