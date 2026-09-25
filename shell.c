#include "kernel.h"
#include "io.h"

char input_buf[64];
int input_idx = 0;
int cursor_col = 0;
int prompt_x = 0, prompt_y = 0;
unsigned int last_blink_tick = 0;
int blink_visible = 1;

static char cmd_buf[FS_MAX_FILE_BYTES];
static char cmd_buf2[FS_MAX_FILE_BYTES];
static char cmd_history[8][64];
static int cmd_history_count = 0;
static int last_draw_active = 0, last_draw_x, last_draw_y, last_draw_w, last_draw_h;

void print_int(int n) {
    char buf[16]; int i = 0;
    if(n == 0) { vga_putc('0'); return; }
    if(n < 0) { vga_putc('-'); n = -n; }
    while(n > 0) { buf[i++] = '0' + n % 10; n /= 10; }
    while(i--) vga_putc(buf[i]);
}

void reboot(void) {
    asm volatile("cli");
    unsigned char status;
    int timeout = 100000;
    do {
        status = inb(0x64);
        if(status & 1) inb(0x60); // wartende Daten abholen, damit sie nicht im Weg stehen
        timeout--;
    } while((status & 2) && timeout > 0); // warten bis Eingabepuffer (Bit 1) leer ist
    outb(0x64, 0xFE);
    for(;;) asm volatile("hlt");
}

void beep(void) {
    outb(0x61, inb(0x61)|3); // Enable speaker and gate 2
    outb(0x43,0xB6);        // Set PIT channel 2 to square wave generator
    outb(0x42,0xA9);        // Set frequency (low byte)
    outb(0x42,0x04);        // Set frequency (high byte) (4A9h = 1193180 / 1193 = ~1kHz)

    // Simple delay to make the beep audible for a short duration
    pit_wait_ms(150);

    outb(0x61, inb(0x61) & 0xFC); // Turn off speaker (clear bits 0 and 1)
}

int parse_int(char** p) {
    while(**p == ' ') (*p)++;
    int neg = 0;
    if(**p == '-') { neg = 1; (*p)++; }
    int n = 0;
    while(**p >= '0' && **p <= '9') { n = n*10 + (**p - '0'); (*p)++; }
    return neg ? -n : n;
}

