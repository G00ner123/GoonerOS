#include "kernel.h"
#include "io.h"

static int shift_state = 0;
static int caps_lock = 0;
static int num_lock = 1;
static int caps_key_down = 0;
static int num_key_down = 0;
static int extended_prefix = 0;
static int keyboard_layout = KEYBOARD_LAYOUT_DE;
static const char keyboard_layout_path[] = "goonkeys.cfg";
static volatile unsigned char keyboard_queue[256];
static volatile unsigned char keyboard_head = 0;
static volatile unsigned char keyboard_tail = 0;
static volatile int keyboard_overflow = 0;

static unsigned char scancode_to_ascii(unsigned char sc, int shift) {
    static const unsigned char de_lo[64] = {
        0, 0, '1','2','3','4','5','6','7','8','9','0', 223,180,'\b','\t',
        'q','w','e','r','t','z','u','i','o','p', 252,'+','\n',0,'a','s',
        'd','f','g','h','j','k','l', 246,228,'^', 0, '#','y','x','c','v',
        'b','n','m',',','.','-', 0, '*', 0, ' ', 0,0,0,0,0,0,
    };
    static const unsigned char de_hi[64] = {
        0, 0, '!','"',167,'$','%','&','/','(',')','=', '?','`','\b','\t',
        'Q','W','E','R','T','Z','U','I','O','P', 220,'*','\n',0,'A','S',
        'D','F','G','H','J','K','L', 214,196,176, 0, '\'','Y','X','C','V',
        'B','N','M',';',':','_', 0, '*', 0, ' ', 0,0,0,0,0,0,
    };
    static const unsigned char en_lo[64] = {
        0, 0, '1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
        'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,'a','s',
        'd','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c','v',
        'b','n','m',',','.','/',0,'*',0,' ',0,0,0,0,0,0,
    };
    static const unsigned char en_hi[64] = {
        0, 0,'!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
        'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,'A','S',
        'D','F','G','H','J','K','L',':','"','~',0,'|','Z','X','C','V',
        'B','N','M','<','>','?',0,'*',0,' ',0,0,0,0,0,0,
    };
    if(sc >= 64) return 0;
    const unsigned char* lower = keyboard_layout == KEYBOARD_LAYOUT_EN ? en_lo : de_lo;
    const unsigned char* upper = keyboard_layout == KEYBOARD_LAYOUT_EN ? en_hi : de_hi;
    unsigned char normal = lower[sc];
    int letter = (normal >= 'a' && normal <= 'z') ||
                 (keyboard_layout == KEYBOARD_LAYOUT_DE &&
                  (normal == 223 || normal == 252 || normal == 246 || normal == 228));
    if(letter) shift ^= caps_lock;
    return shift ? upper[sc] : normal;
}

static unsigned char keypad_to_ascii(unsigned char sc) {
    if(!num_lock) return 0;
    switch(sc) {
        case 0x47: return '7';
        case 0x48: return '8';
        case 0x49: return '9';
        case 0x4B: return '4';
        case 0x4C: return '5';
        case 0x4D: return '6';
        case 0x4F: return '1';
        case 0x50: return '2';
        case 0x51: return '3';
        case 0x52: return '0';
        case 0x53: return '.';
        case 0x4A: return '-';
        case 0x4E: return '+';
        default: return 0;
    }
}

int keyboard_get_layout(void) {
    return keyboard_layout;
}

int keyboard_set_layout(int layout) {
    if(layout != KEYBOARD_LAYOUT_DE && layout != KEYBOARD_LAYOUT_EN) return 0;
    keyboard_layout = layout;
    return 1;
}

void keyboard_load_layout(void) {
    char config[8];
    int length = fs_read(keyboard_layout_path, config, sizeof(config));
    if(length == 5 && config[0] == '1' && config[1] == ',' &&
       config[4] == '\n') {
        if(config[2] == 'd' && config[3] == 'e')
            keyboard_layout = KEYBOARD_LAYOUT_DE;
        else if(config[2] == 'e' && config[3] == 'n')
            keyboard_layout = KEYBOARD_LAYOUT_EN;
    }
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
    int cols = w / 8;
    if(cols > TEXT_COLS) cols = TEXT_COLS;
    for(int col = (x-ox)/8; col < cols; col++) text_buffer_put(row, col, 0);
    text_x = prompt_x + input_idx*8; text_y = prompt_y;
    blink_visible = 1;
    last_blink_tick = ticks;
    draw_cursor_bar(1);
}

