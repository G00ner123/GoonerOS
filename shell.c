#include "kernel.h"
#include "io.h"

char input_buf[64];
int input_idx = 0;
int cursor_col = 0;
int prompt_x = 0, prompt_y = 0;
unsigned int last_blink_tick = 0;
int blink_visible = 1;

static char cmd_buf[FS_MAX_FILE_BYTES+1];
static char cmd_buf2[FS_MAX_FILE_BYTES+1];
static char cmd_history[8][64];
static int cmd_history_count = 0;
static int last_draw_active = 0, last_draw_x, last_draw_y, last_draw_w, last_draw_h;
static char fs_cwd[FS_NAME_LEN];

static int resolve_fs_path(const char* input, char out[FS_NAME_LEN]) {
    int out_len = 0, pos = 0;
    if(!input || !input[0]) return 0;
    out[0] = 0;
    if(input[0] != '/') {
        while(fs_cwd[out_len]) { out[out_len] = fs_cwd[out_len]; out_len++; }
        out[out_len] = 0;
    } else {
        while(input[pos] == '/') pos++;
    }
    while(input[pos]) {
        while(input[pos] == '/') pos++;
        if(!input[pos]) break;
        int start = pos;
        while(input[pos] && input[pos] != '/') pos++;
        int len = pos - start;
        if(len == 1 && input[start] == '.') continue;
        if(len == 2 && input[start] == '.' && input[start+1] == '.') {
            if(out_len) {
                while(out_len > 0 && out[out_len-1] != '/') out_len--;
                if(out_len > 0) out_len--;
                out[out_len] = 0;
            }
            continue;
        }
        int required = len + (out_len ? 1 : 0);
        if(len == 0 || out_len + required >= FS_NAME_LEN) return 0;
        if(out_len) out[out_len++] = '/';
        for(int i = 0; i < len; i++) out[out_len++] = input[start+i];
        out[out_len] = 0;
    }
    return 1;
}

static const char* fs_basename(const char* path) {
    const char* base = path;
    for(int i = 0; path[i]; i++) if(path[i] == '/') base = &path[i+1];
    return base;
}

static void list_directory(const char* path, int long_format, int show_hidden) {
    struct fs_entry* target = path[0] ? fs_find(path) : 0;
    if(target && !fs_is_directory(target)) {
        if(long_format) {
            vga_print("- "); print_int((int)target->size); vga_print(" B  "); vga_print(fs_basename(target->name)); vga_putc('\n');
        } else {
            vga_print(fs_basename(target->name)); vga_putc('\n');
        }
        return;
    }
    if(path[0] && !fs_is_directory(target)) {
        vga_print("ls: directory not found\n");
        return;
    }
    int any = 0;
    if(long_format) vga_print("TYPE  SIZE      NAME\n");
    for(int i = 0; i < FS_MAX_FILES; i++) {
        struct fs_entry* entry = &fs_table[i];
        if(!entry->used || !fs_is_direct_child(entry, path)) continue;
        const char* basename = fs_basename(entry->name);
        if(!show_hidden && basename[0] == '.') continue;
        if(long_format) {
            vga_print(fs_is_directory(entry) ? "DIR   " : "FILE  ");
            print_int((int)entry->size); vga_print(" B     ");
        }
        vga_print(basename);
        if(fs_is_directory(entry)) vga_putc('/');
        vga_putc('\n');
        any = 1;
    }
    if(!any) vga_print("(leer)\n");
}

static void print_working_directory(void) {
    vga_print("/");
    vga_print(fs_cwd);
    vga_putc('\n');
}

static void shell_prompt(void) {
    vga_putc('[');
    vga_print(fs_cwd[0] ? fs_cwd : "root");
    vga_print("] > ");
}

static int make_directories(const char* path) {
    char prefix[FS_NAME_LEN];
    int len = strlen(path);
    for(int i = 0; i <= len; i++) {
        if(path[i] != '/' && path[i] != 0) continue;
        if(i == 0) return 0;
        for(int j = 0; j < i; j++) prefix[j] = path[j];
        prefix[i] = 0;
        struct fs_entry* entry = fs_find(prefix);
        if(entry) {
            if(!fs_is_directory(entry)) return 0;
        } else if(!fs_mkdir(prefix)) return 0;
    }
    return 1;
}

static void leave_removed_directory(const char* path) {
    int len = strlen(path);
    if(strncmp(fs_cwd, path, len) != 0 ||
       (fs_cwd[len] != 0 && fs_cwd[len] != '/')) return;
    int parent_len = len;
    while(parent_len > 0 && path[parent_len-1] != '/') parent_len--;
    if(parent_len > 0) parent_len--;
    for(int i = 0; i < parent_len; i++) fs_cwd[i] = path[i];
    fs_cwd[parent_len] = 0;
}

static void move_working_directory(const char* old_path, const char* new_path) {
    int old_len = strlen(old_path);
    if(strncmp(fs_cwd, old_path, old_len) != 0 ||
       (fs_cwd[old_len] != 0 && fs_cwd[old_len] != '/')) return;
    char updated[FS_NAME_LEN];
    int new_len = strlen(new_path), suffix_len = strlen(&fs_cwd[old_len]);
    if(new_len + suffix_len >= FS_NAME_LEN) { fs_cwd[0] = 0; return; }
    int i = 0;
    while(new_path[i]) { updated[i] = new_path[i]; i++; }
    for(int j = 0; j < suffix_len; j++) updated[i++] = fs_cwd[old_len+j];
    updated[i] = 0;
    i = 0;
    while(updated[i]) { fs_cwd[i] = updated[i]; i++; }
    fs_cwd[i] = 0;
}

static int read_file_from_cwd(const char* name, char* out, int capacity) {
    char path[FS_NAME_LEN];
    if(!resolve_fs_path(name, path) || !path[0]) return -1;
    return fs_read(path, out, capacity);
}

static void goonfetch_prefix(const char* logo) {
    vga_set_text_color(0x39DCCB);
    int col = 0;
    while(logo[col]) { vga_putc(logo[col]); col++; }
    while(col < 24) { vga_putc(' '); col++; }
    vga_set_text_color(0xA9FFF4);
}