void clear_last_draw(void) {
    if(last_draw_active) draw_rect(last_draw_x, last_draw_y, last_draw_w, last_draw_h, 0x000000);
}
void remember_draw(int x, int y, int w, int h) {
    last_draw_x = x; last_draw_y = y; last_draw_w = w; last_draw_h = h;
    last_draw_active = 1;
}
int get_ip(char* out) {
    (void)out;
    return 0; // Kein Netzwerkstack vorhanden -> aktuell nie verfuegbar
}
void handle_command(void) {
    input_buf[input_idx] = 0;
    if(input_idx > 0) {
        for(int i = 0; i < 63 && input_buf[i]; i++) cmd_history[cmd_history_count % 8][i] = input_buf[i];
        cmd_history[cmd_history_count % 8][input_idx < 63 ? input_idx : 63] = 0;
        cmd_history_count++;
    }
    if(strcmp(input_buf, "help") == 0) {
        vga_print("== System & Info ==               |           == Dateisystem ==\n");
        vga_print("help; info; version;              |         ls; cat; write; append;\n");
        vga_print("neofetch; uname; hostname;        |              touch; rm;\n");
        vga_print("whoami; uptime; arch;             |           chmod; type xyz;\n");
        vga_print("lscpu; meminfo; free;             |              stat; find;\n");
        vga_print("df; ps; top;                      |            sum; cksum; wc;\n");
        vga_print("env; id; groups;                  |            head; tail; pwd;\n");
        vga_print("who; battery; sensors;            |           search; wipe; tree;\n");
        vga_print("dmesg; vmstat; lsblk;             |              mv; cp; du\n");
        vga_print("mount; nproc; systemctl;          |                          \n");
        vga_print("journalctl; crontab; history;     |                          \n");
        vga_print("__________________________________|_________________________________\n");
        vga_print("== Text & Verarbeitung ==         |          == Mathe & Zufall ==\n");
        vga_print("echo; grep; sort;                 |               calc; seq;\n");
        vga_print("rev; tr; which;                   |                  fib;\n");
        vga_print("printf x; diff; less;             |             rand; genpass;\n");
        vga_print("more; nano; man;                  |               factorial;\n");
        vga_print("fortune; banner; colors;          |             factor; prime\n");
        vga_print("rot13; base64; hex;               |                          \n");
        vga_print("dec; ascii; chr                   |                          \n");
        vga_print("__________________________________|_________________________________\n");
        vga_print("== Grafik & Desktop ==            |              == Zeit ==\n");
        vga_print("draw ( circle/ rect);             |           time; date; clock;\n");
        vga_print("draw x y z;                       |              sleep; cal;\n");
        vga_print("logo; mintlogo; matrix;           |           (alarm) countdown\n");
        vga_print("ida; fire; snow;                  |                         \n");
        vga_print("sl; cls; clear                    |                         \n");
        vga_print("theme desktop; mouse; desktop;    |                         \n");
        vga_print("fullscreen; window                |                         \n");
        vga_print("                                  |                         \n");
        vga_print("__________________________________|_________________________________\n");
        vga_print("== System-Steuerung ==            |      == Netzwerk & Sonstiges ==\n");
        vga_print("reboot; shutdown; halt;           |          ping x; ifconfig;\n");
        vga_print("poweroff; logout; exit;           |            true; false;\n");
        vga_print("passwd; kill all; yes;            |            apt install x\n");
        vga_print("su; beep                          |              \n");

    }
    else if(strcmp(input_buf, "ls") == 0) {
        int any = 0;
        for(int i = 0; i < FS_MAX_FILES; i++) {
            if(fs_table[i].used) {
                vga_print(fs_table[i].name); vga_putc('\t');
                print_int((int)fs_table[i].size); vga_print(" B\n");
                any = 1;
            }
        }
        if(!any) vga_print("(leer)\n");
    }
    else if(strcmp(input_buf, "clear") == 0)
        vga_clear();
    else if(strncmp(input_buf, "echo ", 5) == 0) {
        vga_print(&input_buf[5]); vga_putc('\n');
    }
    else if(strncmp(input_buf, "cat ", 4) == 0) {
        if(fs_read(&input_buf[4], cmd_buf, sizeof(cmd_buf)) < 0) vga_print("File not found\n");
        else { vga_print(cmd_buf); vga_putc('\n'); }
    }
    else if(strncmp(input_buf, "write ", 6) == 0) {
        char* sp = &input_buf[6];
        while(*sp && *sp!= ' ') sp++;
        if(*sp) {
            *sp = 0;
            if(fs_write(&input_buf[6], sp+1, strlen(sp+1))) vga_print("OK\n");
            else vga_print("Dateitabelle voll\n");
        }
        else vga_print("Usage: write name data\n");
    }
    else if(strcmp(input_buf, "draw rm") == 0) {
        clear_last_draw();
        last_draw_active = 0;
        vga_print("Removed\n");
    }
    else if(strcmp(input_buf, "draw rectangle") == 0 || strcmp(input_buf, "draw rect") == 0) {
        clear_last_draw();
        draw_rect(512-60, 384-40, 120, 80, 0x00FF00);
        remember_draw(512-60, 384-40, 120, 80);
        vga_print("Drawn\n");
    }
    else if(strcmp(input_buf, "draw circle") == 0) {
        clear_last_draw();
        draw_circle_filled(512, 384, 60, 0x00FF00);
        remember_draw(512-60, 384-60, 121, 121);
        vga_print("Drawn\n");
    }
    else if(strncmp(input_buf, "draw ", 5) == 0) {
        char* p = &input_buf[5];
        while(*p == ' ') p++;
        if(!((*p >= '0' && *p <= '9') || *p == '-')) {
            vga_print("Usage: draw x y w h | draw rectangle | draw circle\n");
        } else {
            int x = parse_int(&p);
            int y = parse_int(&p);
            int w = parse_int(&p);
            int h = parse_int(&p);
            if(w <= 0) w = 100;
            if(h <= 0) h = 100;
            clear_last_draw();
            draw_rect(x, y, w, h, 0x00FF00);
            remember_draw(x, y, w, h);
            vga_print("Drawn at "); print_int(x); vga_putc(','); print_int(y);
            vga_print(" size "); print_int(w); vga_putc('x'); print_int(h); vga_putc('\n');
        }
    }
    else if(strcmp(input_buf, "draw") == 0)
        vga_print("Usage: draw x y w h | draw rectangle | draw circle\n");
    else if(strncmp(input_buf, "circle ", 7) == 0) {
        char* p = &input_buf[7];
        int x = parse_int(&p);
        int y = parse_int(&p);
        int r = parse_int(&p);
        if(r <= 0) r = 40;
        clear_last_draw();
        draw_circle_filled(x, y, r, 0x00FFFF);
        remember_draw(x-r, y-r, r*2+1, r*2+1);
        vga_print("Drawn\n");
    }
    else if(strncmp(input_buf, "pixel ", 6) == 0) {
        char* p = &input_buf[6];
        int x = parse_int(&p);
        int y = parse_int(&p);
        put_pixel(x, y, 0xFFFFFF);
        vga_print("OK\n");
    }
    else if(strcmp(input_buf, "uptime") == 0) {
        print_int(ticks/1000); vga_print("s\n");
    }
    else if(strcmp(input_buf, "rand") == 0) {
        // Einfacher Pseudozufall auf Basis der Timer-Ticks (kein Anspruch auf Kryptoqualitaet)
        unsigned int seed = ticks * 2654435761u + 12345;
        print_int((int)(seed % 100)); vga_putc('\n');
    }
    else if(strcmp(input_buf, "shutdown") == 0) {
        vga_print("Shutting down...\n");
        outw(0x604, 0x2000);
        outw(0xB004, 0x2000);
        for(;;) asm volatile("hlt");
    }
    else if(strcmp(input_buf, "version") == 0)
        vga_print("GOonerOS v0.10 Full 780L\n");
    else if(strcmp(input_buf, "neofetch") == 0) {
        vga_print("root@GoonerOS\n");
        vga_print("OS: GOonerOS v0.10\n");
        vga_print("Uptime: "); print_int(ticks/1000); vga_print("s\n");
        vga_print("Res: 1024x768\n");
    }
    else if(strcmp(input_buf, "mintlogo") == 0) {
        mint_visible = !mint_visible;
        draw_mint_logo(mint_visible);
        vga_print(mint_visible ? "Mint-Logo an\n" : "Mint-Logo aus\n");
    }
    else if(strcmp(input_buf, "info") == 0) {
        vga_print("=== GoonerOS Systeminfo ===\n");
        vga_print("Version: v0.10 Full 780L\n");
        vga_print("Aufloesung: 1024x768 ("); print_int(1024*768); vga_print(" Pixel)\n");
        vga_print("Uptime: "); print_int(ticks/1000); vga_print("s ("); print_int((int)ticks); vga_print(" Ticks)\n");
        vga_print("Groesse: ~3200 Zeilen C + Assembler\n");
        vga_print("  Boot: ~200 Zeilen Assembler\n");
        vga_print("  Linker: ~30 Zeilen Linker Script\n");
        vga_print("  Isr/Interrupts: ~125 Zeilen Assembler\n");
        vga_print("  Kernel ~2820 Zeilen C\n");
        vga_print("Modus: 32-Bit Protected Mode\n");
        vga_print("Framebuffer: "); print_int((int)vga_get_pitch()); vga_print(" Bytes/Zeile, "); print_int((int)vga_get_bpp()); vga_print(" bpp\n");
    }
    else if(strcmp(input_buf, "matrix") == 0) {
        static int col_y[VESA_WIDTH/8];
        static int col_speed[VESA_WIDTH/8];
        unsigned int seed = ((unsigned int)text_x << 16) ^ 98765u;
        clear_pixels_only();
        for(int c = 0; c < VESA_WIDTH/8; c++) {
            seed = seed*1103515245u + 12345u;
            col_y[c] = -(int)((seed >> 8) % 700);
            seed = seed*1103515245u + 12345u;
            col_speed[c] = 4 + (int)((seed >> 8) % 12); // 4-15 px pro Frame, unterschiedlich je Spalte
        }
        for(int frame = 0; frame < 260; frame++) {
            for(int c = 0; c < VESA_WIDTH/8; c++) {
                int x = c*8;
                int y = col_y[c];
                if(y >= 0 && y < VESA_HEIGHT) {
                    seed = seed*1103515245u + 12345u;
                    char ch = '0' + (seed % 10);
                    draw_char(x, y, ch, 0x00FF00, 0x000000);
                }
                int tail = y - 16*7;
                if(tail >= 0 && tail < VESA_HEIGHT) draw_char(x, tail, ' ', 0, 0);
                col_y[c] += col_speed[c];
                if(col_y[c] > VESA_HEIGHT + 16*7) {
                    seed = seed*1103515245u + 12345u;
                    col_y[c] = -(int)((seed >> 8) % 300);
                    seed = seed*1103515245u + 12345u;
                    col_speed[c] = 4 + (int)((seed >> 8) % 12);
                }
            }
            pit_wait_ms(40);
        }
        clear_pixels_only();
        draw_arch_logo(900, 20, 330, 100, 0x1793D1);
        if(mint_visible) draw_mint_logo(1);
        redraw_text_buffer();
    }
    else if(strcmp(input_buf, "gooneros") == 0) {
        play_gooneros_animation();
    }
    else if(strcmp(input_buf, "pwd") == 0)
        vga_print("/root\n");
    else if(strcmp(input_buf, "uname") == 0)
        vga_print("GoonerOS 0.10 i686\n");
    else if(strcmp(input_buf, "uname -a") == 0)
        vga_print("GoonerOS 0.10 GOonerOS-kernel i686 GNU/Nichts\n");
    else if(strcmp(input_buf, "hostname") == 0)
        vga_print("GoonerOS\n");
    else if(strcmp(input_buf, "free") == 0) {
        int used = heap_ptr - HEAP_START, total = 0x100000;
        vga_print("       total   used   free\n");
        vga_print("Mem:  "); print_int(total); vga_putc(' ');
        print_int(used); vga_putc(' '); print_int(total-used); vga_putc('\n');
    }
    else if(strcmp(input_buf, "df") == 0) {
        vga_print("Filesystem   Size  Used  Avail\n");
        vga_print("/dev/hda      512   "); print_int(512); vga_print("     0\n");
    }
    else if(strcmp(input_buf, "ps") == 0) {
        vga_print("PID CMD\n");
        vga_print("  1 kernel_main\n");
    }
    else if(strncmp(input_buf, "kill ", 5) == 0)
        vga_print("Kein Multitasking vorhanden - nichts zu killen\n");
    else if(strncmp(input_buf, "sleep ", 6) == 0) {
        char* p = &input_buf[6];
        int secs = parse_int(&p);
        if(secs < 0) secs = 0;
        if(secs > 20) secs = 20; // Sicherheitsbegrenzung
        for(int s = 0; s < secs; s++)
            pit_wait_ms(1000);
        vga_print("OK\n");
    }
    else if(strcmp(input_buf, "yes") == 0) {
        for(int i = 0; i < 15; i++) vga_print("y\n");
    }
    else if(strcmp(input_buf, "cal") == 0) {
        unsigned char s,m,h,d,mo,y,s2,m2,h2,d2,mo2,y2;
        do { read_rtc_raw(&h,&m,&s,&d,&mo,&y); read_rtc_raw(&h2,&m2,&s2,&d2,&mo2,&y2); }
        while(h!=h2||m!=m2||s!=s2||d!=d2||mo!=mo2||y!=y2);
        if(!(cmos_read(0x0B) & 0x04)) { d = bcd_to_bin(d); mo = bcd_to_bin(mo); y = bcd_to_bin(y); }
        vga_print("Heute: "); print_int(d); vga_putc('.'); print_int(mo); vga_print(".20"); print_int(y); vga_putc('\n');
    }
    else if(strcmp(input_buf, "su") == 0)
        vga_print("Du bist schon root\n");
    else if(strcmp(input_buf, "passwd") == 0)
        vga_print("Kein Passwort noetig!\n");
    else if(strcmp(input_buf, "lscpu") == 0) {
        vga_print("Architecture: i686\n");
        vga_print("Mode: 32-bit Protected Mode\n");
        vga_print("CPU(s): 1\n");
    }
    else if(strcmp(input_buf, "top") == 0) {
        vga_print("PID CMD          MEM\n");
        vga_print("  1 kernel_main  "); print_int(heap_ptr-HEAP_START); vga_print(" B\n");
        vga_print("Uptime: "); print_int(ticks/1000); vga_print("s\n");
    }
    else if(strcmp(input_buf, "env") == 0) {
        vga_print("OS=GoonerOS\n");
        vga_print("USER=root\n");
        vga_print("HOME=/root\n");
    }
    else if(strcmp(input_buf, "history") == 0) {
        int start = cmd_history_count > 8 ? cmd_history_count - 8 : 0;
        for(int i = start; i < cmd_history_count; i++) {
            print_int(i+1); vga_putc(' '); vga_print(cmd_history[i % 8]); vga_putc('\n');
        }
    }
    else if(strncmp(input_buf, "head ", 5) == 0 || strncmp(input_buf, "tail ", 5) == 0) {
        int is_tail = (input_buf[0] == 't');
        if(fs_read(&input_buf[5], cmd_buf, sizeof(cmd_buf)) < 0) { vga_print("File not found\n"); }
        else {
            int len = strlen(cmd_buf);
            if(!is_tail) {
                int lines = 0, i = 0;
                while(i < len && lines < 5) { if(cmd_buf[i] == '\n') lines++; i++; }
                char saved = cmd_buf[i]; cmd_buf[i] = 0;
                vga_print(cmd_buf); vga_putc('\n');
                cmd_buf[i] = saved;
            } else {
                int lines = 0, i = len;
                while(i > 0 && lines < 5) { i--; if(cmd_buf[i] == '\n') lines++; }
                if(i > 0) i++;
                vga_print(&cmd_buf[i]); vga_putc('\n');
            }
        }
    }
    else if(strncmp(input_buf, "wc ", 3) == 0) {
        if(fs_read(&input_buf[3], cmd_buf, sizeof(cmd_buf)) < 0) { vga_print("File not found\n"); }
        else {
            int chars = 0, words = 0, lines = 0, in_word = 0;
            for(int i = 0; cmd_buf[i]; i++) {
                chars++;
                if(cmd_buf[i] == '\n') lines++;
                if(cmd_buf[i] != ' ' && cmd_buf[i] != '\n') { if(!in_word) { words++; in_word = 1; } }
                else in_word = 0;
            }
            print_int(lines); vga_putc(' '); print_int(words); vga_putc(' '); print_int(chars); vga_putc('\n');
        }
    }
    else if(strncmp(input_buf, "touch ", 6) == 0) {
        if(fs_create(&input_buf[6])) vga_print("OK\n");
        else vga_print("Dateitabelle voll\n");
    }
    else if(strncmp(input_buf, "rm ", 3) == 0) {
        if(fs_delete(&input_buf[3])) vga_print("Removed\n");
        else vga_print("File not found\n");
    }
    else if(strncmp(input_buf, "cp ", 3) == 0) {
        char* sp = &input_buf[3];
        while(*sp && *sp != ' ') sp++;
        if(*sp) {
            *sp = 0;
            if(fs_read(&input_buf[3], cmd_buf, sizeof(cmd_buf)) < 0) vga_print("File not found\n");
            else if(fs_write(sp+1, cmd_buf, strlen(cmd_buf))) vga_print("OK\n");
            else vga_print("Dateitabelle voll\n");
        } else vga_print("Usage: cp quelle ziel\n");
    }
    else if(strncmp(input_buf, "mv ", 3) == 0) {
        char* sp = &input_buf[3];
        while(*sp && *sp != ' ') sp++;
        if(*sp) {
            *sp = 0;
            struct fs_entry* e = fs_find(&input_buf[3]);
            if(!e) vga_print("File not found\n");
            else {
                int j = 0; while(sp[1+j] && j < FS_NAME_LEN-1) { e->name[j] = sp[1+j]; j++; }
                e->name[j] = 0;
                fs_save();
                vga_print("OK\n");
            }
        } else vga_print("Usage: mv alt neu\n");
    }
    else if(strncmp(input_buf, "stat ", 5) == 0) {
        struct fs_entry* e = fs_find(&input_buf[5]);
        if(!e) vga_print("File not found\n");
        else {
            vga_print("Name: "); vga_print(e->name); vga_putc('\n');
            vga_print("Groesse: "); print_int((int)e->size); vga_print(" B\n");
            vga_print("Start-LBA: "); print_int((int)e->start_lba); vga_putc('\n');
        }
    }
    else if(strncmp(input_buf, "find ", 5) == 0) {
        int any = 0;
        for(int i = 0; i < FS_MAX_FILES; i++) {
            if(fs_table[i].used) {
                int match = 1;
                for(int j = 0; input_buf[5+j]; j++)
                    if(!fs_table[i].name[j] || fs_table[i].name[j] != input_buf[5+j]) { match = 0; break; }
                if(match) { vga_print(fs_table[i].name); vga_putc('\n'); any = 1; }
            }
        }
        if(!any) vga_print("Nichts gefunden\n");
    }
    else if(strcmp(input_buf, "du") == 0) {
        int total = 0;
        for(int i = 0; i < FS_MAX_FILES; i++) if(fs_table[i].used) total += (int)fs_table[i].size;
        vga_print("Belegt: "); print_int(total); vga_print(" B in "); print_int((int)fs_sb.file_count); vga_print(" Dateien\n");
    }
    else if(strncmp(input_buf, "chmod ", 6) == 0)
        vga_print("GoonerOS kennt noch keine Dateirechte - jeder darf alles\n");
    else if(strncmp(input_buf, "type ", 5) == 0)
        vga_print("eingebauter Shell-Befehl\n");
    else if(strncmp(input_buf, "xxd ", 4) == 0) {
        if(fs_read(&input_buf[4], cmd_buf, sizeof(cmd_buf)) < 0) vga_print("File not found\n");
        else {
            int len = strlen(cmd_buf);
            char hx[] = "0123456789abcdef";
            for(int i = 0; i < len; i++) {
                unsigned char b = cmd_buf[i];
                vga_putc(hx[(b>>4)&0xF]); vga_putc(hx[b&0xF]); vga_putc(' ');
                if(i % 16 == 15) vga_putc('\n');
            }
            vga_putc('\n');
        }
    }
    else if(strncmp(input_buf, "sum ", 4) == 0) {
        if(fs_read(&input_buf[4], cmd_buf, sizeof(cmd_buf)) < 0) vga_print("File not found\n");
        else {
            unsigned int s = 0;
            for(int i = 0; cmd_buf[i]; i++) s = s*31 + (unsigned char)cmd_buf[i];
            print_int((int)s); vga_putc('\n');
        }
    }
    else if(strcmp(input_buf, "fortune") == 0) {
        const char* quotes[] = {
            "Ein guter Kernel bootet beim ersten Versuch. Meiner nicht immer.\n",
            "Es gibt kein Problem, das nicht durch mehr Interrupts geloest werden kann.\n",
            "Triple Fault: die Art des Computers zu sagen 'nochmal von vorn'.\n",
            "Echte OS-Entwickler debuggen mit blinkenden LEDs statt printf.\n",
            "Ein Bootloader ist nur ein sehr, sehr kurzes Betriebssystem.\n"
        };
        unsigned int seed = ticks*2654435761u + (unsigned int)text_x;
        vga_print(quotes[seed % 5]);
    }
    else if(strncmp(input_buf, "banner ", 7) == 0) {
        char* msg = &input_buf[7];
        int len = strlen(msg);
        if(len > 10) len = 10; // Bildschirmbreite begrenzen
        int scale = 3, char_w = 8*scale;
        int bx = (VESA_WIDTH - len*char_w)/2, by = 300;
        clear_last_draw();
        draw_rect(bx-10, by-10, len*char_w+20, 16*scale+20, 0x000000);
        for(int i = 0; i < len; i++)
            draw_char_scaled(bx+i*char_w, by, msg[i], scale, 0x55FFAA, 0x000000);
        remember_draw(bx-10, by-10, len*char_w+20, 16*scale+20);
    }
    else if(strcmp(input_buf, "colors") == 0) {
        unsigned int pal[] = {0xFF0000,0x00FF00,0x0000FF,0xFFFF00,0xFF00FF,0x00FFFF,0xFFFFFF,0xFF8800};
        clear_last_draw();
        for(int i = 0; i < 8; i++) draw_rect(400+i*30, 300, 28, 40, pal[i]);
        remember_draw(400, 300, 8*30, 40);
    }
    else if(strncmp(input_buf, "grep ", 5) == 0) {
        char* sp = &input_buf[5];
        while(*sp && *sp != ' ') sp++;
        if(!*sp) { vga_print("Usage: grep wort datei\n"); }
        else {
            *sp = 0;
            if(fs_read(sp+1, cmd_buf, sizeof(cmd_buf)) < 0) vga_print("File not found\n");
            else {
                int start = 0, any = 0;
                for(int i = 0; ; i++) {
                    if(cmd_buf[i] == '\n' || cmd_buf[i] == 0) {
                        char saved = cmd_buf[i]; cmd_buf[i] = 0;
                        if(my_strstr(&cmd_buf[start], &input_buf[5])) { vga_print(&cmd_buf[start]); vga_putc('\n'); any = 1; }
                        cmd_buf[i] = saved;
                        start = i+1;
                        if(cmd_buf[i] == 0) break;
                    }
                }
                if(!any) vga_print("(keine Treffer)\n");
            }
        }
    }
    else if(strncmp(input_buf, "sort ", 5) == 0) {
        if(fs_read(&input_buf[5], cmd_buf, sizeof(cmd_buf)) < 0) vga_print("File not found\n");
        else {
            char* lines[40]; int n = 0, start = 0;
            int len = strlen(cmd_buf);
            for(int i = 0; i <= len && n < 40; i++) {
                if(cmd_buf[i] == '\n' || cmd_buf[i] == 0) {
                    cmd_buf[i] = 0;
                    lines[n++] = &cmd_buf[start];
                    start = i+1;
                }
            }
            for(int i = 0; i < n-1; i++)
                for(int j = 0; j < n-1-i; j++)
                    if(strcmp(lines[j], lines[j+1]) > 0) { char* t = lines[j]; lines[j] = lines[j+1]; lines[j+1] = t; }
            for(int i = 0; i < n; i++) { vga_print(lines[i]); vga_putc('\n'); }
        }
    }
    else if(strncmp(input_buf, "rev ", 4) == 0) {
        int len = strlen(&input_buf[4]);
        for(int i = len-1; i >= 0; i--) vga_putc(input_buf[4+i]);
        vga_putc('\n');
    }
    else if(strncmp(input_buf, "tr ", 3) == 0) {
        char a = input_buf[3], b = input_buf[5];
        char* text = &input_buf[7];
        for(int i = 0; text[i]; i++) vga_putc(text[i] == a ? b : text[i]);
        vga_putc('\n');
    }
    else if(strncmp(input_buf, "which ", 6) == 0 || strncmp(input_buf, "whereis ", 8) == 0)
        vga_print("eingebauter Shell-Befehl\n");
    else if(strcmp(input_buf, "id") == 0)
        vga_print("uid=0(root) gid=0(root)\n");
    else if(strcmp(input_buf, "groups") == 0)
        vga_print("root\n");
    else if(strcmp(input_buf, "arch") == 0)
        vga_print("i686\n");
    else if(strcmp(input_buf, "dmesg") == 0) {
        vga_print("[0.000] GoonerOS boot\n");
        vga_print("[0.010] VESA Grafik initialisiert\n");
        vga_print("[0.020] PS/2 Controller bereit\n");
        vga_print("[0.030] ATA Festplatte erkannt\n");
        vga_print("[0.040] Dateisystem geladen\n");
    }
    else if(strcmp(input_buf, "vmstat") == 0) {
        vga_print("procs  mem\n");
        vga_print("   1   "); print_int(heap_ptr-HEAP_START); vga_print(" B belegt\n");
    }
    else if(strcmp(input_buf, "lsblk") == 0)
        vga_print("hda  512M  ATA-Festplatte\n");
    else if(strcmp(input_buf, "mount") == 0)
        vga_print("/dev/hda on / type goonerfs\n");
    else if(strcmp(input_buf, "nproc") == 0)
        vga_print("1\n");
    else if(strncmp(input_buf, "printf ", 7) == 0)
        vga_print(&input_buf[7]);
    else if(strcmp(input_buf, "true") == 0)
        {}
    else if(strcmp(input_buf, "false") == 0)
        {}
    else if(strcmp(input_buf, "logout") == 0 || strcmp(input_buf, "halt") == 0 || strcmp(input_buf, "poweroff") == 0) {
        vga_print("Shutting down...\n");
        outw(0x604, 0x2000);
        outw(0xB004, 0x2000);
        for(;;) asm volatile("hlt");
    }
    else if(strcmp(input_buf, "sl") == 0) {
        const char* train = "o==[GoonerOS]==o  ";
        int tlen = strlen(train);
        int y = 400;
        clear_last_draw();
        for(int x = VESA_WIDTH; x > -tlen*8; x -= 24) {
            draw_rect(0, y, VESA_WIDTH, 20, 0x000000);
            for(int i = 0; i < tlen; i++) {
                int cx = x + i*8;
                if(cx >= 0 && cx < VESA_WIDTH) draw_char(cx, y, train[i], 0xFFAA00, 0x000000);
            }
            pit_wait_ms(18);
        }
        draw_rect(0, y, VESA_WIDTH, 20, 0x000000);
    }
    else if(strncmp(input_buf, "diff ", 5) == 0) {
        char* sp = &input_buf[5];
        while(*sp && *sp != ' ') sp++;
        if(!*sp) vga_print("Usage: diff datei1 datei2\n");
        else {
            *sp = 0;
            int r1 = fs_read(&input_buf[5], cmd_buf, sizeof(cmd_buf));
            int r2 = fs_read(sp+1, cmd_buf2, sizeof(cmd_buf2));
            if(r1 < 0 || r2 < 0) vga_print("File not found\n");
            else if(strcmp(cmd_buf, cmd_buf2) == 0) vga_print("Identisch\n");
            else vga_print("Unterschiedlich\n");
        }
    }
    else if(strncmp(input_buf, "less ", 5) == 0 || strncmp(input_buf, "more ", 5) == 0) {
        char* name = &input_buf[5];
        if(fs_read(name, cmd_buf, sizeof(cmd_buf)) < 0) vga_print("File not found\n");
        else { vga_print(cmd_buf); vga_putc('\n'); }
    }
    else if(strcmp(input_buf, "nano") == 0 || strcmp(input_buf, "vi") == 0 || strcmp(input_buf, "vim") == 0)
        vga_print("Kein Texteditor vorhanden - nutze 'write name text'\n");
    else if(strncmp(input_buf, "apt ", 4) == 0 || strncmp(input_buf, "pacman ", 7) == 0)
        vga_print("Kein Paketmanager vorhanden\n");
    else if(strcmp(input_buf, "battery") == 0)
        vga_print("Kein Akku vorhanden (virtuelle Maschine)\n");
    else if(strcmp(input_buf, "cls") == 0)
        vga_clear();
    else if(strncmp(input_buf, "calc ", 5) == 0) {
        char* p = &input_buf[5];
        int a = parse_int(&p);
        while(*p == ' ') p++;
        char op = *p; p++;
        int b = parse_int(&p);
        if(op == '+') { print_int(a+b); vga_putc('\n'); }
        else if(op == '-') { print_int(a-b); vga_putc('\n'); }
        else if(op == '*') { print_int(a*b); vga_putc('\n'); }
        else if(op == '/') {
            if(b != 0) { print_int(a/b); vga_putc('\n'); }
            else vga_print("Division durch 0\n");
        }
        else vga_print("Unbekannter Operator (+ - * /)\n");
    }
    else if(strcmp(input_buf, "logo") == 0)
        draw_arch_logo(900, 20, 330, 100, 0x1793D1);
    else if(strcmp(input_buf, "whoami") == 0) {
        // GoonerOS hat noch keinen Netzwerkstack -> es gibt aktuell nie eine echte IP.
        // get_ip() gibt 0 zurueck solange das so ist; sobald Networking existiert, hier einhaengen.
        char ip[16];
        if(get_ip(ip)) { vga_print("root@GoonerOS ("); vga_print(ip); vga_print(")\n"); }
        else vga_print("Root@GoonerOS\n");
    }
    else if(strcmp(input_buf, "exit") == 0) {
        vga_print("Shutting down...\n");
        outw(0x604, 0x2000);  // ACPI-Shutdown-Trick, funktioniert bei den meisten QEMU-Standardkonfigurationen
        outw(0xB004, 0x2000); // Fallback fuer manche QEMU-Versionen
        for(;;) asm volatile("hlt");
    }
    else if(strcmp(input_buf, "time") == 0 || strcmp(input_buf, "date") == 0)
        print_time_date();
    else if(strcmp(input_buf, "reboot") == 0) {
        vga_print("Rebooting...\n"); reboot();
    }
    else if(strcmp(input_buf, "meminfo") == 0) {
        vga_print("Heap used: ");
        char buf[16]; int n = heap_ptr - HEAP_START;
        int i=0; if(n==0) buf[i++]='0'; while(n>0){buf[i++]='0'+n%10;n/=10;}
        while(i--) {
    vga_putc(buf[i]);
}
vga_print(" bytes\n");
    }
    else if(strcmp(input_buf, "beep") == 0) {
        vga_print("Beep!\n"); beep();
    }
    /* ==================== Neue "echte" Befehle ==================== */
    else if(strncmp(input_buf, "seq ", 4) == 0) {
        char* p = &input_buf[4];
        int n = parse_int(&p);
        if(n < 1) n = 1;
        if(n > 200) n = 200; // Sicherheitsbegrenzung, kein Rollen ins Unendliche
        for(int i = 1; i <= n; i++) { print_int(i); vga_putc('\n'); }
    }
    else if(strncmp(input_buf, "factor ", 7) == 0) {
        char* p = &input_buf[7];
        int n = parse_int(&p);
        print_int(n); vga_print(":");
        if(n < 2) vga_putc('\n');
        else {
            int d = 2;
            while(n > 1) {
                while(n % d == 0) { vga_putc(' '); print_int(d); n /= d; }
                d++;
                if(d*d > n && n > 1) { vga_putc(' '); print_int(n); n = 1; }
            }
            vga_putc('\n');
        }
    }
    else if(strncmp(input_buf, "prime ", 6) == 0) {
        char* p = &input_buf[6];
        int n = parse_int(&p);
        int is_p = (n >= 2);
        for(int d = 2; d*d <= n && is_p; d++) if(n % d == 0) is_p = 0;
        print_int(n); vga_print(is_p ? " ist eine Primzahl\n" : " ist keine Primzahl\n");
    }
    else if(strncmp(input_buf, "fib ", 4) == 0) {
        char* p = &input_buf[4];
        int n = parse_int(&p);
        if(n < 0) n = 0;
        if(n > 45) n = 45; // 32-Bit int laeuft danach ueber
        int a = 0, b = 1;
        for(int i = 0; i < n; i++) { int t = a+b; a = b; b = t; }
        print_int(a); vga_putc('\n');
    }
    else if(strncmp(input_buf, "factorial ", 10) == 0) {
        char* p = &input_buf[10];
        int n = parse_int(&p);
        if(n < 0) n = 0;
        if(n > 12) n = 12; // 13! passt nicht mehr in 32 Bit
        unsigned int r = 1;
        for(int i = 2; i <= n; i++) r *= i;
        print_int((int)r); vga_putc('\n');
    }
    else if(strncmp(input_buf, "rot13 ", 6) == 0) {
        char* p = &input_buf[6];
        for(int i = 0; p[i]; i++) {
            char c = p[i];
            if(c >= 'a' && c <= 'z') c = 'a' + (c - 'a' + 13) % 26;
            else if(c >= 'A' && c <= 'Z') c = 'A' + (c - 'A' + 13) % 26;
            vga_putc(c);
        }
        vga_putc('\n');
    }
    else if(strncmp(input_buf, "base64 ", 7) == 0) {
        static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        char* p = &input_buf[7];
        int len = strlen(p);
        for(int i = 0; i < len; i += 3) {
            unsigned int b0 = (unsigned char)p[i];
            unsigned int b1 = (i+1 < len) ? (unsigned char)p[i+1] : 0;
            unsigned int b2 = (i+2 < len) ? (unsigned char)p[i+2] : 0;
            unsigned int n = (b0 << 16) | (b1 << 8) | b2;
            vga_putc(tbl[(n >> 18) & 0x3F]);
            vga_putc(tbl[(n >> 12) & 0x3F]);
            vga_putc((i+1 < len) ? tbl[(n >> 6) & 0x3F] : '=');
            vga_putc((i+2 < len) ? tbl[n & 0x3F] : '=');
        }
        vga_putc('\n');
    }
    else if(strncmp(input_buf, "hex ", 4) == 0) {
        char* p = &input_buf[4];
        int n = parse_int(&p);
        unsigned int u = (unsigned int)n;
        char buf[9]; const char* digits = "0123456789abcdef";
        for(int i = 7; i >= 0; i--) { buf[i] = digits[u & 0xF]; u >>= 4; }
        vga_print("0x");
        for(int i = 0; i < 8; i++) vga_putc(buf[i]);
        vga_putc('\n');
    }
    else if(strncmp(input_buf, "dec ", 4) == 0) {
        char* p = &input_buf[4];
        while(*p == ' ') p++;
        if(p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        unsigned int v = 0;
        while(*p) {
            char c = *p; int digit = -1;
            if(c >= '0' && c <= '9') digit = c - '0';
            else if(c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if(c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            if(digit < 0) break;
            v = v*16 + digit;
            p++;
        }
        print_int((int)v); vga_putc('\n');
    }
    else if(strncmp(input_buf, "ascii ", 6) == 0) {
        print_int((int)(unsigned char)input_buf[6]); vga_putc('\n');
    }
    else if(strncmp(input_buf, "chr ", 4) == 0) {
        char* p = &input_buf[4];
        int n = parse_int(&p);
        if(n < 0) n = 0;
        if(n > 255) n = 255;
        vga_putc((char)n); vga_putc('\n');
    }
    else if(strncmp(input_buf, "man ", 4) == 0) {
        char* c = &input_buf[4];
        if(strcmp(c, "ls") == 0) vga_print("ls - zeigt alle gespeicherten Dateien mit Groesse\n");
        else if(strcmp(c, "help") == 0) vga_print("help - zeigt jeden existierenden Befehl an\n");
        else if(strcmp(c, "countdown") == 0) vga_print("countdown - zählt von einer angegebenen Zahl runter, zb: countdown 10\n");
        else if(strcmp(c, "apt install") == 0) vga_print("apt install - kann nichts installieren, es gibt noch keinen Paketmanager\nzb: apt install firefox\n");
        else if(strcmp(c, "ping") == 0) vga_print("ping - kann noch nichts pingen, es gibt noch kein Internet, zb: ping archlinux.org\n");
        else if(strcmp(c, "window") == 0) vga_print("window - zeigt einen prototypen für den Desktop\n");
        else if(strcmp(c, "draw") == 0) vga_print("Draw - draw rectangle erzeugt ein grünes Rechteck in der mitte des Bildschirms\ndraw circle erzeugt einen grünen Kreis in der mitte des Bildschirms\ndraw x y z erzeugt ein Rechteck/quadrat an den angegebenen Koordinaten\n");
        else if(strcmp(c, "stat") == 0) vga_print("stat - zeigt den Aktuellen Status einer Datei an,\nzb: stat test -> zeigt die groesse der Date an\n");
        else if(strcmp(c, "wipe") == 0) vga_print("wipe - loescht und entfernt jede Datei im system\n");
        else if(strcmp(c, "tail") == 0) vga_print("tail - zeigt die letzte zeile einer Datei an\n");
        else if(strcmp(c, "head") == 0) vga_print("head - zeigt die erste zeile einer Datei an\n");
        else if(strcmp(c, "du") == 0) vga_print("du - zeigt die insgesamt durch dateien verbrauchten Bits\n");
        else if(strcmp(c, "cksum") == 0) vga_print("cksum - erzeugt eine Prüfnummer mit modernem 32-Bit-CRC\nSehr hohe Zuverlaessigkeit\n");
        else if(strcmp(c, "sum") == 0) vga_print("sum - erzeugt eine Prüfnummer mit altem 16-Bit Verfahren,\nGeringe Zuverlaessigkeit\n");
        else if(strcmp(c, "touch") == 0) vga_print("touch - erstellt eine leere Datei oder Aktualisiert diese\n");
        else if(strcmp(c, "cp") == 0) vga_print("cp - kopiert Dateien von Punkt A zu Punkt B\n");
        else if(strcmp(c, "append") == 0) vga_print("append - hängt text an das ende der jeweiligen Datei an\n");
        else if(strcmp(c, "echo") == 0) vga_print("echo - gibt angehaengten Text in der nächsten Zeile aus\n");
        else if(strcmp(c, "find") == 0) vga_print("find - Durchsucht das System nach Dateien\n");
        else if(strcmp(c, "search") == 0) vga_print("search - durchsucht jede Datei nach angegebenen Text\n");
        else if(strcmp(c, "tree") == 0) vga_print("tree - zeigt Ordner und Dateien als Stammbaum an\n");
        else if(strcmp(c, "wc") == 0) vga_print("wc - zaehlt Zeilen, Woerter und Bytes in einer Datei\n");
        else if(strcmp(c, "seq") == 0) vga_print("seq - zaehlt bis zur angegebenen Zahl\nLimit ist 200\n");
        else if(strcmp(c, "cat") == 0) vga_print("cat DATEI - gibt den Inhalt einer Datei aus\n");
        else if(strcmp(c, "write") == 0) vga_print("write DATEI text - schreibt TEXT in DATEI\n");
        else if(strcmp(c, "calc") == 0) vga_print("calc A OP B - Taschenrechner (+ - * /)\n");
        else if(strcmp(c, "seq") == 0) vga_print("seq N - zaehlt von 1 bis N hoch\n");
        else if(strcmp(c, "factor") == 0) vga_print("factor N - Primfaktorzerlegung von N\n");
        else if(strcmp(c, "rand") == 0) vga_print("rand - erzeugt eine zufaellige Zahl\n");
        else if(strcmp(c, "ida") == 0) vga_print("ida - malt ein rotes Herz, das nach 10s verschwindet\n");
        else if(strcmp(c, "fib") == 0) vga_print("fib - berechnet die N-te Zahl der Fibonacci-Folge\njede Zahl ist die Summe ihrer beiden Vorgaenger\n");
        else if(strcmp(c, "genpass") == 0) vga_print("genpass - generiert ein zufaelliges und sicheres Passwort\n");
        else if(strcmp(c, "factorial") == 0) vga_print("factorial - berechnet die Fakultät einer Zahl\nzb: factorial 5 = 5x4x3x2x1 = 120\n");
        else if(strcmp(c, "lsblk") == 0) vga_print("lsblk - zeigt alle Datenträger\n");
        else if(strcmp(c, "sensors") == 0) vga_print("sensors - zeigt CPU Temperatur\n");
        else if(strcmp(c, "journalctl") == 0) vga_print("journalctl - zeigt Protokolle und Fehlermeldungen des System\n");
        else if(strcmp(c, "mouse") == 0) vga_print("mouse - zeigt die aktuellen Maus Koordinaten an\n");
        else if(strcmp(c, "banner") == 0) vga_print("banner - zeigt gewünschten Text groß in der mitte des Bildschirms\n");
        else if(strcmp(c, "sl") == 0) vga_print("sl - lässt einen schnellen Zug von rechts nach links fahren\n");
        else if(strcmp(c, "cls") == 0) vga_print("cls - leert den Bildschirm, genau wie clear\n");
        else { vga_print("Kein Handbucheintrag fuer '"); vga_print(c); vga_print("'\n"); }
    }
    else if(strcmp(input_buf, "who") == 0)
        vga_print("root     tty1         GoonerOS\n");
    else if(strcmp(input_buf, "tree") == 0) {
        vga_print("/root\n");
        int any = 0;
        for(int i = 0; i < FS_MAX_FILES; i++)
            if(fs_table[i].used) { vga_print("|-- "); vga_print(fs_table[i].name); vga_putc('\n'); any = 1; }
        if(!any) vga_print("(leer)\n");
    }
    else if(strncmp(input_buf, "cksum ", 6) == 0) {
        if(fs_read(&input_buf[6], cmd_buf, sizeof(cmd_buf)) < 0) vga_print("File not found\n");
        else {
            unsigned int c = 0xFFFFFFFFu;
            for(int i = 0; cmd_buf[i]; i++) c = (c ^ (unsigned char)cmd_buf[i]) * 16777619u;
            print_int((int)c); vga_putc('\n');
        }
    }
    else if(strncmp(input_buf, "ping ", 5) == 0) {
        vga_print("PING "); vga_print(&input_buf[5]); vga_print(": kein Netzwerkstack vorhanden\n");
    }
    else if(strcmp(input_buf, "ifconfig") == 0)
        vga_print("lo: Loopback, keine echte Netzwerkkarte vorhanden\n");
    else if(strcmp(input_buf, "sensors") == 0)
        vga_print("CPU-Temp: 42 C (virtuelle Maschine, immer im gruenen Bereich)\n");
    else if(strncmp(input_buf, "systemctl", 9) == 0) {
        vga_print("kernel.service   loaded active running GoonerOS Kernel\n");
        vga_print("shell.service    loaded active running GoonerOS Shell\n");
    }
    else if(strcmp(input_buf, "journalctl") == 0) {
        vga_print("[boot] GoonerOS gestartet\n");
        vga_print("[ok] VESA Grafik initialisiert\n");
        vga_print("[ok] PS/2 Controller bereit\n");
        vga_print("[ok] ATA Festplatte erkannt\n");
        vga_print("[ok] Dateisystem geladen\n");
    }
    else if(strncmp(input_buf, "crontab", 7) == 0)
        vga_print("Keine Cronjobs eingetragen\n");

    /* ==================== Optische Befehle ==================== */
    else if(strcmp(input_buf, "ida") == 0) {
        clear_last_draw();
        int cx = VESA_WIDTH/2, cy = VESA_HEIGHT/2;
        int scale = 7;
        draw_heart(cx, cy, scale, 0xFF1744);
        remember_draw(cx-8*scale, cy-8*scale, 16*scale, 16*scale);
        for(int s = 0; s < 10; s++)
            pit_wait_ms(100); // ~2 Sekunden stehen lassen
        int sc = scale;
        while(sc > 0) {
            draw_heart(cx, cy, sc, 0x000000); // aktuelle Grösse loeschen
            sc--;
            if(sc > 0) draw_heart(cx, cy, sc, 0xFF1744); // nächstkleinere Grösse
            pit_wait_ms(80);
        }
        last_draw_active = 0;
        redraw_text_buffer();
    }
    else if(strcmp(input_buf, "fire") == 0) {
        clear_last_draw();
        unsigned int seed = ticks*2654435761u + 777u;
        unsigned int colors[] = {0x330000,0x992200,0xFF5500,0xFFAA00,0xFFFF66};
        int base = VESA_HEIGHT, fh = 140;
        for(int frame = 0; frame < 150; frame++) {
            for(int x = 0; x < VESA_WIDTH; x += 8) {
                seed = seed*1103515245u + 12345u;
                int h = 20 + (int)((seed >> 8) % 100);
                int idx = h/20; if(idx > 4) idx = 4;
                draw_rect(x, base-h, 8, h, colors[idx]);
                draw_rect(x, base-fh, 8, fh-h, 0x000000);
            }
            pit_wait_ms(35);
        }
        draw_rect(0, base-fh, VESA_WIDTH, fh, 0x000000);
        redraw_text_buffer();
    }
    else if(strcmp(input_buf, "snow") == 0) {
        clear_pixels_only();
        unsigned int seed = ticks + 99u;
        int fx[60], fy[60];
        for(int i = 0; i < 60; i++) {
            seed = seed*1103515245u + 12345u; fx[i] = (seed>>8) % VESA_WIDTH;
            seed = seed*1103515245u + 12345u; fy[i] = (seed>>8) % VESA_HEIGHT;
        }
        for(int frame = 0; frame < 220; frame++) {
            for(int i = 0; i < 60; i++) {
                put_pixel(fx[i], fy[i], 0x000000);
                fy[i] += 2;
                seed = seed*1103515245u + 12345u;
                fx[i] += (int)((seed>>8) % 3) - 1;
                if(fx[i] < 0) fx[i] = 0;
                if(fx[i] >= VESA_WIDTH) fx[i] = VESA_WIDTH-1;
                if(fy[i] >= VESA_HEIGHT) {
                    fy[i] = 0;
                    seed = seed*1103515245u + 12345u;
                    fx[i] = (seed>>8) % VESA_WIDTH;
                }
                put_pixel(fx[i], fy[i], 0xFFFFFF);
            }
            pit_wait_ms(35);
        }
        clear_pixels_only();
        redraw_text_buffer();
    }
    else if(strcmp(input_buf, "clock") == 0) {
        // Echtzeit-Uhr: liest die RTC jede Runde neu und zeichnet nur bei
        // Sekundenwechsel neu. Läuft ~60s, dann automatisch zurück zum
        // Prompt - währenddessen ist die Shell blockiert (siehe Hinweistext).
        int scale = 6, char_w = 8*scale, len = 8;
        int gx = (VESA_WIDTH - len*char_w) / 2, gy = (VESA_HEIGHT - 16*scale) / 2;
        clear_last_draw();
        draw_rect(gx-20, gy-20, len*char_w+40, 16*scale+40, 0x000000);
        remember_draw(gx-20, gy-20, len*char_w+40, 16*scale+40);
        vga_print("Live-Uhr, 60s (Shell blockiert solange - danach normal weiter)\n");
        char last_buf[9] = {0};
        for(int frame = 0; frame < 300; frame++) {
            unsigned char s,m,h,d,mo,y,s2,m2,h2,d2,mo2,y2;
            do { read_rtc_raw(&h,&m,&s,&d,&mo,&y); read_rtc_raw(&h2,&m2,&s2,&d2,&mo2,&y2); }
            while(h!=h2||m!=m2||s!=s2||d!=d2||mo!=mo2||y!=y2);
            if(!(cmos_read(0x0B) & 0x04)) {
                h = bcd_to_bin(h & 0x7F) | (h & 0x80); m = bcd_to_bin(m); s = bcd_to_bin(s);
            }
            h &= 0x7F;
            char buf[9];
            buf[0]='0'+h/10; buf[1]='0'+h%10; buf[2]=':';
            buf[3]='0'+m/10; buf[4]='0'+m%10; buf[5]=':';
            buf[6]='0'+s/10; buf[7]='0'+s%10; buf[8]=0;
            if(strcmp(buf, last_buf) != 0) {
                for(int i = 0; i < 8; i++)
                    draw_char_scaled(gx+i*char_w, gy, last_buf[i] ? last_buf[i] : ' ', scale, 0x000000, 0x000000);
                for(int i = 0; i < 8; i++)
                    draw_char_scaled(gx+i*char_w, gy, buf[i], scale, ui_theme_color, 0x000000);
                for(int i = 0; i < 9; i++) last_buf[i] = buf[i];
            }
            pit_wait_ms(200);
        }
        draw_rect(gx-20, gy-20, len*char_w+40, 16*scale+40, 0x000000);
        last_draw_active = 0;
        redraw_text_buffer();
    }

    /* ==================== Desktop-Vorkehrungen ==================== */
    else if(strncmp(input_buf, "theme ", 6) == 0) {
        char* c = &input_buf[6];
        unsigned int newc = 0; int ok = 1;
        if(strcmp(c, "cyan") == 0) newc = 0x00FFAA;
        else if(strcmp(c, "blue") == 0) newc = 0x3399FF;
        else if(strcmp(c, "red") == 0) newc = 0xFF4444;
        else if(strcmp(c, "green") == 0) newc = 0x55FF55;
        else if(strcmp(c, "purple") == 0) newc = 0xAA55FF;
        else if(strcmp(c, "orange") == 0) newc = 0xFFAA00;
        else ok = 0;
        if(ok) { ui_theme_color = newc; vga_print("Theme geaendert\n"); }
        else vga_print("Farben: cyan blue red green purple orange\n");
    }
    else if(strcmp(input_buf, "window") == 0) {
        clear_last_draw();
        draw_window(300, 200, 400, 250, "Demo-Fenster");
        remember_draw(300, 200, 400, 250);
        vga_print("Baustein fuer den kommenden Desktop - noch nicht beweglich\n");
    }
    else if(strcmp(input_buf, "mouse") == 0) {
        vga_print("X: "); print_int(mouse_get_x());
        vga_print("  Y: "); print_int(mouse_get_y());
        vga_print("  Links: "); vga_print(mouse_left_pressed() ? "gedrueckt\n" : "los\n");
    }

    /* ==================== Weitere nuetzliche Befehle ==================== */
    else if(strncmp(input_buf, "append ", 7) == 0) {
        char* sp = &input_buf[7];
        while(*sp && *sp != ' ') sp++;
        if(*sp) {
            *sp = 0;
            int old_len = fs_read(&input_buf[7], cmd_buf, sizeof(cmd_buf));
            if(old_len < 0) old_len = 0;
            char* add = sp+1;
            int add_len = strlen(add);
            int total = old_len + add_len;
            if(total > FS_MAX_FILE_BYTES-1) total = FS_MAX_FILE_BYTES-1;
            for(int i = 0; old_len+i < total; i++) cmd_buf[old_len+i] = add[i];
            cmd_buf[total] = 0;
            if(fs_write(&input_buf[7], cmd_buf, total)) vga_print("OK\n");
            else vga_print("Dateitabelle voll\n");
        } else vga_print("Usage: append datei text\n");
    }
    else if(strncmp(input_buf, "search ", 7) == 0) {
        char* needle = &input_buf[7];
        int any = 0;
        for(int i = 0; i < FS_MAX_FILES; i++) {
            if(!fs_table[i].used) continue;
            if(fs_read(fs_table[i].name, cmd_buf, sizeof(cmd_buf)) < 0) continue;
            if(my_strstr(cmd_buf, needle)) { vga_print(fs_table[i].name); vga_putc('\n'); any = 1; }
        }
        if(!any) vga_print("Nichts gefunden\n");
    }
    else if(strncmp(input_buf, "alarm ", 6) == 0) {
        char* p = &input_buf[6];
        int secs = parse_int(&p);
        if(secs < 0) secs = 0;
        if(secs > 30) secs = 30; // Sicherheitsbegrenzung, Shell ist waehrenddessen blockiert
        vga_print("Warte "); print_int(secs); vga_print("s...\n");
        for(int s = 0; s < secs; s++)
            pit_wait_ms(1000);
        vga_print("Alarm!\n");
        beep(); beep();
    }
    else if(strncmp(input_buf, "countdown ", 10) == 0) {
        char* p = &input_buf[10];
        int n = parse_int(&p);
        if(n < 1) n = 1;
        if(n > 20) n = 20;
        int scale = 8, char_w = 8*scale;
        int gx = VESA_WIDTH/2 - char_w, gy = VESA_HEIGHT/2 - 16*scale/2;
        clear_last_draw();
        draw_rect(gx-30, gy-20, char_w*2+60, 16*scale+40, 0x000000);
        remember_draw(gx-30, gy-20, char_w*2+60, 16*scale+40);
        for(int i = n; i >= 1; i--) {
            draw_rect(gx, gy, char_w*2, 16*scale, 0x000000);
            char buf[3];
            if(i >= 10) { buf[0] = '0'+i/10; buf[1] = '0'+i%10; buf[2] = 0; }
            else { buf[0] = '0'+i; buf[1] = 0; }
            int blen = strlen(buf);
            int bx = gx + (2-blen)*char_w/2;
            for(int k = 0; k < blen; k++)
                draw_char_scaled(bx + k*char_w, gy, buf[k], scale, 0xFF4444, 0x000000);
            pit_wait_ms(1000);
        }
        draw_rect(gx-30, gy-20, char_w*2+60, 16*scale+40, 0x000000);
        last_draw_active = 0;
        vga_print("Los!\n");
        beep();
        redraw_text_buffer();
    }
    else if(strcmp(input_buf, "genpass") == 0) {
        static const char* charset = "abcdefghijkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789!@#$%";
        unsigned int seed = ticks*2654435761u + 13579u;
        for(int i = 0; i < 12; i++) {
            seed = seed*1103515245u + 12345u;
            vga_putc(charset[(seed >> 8) % 61]);
        }
        vga_putc('\n');
    }
    else if(strcmp(input_buf, "wipe") == 0) {
        fs_sb.magic = FS_MAGIC;
        fs_sb.file_count = 0;
        fs_sb.next_free_lba = 0;
        for(int i = 0; i < FS_MAX_FILES; i++) fs_table[i].used = 0;
        fs_save();
        vga_print("Dateisystem geleert\n");
    }
    else if(strcmp(input_buf, "desktop") == 0) {
        desktop_enter();
    }
    else if(strcmp(input_buf, "fullscreen") == 0) {
        if(desktop_active) desktop_fullscreen();
        else vga_print("Schon im Vollbild\n");
    }
    else if(strcmp(input_buf, "dih") == 0) {
        vga_print("          _____\n         /  |  |\n         (     )\n         |     |\n         |     |\n         |     |\n         |     |\n         |     |\n");
        vga_print("         |     |\n         |     |\n         |     |\n      __-_     _-_\n     /     `´     )\n    (              )\n");
        vga_print("     (      |      /\n      -____/|_____-\n");
    }
    else if(strcmp(input_buf,"man") == 0) {
        vga_print("man - das Handbuch\nKombiniere man und den gesuchten Befehl um eine nutzungsanleitung zu bekommen\n");
    }
    else if(strcmp(input_buf, "fasfetch") == 0) {
        vga_print("Versuch fastfetch oder neofetch!\n");
    }
    /*else if(strcmp(input_buf, "test") == 0) {
        vga_print("hej\ntest\ntest\ntest\ntest\n");
    }
    else if(strcmp(input_buf, "banane") == 0) {
        vga_print("banane gegessen!");
    }*/
    else if(input_idx > 0) {
        vga_print("Unbekannt: ");
        vga_print(input_buf); vga_putc('\n');
    }
    input_idx = 0;
    cursor_col = 0;
    vga_print("> ");
    prompt_x = text_x; prompt_y = text_y;
    blink_visible = 1; last_blink_tick = ticks;
    draw_cursor_bar(1);
}
