#include "kernel.h"
#include "io.h"

static volatile int mouse_x = 512;
static volatile int mouse_y = 384;
static volatile int mouse_left_down = 0;
static volatile int mouse_event_pending = 0;

static int cursor_x = 512;
static int cursor_y = 384;
static unsigned int cursor_backup[8 * 12];
static int cursor_backup_valid = 0;
static int cursor_hidden = 0;

void mouse_init(void) {
    while(inb(0x64) & 1) inb(0x60);
    outb(0x64, 0xD4); outb(0x60, 0xF6); inb(0x60);
    outb(0x64, 0xD4); outb(0x60, 0xF4); inb(0x60);
    while(inb(0x64) & 1) inb(0x60);
}

void mouse_handler(unsigned char data) {
    static unsigned char cycle;
    static unsigned char packet[3];

    if(cycle == 0 && (!(data & 0x08) || (data & 0xC0))) return;
    packet[cycle++] = data;
    if(cycle != 3) return;

    mouse_left_down = packet[0] & 1;
    mouse_x += (signed char)packet[1];
    mouse_y -= (signed char)packet[2];
    if(mouse_x < 0) mouse_x = 0;
    if(mouse_y < 0) mouse_y = 0;
    if(mouse_x > VESA_WIDTH - 8) mouse_x = VESA_WIDTH - 8;
    if(mouse_y > VESA_HEIGHT - 12) mouse_y = VESA_HEIGHT - 12;

    // IRQs only collect input. Framebuffer writes belong to the main loop.
    mouse_event_pending = 1;
    cycle = 0;
}

void irq12_handler(void) {
    mouse_handler(inb(0x60));
    irq_ack(12);
}

int mouse_get_x(void) { return mouse_x; }
int mouse_get_y(void) { return mouse_y; }
int mouse_left_pressed(void) { return mouse_left_down; }

int mouse_poll_event(int* x, int* y, int* left) {
    int pending;
    asm volatile("cli");
    pending = mouse_event_pending;
    if(pending) {
        *x = mouse_x;
        *y = mouse_y;
        *left = mouse_left_down;
        mouse_event_pending = 0;
    }
    asm volatile("sti");
    return pending;
}

void mouse_cursor_hide(void) {
    asm volatile("cli");
    if(cursor_backup_valid) {
        for(int y = 0; y < 12; y++)
            for(int x = 0; x < 8; x++)
                put_pixel(cursor_x + x, cursor_y + y, cursor_backup[y * 8 + x]);
        cursor_backup_valid = 0;
        cursor_hidden = 1;
    }
    asm volatile("sti");
}

void mouse_refresh_cursor(void) {
    asm volatile("cli");
    int x = mouse_x;
    int y = mouse_y;

    if(cursor_backup_valid && !cursor_hidden) {
        for(int row = 0; row < 12; row++)
            for(int col = 0; col < 8; col++)
                put_pixel(cursor_x + col, cursor_y + row, cursor_backup[row * 8 + col]);
    }

    if(x < 0) x = 0;
    if(y < 0) y = 0;
    if(x > VESA_WIDTH - 8) x = VESA_WIDTH - 8;
    if(y > VESA_HEIGHT - 12) y = VESA_HEIGHT - 12;

    for(int row = 0; row < 12; row++)
        for(int col = 0; col < 8; col++)
            cursor_backup[row * 8 + col] = get_pixel(x + col, y + row);
    cursor_x = x;
    cursor_y = y;
    cursor_backup_valid = 1;
    cursor_hidden = 0;
    draw_mouse(x, y, 0xFFFFFF);
    asm volatile("sti");
}
