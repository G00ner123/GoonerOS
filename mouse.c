#include "kernel.h"
#include "io.h"

static volatile int mouse_x = 512;
static volatile int mouse_y = 384;
static volatile int mouse_left_down = 0;
static volatile int mouse_event_pending = 0;

static int cursor_x = 512;
static int cursor_y = 384;
static unsigned int cursor_backup[12 * 12];
static int cursor_backup_valid = 0;
static int cursor_hidden = 0;
static int cursor_shape = 0;

static int ps2_wait_input_empty(void) {
    unsigned int timeout = 100000;
    while(timeout--) {
        if(!(inb(0x64) & 0x02)) return 1;
    }
    return 0;
}

static int ps2_wait_output_full(void) {
    unsigned int timeout = 100000;
    while(timeout--) {
        if(inb(0x64) & 0x01) return 1;
    }
    return 0;
}

static int mouse_send_command(unsigned char command) {
    for(int attempt = 0; attempt < 3; attempt++) {
        if(!ps2_wait_input_empty()) return 0;
        outb(0x64, 0xD4);
        if(!ps2_wait_input_empty()) return 0;
        outb(0x60, command);
        if(!ps2_wait_output_full()) return 0;
        unsigned char response = inb(0x60);
        if(response == 0xFA) return 1;
        if(response != 0xFE) return 0;
    }
    return 0;
}

int mouse_init(void) {
    for(int i = 0; i < 100000 && (inb(0x64) & 0x01); i++)
        inb(0x60);
    if(!mouse_send_command(0xF6)) return 0;
    return mouse_send_command(0xF4);
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
    if(mouse_x >= VESA_WIDTH) mouse_x = VESA_WIDTH - 1;
    if(mouse_y >= VESA_HEIGHT) mouse_y = VESA_HEIGHT - 1;

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

void mouse_set_cursor_shape(int shape) {
    if(shape < 0 || shape > 4 || cursor_shape == shape) return;
    cursor_shape = shape;
}

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

static void draw_resize_cursor(int x, int y) {
    static const char* horizontal[12] = {
        "............", "...##....##.", "..###..###..", ".####..####.",
        "##++++++++##", "##++++++++##", "##++++++++##", "##++++++++##",
        ".####..####.", "..###..###..", "...##....##.", "............"
    };
    static const char* vertical[12] = {
        "....##......", "...####.....", "..##++##....", "..##++##....",
        "....++......", "....++......", "....++......", "....++......",
        "..##++##....", "..##++##....", "...####.....", "....##......"
    };
    static const char* diagonal_down[12] = {
        "##..........", "#+#.........", ".#+#........", "..#++#......",
        "...#++#.....", "....#++#....", ".....#++#...", "......#++#..",
        ".......#++#.", "........#+##", ".........###", "..........##"
    };
    static const char* diagonal_up[12] = {
        "..........##", ".........###", "........#+##", ".......#++#.",
        "......#++#..", ".....#++#...", "....#++#....", "...#++#.....",
        "..#++#......", ".#+#........", "#+#.........", "##.........."
    };
    const char** pattern = cursor_shape == 1 ? horizontal
        : cursor_shape == 2 ? vertical
        : cursor_shape == 3 ? diagonal_down : diagonal_up;
    for(int row = 0; row < 12; row++) {
        for(int col = 0; col < 12; col++) {
            char pixel = pattern[row][col];
            if(pixel == '#') put_pixel(x+col, y+row, 0x52616E);
            else if(pixel == '+') put_pixel(x+col, y+row, 0xFFFFFF);
        }
    }
}

void mouse_cursor_hide(void) {
    asm volatile("cli");
    if(cursor_backup_valid) {
        for(int y = 0; y < 12; y++)
            for(int x = 0; x < 12; x++)
                put_pixel(cursor_x + x, cursor_y + y, cursor_backup[y * 12 + x]);
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
            for(int col = 0; col < 12; col++)
                put_pixel(cursor_x + col, cursor_y + row, cursor_backup[row * 12 + col]);
    }

    if(x < 0) x = 0;
    if(y < 0) y = 0;
    cursor_x = cursor_shape ? x - 5 : x;
    cursor_y = cursor_shape ? y - 5 : y;

    for(int row = 0; row < 12; row++)
        for(int col = 0; col < 12; col++)
            cursor_backup[row * 12 + col] = get_pixel(cursor_x + col, cursor_y + row);
    cursor_backup_valid = 1;
    cursor_hidden = 0;
    if(cursor_shape) draw_resize_cursor(cursor_x, cursor_y);
    else draw_mouse(x, y, 0xFFFFFF);
    asm volatile("sti");
}