static void goonfetch_two_digits(unsigned char value) {
    if(value < 10) vga_putc('0');
    print_int(value);
}

static void print_hex32(unsigned int value) {
    static const char digits[] = "0123456789ABCDEF";
    for(int shift = 28; shift >= 0; shift -= 4)
        vga_putc(digits[(value >> shift) & 0x0F]);
}

static void goonfetch(void) {
    vga_print("\n\n");
    static const char* logo[] = {
        "      .-GOONER-.",
        "     /  .---.  \\",
        "    |  / ___ \\  |",
        "    | | |   | | |",
        "    | | |OS | | |",
        "    | | |___| | |",
        "    |  \\_____/  |",
        "     \\         /",
        "      '-------'",
        "        /|\\",
        "       /_|_\\",
        "     GOONER OS"
    };
    unsigned int file_count = 0, used_bytes = 0;
    for(int i = 0; i < FS_MAX_FILES; i++) {
        if(fs_table[i].used) {
            file_count++;
            used_bytes += fs_table[i].size;
        }
    }

    unsigned char s, m, h, d, mo, y, s2, m2, h2, d2, mo2, y2;
    do {
        read_rtc_raw(&h, &m, &s, &d, &mo, &y);
        read_rtc_raw(&h2, &m2, &s2, &d2, &mo2, &y2);
    } while(h != h2 || m != m2 || s != s2 || d != d2 || mo != mo2 || y != y2);
    unsigned char reg_b = cmos_read(0x0B);
    if(!(reg_b & 0x04)) {
        s = bcd_to_bin(s); m = bcd_to_bin(m); d = bcd_to_bin(d);
        mo = bcd_to_bin(mo); y = bcd_to_bin(y);
        h = bcd_to_bin(h & 0x7F) | (h & 0x80);
    }
    if(!(reg_b & 0x02) && (h & 0x80)) h = ((h & 0x7F) + 12) % 24;
    else h &= 0x7F;

    goonfetch_prefix(logo[0]); vga_print("root@GoonerOS\n");
    goonfetch_prefix(logo[1]); vga_print("--------------\n");
    goonfetch_prefix(logo[2]); vga_print("OS: GoonerOS v0.10\n");
    goonfetch_prefix(logo[3]); vga_print("Kernel: i686 / 32-bit\n");
    goonfetch_prefix(logo[4]); vga_print("Shell: GoonerOS shell\n");
    goonfetch_prefix(logo[5]); vga_print(desktop_active ? "Session: desktop\n" : "Session: console\n");
    goonfetch_prefix(logo[6]); vga_print("Display: ");
    print_int(VESA_WIDTH); vga_putc('x'); print_int(VESA_HEIGHT); vga_putc('x');
    print_int((int)vga_get_bpp()); vga_print("\n");
    goonfetch_prefix(logo[7]); vga_print("Keys: ");
    vga_print(keyboard_get_layout() == KEYBOARD_LAYOUT_EN ? "en / QWERTY\n" : "de / QWERTZ\n");
    goonfetch_prefix(logo[8]); vga_print("Uptime: "); print_int((int)(ticks / 1000)); vga_print(" seconds\n");
    goonfetch_prefix(logo[9]); vga_print("RTC: 20"); goonfetch_two_digits(y); vga_putc('-');
    goonfetch_two_digits(mo); vga_putc('-'); goonfetch_two_digits(d); vga_putc(' ');
    goonfetch_two_digits(h); vga_putc(':'); goonfetch_two_digits(m); vga_putc(':');
    goonfetch_two_digits(s); vga_putc('\n');
    goonfetch_prefix(logo[10]); vga_print("FS entries: "); print_int((int)file_count); vga_putc('/');
    print_int(FS_MAX_FILES); vga_print("  data: "); print_int((int)used_bytes); vga_print(" B\n");
    goonfetch_prefix(logo[11]); vga_print("Heap: ");
    print_int((int)(heap_ptr-HEAP_START)); vga_print(" B  Jobs: "); print_int(scheduler_task_count());
    vga_putc('/'); print_int(SCHEDULER_MAX_TASKS); vga_print(" cooperative\n");
    vga_set_text_color(0xFFFFFF);
    vga_putc('\n');
}

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
        vga_print("== System & Info ==               |           == File System ==\n");
        vga_print("help; info; version; goonfetch;   |  ls [-la]; cd; pwd; tree; find;\n");
        vga_print("neofetch; uname; hostname;        |  mkdir [-p]; rmdir; rm [-r]; mv;\n");
        vga_print("                                  |       touch; cat; write; append;\n");
        vga_print("whoami; uptime; arch;             |           chmod; type xyz;\n");
        vga_print("lscpu; meminfo; free;             |       cat [-n]; stat; find;\n");
        vga_print("df; ps; top;                      |            sum; cksum; wc;\n");
        vga_print("env; id; groups;                  |            head; tail; pwd;\n");
        vga_print("who; battery; sensors;            |           search; wipe; tree;\n");
        vga_print("dmesg; vmstat; lsblk;             |              mv; cp; du\n");
        vga_print("mount; nproc; systemctl;          |        cp; du; df; fsck; stat;\n");
        vga_print("journalctl; crontab; history;     |                          \n");
        vga_print("__________________________________|_________________________________\n");
        vga_print("== Text & Processing ==           |            == Math & Random ==\n");
        vga_print("echo; grep; sort;                 |               calc; seq;\n");
        vga_print("rev; tr; which;                   |                  fib;\n");
        vga_print("printf x; diff; less;             |             rand; genpass;\n");
        vga_print("more; nano; man;                  |               factorial;\n");
        vga_print("fortune; banner; colors;          |             factor; prime\n");
        vga_print("rot13; base64; hex;               |                          \n");
        vga_print("dec; ascii; chr                   |                          \n");
        vga_print("__________________________________|_________________________________\n");
        vga_print("== Graphics & Desktop ==          |                == Time ==\n");
        vga_print("draw ( circle/ rect);             |           time; date; clock;\n");
        vga_print("draw x y z;                       |              sleep; cal;\n");
        vga_print("logo; mintlogo; matrix;           |           (alarm) countdown\n");
        vga_print("ida; fire; snow;                  |                         \n");
        vga_print("sl; cls; clear; clean              |                         \n");
        vga_print("theme desktop; mouse; desktop;    |     loadkeys [de|en]\n");
        vga_print("fullscreen; window                |                         \n");
        vga_print("                                  |                         \n");
        vga_print("__________________________________|_________________________________\n");
        vga_print("== System Control ==              |         == Network & Misc ==\n");
        vga_print("reboot; shutdown; halt;           |          ping x; ifconfig;\n");
        vga_print("poweroff; logout; exit;           |            true; false;\n");
        vga_print("passwd; spawn counter;             |    spawn checksum PATH; kill PID\n");
        vga_print("                                   |       ps; jobs; apt install x\n");
        vga_print("su; beep                          |              \n");

    }
    else if(strncmp(input_buf, "ls", 2) == 0 && (input_buf[2] == 0 || input_buf[2] == ' ')) {
        char path[FS_NAME_LEN];
        char* arg = &input_buf[2];
        while(*arg == ' ') arg++;
        int long_format = 0;
        int show_hidden = 0;
        int invalid_option = 0;
        while(*arg == '-') {
            char* end = arg;
            while(*end && *end != ' ') end++;
            int option_len = end - arg;
            if(option_len == 5 && strncmp(arg, "--all", 5) == 0) {
                show_hidden = 1;
            } else if(option_len == 6 && strncmp(arg, "--long", 6) == 0) {
                long_format = 1;
            } else if(option_len >= 2 && arg[1] != '-') {
                for(int i = 1; i < option_len; i++) {
                    if(arg[i] == 'a') show_hidden = 1;
                    else if(arg[i] == 'l') long_format = 1;
                    else invalid_option = 1;
                }
            } else {
                invalid_option = 1;
            }
            arg = end;
            while(*arg == ' ') arg++;
        }
        if(invalid_option) {
            vga_print("Usage: ls [-a] [-l] [PATH] (also -la/-al)\n");
        } else if(!*arg) {
            for(int i = 0; i < FS_NAME_LEN; i++) path[i] = fs_cwd[i];
            list_directory(path, long_format, show_hidden);
        } else if(resolve_fs_path(arg, path)) {
            list_directory(path, long_format, show_hidden);
        } else vga_print("ls: Path is too long or invalid\n");
    }
    else if(strcmp(input_buf, "loadkeys") == 0) {
        vga_print("Keyboard layout: ");
        vga_print(keyboard_get_layout() == KEYBOARD_LAYOUT_EN ? "en (QWERTY)\n" : "de (QWERTZ)\n");
        vga_print("Switch with: loadkeys de | loadkeys en\n");
    }
    else if(strcmp(input_buf, "loadkeys de") == 0 || strcmp(input_buf, "loadkeys en") == 0) {
        int layout = input_buf[9] == 'e' ? KEYBOARD_LAYOUT_EN : KEYBOARD_LAYOUT_DE;
        keyboard_set_layout(layout);
        const char config[] = {'1', ',', input_buf[9], input_buf[10], '\n'};
        if(fs_write("goonkeys.cfg", config, sizeof(config))) {
            vga_print("Keyboard layout ");
            vga_print(layout == KEYBOARD_LAYOUT_EN ? "en (QWERTY)" : "de (QWERTZ)");
            vga_print(" selected and saved\n");
        } else {
            vga_print("Layout selected but not saved; check the filesystem or disk\n");
        }
    }
    else if(strcmp(input_buf, "pwd") == 0)
        print_working_directory();
    else if(strncmp(input_buf, "loadkeys ", 9) == 0)
        vga_print("Usage: loadkeys de | loadkeys en\n");
    else if(strcmp(input_buf, "cd") == 0 || strcmp(input_buf, "cd /") == 0)
        fs_cwd[0] = 0;
    else if(strncmp(input_buf, "cd ", 3) == 0) {
        char path[FS_NAME_LEN];
        if(resolve_fs_path(&input_buf[3], path) && (!path[0] || fs_is_directory(fs_find(path)))) {
            int i = 0;
            while(path[i]) { fs_cwd[i] = path[i]; i++; }
            fs_cwd[i] = 0;
        } else vga_print("cd: Directory not found or path too long\n");
    }
    else if(strncmp(input_buf, "mkdir ", 6) == 0) {
        char* arg = &input_buf[6];
        while(*arg == ' ') arg++;
        int parents = 0;
        if(strncmp(arg, "-p", 2) == 0 && (arg[2] == 0 || arg[2] == ' ')) {
            parents = 1;
            arg += 2;
            while(*arg == ' ') arg++;
        }
        char path[FS_NAME_LEN];
        if(!*arg) vga_print("Usage: mkdir [-p] DIRECTORY\n");
        else if(!resolve_fs_path(arg, path)) vga_print("mkdir: invalid or overlong path\n");
        else if(parents ? make_directories(path) : fs_mkdir(path)) vga_print("Directory created\n");
        else vga_print("mkdir: already exists, parent missing, or filesystem full\n");
    }
    else if(strcmp(input_buf, "mkdir") == 0)
        vga_print("Usage: mkdir DIRECTORY\n");
    else if(strncmp(input_buf, "rmdir ", 6) == 0) {
        char path[FS_NAME_LEN];
        if(!resolve_fs_path(&input_buf[6], path)) vga_print("rmdir: invalid or overlong path\n");
        else if(fs_rmdir(path)) { leave_removed_directory(path); vga_print("Directory removed\n"); }
        else vga_print("rmdir: directory is not empty or was not found\n");
    }
    else if(strcmp(input_buf, "rmdir") == 0)
        vga_print("Usage: rmdir DIRECTORY (must be empty)\n");
    else if(strncmp(input_buf, "rm -r ", 6) == 0) {
        char path[FS_NAME_LEN];
        if(!resolve_fs_path(&input_buf[6], path)) vga_print("rm -r: invalid or overlong path\n");
        else if(fs_remove_tree(path)) {
            leave_removed_directory(path);
            vga_print("Directory tree removed\n");
        } else vga_print("rm -r: directory not found\n");
    }
    else if(strcmp(input_buf, "df") == 0) {
        int used_blocks = 0;
        for(int i = 0; i < FS_MAX_FILES; i++)
            if(fs_table[i].used) used_blocks++;
        int capacity = FS_MAX_FILES * FS_MAX_FILE_BYTES;
        int used = used_blocks * FS_MAX_FILE_BYTES;
        vga_print("Filesystem      Size(KiB) Used(KiB) Avail(KiB) (reserved blocks)\n");
        vga_print("/dev/ata0-fs    ");
        print_int(capacity / 1024); vga_print("        ");
        print_int(used / 1024); vga_print("        ");
        print_int((capacity - used) / 1024); vga_putc('\n');
    }
    else if(strcmp(input_buf, "fsck") == 0) {
        int errors = fs_check();
        if(errors == 0) vga_print("Filesystem clean\n");
        else { vga_print("Filesystem errors: "); print_int(errors); vga_putc('\n'); }
    }
    else if(strcmp(input_buf, "clear") == 0 || strcmp(input_buf, "clean") == 0) {
        if(desktop_active) vga_clear_region(); else vga_clear();
    }
    else if(strncmp(input_buf, "echo ", 5) == 0) {
        vga_print(&input_buf[5]); vga_putc('\n');
    }
    else if(strncmp(input_buf, "cat ", 4) == 0) {
        char* arg = &input_buf[4];
        int number_lines = 0;
        if(strncmp(arg, "-n", 2) == 0 && arg[2] == ' ') {
            number_lines = 1;
            arg += 2;
            while(*arg == ' ') arg++;
        }
        char path[FS_NAME_LEN];
        if(!resolve_fs_path(arg, path)) vga_print("cat: invalid or overlong path\n");
        else if(fs_read(path, cmd_buf, sizeof(cmd_buf)) < 0) vga_print("cat: file not found or not a regular file\n");
        else if(!number_lines) { vga_print(cmd_buf); vga_putc('\n'); }
        else {
            int line = 1;
            if(cmd_buf[0]) {
                vga_print("1  ");
                for(int i = 0; cmd_buf[i]; i++) {
                    vga_putc(cmd_buf[i]);
                    if(cmd_buf[i] == '\n' && cmd_buf[i+1]) {
                        print_int(++line);
                        vga_print("  ");
                    }
                }
                if(cmd_buf[strlen(cmd_buf)-1] != '\n') vga_putc('\n');
            }
        }
    }
    else if(strncmp(input_buf, "write ", 6) == 0) {
        char* sp = &input_buf[6];
        while(*sp && *sp!= ' ') sp++;
        if(*sp) {
            *sp = 0;
            char path[FS_NAME_LEN];
            if(!resolve_fs_path(&input_buf[6], path)) vga_print("write: invalid or overlong path\n");
            else if(fs_write(path, sp+1, strlen(sp+1))) vga_print("OK\n");
            else vga_print("write: failed (check parent directory, size, or disk)\n");
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
    else if(strcmp(input_buf, "goonfetch") == 0)
        goonfetch();
    else if(strcmp(input_buf, "neofetch") == 0) {
        goonfetch();
    }
    else if(strcmp(input_buf, "mintlogo") == 0) {
        mint_visible = !mint_visible;
        draw_mint_logo(mint_visible);
        vga_print(mint_visible ? "Mint-Logo an\n" : "Mint-Logo aus\n");
    }
    else if(strcmp(input_buf, "info") == 0) {
        vga_print("=== GoonerOS System Information ===\n");
        vga_print("Version: v0.10 Full 780L\n");
        vga_print("Resolution: 1024x768 ("); print_int(1024*768); vga_print(" pixels)\n");
        vga_print("Uptime: "); print_int(ticks/1000); vga_print("s ("); print_int((int)ticks); vga_print(" Ticks)\n");
        vga_print("Size: ~3200 lines of C and assembly\n");
        vga_print("  Boot: ~200 lines of assembly\n");
        vga_print("  Linker: ~30 lines of linker script\n");
        vga_print("  ISR/interrupts: ~125 lines of assembly\n");
        vga_print("  Kernel: ~2820 lines of C\n");
        vga_print("Mode: 32-bit protected mode\n");
        vga_print("Framebuffer: "); print_int((int)vga_get_pitch()); vga_print(" bytes/row, "); print_int((int)vga_get_bpp()); vga_print(" bpp\n");
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
    else if(strcmp(input_buf, "uname") == 0)
        vga_print("GoonerOS 0.10 i686\n");
    else if(strcmp(input_buf, "uname -a") == 0)
        vga_print("GoonerOS 0.10 GoonerOS-kernel i686 GNU-like\n");
    else if(strcmp(input_buf, "hostname") == 0)
        vga_print("GoonerOS\n");
    else if(strcmp(input_buf, "free") == 0) {
        vga_print("Heap allocator (not total physical RAM)\n");
        vga_print("Allocated: "); print_int((int)(heap_ptr-HEAP_START)); vga_print(" bytes\n");
        vga_print("Total RAM: not detected by this kernel\n");
    }
    else if(strcmp(input_buf, "ps") == 0 || strcmp(input_buf, "jobs") == 0) {
        vga_print("PID  TASK       STATE      PROGRESS / RESULT\n");
        vga_print("  1  kernel_main      running     event loop\n");
        for(int i = 0; i < scheduler_task_count(); i++) {
            int pid, type, state;
            unsigned int steps, progress, total, result;
            if(!scheduler_get_task_info(i, &pid, &type, &state, &steps, &progress, &total, &result))
                continue;
            vga_putc(' '); print_int(pid); vga_print(type == SCHEDULER_TASK_CHECKSUM ? "  checksum   " : "  counter    ");
            vga_print(state == SCHEDULER_STATE_DONE ? "done       " : "runnable   ");
            if(type == SCHEDULER_TASK_CHECKSUM) {
                print_int((int)progress); vga_putc('/'); print_int((int)total);
                if(state == SCHEDULER_STATE_DONE) {
                    vga_print("  CRC32=0x"); print_hex32(result);
                }
            } else {
                print_int((int)steps); vga_print(" steps");
            }
            vga_putc('\n');
        }
        vga_print("Cooperative tasks: "); print_int(scheduler_task_count());
        vga_print(" (one bounded work-slice per event-loop turn)\n");
    }
    else if(strcmp(input_buf, "spawn counter") == 0) {
        int pid = scheduler_spawn_counter();
        if(pid < 0) vga_print("spawn: task table is full\n");
        else { vga_print("Cooperative counter started, PID "); print_int(pid); vga_putc('\n'); }
    }
    else if(strncmp(input_buf, "spawn checksum ", 15) == 0) {
        char path[FS_NAME_LEN];
        if(!resolve_fs_path(&input_buf[15], path) || !path[0]) {
            vga_print("Usage: spawn checksum FILE (path is invalid or too long)\n");
        } else {
            int pid = scheduler_spawn_checksum(path);
            if(pid < 0) vga_print("spawn checksum: file unreadable, task table full, or CRC task already running\n");
            else { vga_print("Cooperative CRC32 job started, PID "); print_int(pid); vga_putc('\n'); }
        }
    }
    else if(strcmp(input_buf, "spawn checksum") == 0)
        vga_print("Usage: spawn checksum FILE\n");
    else if(strncmp(input_buf, "kill ", 5) == 0) {
        char* p = &input_buf[5];
        int pid = parse_int(&p);
        if(scheduler_kill(pid)) { vga_print("Task stopped: "); print_int(pid); vga_putc('\n'); }
        else vga_print("kill: unknown or protected PID\n");
    }
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
        vga_print("Today: "); print_int(d); vga_putc('/'); print_int(mo); vga_print("/20"); print_int(y); vga_putc('\n');
    }
    else if(strcmp(input_buf, "su") == 0)
        vga_print("You are already root\n");
    else if(strcmp(input_buf, "passwd") == 0)
        vga_print("No password required!\n");
    else if(strcmp(input_buf, "lscpu") == 0) {
        vga_print("Architecture: i686-compatible x86 target\n");
        vga_print("Mode: 32-bit Protected Mode\n");
        vga_print("CPU topology: not detected\n");
    }
    else if(strcmp(input_buf, "top") == 0) {
        vga_print("CONTEXT       STATE       DETAIL\n");
        vga_print("kernel_main   running     "); print_int(heap_ptr-HEAP_START); vga_print(" B heap\n");
        vga_print("cooperative   runnable    "); print_int(scheduler_task_count()); vga_print(" tasks, bounded slices\n");
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
        if(read_file_from_cwd(&input_buf[5], cmd_buf, sizeof(cmd_buf)) < 0) { vga_print("File not found\n"); }
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
        if(read_file_from_cwd(&input_buf[3], cmd_buf, sizeof(cmd_buf)) < 0) { vga_print("File not found\n"); }
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
        char path[FS_NAME_LEN];
        if(!resolve_fs_path(&input_buf[6], path)) vga_print("touch: invalid or overlong path\n");
        else if(fs_create(path)) vga_print("OK\n");
        else vga_print("touch: could not create file\n");
    }
    else if(strncmp(input_buf, "rm ", 3) == 0) {
        char path[FS_NAME_LEN];
        if(!resolve_fs_path(&input_buf[3], path)) vga_print("rm: invalid or overlong path\n");
        else if(fs_delete(path)) vga_print("Removed\n");
        else vga_print("rm: file not found (use rmdir for directories)\n");
    }
    else if(strncmp(input_buf, "cp ", 3) == 0) {
        char* sp = &input_buf[3];
        while(*sp && *sp != ' ') sp++;
        if(*sp) {
            *sp = 0;
            char source[FS_NAME_LEN], destination[FS_NAME_LEN];
            if(!resolve_fs_path(&input_buf[3], source) || !resolve_fs_path(sp+1, destination))
                vga_print("cp: invalid or overlong path\n");
            else if(fs_read(source, cmd_buf, sizeof(cmd_buf)) < 0) vga_print("cp: source file not found\n");
            else if(fs_write(destination, cmd_buf, strlen(cmd_buf))) vga_print("OK\n");
            else vga_print("cp: copy failed\n");
        } else vga_print("Usage: cp SOURCE DESTINATION\n");
    }
    else if(strncmp(input_buf, "mv ", 3) == 0) {
        char* sp = &input_buf[3];
        while(*sp && *sp != ' ') sp++;
        if(*sp) {
            *sp = 0;
            char source[FS_NAME_LEN], destination[FS_NAME_LEN];
            if(!resolve_fs_path(&input_buf[3], source) || !resolve_fs_path(sp+1, destination))
                vga_print("mv: invalid or overlong path\n");
            else {
                struct fs_entry* entry = fs_find(source);
                int moving_directory = fs_is_directory(entry);
                if(fs_rename(source, destination)) {
                    if(moving_directory) move_working_directory(source, destination);
                    vga_print("OK\n");
                } else vga_print("mv: move/rename failed\n");
            }
        } else vga_print("Usage: mv OLD NEW\n");
    }
    else if(strncmp(input_buf, "stat ", 5) == 0) {
        char path[FS_NAME_LEN];
        struct fs_entry* e = resolve_fs_path(&input_buf[5], path) ? fs_find(path) : 0;
        if(!e) vga_print("File not found\n");
        else {
            vga_print("Name: "); vga_print(fs_basename(e->name)); vga_putc('\n');
            vga_print("Path: /"); vga_print(e->name); vga_putc('\n');
            vga_print("Type: "); vga_print(fs_is_directory(e) ? "Directory\n" : "File\n");
            vga_print("Size: "); print_int((int)e->size); vga_print(" B\n");
            vga_print("Start-LBA: "); print_int((int)e->start_lba); vga_putc('\n');
        }
    }
    else if(strncmp(input_buf, "find ", 5) == 0) {
        int any = 0;
        for(int i = 0; i < FS_MAX_FILES; i++) {
            if(fs_table[i].used && my_strstr(fs_table[i].name, &input_buf[5])) {
                vga_putc('/'); vga_print(fs_table[i].name);
                if(fs_is_directory(&fs_table[i])) vga_putc('/');
                vga_putc('\n'); any = 1;
            }
        }
        if(!any) vga_print("Nothing found\n");
    }
    else if(strcmp(input_buf, "du") == 0) {
        int total = 0, dirs = 0, files = 0;
        for(int i = 0; i < FS_MAX_FILES; i++) if(fs_table[i].used) {
            if(fs_is_directory(&fs_table[i])) dirs++;
            else { files++; total += (int)fs_table[i].size; }
        }
        vga_print("Data: "); print_int(total); vga_print(" B in ");
        print_int(files); vga_print(" files and "); print_int(dirs); vga_print(" directories\n");
    }
    else if(strncmp(input_buf, "du ", 3) == 0) {
        char path[FS_NAME_LEN];
        if(!resolve_fs_path(&input_buf[3], path)) vga_print("du: invalid path\n");
        else {
            int total = 0, files = 0;
            for(int i = 0; i < FS_MAX_FILES; i++) {
                if(!fs_table[i].used || fs_is_directory(&fs_table[i])) continue;
                if(strcmp(path, fs_table[i].name) == 0 ||
                   (path[0] && strncmp(fs_table[i].name, path, strlen(path)) == 0 &&
                    fs_table[i].name[strlen(path)] == '/') ||
                   (!path[0] && fs_table[i].used)) {
                    total += (int)fs_table[i].size; files++;
                }
            }
            vga_print("Data: "); print_int(total); vga_print(" B in ");
            print_int(files); vga_print(" files\n");
        }
    }
    else if(strncmp(input_buf, "chmod ", 6) == 0)
        vga_print("GoonerOS has no file permissions yet; everyone has full access\n");
    else if(strncmp(input_buf, "type ", 5) == 0)
        vga_print("built-in shell command\n");
    else if(strncmp(input_buf, "xxd ", 4) == 0) {
        if(read_file_from_cwd(&input_buf[4], cmd_buf, sizeof(cmd_buf)) < 0) vga_print("File not found\n");
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
        if(read_file_from_cwd(&input_buf[4], cmd_buf, sizeof(cmd_buf)) < 0) vga_print("File not found\n");
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
            if(read_file_from_cwd(sp+1, cmd_buf, sizeof(cmd_buf)) < 0) vga_print("File not found\n");
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
                if(!any) vga_print("(no matches)\n");
            }
        }
    }
    else if(strncmp(input_buf, "sort ", 5) == 0) {
        if(read_file_from_cwd(&input_buf[5], cmd_buf, sizeof(cmd_buf)) < 0) vga_print("File not found\n");
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
        vga_print("built-in shell command\n");
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
        vga_print("[0.040] filesystem loaded\n");
    }
    else if(strcmp(input_buf, "vmstat") == 0) {
        vga_print("procs  mem\n");
        vga_print("   1   "); print_int(heap_ptr-HEAP_START); vga_print(" B belegt\n");
    }
    else if(strcmp(input_buf, "lsblk") == 0) {
        vga_print("DEVICE       SIZE    TYPE\n");
        vga_print("ata0         1 MiB   QEMU image (build configuration)\n");
        vga_print("ata0-fs      256 KiB filesystem data capacity\n");
    }
    else if(strcmp(input_buf, "mount") == 0) {
        vga_print("ata0-fs on / type goonerfs (hierarchical, fixed table)\n");
        vga_print("Metadata: LBA 512-514; data begins at LBA 515\n");
    }
    else if(strcmp(input_buf, "nproc") == 0)
        vga_print("CPU topology is not detected by this kernel\n");
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
            int r1 = read_file_from_cwd(&input_buf[5], cmd_buf, sizeof(cmd_buf));
            int r2 = read_file_from_cwd(sp+1, cmd_buf2, sizeof(cmd_buf2));
            if(r1 < 0 || r2 < 0) vga_print("File not found\n");
            else if(strcmp(cmd_buf, cmd_buf2) == 0) vga_print("Identisch\n");
            else vga_print("Different\n");
        }
    }
    else if(strncmp(input_buf, "less ", 5) == 0 || strncmp(input_buf, "more ", 5) == 0) {
        char* name = &input_buf[5];
        if(read_file_from_cwd(name, cmd_buf, sizeof(cmd_buf)) < 0) vga_print("File not found\n");
        else { vga_print(cmd_buf); vga_putc('\n'); }
    }
    else if(strcmp(input_buf, "nano") == 0 || strcmp(input_buf, "vi") == 0 || strcmp(input_buf, "vim") == 0)
        vga_print("Kein Texteditor vorhanden - nutze 'write name text'\n");
    else if(strncmp(input_buf, "apt ", 4) == 0 || strncmp(input_buf, "pacman ", 7) == 0)
        vga_print("Kein Paketmanager vorhanden\n");
    else if(strcmp(input_buf, "battery") == 0)
        vga_print("Kein Akku vorhanden (virtuelle Maschine)\n");
    else if(strcmp(input_buf, "cls") == 0) {
        if(desktop_active) vga_clear_region(); else vga_clear();
    }
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
        else vga_print("Unknown operator (+ - * /)\n");
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
        if(strcmp(c, "ls") == 0) vga_print("ls [-a] [-l] [PATH] - -a shows hidden entries, -l shows details; combine as -la\n");
        else if(strcmp(c, "loadkeys") == 0) vga_print("loadkeys de|en - select and save the German QWERTZ or English QWERTY layout\n");
        else if(strcmp(c, "mkdir") == 0) vga_print("mkdir [-p] PATH - create a directory; -p also creates parent directories\n");
        else if(strcmp(c, "rmdir") == 0) vga_print("rmdir PATH - remove an empty directory\n");
        else if(strcmp(c, "cd") == 0) vga_print("cd [PATH] - change the working directory; cd .. moves up one level\n");
        else if(strcmp(c, "pwd") == 0) vga_print("pwd - show the current working directory\n");
        else if(strcmp(c, "rm") == 0) vga_print("rm FILE | rm -r DIRECTORY - remove a file or directory tree\n");
        else if(strcmp(c, "help") == 0) vga_print("help - list available commands\n");
        else if(strcmp(c, "countdown") == 0) vga_print("countdown N - count down from N; example: countdown 10\n");
        else if(strcmp(c, "apt install") == 0) vga_print("apt install - unavailable: no package manager yet\nExample: apt install firefox\n");
        else if(strcmp(c, "ping") == 0) vga_print("ping - unavailable: no network stack yet\nExample: ping example.com\n");
        else if(strcmp(c, "window") == 0) vga_print("window - show a desktop window prototype\n");
        else if(strcmp(c, "draw") == 0) vga_print("draw rectangle - draw a green rectangle at screen center\ndraw circle - draw a green circle at screen center\ndraw x y z - draw a rectangle at the given coordinates\n");
        else if(strcmp(c, "stat") == 0) vga_print("stat PATH - show file details\nExample: stat test\n");
        else if(strcmp(c, "wipe") == 0) vga_print("wipe - remove every file from the filesystem\n");
        else if(strcmp(c, "tail") == 0) vga_print("tail FILE - show the last lines of a file\n");
        else if(strcmp(c, "head") == 0) vga_print("head FILE - show the first lines of a file\n");
        else if(strcmp(c, "du") == 0) vga_print("du - show the total file data size in bytes\n");
        else if(strcmp(c, "cksum") == 0) vga_print("cksum FILE - calculate CRC-32/IEEE and file length\n");
        else if(strcmp(c, "clean") == 0) vga_print("clean - clear the terminal; same as clear\n");
        else if(strcmp(c, "spawn") == 0) vga_print("spawn counter | spawn checksum FILE - start cooperative background jobs\n");
        else if(strcmp(c, "jobs") == 0) vga_print("jobs - show progress and results of cooperative tasks\n");
        else if(strcmp(c, "sum") == 0) vga_print("sum - calculate a legacy 16-bit checksum\nLow reliability\n");
        else if(strcmp(c, "touch") == 0) vga_print("touch FILE - create an empty file\n");
        else if(strcmp(c, "cp") == 0) vga_print("cp SOURCE DESTINATION - copy a file\n");
        else if(strcmp(c, "append") == 0) vga_print("append FILE TEXT - append text to a file\n");
        else if(strcmp(c, "echo") == 0) vga_print("echo TEXT - print text\n");
        else if(strcmp(c, "find") == 0) vga_print("find NAME - search the filesystem for names\n");
        else if(strcmp(c, "search") == 0) vga_print("search TEXT - search file contents\n");
        else if(strcmp(c, "tree") == 0) vga_print("tree [PATH] - show directories and files as a tree\n");
        else if(strcmp(c, "wc") == 0) vga_print("wc FILE - count lines, words, and bytes in a file\n");
        else if(strcmp(c, "seq") == 0) vga_print("seq N - count up to N\nLimit: 200\n");
        else if(strcmp(c, "cat") == 0) vga_print("cat [-n] FILE - print a file; -n numbers lines\n");
        else if(strcmp(c, "write") == 0) vga_print("write FILE TEXT - write TEXT to FILE\n");
        else if(strcmp(c, "calc") == 0) vga_print("calc A OP B - Taschenrechner (+ - * /)\n");
        else if(strcmp(c, "seq") == 0) vga_print("seq N - count from 1 to N\n");
        else if(strcmp(c, "factor") == 0) vga_print("factor N - show the prime factorization of N\n");
        else if(strcmp(c, "rand") == 0) vga_print("rand - generate a pseudo-random number\n");
        else if(strcmp(c, "ida") == 0) vga_print("ida - draw a red heart that disappears after 10 seconds\n");
        else if(strcmp(c, "fib") == 0) vga_print("fib N - calculate the Nth Fibonacci number\neach number is the sum of the previous two\n");
        else if(strcmp(c, "genpass") == 0) vga_print("genpass - generate a pseudo-random password\n");
        else if(strcmp(c, "factorial") == 0) vga_print("factorial N - calculate N factorial\nExample: factorial 5 = 5x4x3x2x1 = 120\n");
        else if(strcmp(c, "lsblk") == 0) vga_print("lsblk - list block devices\n");
        else if(strcmp(c, "sensors") == 0) vga_print("sensors - show available CPU temperature data\n");
        else if(strcmp(c, "journalctl") == 0) vga_print("journalctl - show system logs and errors\n");
        else if(strcmp(c, "mouse") == 0) vga_print("mouse - show the current mouse coordinates\n");
        else if(strcmp(c, "banner") == 0) vga_print("banner TEXT - show large text in the center of the screen\n");
        else if(strcmp(c, "sl") == 0) vga_print("sl - animate a train moving across the screen\n");
        else if(strcmp(c, "cls") == 0) vga_print("cls - clear the screen, like clear\n");
        else { vga_print("No manual entry for '"); vga_print(c); vga_print("'\n"); }
    }
    else if(strcmp(input_buf, "who") == 0)
        vga_print("root     tty1         GoonerOS\n");
    else if(strcmp(input_buf, "tree") == 0 || strncmp(input_buf, "tree ", 5) == 0) {
        char path[FS_NAME_LEN];
        int valid_path = 1;
        if(input_buf[4] == ' ') {
            if(!resolve_fs_path(&input_buf[5], path)) valid_path = 0;
        } else {
            int i = 0; while(fs_cwd[i]) { path[i] = fs_cwd[i]; i++; } path[i] = 0;
        }
        struct fs_entry* root = path[0] ? fs_find(path) : 0;
        if(!valid_path) vga_print("tree: invalid or overlong path\n");
        else if(path[0] && !fs_is_directory(root)) vga_print("tree: directory not found\n");
        else {
            vga_print("/root");
            if(path[0]) { vga_putc('/'); vga_print(path); }
            vga_putc('\n');
            int any = 0, path_len = strlen(path);
            for(int i = 0; i < FS_MAX_FILES; i++) {
                if(!fs_table[i].used) continue;
                const char* relative = fs_table[i].name;
                if(path_len) {
                    if(strncmp(relative, path, path_len) != 0 || relative[path_len] != '/') continue;
                    relative += path_len + 1;
                }
                int depth = 0;
                for(int j = 0; relative[j]; j++) if(relative[j] == '/') depth++;
                for(int j = 0; j < depth; j++) vga_print("  ");
                vga_print("|-- ");
                vga_print(fs_basename(relative));
                if(fs_is_directory(&fs_table[i])) vga_putc('/');
                vga_putc('\n');
                any = 1;
            }
            if(!any) vga_print("(leer)\n");
        }
    }
    else if(strncmp(input_buf, "cksum ", 6) == 0) {
        char path[FS_NAME_LEN];
        int len = resolve_fs_path(&input_buf[6], path) ? fs_read(path, cmd_buf, sizeof(cmd_buf)) : -1;
        if(len < 0) vga_print("cksum: file not found or unreadable\n");
        else {
            unsigned int crc = 0xFFFFFFFFu;
            for(int i = 0; i < len; i++) {
                crc ^= (unsigned char)cmd_buf[i];
                for(int bit = 0; bit < 8; bit++)
                    crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
            }
            vga_print("CRC32 0x"); print_hex32(~crc); vga_print("  ");
            print_int(len); vga_print(" bytes\n");
        }
    }
    else if(strncmp(input_buf, "ping ", 5) == 0) {
        vga_print("PING "); vga_print(&input_buf[5]); vga_print(": no network stack available\n");
    }
    else if(strcmp(input_buf, "ifconfig") == 0)
        vga_print("lo: loopback; no physical network interface detected\n");
    else if(strcmp(input_buf, "sensors") == 0)
        vga_print("CPU temperature: unavailable (no thermal sensor driver)\n");
    else if(strncmp(input_buf, "systemctl", 9) == 0) {
        vga_print("kernel.service   loaded active running GoonerOS Kernel\n");
        vga_print("shell.service    loaded active running GoonerOS Shell\n");
    }
    else if(strcmp(input_buf, "journalctl") == 0) {
        vga_print("[boot] GoonerOS started\n");
        vga_print("[ok] VESA Grafik initialisiert\n");
        vga_print("[ok] PS/2 Controller bereit\n");
        vga_print("[ok] ATA Festplatte erkannt\n");
        vga_print("[ok] filesystem loaded\n");
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
        vga_print("Live clock for 60s (the shell is paused, then resumes)\n");
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
        if(ok) {
            ui_theme_color = newc;
            if(desktop_save_preferences()) vga_print("Theme changed and saved\n");
            else vga_print("Theme changed, but settings could not be saved\n");
        }
        else vga_print("Colors: cyan blue red green purple orange\n");
    }
    else if(strcmp(input_buf, "window") == 0) {
        clear_last_draw();
        draw_window(300, 200, 400, 250, "Demo-Fenster");
        remember_draw(300, 200, 400, 250);
        vga_print("Desktop window prototype (not movable yet)\n");
    }
    else if(strcmp(input_buf, "mouse") == 0) {
        vga_print("X: "); print_int(mouse_get_x());
        vga_print("  Y: "); print_int(mouse_get_y());
        vga_print("  Left: "); vga_print(mouse_left_pressed() ? "pressed\n" : "released\n");
    }

    /* ==================== Weitere nuetzliche Befehle ==================== */
    else if(strncmp(input_buf, "append ", 7) == 0) {
        char* sp = &input_buf[7];
        while(*sp && *sp != ' ') sp++;
        if(*sp) {
            *sp = 0;
            char path[FS_NAME_LEN];
            if(!resolve_fs_path(&input_buf[7], path)) {
                vga_print("append: invalid or overlong path\n");
                goto command_done;
            }
            int old_len = fs_read(path, cmd_buf, sizeof(cmd_buf));
            if(old_len < 0) old_len = 0;
            char* add = sp+1;
            int add_len = strlen(add);
            int total = old_len + add_len;
            if(total > FS_MAX_FILE_BYTES) {
                vga_print("append: file would exceed the maximum file size\n");
                goto command_done;
            }
            for(int i = 0; old_len+i < total; i++) cmd_buf[old_len+i] = add[i];
            cmd_buf[total] = 0;
            if(fs_write(path, cmd_buf, total)) vga_print("OK\n");
            else vga_print("append: write failed\n");
        } else vga_print("Usage: append datei text\n");
    }
    else if(strncmp(input_buf, "search ", 7) == 0) {
        char* needle = &input_buf[7];
        int any = 0;
        for(int i = 0; i < FS_MAX_FILES; i++) {
            if(!fs_table[i].used) continue;
            if(fs_is_directory(&fs_table[i])) continue;
            if(fs_read(fs_table[i].name, cmd_buf, sizeof(cmd_buf)) < 0) continue;
            if(my_strstr(cmd_buf, needle)) { vga_putc('/'); vga_print(fs_table[i].name); vga_putc('\n'); any = 1; }
        }
        if(!any) vga_print("Nothing found\n");
    }
    else if(strncmp(input_buf, "alarm ", 6) == 0) {
        char* p = &input_buf[6];
        int secs = parse_int(&p);
        if(secs < 0) secs = 0;
        if(secs > 30) secs = 30; // Sicherheitsbegrenzung, Shell ist waehrenddessen blockiert
        vga_print("Waiting "); print_int(secs); vga_print("s...\n");
        for(int s = 0; s < secs; s++)
            pit_wait_ms(1000);
        vga_print("Time's up!\n");
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
        vga_print("Go!\n");
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
        for(int i = 0; i < FS_MAX_FILES; i++) {
            fs_table[i].used = 0;
            fs_table[i].name[0] = 0;
            fs_table[i].size = 0;
            fs_table[i].start_lba = 0;
            for(int j = 0; j < sizeof(fs_table[i].reserved); j++)
                fs_table[i].reserved[j] = 0;
        }
        fs_cwd[0] = 0;
        if(fs_save()) vga_print("Filesystem cleared\n");
        else vga_print("Error: could not save the filesystem\n");
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
        vga_print("man - the manual\nUse 'man' followed by a command for usage information.\n");
    }
    else if(strcmp(input_buf, "fasfetch") == 0) {
        vga_print("Try fastfetch or neofetch!\n");
    }
    /*else if(strcmp(input_buf, "test") == 0) {
        vga_print("hej\ntest\ntest\ntest\ntest\n");
    }
    else if(strcmp(input_buf, "banane") == 0) {
        vga_print("banane gegessen!");
    }*/
    else if(input_idx > 0) {
        vga_print("Unknown command: ");
        vga_print(input_buf); vga_putc('\n');
    }
command_done:
    input_idx = 0;
    cursor_col = 0;
    if(desktop_active && !desktop_terminal_focused()) return;
    shell_prompt();
    prompt_x = text_x; prompt_y = text_y;
    blink_visible = 1; last_blink_tick = ticks;
    draw_cursor_bar(1);
}
