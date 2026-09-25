#include "kernel.h"
#include "io.h"

static int shift_down = 0;
static int extended_prefix = 0;

static unsigned char scancode_to_ascii(unsigned char sc, int shift) {
    // Deutsches QWERTZ-Layout (Set-1-Scancodes)
    static const unsigned char map_lo[64] = {
        0, 0, '1','2','3','4','5','6','7','8','9','0', 223,180,'\b','\t',
        'q','w','e','r','t','z','u','i','o','p', 252,'+','\n',0,'a','s',
        'd','f','g','h','j','k','l', 246,228,'^', 0, '#','y','x','c','v',
        'b','n','m',',','.','-', 0, '*', 0, ' ', 0,0,0,0,0,0,
    };
    static const unsigned char map_hi[64] = {
        0, 0, '!','"',167,'$','%','&','/','(',')','=', '?','`','\b','\t',
        'Q','W','E','R','T','Z','U','I','O','P', 220,'*','\n',0,'A','S',
        'D','F','G','H','J','K','L', 214,196,176, 0, '\'','Y','X','C','V',
        'B','N','M',';',':','_', 0, '*', 0, ' ', 0,0,0,0,0,0,
    };
    if(sc >= 64) return 0;
    return shift? map_hi[sc] : map_lo[sc];
}
void ps2_init(void) {
    // Beide Ports kurz deaktivieren, Ausgabepuffer leeren
    outb(0x64, 0xAD);
    outb(0x64, 0xA7);
    inb(0x60);

    // Konfigurationsbyte des PS/2-Controllers lesen
    outb(0x64, 0x20);
    unsigned char config = inb(0x60);

    // Bit 0 = IRQ1 (Tastatur) aktivieren, Bit 1 = IRQ12 (Maus) aktivieren,
    // Bit 4/5 (Takt-Deaktivierung beider Ports) sicherheitshalber löschen
    config |= 0x03;
    config &= ~0x30;

    outb(0x64, 0x60);
    outb(0x60, config);

    // Beide Ports wieder aktivieren
    outb(0x64, 0xAE); // Port 1 (Tastatur)
    outb(0x64, 0xA8); // Port 2 (Maus)
}

void draw_cursor_bar(int on) {
    int cx = prompt_x + cursor_col*8;
    if(on) {
        draw_rect(cx, prompt_y+13, 8, 2, ui_theme_color);
    } else if(cursor_col < input_idx) {
        draw_char(cx, prompt_y, input_buf[cursor_col], 0xFFFFFF, 0x000000);
    } else {
        draw_rect(cx, prompt_y+13, 8, 2, 0x000000);
    }
}
void redraw_input_line(void) {
    int ox, oy, w, h;
    vga_get_region(&ox, &oy, &w, &h);
    draw_rect(prompt_x, prompt_y, ox+w - prompt_x, 16, 0x000000);
    int x = prompt_x;
    int row = (prompt_y-oy)/16;
    for(int i = 0; i < input_idx; i++) {
        draw_char(x, prompt_y, input_buf[i], 0xFFFFFF, 0x000000);
        text_buffer_put(row, (x-ox)/8, input_buf[i]);
        x += 8;
    }
    for(int col = (x-ox)/8; col < TEXT_COLS; col++) text_buffer_put(row, col, 0);
    text_x = prompt_x + input_idx*8; text_y = prompt_y;
    blink_visible = 1;
    last_blink_tick = ticks;
    draw_cursor_bar(1);
}

static void keyboard_handler(void) {
    unsigned char sc = inb(0x60);
    // Ein anderes Fenster liegt obenauf, oder es ist gar kein Terminal
    // offen -> Tasten werden verworfen. Der Desktop ist damit kein reiner
    // "interaktiver Hintergrund fuers Terminal" mehr - das Terminal muss
    // wie jedes andere Fenster erst fokussiert (angeklickt) sein, um
    // Eingaben zu bekommen.
    int focused = desktop_terminal_focused();

    if(sc == 0xE0) { extended_prefix = 1; irq_ack(1); return; }

    if(extended_prefix) {
        extended_prefix = 0;
        if(focused && !(sc & 0x80)) {
            if(sc == 0x4B && cursor_col > 0) { // Pfeil links
                draw_cursor_bar(0); cursor_col--; draw_cursor_bar(1);
                blink_visible = 1; last_blink_tick = ticks;
            } else if(sc == 0x4D && cursor_col < input_idx) { // Pfeil rechts
                draw_cursor_bar(0); cursor_col++; draw_cursor_bar(1);
                blink_visible = 1; last_blink_tick = ticks;
            }
        }
        irq_ack(1);
        return;
    }

    // Shift-Zustand wird bewusst IMMER mitverfolgt (auch unfokussiert),
    // damit er nicht "haengen bleibt", wenn man waehrend gedrueckter
    // Shift-Taste den Fokus wechselt.
    if(sc == 0x2A || sc == 0x36) { shift_down = 1; irq_ack(1); return; }
    if(sc == 0xAA || sc == 0xB6) { shift_down = 0; irq_ack(1); return; }
    if(sc & 0x80) { irq_ack(1); return; }

    if(!focused) { irq_ack(1); return; }

    if(sc == 0x1C) { // Enter
        draw_cursor_bar(0);
        text_x = prompt_x + input_idx*8; text_y = prompt_y;
        vga_putc('\n');
        handle_command();
        irq_ack(1);
        return;
    }
    if(sc == 0x0E) { // Backspace: loescht das Zeichen VOR der Cursor-Position
        if(cursor_col > 0) {
            for(int i = cursor_col-1; i < input_idx-1; i++) input_buf[i] = input_buf[i+1];
            input_idx--;
            cursor_col--;
            redraw_input_line();
        }
        irq_ack(1); return;
    }

    unsigned char c = scancode_to_ascii(sc, shift_down);
    if(c && input_idx < 63) {
        for(int i = input_idx; i > cursor_col; i--) input_buf[i] = input_buf[i-1];
        input_buf[cursor_col] = c;
        input_idx++;
        cursor_col++;
        redraw_input_line();
    }
    irq_ack(1);
}

void irq1_handler(void) { keyboard_handler(); }