static void keyboard_process_scancode(unsigned char sc) {
    // Ein anderes Fenster liegt obenauf, oder es ist gar kein Terminal
    // offen -> Tasten werden verworfen. Der Desktop ist damit kein reiner
    // "interaktiver Hintergrund fuers Terminal" mehr - das Terminal muss
    // wie jedes andere Fenster erst fokussiert (angeklickt) sein, um
    // Eingaben zu bekommen.
    int focused = desktop_terminal_focused();

    if(sc == 0xE0) { extended_prefix = 1; return; }

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
        return;
    }

    // Shift-Zustand wird bewusst IMMER mitverfolgt (auch unfokussiert),
    // damit er nicht "haengen bleibt", wenn man waehrend gedrueckter
    // Shift-Taste den Fokus wechselt.
    if(sc == 0x2A) { shift_state |= 1; return; }
    if(sc == 0x36) { shift_state |= 2; return; }
    if(sc == 0xAA) { shift_state &= ~1; return; }
    if(sc == 0xB6) { shift_state &= ~2; return; }
    if(sc == 0xBA) { caps_key_down = 0; return; }
    if(sc == 0xC5) { num_key_down = 0; return; }
    if(sc & 0x80) return;
    if(sc == 0x3A) {
        if(!caps_key_down) caps_lock = !caps_lock;
        caps_key_down = 1;
        return;
    }
    if(sc == 0x45) {
        if(!num_key_down) num_lock = !num_lock;
        num_key_down = 1;
        return;
    }

    if(desktop_editor_active()) {
        if(sc == 0x1C) desktop_editor_key(0, 2);
        else if(sc == 0x0E) desktop_editor_key(0, 1);
        else {
            unsigned char editor_c = keypad_to_ascii(sc);
            if(!editor_c) editor_c = scancode_to_ascii(sc, shift_state != 0);
            if(editor_c) desktop_editor_key(editor_c, 0);
        }
        return;
    }

    if(!focused) return;

    if(sc == 0x1C) { // Enter
        draw_cursor_bar(0);
        text_x = prompt_x + input_idx*8; text_y = prompt_y;
        vga_putc('\n');
        handle_command();
        return;
    }
    if(sc == 0x0E) { // Backspace: loescht das Zeichen VOR der Cursor-Position
        if(cursor_col > 0) {
            for(int i = cursor_col-1; i < input_idx-1; i++) input_buf[i] = input_buf[i+1];
            input_idx--;
            cursor_col--;
            redraw_input_line();
        }
        return;
    }

    unsigned char c = keypad_to_ascii(sc);
    if(!c) c = scancode_to_ascii(sc, shift_state != 0);
    if(c && input_idx < 63) {
        for(int i = input_idx; i > cursor_col; i--) input_buf[i] = input_buf[i-1];
        input_buf[cursor_col] = c;
        input_idx++;
        cursor_col++;
        redraw_input_line();
    }
}

void irq1_handler(void) {
    unsigned char sc = inb(0x60);
    unsigned char next = (unsigned char)(keyboard_head + 1);
    if(next != keyboard_tail) {
        keyboard_queue[keyboard_head] = sc;
        keyboard_head = next;
    } else {
        keyboard_overflow = 1;
    }
    irq_ack(1);
}

void keyboard_poll(void) {
    for(;;) {
        unsigned char sc;
        asm volatile("cli");
        if(keyboard_tail == keyboard_head) {
            int overflow = keyboard_overflow;
            keyboard_overflow = 0;
            if(overflow) {
                shift_state = 0;
                extended_prefix = 0;
                caps_key_down = 0;
                num_key_down = 0;
            }
            asm volatile("sti");
            if(overflow) vga_print("\n[Warning: keyboard input dropped (buffer full)]\n");
            return;
        }
        sc = keyboard_queue[keyboard_tail];
        keyboard_tail = (unsigned char)(keyboard_tail + 1);
        asm volatile("sti");
        keyboard_process_scancode(sc);
    }
}
