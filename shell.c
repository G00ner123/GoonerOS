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
static const char* shell_stdin_data;
static int shell_status;

#define SHELL_PIPE_STAGES 8
#define SHELL_CAPTURE_SIZE (FS_MAX_FILE_BYTES + 1)

static char shell_pipe_buffer[SHELL_CAPTURE_SIZE];

static void print_ui(const char* english, const char* german) {
    vga_print(ui_text(english, german));
}

static void shell_printf(const char* format) {
    for(int i = 0; format[i]; i++) {
        if(format[i] == '\\' && format[i+1]) {
            i++;
            if(format[i] == 'n') vga_putc('\n');
            else if(format[i] == 't') vga_putc('\t');
            else if(format[i] == '\\') vga_putc('\\');
            else { vga_putc('\\'); vga_putc(format[i]); }
        } else if(format[i] == '%' && format[i+1] == '%') {
            vga_putc('%');
            i++;
        } else {
            vga_putc(format[i]);
        }
    }
}

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
        print_ui("ls: directory not found\n", "ls: Verzeichnis nicht gefunden\n");
        return;
    }
    int any = 0;
    if(long_format) print_ui("TYPE  SIZE      NAME\n", "TYP   GROESSE   NAME\n");
    for(int i = 0; i < FS_MAX_FILES; i++) {
        struct fs_entry* entry = &fs_table[i];
        if(!entry->used || !fs_is_direct_child(entry, path)) continue;
        const char* basename = fs_basename(entry->name);
        if(!show_hidden && basename[0] == '.') continue;
        if(long_format) {
            vga_print(fs_is_directory(entry)
                ? ui_text("DIR   ", "ORDNER") : ui_text("FILE  ", "DATEI "));
            print_int((int)entry->size); vga_print(" B     ");
        }
        vga_print(basename);
        if(fs_is_directory(entry)) vga_putc('/');
        vga_putc('\n');
        any = 1;
    }
    if(!any) print_ui("(empty)\n", "(leer)\n");
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
    if(!path[0]) return 1;
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

static int read_shell_input(const char* name, char* out, int capacity) {
    if(name && name[0]) return read_file_from_cwd(name, out, capacity);
    if(!shell_stdin_data || capacity <= 0) return -1;
    int len = strlen(shell_stdin_data);
    if(len >= capacity) len = capacity - 1;
    for(int i = 0; i < len; i++) out[i] = shell_stdin_data[i];
    out[len] = 0;
    return len;
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
        "      .-------.",
        "     /  .---.  \\",
        "    |  / ___ \\  |",
        " |--| | |   | | |--|",
        " |  |G|O|O N|E|R|  |",
        " |  | | |O S| | |  |",
        " |  | | \\   / | |  |",
        " |  | |  -_-  | |  |",
        " |--|  \\/  \\ /  |--|",
        "    |   -___-   |",
        "     \\/      \\ /",
        "      '-------'",
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
    goonfetch_prefix(logo[2]); print_ui("OS: GoonerOS v0.10\n", "Betriebssystem: GoonerOS v0.10\n");
    goonfetch_prefix(logo[3]); print_ui("Kernel: i686 / 32-bit\n", "Kernel: i686 / 32-Bit\n");
    goonfetch_prefix(logo[4]); print_ui("Shell: GoonerOS shell\n", "Shell: GoonerOS-Shell\n");
    goonfetch_prefix(logo[5]); print_ui(desktop_active ? "Session: desktop\n" : "Session: console\n",
                                        desktop_active ? "Sitzung: Desktop\n" : "Sitzung: Konsole\n");
    goonfetch_prefix(logo[6]); print_ui("Display: ", "Anzeige: ");
    print_int(VESA_WIDTH); vga_putc('x'); print_int(VESA_HEIGHT); vga_putc('x');
    print_int((int)vga_get_bpp()); vga_print("\n");
    goonfetch_prefix(logo[7]); print_ui("Keys: ", "Tastatur: ");
    vga_print(keyboard_get_layout() == KEYBOARD_LAYOUT_EN ? "en / QWERTY\n" : "de / QWERTZ\n");
    goonfetch_prefix(logo[8]); print_ui("Uptime: ", "Laufzeit: ");
    print_int((int)(ticks / 1000)); print_ui(" seconds\n", " Sekunden\n");
    goonfetch_prefix(logo[9]); vga_print("RTC: 20"); goonfetch_two_digits(y); vga_putc('-');
    goonfetch_two_digits(mo); vga_putc('-'); goonfetch_two_digits(d); vga_putc(' ');
    goonfetch_two_digits(h); vga_putc(':'); goonfetch_two_digits(m); vga_putc(':');
    goonfetch_two_digits(s); vga_putc('\n');
    goonfetch_prefix(logo[10]); print_ui("FS entries: ", "Dateisystemeintraege: ");
    print_int((int)file_count); vga_putc('/');
    print_int(FS_MAX_FILES); print_ui("  data: ", "  Daten: ");
    print_int((int)used_bytes); vga_print(" B\n");
    goonfetch_prefix(logo[11]); print_ui("Heap: ", "Heap: ");
    print_int((int)(heap_ptr-HEAP_START)); vga_print(" B  ");
    print_ui("Jobs: ", "Aufgaben: "); print_int(scheduler_task_count());
    vga_putc('/'); print_int(SCHEDULER_MAX_TASKS);
    print_ui(" cooperative\n", " kooperativ\n");
    vga_set_text_color(0xFFFFFF);
    vga_putc('\n');
}

void print_int(int n) {
    char buf[16]; int i = 0;
    if(n == 0) { vga_putc('0'); return; }
    unsigned int value;
    if(n < 0) {
        vga_putc('-');
        value = 0u - (unsigned int)n;
    } else value = (unsigned int)n;
    while(value > 0) { buf[i++] = '0' + value % 10; value /= 10; }
    while(i--) vga_putc(buf[i]);
}

static int calc_overflow(int a, int b, char op) {
    const int max = 2147483647;
    const int min = (-2147483647 - 1);
    if(op == '+') return (b > 0 && a > max - b) || (b < 0 && a < min - b);
    if(op == '-') return (b < 0 && a > max + b) || (b > 0 && a < min + b);
    if(op == '*') {
        if(a > 0) return b > 0 ? a > max / b : b < min / a;
        if(a < 0) return b > 0 ? a < min / b : b < 0 && a < max / b;
    }
    return op == '/' && a == min && b == -1;
}

void reboot(void) {
    asm volatile("cli");
    unsigned char status;
    int timeout = 100000;
    do {
        status = inb(0x64);
        if(status & 1) inb(0x60); // wartende Daten abholen, damit nicht im Weg stehen
        timeout--;
    } while((status & 2) && timeout > 0); // warten bis Eingabepuffer Bit 1 leer is
    outb(0x64, 0xFE);
    for(;;) asm volatile("hlt");
}

void beep(void) {
    outb(0x61, inb(0x61)|3); // macht speaker und gate 2 an
    outb(0x43,0xB6);        // setzt PIT channel 2 auf wellen generator 
    outb(0x42,0xA9);        // setzt frequenz auf low
    outb(0x42,0x04);        // setzut frequenz auf high, 4A9h=1193180/ 1193=~~1kHz

    // beep audio
    pit_wait_ms(150);

    outb(0x61, inb(0x61) & 0xFC); // macht speaker aus
}

int parse_int(char** p) {
    while(**p == ' ') (*p)++;
    int neg = 0;
    if(**p == '-') { neg = 1; (*p)++; }
    unsigned int limit = neg ? 0x80000000u : 0x7FFFFFFFu;
    unsigned int value = 0;
    while(**p >= '0' && **p <= '9') {
        unsigned int digit = (unsigned int)(**p - '0');
        if(value > (limit - digit) / 10) value = limit;
        else if(value < limit) value = value * 10 + digit;
        (*p)++;
    }
    if(neg) {
        if(value >= 0x80000000u) return (-2147483647 - 1);
        return -(int)value;
    }
    if(value >= 0x7FFFFFFFu) return 2147483647;
    return (int)value;
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
    return 0; // (noch ?) Kein Netzwerkstack da aktuell nicht verfügbar
}
static int shell_is_builtin(const char* name) {
    static const char* commands[] = {
        "help", "ls", "loadkeys", "pwd", "cd", "mkdir", "rmdir", "rm", "df", "fsck",
        "clear", "clean", "echo", "cat", "write", "draw", "circle", "pixel", "uptime",
        "rand", "shutdown", "version", "goonfetch", "neofetch", "mintlogo", "info", "matrix",
        "gooneros", "uname", "hostname", "free", "ps", "jobs", "spawn", "kill", "sleep",
        "yes", "cal", "su", "passwd", "lscpu", "top", "env", "history", "basename", "dirname",
        "head", "tail", "wc", "touch", "cp", "mv", "stat", "find", "du", "chmod", "type",
        "xxd", "sum", "fortune", "banner", "colors", "grep", "sort", "rev", "tr", "which",
        "whereis", "id", "groups", "arch", "dmesg", "vmstat", "lsblk", "mount", "nproc",
        "printf", "true", "false", "logout", "halt", "poweroff", "sl", "diff", "less", "more",
        "nano", "vi", "vim", "apt", "pacman", "battery", "cls", "calc", "logo", "whoami",
        "exit", "time", "date", "reboot", "meminfo", "beep", "seq", "factor", "prime", "fib",
        "factorial", "rot13", "base64", "hex", "dec", "ascii", "chr", "man", "who", "cksum",
        "ping", "ifconfig", "sensors", "systemctl", "journalctl", "crontab", "ida", "fire",
        "snow", "clock", "theme", "window", "mouse", "append", "search", "alarm", "countdown",
        "genpass", "wipe", "desktop", "fullscreen", "dih", "fasfetch", "uniq", "paging",
        "userdemo", "userfault"
    };
    if(!name || !name[0]) return 0;
    for(unsigned int i = 0; i < sizeof(commands)/sizeof(commands[0]); i++)
        if(strcmp(name, commands[i]) == 0) return 1;
    return 0;
}

static void shell_execute_simple(void) {
    input_buf[input_idx] = 0;
    shell_status = 0;
    if(strcmp(input_buf, "help") == 0) {
        print_ui(
            "Core tools: ls cd pwd tree find mkdir rmdir rm mv touch cat write append cp stat df du fsck grep sort uniq wc head tail basename dirname cksum xxd echo printf loadkeys date time uptime uname hostname calc userdemo userfault\n",
            "Grundbefehle: ls cd pwd tree find mkdir rmdir rm mv touch cat write append cp stat df du fsck grep sort uniq wc head tail basename dirname cksum xxd echo printf loadkeys date time uptime uname hostname calc userdemo userfault\n");
        print_ui(
            "More: clear diff rev tr seq factor prime fib factorial base64 hex dec ascii chr man paging spawn kill ps jobs top env history cal free lscpu lsblk mount theme mouse fullscreen\n",
            "Weitere: clear diff rev tr seq factor prime fib factorial base64 hex dec ascii chr man paging spawn kill ps jobs top env history cal free lscpu lsblk mount theme mouse fullscreen\n");
        print_ui("Operators: COMMAND && COMMAND, COMMAND | FILTER, COMMAND > FILE, COMMAND >> FILE\n",
                 "Operatoren: BEFEHL && BEFEHL, BEFEHL | FILTER, BEFEHL > DATEI, BEFEHL >> DATEI\n");
        print_ui("Pipelines: cat, grep, sort, uniq, wc, head, tail, tr accept piped text\n",
                 "Pipes: cat, grep, sort, uniq, wc, head, tail, tr verarbeiten Text aus einer Pipe\n");
        print_ui(
            "Not implemented: chmod, systemctl, apt, ping, ifconfig, sensors, journalctl, crontab, nano\n",
            "Nicht implementiert: chmod, systemctl, apt, ping, ifconfig, sensors, journalctl, crontab, nano\n");
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
            shell_status = 1;
            print_ui("Usage: ls [-a] [-l] [PATH] (also -la/-al)\n",
                     "Aufruf: ls [-a] [-l] [PFAD] (auch -la/-al)\n");
        } else if(!*arg) {
            for(int i = 0; i < FS_NAME_LEN; i++) path[i] = fs_cwd[i];
            list_directory(path, long_format, show_hidden);
        } else if(resolve_fs_path(arg, path)) {
            list_directory(path, long_format, show_hidden);
        } else {
            shell_status = 1;
            print_ui("ls: Path is too long or invalid\n", "ls: Pfad ist zu lang oder ungueltig\n");
        }
    }
    else if(strcmp(input_buf, "loadkeys") == 0) {
        print_ui("Keyboard layout: ", "Tastaturlayout: ");
        vga_print(keyboard_get_layout() == KEYBOARD_LAYOUT_EN ? "en (QWERTY)\n" : "de (QWERTZ)\n");
        print_ui("Language follows the keyboard layout. Switch with: loadkeys de | loadkeys en\n",
                 "Die Sprache folgt dem Tastaturlayout. Wechsel mit: loadkeys de | loadkeys en\n");
    }
    else if(strcmp(input_buf, "loadkeys de") == 0 || strcmp(input_buf, "loadkeys en") == 0) {
        int layout = input_buf[9] == 'e' ? KEYBOARD_LAYOUT_EN : KEYBOARD_LAYOUT_DE;
        keyboard_set_layout(layout);
        const char config[] = {'1', ',', input_buf[9], input_buf[10], '\n'};
        if(fs_write("goonkeys.cfg", config, sizeof(config))) {
            vga_print(layout == KEYBOARD_LAYOUT_EN ? "en (QWERTY)" : "de (QWERTZ)");
            print_ui(" selected; system language saved\n", " ausgewaehlt; Systemsprache gespeichert\n");
        } else {
            shell_status = 1;
            print_ui("Layout selected but not saved; check the filesystem or disk\n",
                     "Layout gewaehlt, aber nicht gespeichert; Dateisystem oder Datentraeger pruefen\n");
        }
    }
    else if(strcmp(input_buf, "pwd") == 0)
        print_working_directory();
    else if(strncmp(input_buf, "loadkeys ", 9) == 0)
    {
        shell_status = 1;
        print_ui("Usage: loadkeys de | loadkeys en\n", "Aufruf: loadkeys de | loadkeys en\n");
    }
    else if(strcmp(input_buf, "cd") == 0 || strcmp(input_buf, "cd /") == 0)
        fs_cwd[0] = 0;
    else if(strncmp(input_buf, "cd ", 3) == 0) {
        char path[FS_NAME_LEN];
        if(resolve_fs_path(&input_buf[3], path) && (!path[0] || fs_is_directory(fs_find(path)))) {
            int i = 0;
            while(path[i]) { fs_cwd[i] = path[i]; i++; }
            fs_cwd[i] = 0;
        } else {
            shell_status = 1;
            print_ui("cd: Directory not found or path too long\n",
                     "cd: Verzeichnis nicht gefunden oder Pfad zu lang\n");
        }
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
        if(!*arg) print_ui("Usage: mkdir [-p] DIRECTORY\n", "Aufruf: mkdir [-p] VERZEICHNIS\n");
        else if(!resolve_fs_path(arg, path)) {
            shell_status = 1;
            print_ui("mkdir: invalid or overlong path\n", "mkdir: ungueltiger oder zu langer Pfad\n");
        }
        else if(parents ? make_directories(path) : fs_mkdir(path)) print_ui("Directory created\n", "Verzeichnis erstellt\n");
        else {
            shell_status = 1;
            print_ui("mkdir: already exists, parent missing, or filesystem full\n",
                     "mkdir: bereits vorhanden, Elternverzeichnis fehlt oder Dateisystem voll\n");
        }
    }
    else if(strcmp(input_buf, "mkdir") == 0)
    {
        shell_status = 1;
        print_ui("Usage: mkdir DIRECTORY\n", "Aufruf: mkdir VERZEICHNIS\n");
    }
    else if(strncmp(input_buf, "rmdir ", 6) == 0) {
        char path[FS_NAME_LEN];
        if(!resolve_fs_path(&input_buf[6], path)) {
            shell_status = 1;
            print_ui("rmdir: invalid or overlong path\n", "rmdir: ungueltiger oder zu langer Pfad\n");
        }
        else if(fs_rmdir(path)) { leave_removed_directory(path); print_ui("Directory removed\n", "Verzeichnis entfernt\n"); }
        else {
            shell_status = 1;
            print_ui("rmdir: directory is not empty or was not found\n",
                     "rmdir: Verzeichnis nicht leer oder nicht gefunden\n");
        }
    }
    else if(strcmp(input_buf, "rmdir") == 0)
    {
        shell_status = 1;
        print_ui("Usage: rmdir DIRECTORY (must be empty)\n",
                 "Aufruf: rmdir VERZEICHNIS (muss leer sein)\n");
    }
    else if(strncmp(input_buf, "rm -r ", 6) == 0) {
        char path[FS_NAME_LEN];
        if(!resolve_fs_path(&input_buf[6], path)) {
            shell_status = 1;
            print_ui("rm -r: invalid or overlong path\n", "rm -r: ungueltiger oder zu langer Pfad\n");
        }
        else if(fs_remove_tree(path)) {
            leave_removed_directory(path);
            print_ui("Directory tree removed\n", "Verzeichnisbaum entfernt\n");
        } else {
            shell_status = 1;
            print_ui("rm -r: directory not found\n", "rm -r: Verzeichnis nicht gefunden\n");
        }
    }
    else if(strcmp(input_buf, "df") == 0) {
        int used_blocks = 0;
        for(int i = 0; i < FS_MAX_FILES; i++)
            if(fs_table[i].used) used_blocks++;
        int capacity = FS_MAX_FILES * FS_MAX_FILE_BYTES;
        int used = used_blocks * FS_MAX_FILE_BYTES;
        print_ui("Filesystem      Size(KiB) Used(KiB) Avail(KiB) (reserved blocks)\n",
                 "Dateisystem     Groesse(KiB) Belegt(KiB) Frei(KiB) (reservierte Bloecke)\n");
        vga_print("/dev/ata0-fs    ");
        print_int(capacity / 1024); vga_print("        ");
        print_int(used / 1024); vga_print("        ");
        print_int((capacity - used) / 1024); vga_putc('\n');
    }
    else if(strcmp(input_buf, "fsck") == 0) {
        int errors = fs_check();
        if(errors == 0) print_ui("Filesystem clean\n", "Dateisystem fehlerfrei\n");
        else { shell_status = 1; print_ui("Filesystem errors: ", "Dateisystemfehler: "); print_int(errors); vga_putc('\n'); }
    }
    else if(strcmp(input_buf, "clear") == 0 || strcmp(input_buf, "clean") == 0) {
        if(desktop_active) vga_clear_region(); else vga_clear();
    }
    else if(strncmp(input_buf, "echo ", 5) == 0) {
        vga_print(&input_buf[5]); vga_putc('\n');
    }
    else if(strcmp(input_buf, "echo") == 0)
        vga_putc('\n');
    else if(strcmp(input_buf, "cat") == 0) {
        if(!shell_stdin_data)
            print_ui("Usage: cat FILE or use cat in a pipeline\n",
                     "Aufruf: cat DATEI oder cat in einer Pipe nutzen\n");
        else vga_print(shell_stdin_data);
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
        if(!resolve_fs_path(arg, path)) {
            shell_status = 1;
            print_ui("cat: invalid or overlong path\n", "cat: ungueltiger oder zu langer Pfad\n");
        } else if(fs_read(path, cmd_buf, sizeof(cmd_buf)) < 0) {
            shell_status = 1;
            print_ui("cat: file not found or not a regular file\n", "cat: Datei nicht gefunden oder keine regulaere Datei\n");
        }
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
            if(!resolve_fs_path(&input_buf[6], path)) {
                shell_status = 1;
                print_ui("write: invalid or overlong path\n", "write: ungueltiger oder zu langer Pfad\n");
            }
            else if(fs_write(path, sp+1, strlen(sp+1))) vga_print("OK\n");
            else {
                shell_status = 1;
                print_ui("write: failed (check parent directory, size, or disk)\n",
                         "write: fehlgeschlagen (Elternverzeichnis, Groesse oder Datentraeger pruefen)\n");
            }
        }
        else {
            shell_status = 1;
            print_ui("Usage: write FILE TEXT\n", "Aufruf: write DATEI TEXT\n");
        }
    }
    else if(strcmp(input_buf, "draw rm") == 0) {
        clear_last_draw();
        last_draw_active = 0;
        print_ui("Removed\n", "Entfernt\n");
    }
    else if(strcmp(input_buf, "draw rectangle") == 0 || strcmp(input_buf, "draw rect") == 0) {
        clear_last_draw();
        draw_rect(512-60, 384-40, 120, 80, 0x00FF00);
        remember_draw(512-60, 384-40, 120, 80);
        print_ui("Drawn\n", "Gezeichnet\n");
    }
    else if(strcmp(input_buf, "draw circle") == 0) {
        clear_last_draw();
        draw_circle_filled(512, 384, 60, 0x00FF00);
        remember_draw(512-60, 384-60, 121, 121);
        print_ui("Drawn\n", "Gezeichnet\n");
    }
    else if(strncmp(input_buf, "draw ", 5) == 0) {
        char* p = &input_buf[5];
        while(*p == ' ') p++;
        if(!((*p >= '0' && *p <= '9') || *p == '-')) {
            print_ui("Usage: draw x y w h | draw rectangle | draw circle\n",
                     "Aufruf: draw x y breite hoehe | draw rectangle | draw circle\n");
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
            print_ui("Drawn at ", "Gezeichnet bei "); print_int(x); vga_putc(','); print_int(y);
            print_ui(" size ", " Groesse "); print_int(w); vga_putc('x'); print_int(h); vga_putc('\n');
        }
    }
    else if(strcmp(input_buf, "draw") == 0)
        print_ui("Usage: draw x y w h | draw rectangle | draw circle\n",
                 "Aufruf: draw x y breite hoehe | draw rectangle | draw circle\n");
    else if(strncmp(input_buf, "circle ", 7) == 0) {
        char* p = &input_buf[7];
        int x = parse_int(&p);
        int y = parse_int(&p);
        int r = parse_int(&p);
        if(r <= 0) r = 40;
        if(r > VESA_WIDTH) r = VESA_WIDTH;
        if(x < -r || x >= VESA_WIDTH+r || y < -r || y >= VESA_HEIGHT+r) {
            print_ui("circle: drawing is outside the display\n",
                     "circle: Kreis liegt ausserhalb der Anzeige\n");
            goto command_done;
        }
        clear_last_draw();
        draw_circle_filled(x, y, r, 0x00FFFF);
        int left = x-r, top = y-r, right = x+r+1, bottom = y+r+1;
        if(left < 0) left = 0;
        if(top < 0) top = 0;
        if(right > VESA_WIDTH) right = VESA_WIDTH;
        if(bottom > VESA_HEIGHT) bottom = VESA_HEIGHT;
        remember_draw(left, top, right-left, bottom-top);
        print_ui("Drawn\n", "Gezeichnet\n");
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
        // Einfacher Pseudozufall auf Basis der Timer-Ticks
        unsigned int seed = ticks * 2654435761u + 12345;
        print_int((int)(seed % 100)); vga_putc('\n');
    }
    else if(strcmp(input_buf, "shutdown") == 0) {
        print_ui("Shutting down...\n", "System wird heruntergefahren...\n");
        outw(0x604, 0x2000);
        outw(0xB004, 0x2000);
        for(;;) asm volatile("hlt");
    }
    else if(strcmp(input_buf, "version") == 0)
        print_ui("GOonerOS v0.10 Full 780L\n", "GOonerOS v0.10 Vollversion 780L\n");
    else if(strcmp(input_buf, "goonfetch") == 0)
        goonfetch();
    else if(strcmp(input_buf, "neofetch") == 0) {
        goonfetch();
    }
    else if(strcmp(input_buf, "mintlogo") == 0) {
        mint_visible = !mint_visible;
        draw_mint_logo(mint_visible);
        vga_print(mint_visible ? ui_text("Mint logo on\n", "Mint-Logo an\n")
                               : ui_text("Mint logo off\n", "Mint-Logo aus\n"));
    }
    else if(strcmp(input_buf, "info") == 0) {
        print_ui("=== GoonerOS System Information ===\n", "=== GoonerOS-Systeminformationen ===\n");
        print_ui("Version: v0.10 Full 780L\n", "Version: v0.10 Vollversion 780L\n");
        print_ui("Resolution: ", "Aufloesung: ");
        print_int(VESA_WIDTH); vga_putc('x'); print_int(VESA_HEIGHT);
        vga_print(" ("); print_int(VESA_WIDTH*VESA_HEIGHT); print_ui(" pixels)\n", " Pixel)\n");
        print_ui("Uptime: ", "Laufzeit: "); print_int(ticks/1000);
        print_ui("s (", "s ("); print_int((int)ticks); print_ui(" ticks)\n", " Ticks)\n");
        print_ui("Kernel: 32-bit protected mode\n", "Kernel: 32-Bit Protected Mode\n");
        print_ui("Framebuffer: ", "Framebuffer: ");
        print_int((int)vga_get_pitch()); print_ui(" bytes/row, ", " Bytes/Zeile, ");
        print_int((int)vga_get_bpp()); vga_print(" bpp\n");
        print_ui("Keyboard layout: ", "Tastaturlayout: ");
        vga_print(keyboard_get_layout() == KEYBOARD_LAYOUT_EN ? "en (QWERTY)\n" : "de (QWERTZ)\n");
        print_ui("Filesystem check: ", "Dateisystempruefung: ");
        int errors = fs_check();
        if(errors == 0) print_ui("clean\n", "fehlerfrei\n");
        else { print_int(errors); print_ui(" errors\n", " Fehler\n"); }
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
        print_ui("Kernel heap (not total physical memory)\n",
                 "Kernel-Heap (nicht der gesamte physische Speicher)\n");
        print_ui("Allocated: ", "Belegt: ");
        print_int((int)(heap_ptr-HEAP_START)); print_ui(" bytes\n", " Bytes\n");
        print_ui("Total RAM: not detected by this kernel\n",
                 "Gesamter Arbeitsspeicher: vom Kernel nicht erkannt\n");
    }
    else if(strcmp(input_buf, "ps") == 0 || strcmp(input_buf, "jobs") == 0) {
        print_ui("PID  TASK       STATE      PROGRESS / RESULT\n",
                 "PID  AUFGABE    STATUS     FORTSCHRITT / ERGEBNIS\n");
        vga_print("  1  kernel_main      ");
        print_ui("running     event loop\n", "aktiv       Ereignisschleife\n");
        for(int i = 0; i < scheduler_task_count(); i++) {
            int pid, type, state;
            unsigned int steps, progress, total, result;
            if(!scheduler_get_task_info(i, &pid, &type, &state, &steps, &progress, &total, &result))
                continue;
            vga_putc(' '); print_int(pid);
            vga_print(type == SCHEDULER_TASK_CHECKSUM
                ? ui_text("  checksum   ", "  Pruefsumme  ")
                : ui_text("  counter    ", "  Zaehler     "));
            vga_print(state == SCHEDULER_STATE_DONE
                ? ui_text("done       ", "fertig      ")
                : ui_text("runnable   ", "bereit      "));
            if(type == SCHEDULER_TASK_CHECKSUM) {
                print_int((int)progress); vga_putc('/'); print_int((int)total);
                if(state == SCHEDULER_STATE_DONE) {
                    vga_print("  CRC32=0x"); print_hex32(result);
                }
            } else {
                print_int((int)steps); print_ui(" steps", " Schritte");
            }
            vga_putc('\n');
        }
        print_ui("Cooperative tasks: ", "Kooperative Aufgaben: ");
        print_int(scheduler_task_count());
        print_ui(" (one bounded work-slice per event-loop turn)\n",
                 " (ein begrenzter Arbeitsschritt pro Ereignisschleifen-Durchlauf)\n");
    }
    else if(strcmp(input_buf, "spawn counter") == 0) {
        int pid = scheduler_spawn_counter();
        if(pid < 0) print_ui("spawn: task table is full\n", "spawn: Aufgabentabelle ist voll\n");
        else { print_ui("Cooperative counter started, PID ", "Kooperativer Zaehler gestartet, PID "); print_int(pid); vga_putc('\n'); }
    }
    else if(strncmp(input_buf, "spawn checksum ", 15) == 0) {
        char path[FS_NAME_LEN];
        if(!resolve_fs_path(&input_buf[15], path) || !path[0]) {
            print_ui("Usage: spawn checksum FILE (path is invalid or too long)\n",
                     "Aufruf: spawn checksum DATEI (Pfad ungueltig oder zu lang)\n");
        } else {
            int pid = scheduler_spawn_checksum(path);
            if(pid < 0) print_ui("spawn checksum: file unreadable, task table full, or CRC task already running\n",
                                 "spawn checksum: Datei unlesbar, Aufgabentabelle voll oder CRC-Aufgabe laeuft bereits\n");
            else { print_ui("Cooperative CRC32 job started, PID ", "Kooperative CRC32-Aufgabe gestartet, PID "); print_int(pid); vga_putc('\n'); }
        }
    }
    else if(strcmp(input_buf, "spawn checksum") == 0)
        print_ui("Usage: spawn checksum FILE\n", "Aufruf: spawn checksum DATEI\n");
    else if(strncmp(input_buf, "kill ", 5) == 0) {
        char* p = &input_buf[5];
        int pid = parse_int(&p);
        if(scheduler_kill(pid)) { print_ui("Task stopped: ", "Aufgabe beendet: "); print_int(pid); vga_putc('\n'); }
        else print_ui("kill: unknown or protected PID\n", "kill: unbekannte oder geschuetzte PID\n");
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
        print_ui("Today: ", "Heute: "); print_int(d); vga_putc('/'); print_int(mo); vga_print("/20"); print_int(y); vga_putc('\n');
    }
    else if(strcmp(input_buf, "su") == 0)
        print_ui("You are already root\n", "Du bist bereits root\n");
    else if(strcmp(input_buf, "passwd") == 0)
        print_ui("No password is configured\n", "Es ist kein Passwort eingerichtet\n");
    else if(strcmp(input_buf, "lscpu") == 0) {
        print_ui("Architecture: i686-compatible x86 target\n", "Architektur: i686-kompatibles x86-Ziel\n");
        print_ui("Mode: 32-bit Protected Mode\n", "Modus: 32-Bit Protected Mode\n");
        print_ui("CPU topology: not detected\n", "CPU-Topologie: nicht erkannt\n");
    }
    else if(strcmp(input_buf, "top") == 0) {
        print_ui("CONTEXT       STATE       DETAIL\n", "KONTEXT       STATUS      DETAIL\n");
        vga_print("kernel_main   "); print_ui("running     ", "aktiv       ");
        print_int(heap_ptr-HEAP_START); print_ui(" B heap\n", " B Heap\n");
        print_ui("cooperative   runnable    ", "kooperativ    bereit      ");
        print_int(scheduler_task_count()); print_ui(" tasks, bounded slices\n", " Aufgaben, begrenzte Zeitscheiben\n");
        print_ui("Uptime: ", "Laufzeit: "); print_int(ticks/1000); vga_print("s\n");
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
    else if(strncmp(input_buf, "basename ", 9) == 0) {
        char* path = &input_buf[9];
        if(!*path) {
            print_ui("Usage: basename PATH\n", "Aufruf: basename PFAD\n");
            goto command_done;
        }
        int len = strlen(path);
        while(len > 1 && path[len-1] == '/') len--;
        if(len == 1 && path[0] == '/') {
            vga_print("/\n");
        } else {
            int start = len;
            while(start > 0 && path[start-1] != '/') start--;
            while(start < len) vga_putc(path[start++]);
            vga_putc('\n');
        }
    }
    else if(strcmp(input_buf, "basename") == 0)
        print_ui("Usage: basename PATH\n", "Aufruf: basename PFAD\n");
    else if(strncmp(input_buf, "dirname ", 8) == 0) {
        char* path = &input_buf[8];
        if(!*path) {
            print_ui("Usage: dirname PATH\n", "Aufruf: dirname PFAD\n");
            goto command_done;
        }
        int len = strlen(path);
        while(len > 1 && path[len-1] == '/') len--;
        while(len > 0 && path[len-1] != '/') len--;
        while(len > 1 && path[len-1] == '/') len--;
        if(len == 0) vga_print(".\n");
        else {
            for(int i = 0; i < len; i++) vga_putc(path[i]);
            vga_putc('\n');
        }
    }
    else if(strcmp(input_buf, "dirname") == 0)
        print_ui("Usage: dirname PATH\n", "Aufruf: dirname PFAD\n");
    else if(strcmp(input_buf, "head") == 0 || strcmp(input_buf, "tail") == 0) {
        if(!shell_stdin_data)
            print_ui("Usage: head|tail [-n COUNT] FILE\n",
                     "Aufruf: head|tail [-n ANZAHL] DATEI\n");
        else {
            int is_tail = input_buf[0] == 't';
            int len = read_shell_input(0, cmd_buf, sizeof(cmd_buf));
            int start = 0, end = len, lines = 0;
            if(is_tail) {
                start = len;
                while(start > 0) {
                    start--;
                    if(cmd_buf[start] == '\n' && start < len-1 && ++lines == 5) { start++; break; }
                }
                if(lines < 5) start = 0;
            } else {
                end = 0;
                while(end < len && lines < 5) if(cmd_buf[end++] == '\n') lines++;
            }
            char saved = cmd_buf[end]; cmd_buf[end] = 0; vga_print(&cmd_buf[start]); cmd_buf[end] = saved;
        }
    }
    else if(strncmp(input_buf, "head ", 5) == 0 || strncmp(input_buf, "tail ", 5) == 0) {
        int is_tail = (input_buf[0] == 't');
        char* arg = &input_buf[5];
        while(*arg == ' ') arg++;
        int line_limit = 5;
        if(strncmp(arg, "-n", 2) == 0 && (arg[2] == ' ' || arg[2] == 0)) {
            arg += 2;
            while(*arg == ' ') arg++;
            if(*arg < '0' || *arg > '9') {
                print_ui("Usage: head|tail [-n COUNT] FILE\n",
                         "Aufruf: head|tail [-n ANZAHL] DATEI\n");
                goto command_done;
            }
            line_limit = parse_int(&arg);
            while(*arg == ' ') arg++;
        }
        if(!*arg && !shell_stdin_data) {
            print_ui("Usage: head|tail [-n COUNT] FILE\n",
                     "Aufruf: head|tail [-n ANZAHL] DATEI\n");
        } else if(read_shell_input(*arg ? arg : 0, cmd_buf, sizeof(cmd_buf)) < 0) {
            print_ui("File not found\n", "Datei nicht gefunden\n");
        } else if(line_limit > 0) {
            int len = strlen(cmd_buf);
            int start = 0, end = len;
            if(!is_tail) {
                int lines = 0;
                end = 0;
                while(end < len && lines < line_limit) {
                    if(cmd_buf[end++] == '\n') lines++;
                }
            } else {
                int lines = 0;
                start = len;
                while(start > 0) {
                    start--;
                    if(cmd_buf[start] != '\n' || start == len-1) continue;
                    if(++lines == line_limit) {
                        start++;
                        break;
                    }
                }
                if(lines < line_limit) start = 0;
            }
            char saved = cmd_buf[end];
            cmd_buf[end] = 0;
            vga_print(&cmd_buf[start]);
            cmd_buf[end] = saved;
            if(end > start && cmd_buf[end-1] != '\n') vga_putc('\n');
        }
    }
    else if(strcmp(input_buf, "wc") == 0 || strncmp(input_buf, "wc ", 3) == 0) {
        const char* source = input_buf[2] ? &input_buf[3] : 0;
        if(read_shell_input(source, cmd_buf, sizeof(cmd_buf)) < 0)
            print_ui("File not found\n", "Datei nicht gefunden\n");
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
    else if(strcmp(input_buf, "uniq") == 0 || strncmp(input_buf, "uniq ", 5) == 0) {
        const char* source = input_buf[4] ? &input_buf[5] : 0;
        if(read_shell_input(source, cmd_buf, sizeof(cmd_buf)) < 0) {
            shell_status = 1;
            print_ui("Usage: uniq [FILE] or use uniq in a pipeline\n",
                     "Aufruf: uniq [DATEI] oder uniq in einer Pipe nutzen\n");
        } else {
            int start = 0, len = strlen(cmd_buf);
            char* previous = 0;
            for(int i = 0; i <= len; i++) {
                if(cmd_buf[i] != '\n' && cmd_buf[i] != 0) continue;
                char saved = cmd_buf[i];
                cmd_buf[i] = 0;
                if(!previous || strcmp(previous, &cmd_buf[start]) != 0) {
                    vga_print(&cmd_buf[start]);
                    if(saved == '\n') vga_putc('\n');
                }
                previous = &cmd_buf[start];
                start = i + 1;
                if(saved == 0) break;
            }
        }
    }
    else if(strncmp(input_buf, "touch ", 6) == 0) {
        char path[FS_NAME_LEN];
        if(!resolve_fs_path(&input_buf[6], path)) {
            shell_status = 1;
            print_ui("touch: invalid or overlong path\n", "touch: ungueltiger oder zu langer Pfad\n");
        }
        else if(fs_create(path)) vga_print("OK\n");
        else {
            shell_status = 1;
            print_ui("touch: could not create file\n", "touch: Datei konnte nicht erstellt werden\n");
        }
    }
    else if(strncmp(input_buf, "rm ", 3) == 0) {
        char path[FS_NAME_LEN];
        if(!resolve_fs_path(&input_buf[3], path)) {
            shell_status = 1;
            print_ui("rm: invalid or overlong path\n", "rm: ungueltiger oder zu langer Pfad\n");
        }
        else if(fs_delete(path)) print_ui("Removed\n", "Entfernt\n");
        else {
            shell_status = 1;
            print_ui("rm: file not found (use rmdir for directories)\n",
                     "rm: Datei nicht gefunden (fuer Verzeichnisse rmdir nutzen)\n");
        }
    }
    else if(strncmp(input_buf, "cp ", 3) == 0) {
        char* sp = &input_buf[3];
        while(*sp && *sp != ' ') sp++;
        if(*sp) {
            *sp = 0;
            char source[FS_NAME_LEN], destination[FS_NAME_LEN];
            if(!resolve_fs_path(&input_buf[3], source) || !resolve_fs_path(sp+1, destination)) {
                shell_status = 1;
                print_ui("cp: invalid or overlong path\n", "cp: ungueltiger oder zu langer Pfad\n");
            }
            else if(fs_read(source, cmd_buf, sizeof(cmd_buf)) < 0) {
                shell_status = 1;
                print_ui("cp: source file not found\n", "cp: Quelldatei nicht gefunden\n");
            }
            else if(fs_write(destination, cmd_buf, strlen(cmd_buf))) vga_print("OK\n");
            else {
                shell_status = 1;
                print_ui("cp: copy failed\n", "cp: Kopieren fehlgeschlagen\n");
            }
        } else {
            shell_status = 1;
            print_ui("Usage: cp SOURCE DESTINATION\n", "Aufruf: cp QUELLE ZIEL\n");
        }
    }
    else if(strncmp(input_buf, "mv ", 3) == 0) {
        char* sp = &input_buf[3];
        while(*sp && *sp != ' ') sp++;
        if(*sp) {
            *sp = 0;
            char source[FS_NAME_LEN], destination[FS_NAME_LEN];
            if(!resolve_fs_path(&input_buf[3], source) || !resolve_fs_path(sp+1, destination)) {
                shell_status = 1;
                print_ui("mv: invalid or overlong path\n", "mv: ungueltiger oder zu langer Pfad\n");
            } else {
                struct fs_entry* entry = fs_find(source);
                int moving_directory = fs_is_directory(entry);
                if(fs_rename(source, destination)) {
                    if(moving_directory) move_working_directory(source, destination);
                    vga_print("OK\n");
                } else {
                    shell_status = 1;
                    print_ui("mv: move/rename failed\n", "mv: Verschieben/Umbenennen fehlgeschlagen\n");
                }
            }
        } else {
            shell_status = 1;
            print_ui("Usage: mv OLD NEW\n", "Aufruf: mv ALT NEU\n");
        }
    }
    else if(strncmp(input_buf, "stat ", 5) == 0) {
        char path[FS_NAME_LEN];
        struct fs_entry* e = resolve_fs_path(&input_buf[5], path) ? fs_find(path) : 0;
        if(!e) {
            shell_status = 1;
            print_ui("File not found\n", "Datei nicht gefunden\n");
        }
        else {
            print_ui("Name: ", "Name: "); vga_print(fs_basename(e->name)); vga_putc('\n');
            print_ui("Path: /", "Pfad: /"); vga_print(e->name); vga_putc('\n');
            print_ui("Type: ", "Typ: ");
            vga_print(fs_is_directory(e) ? ui_text("Directory\n", "Verzeichnis\n")
                                         : ui_text("File\n", "Datei\n"));
            print_ui("Size: ", "Groesse: "); print_int((int)e->size); vga_print(" B\n");
            print_ui("Start-LBA: ", "Start-LBA: "); print_int((int)e->start_lba); vga_putc('\n');
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
        if(!any) print_ui("Nothing found\n", "Nichts gefunden\n");
    }
    else if(strcmp(input_buf, "du") == 0) {
        int total = 0, dirs = 0, files = 0;
        for(int i = 0; i < FS_MAX_FILES; i++) if(fs_table[i].used) {
            if(fs_is_directory(&fs_table[i])) dirs++;
            else { files++; total += (int)fs_table[i].size; }
        }
        print_ui("Data: ", "Daten: "); print_int(total); vga_print(" B in ");
        print_int(files); print_ui(" files and ", " Dateien und ");
        print_int(dirs); print_ui(" directories\n", " Verzeichnissen\n");
    }
    else if(strncmp(input_buf, "du ", 3) == 0) {
        char path[FS_NAME_LEN];
        if(!resolve_fs_path(&input_buf[3], path)) print_ui("du: invalid path\n", "du: ungueltiger Pfad\n");
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
            print_ui("Data: ", "Daten: "); print_int(total); vga_print(" B in ");
            print_int(files); print_ui(" files\n", " Dateien\n");
        }
    }
    else if(strncmp(input_buf, "chmod ", 6) == 0)
        print_ui("GoonerOS has no file permissions yet; everyone has full access\n",
                 "GoonerOS hat noch keine Dateirechte; jeder hat vollen Zugriff\n");
    else if(strncmp(input_buf, "type ", 5) == 0) {
        const char* name = &input_buf[5];
        vga_print(name);
        if(shell_is_builtin(name)) print_ui(" is a shell builtin\n", " ist ein Shell-Builtin\n");
        else print_ui(": not found\n", ": nicht gefunden\n");
    }
    else if(strncmp(input_buf, "xxd ", 4) == 0) {
        if(read_file_from_cwd(&input_buf[4], cmd_buf, sizeof(cmd_buf)) < 0)
            print_ui("File not found\n", "Datei nicht gefunden\n");
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
        if(read_file_from_cwd(&input_buf[4], cmd_buf, sizeof(cmd_buf)) < 0)
            print_ui("File not found\n", "Datei nicht gefunden\n");
        else {
            unsigned int s = 0;
            for(int i = 0; cmd_buf[i]; i++) s = s*31 + (unsigned char)cmd_buf[i];
            print_int((int)s); vga_putc('\n');
        }
    }
    else if(strcmp(input_buf, "fortune") == 0) {
        const char* quotes[] = {
            "Platzhalter\n",
            "Platzhalter.\n",
            "Triple Fault: die Art des Computers zu sagen 'nochmal von vorn'.\n",
            "Platzthalter.\n",
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
        if(!*sp && !shell_stdin_data) {
            shell_status = 1;
            print_ui("Usage: grep PATTERN FILE\n", "Aufruf: grep MUSTER DATEI\n");
        }
        else {
            const char* source = 0;
            if(*sp) { *sp = 0; source = sp+1; }
            if(read_shell_input(source, cmd_buf, sizeof(cmd_buf)) < 0) {
                shell_status = 1;
                print_ui("File not found\n", "Datei nicht gefunden\n");
            } else {
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
                if(!any) {
                    shell_status = 1;
                    print_ui("(no matches)\n", "(keine Treffer)\n");
                }
            }
        }
    }
    else if(strcmp(input_buf, "sort") == 0 || strncmp(input_buf, "sort ", 5) == 0) {
        const char* source = input_buf[4] ? &input_buf[5] : 0;
        if(read_shell_input(source, cmd_buf, sizeof(cmd_buf)) < 0) {
            shell_status = 1;
            print_ui("File not found\n", "Datei nicht gefunden\n");
        } else {
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
        const char* text = shell_stdin_data ? shell_stdin_data : &input_buf[7];
        for(int i = 0; text[i]; i++) vga_putc(text[i] == a ? b : text[i]);
        if(!shell_stdin_data) vga_putc('\n');
    }
    else if(strncmp(input_buf, "which ", 6) == 0 || strncmp(input_buf, "whereis ", 8) == 0) {
        const char* name = input_buf[2] == 'i' ? &input_buf[6] : &input_buf[8];
        if(!*name) print_ui("Usage: which COMMAND\n", "Aufruf: which BEFEHL\n");
        else {
            vga_print(name);
            if(shell_is_builtin(name)) print_ui(": shell builtin\n", ": Shell-Builtin\n");
            else print_ui(": not found\n", ": nicht gefunden\n");
        }
    }
    else if(strcmp(input_buf, "id") == 0)
        vga_print("uid=0(root) gid=0(root)\n");
    else if(strcmp(input_buf, "groups") == 0)
        vga_print("root\n");
    else if(strcmp(input_buf, "arch") == 0)
        vga_print("i686\n");
    else if(strcmp(input_buf, "dmesg") == 0)
        print_ui("No kernel message buffer is available yet\n",
                 "Noch kein Kernel-Nachrichtenpuffer verfuegbar\n");
    else if(strcmp(input_buf, "vmstat") == 0) {
        vga_print("procs  mem\n");
        vga_print("   1   "); print_int(heap_ptr-HEAP_START); vga_print(" B belegt\n");
    }
    else if(strcmp(input_buf, "lsblk") == 0) {
        print_ui("DEVICE       SIZE    TYPE\n", "GERAET       GROESSE TYP\n");
        print_ui("ata0         1 MiB   QEMU image (build configuration)\n",
                 "ata0         1 MiB   QEMU-Abbild (Build-Konfiguration)\n");
        print_ui("ata0-fs      256 KiB filesystem data capacity\n",
                 "ata0-fs      256 KiB Dateisystem-Datenkapazitaet\n");
    }
    else if(strcmp(input_buf, "mount") == 0) {
        print_ui("ata0-fs on / type goonerfs (hierarchical, fixed table)\n",
                 "ata0-fs auf /, Typ goonerfs (hierarchisch, feste Tabelle)\n");
        print_ui("Metadata: LBA 512-514; data begins at LBA 515\n",
                 "Metadaten: LBA 512-514; Daten beginnen ab LBA 515\n");
    }
    else if(strcmp(input_buf, "nproc") == 0)
        print_ui("CPU topology is not detected by this kernel\n",
                 "CPU-Topologie wird von diesem Kernel nicht erkannt\n");
    else if(strncmp(input_buf, "printf ", 7) == 0)
        shell_printf(&input_buf[7]);
    else if(strcmp(input_buf, "printf") == 0)
        print_ui("Usage: printf TEXT\n", "Aufruf: printf TEXT\n");
    else if(strcmp(input_buf, "true") == 0)
        {}
    else if(strcmp(input_buf, "false") == 0)
        shell_status = 1;
    else if(strcmp(input_buf, "paging") == 0) {
        if(paging_is_active()) {
            print_ui("Paging: enabled, supervisor-only, null page unmapped\n",
                     "Paging: aktiv, nur Supervisor, Nullseite nicht abgebildet\n");
            print_ui("RAM: identity-mapped to 4 MiB; kernel text/rodata read-only; framebuffer mapped\n",
                     "RAM: bis 4 MiB identisch abgebildet; Kernelcode/Read-only-Daten schreibgeschuetzt; Framebuffer abgebildet\n");
        } else {
            shell_status = 1;
            print_ui("Paging is not active\n", "Paging ist nicht aktiv\n");
        }
    }
    else if(strcmp(input_buf, "userdemo") == 0) {
        print_ui("Starting two isolated ring-3 processes:\n",
                 "Starte zwei isolierte Ring-3-Prozesse:\n");
        for(int pid = 1; pid <= 2; pid++) {
            int result = user_process_run(pid, 0);
            if(result <= 0) {
                shell_status = 1;
                print_ui(result < 0 ? "User process hit a protection fault\n"
                                    : "User process could not be started\n",
                         result < 0 ? "User-Prozess stiess auf einen Zugriffsschutzfehler\n"
                                    : "User-Prozess konnte nicht gestartet werden\n");
                break;
            }
        }
    }
    else if(strcmp(input_buf, "userfault") == 0) {
        print_ui("Testing user-mode memory isolation:\n",
                 "Teste die Speicherschutzgrenze im Usermode:\n");
        if(user_process_run(1, 1) < 0)
            print_ui("User process stopped safely after a protected-memory fault\n",
                     "User-Prozess nach Zugriffsschutzfehler sicher beendet\n");
        else {
            shell_status = 1;
            print_ui("Expected user-mode page fault did not occur\n",
                     "Erwarteter Usermode-Seitenfehler trat nicht auf\n");
        }
    }
    else if(strcmp(input_buf, "logout") == 0 || strcmp(input_buf, "halt") == 0 || strcmp(input_buf, "poweroff") == 0) {
        print_ui("Shutting down...\n", "System wird heruntergefahren...\n");
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
        if(!*sp) print_ui("Usage: diff FILE1 FILE2\n", "Aufruf: diff DATEI1 DATEI2\n");
        else {
            *sp = 0;
            int r1 = read_file_from_cwd(&input_buf[5], cmd_buf, sizeof(cmd_buf));
            int r2 = read_file_from_cwd(sp+1, cmd_buf2, sizeof(cmd_buf2));
            if(r1 < 0 || r2 < 0) print_ui("File not found\n", "Datei nicht gefunden\n");
            else if(strcmp(cmd_buf, cmd_buf2) == 0) print_ui("Identical\n", "Identisch\n");
            else print_ui("Different\n", "Unterschiedlich\n");
        }
    }
    else if(strncmp(input_buf, "less ", 5) == 0 || strncmp(input_buf, "more ", 5) == 0) {
        char* name = &input_buf[5];
        if(read_file_from_cwd(name, cmd_buf, sizeof(cmd_buf)) < 0)
            print_ui("File not found\n", "Datei nicht gefunden\n");
        else { vga_print(cmd_buf); vga_putc('\n'); }
    }
    else if(strcmp(input_buf, "nano") == 0 || strcmp(input_buf, "vi") == 0 || strcmp(input_buf, "vim") == 0)
        print_ui("No terminal editor is available; use 'write name text'\n",
                 "Kein Terminal-Editor vorhanden - nutze 'write name text'\n");
    else if(strncmp(input_buf, "apt ", 4) == 0 || strncmp(input_buf, "pacman ", 7) == 0)
        print_ui("No package manager is available\n", "Kein Paketmanager vorhanden\n");
    else if(strcmp(input_buf, "battery") == 0)
        print_ui("No battery is available (virtual machine)\n",
                 "Kein Akku vorhanden (virtuelle Maschine)\n");
    else if(strcmp(input_buf, "cls") == 0) {
        if(desktop_active) vga_clear_region(); else vga_clear();
    }
    else if(strncmp(input_buf, "calc ", 5) == 0) {
        char* p = &input_buf[5];
        int a = parse_int(&p);
        while(*p == ' ') p++;
        char op = *p; p++;
        int b = parse_int(&p);
        if(op != '+' && op != '-' && op != '*' && op != '/')
            print_ui("Unknown operator (+ - * /)\n", "Unbekannter Operator (+ - * /)\n");
        else if(op == '/' && b == 0)
            print_ui("Division by zero\n", "Division durch 0\n");
        else if(calc_overflow(a, b, op))
            print_ui("Result exceeds the 32-bit integer range\n",
                     "Ergebnis ausserhalb des 32-Bit-Ganzzahlbereichs\n");
        else if(op == '+') { print_int(a+b); vga_putc('\n'); }
        else if(op == '-') { print_int(a-b); vga_putc('\n'); }
        else if(op == '*') { print_int(a*b); vga_putc('\n'); }
        else { print_int(a/b); vga_putc('\n'); }
    }
    else if(strcmp(input_buf, "logo") == 0)
        draw_arch_logo(900, 20, 330, 100, 0x1793D1);
    else if(strcmp(input_buf, "whoami") == 0) {
        // gibt aktuell noch keine echte IP.
        // get_ip() gibt 0 zurueck solange das so ist;
        char ip[16];
        if(get_ip(ip)) { vga_print("root@GoonerOS ("); vga_print(ip); vga_print(")\n"); }
        else vga_print("Root@GoonerOS\n");
    }
    else if(strcmp(input_buf, "exit") == 0) {
        print_ui("Shutting down...\n", "System wird heruntergefahren...\n");
        outw(0x604, 0x2000);  // ACPI-Shutdown, funktioniert bei den meisten QEMU Standardkonfigurationen
        outw(0xB004, 0x2000); // Fallback für manche QEMU-Versionen
        for(;;) asm volatile("hlt");
    }
    else if(strcmp(input_buf, "time") == 0 || strcmp(input_buf, "date") == 0)
        print_time_date();
    else if(strcmp(input_buf, "reboot") == 0) {
        print_ui("Rebooting...\n", "System wird neu gestartet...\n"); reboot();
    }
    else if(strcmp(input_buf, "meminfo") == 0) {
        print_ui("Heap used: ", "Heap belegt: ");
        print_int((int)(heap_ptr - HEAP_START));
        print_ui(" bytes\n", " Bytes\n");
        print_ui("Free physical pages: ", "Freie physische Seiten: ");
        print_int((int)page_free_count());
        print_ui(" / 256 (4 KiB each)\n", " / 256 (je 4 KiB)\n");
    }
    else if(strcmp(input_buf, "beep") == 0) {
        print_ui("Beep!\n", "Piep!\n"); beep();
    }
    /* ====================  Befehle ==================== */
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
        if(n > 45) n = 45; // 32-Bit int läuft danach über
        int a = 0, b = 1;
        for(int i = 0; i < n; i++) { int t = a+b; a = b; b = t; }
        print_int(a); vga_putc('\n');
    }
    else if(strncmp(input_buf, "factorial ", 10) == 0) {
        char* p = &input_buf[10];
        int n = parse_int(&p);
        if(n < 0) n = 0;
        if(n > 12) n = 12; // 13! passt nicht in 32 Bit
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
        if(strcmp(c, "ls") == 0) print_ui("ls [-a] [-l] [PATH] - -a shows hidden entries, -l shows details; combine as -la\n", "ls [-a] [-l] [PFAD] - -a zeigt versteckte Eintraege, -l zeigt Details; kombinierbar als -la\n");
        else if(strcmp(c, "loadkeys") == 0) print_ui("loadkeys de|en - select and save the German QWERTZ or English QWERTY layout\n", "loadkeys de|en - deutsches QWERTZ- oder englisches QWERTY-Layout waehlen und speichern\n");
        else if(strcmp(c, "mkdir") == 0) print_ui("mkdir [-p] PATH - create a directory; -p also creates parent directories\n", "mkdir [-p] PFAD - Verzeichnis erstellen; -p erstellt auch Elternverzeichnisse\n");
        else if(strcmp(c, "rmdir") == 0) print_ui("rmdir PATH - remove an empty directory\n", "rmdir PFAD - leeres Verzeichnis entfernen\n");
        else if(strcmp(c, "cd") == 0) print_ui("cd [PATH] - change the working directory; cd .. moves up one level\n", "cd [PFAD] - Arbeitsverzeichnis wechseln; cd .. geht eine Ebene nach oben\n");
        else if(strcmp(c, "pwd") == 0) print_ui("pwd - show the current working directory\n", "pwd - aktuelles Arbeitsverzeichnis anzeigen\n");
        else if(strcmp(c, "rm") == 0) print_ui("rm FILE | rm -r DIRECTORY - remove a file or directory tree\n", "rm DATEI | rm -r VERZEICHNIS - Datei oder Verzeichnisbaum entfernen\n");
        else if(strcmp(c, "help") == 0) print_ui("help - list available commands\n", "help - verfuegbare Befehle anzeigen\n");
        else if(strcmp(c, "countdown") == 0) print_ui("countdown N - count down from N; example: countdown 10\n", "countdown N - von N herunterzaehlen; Beispiel: countdown 10\n");
        else if(strcmp(c, "apt install") == 0) print_ui("apt install - unavailable: no package manager yet\nExample: apt install firefox\n", "apt install - nicht verfuegbar: noch kein Paketmanager\nBeispiel: apt install firefox\n");
        else if(strcmp(c, "ping") == 0) print_ui("ping - unavailable: no network stack yet\nExample: ping example.com\n", "ping - nicht verfuegbar: noch kein Netzwerk-Stack\nBeispiel: ping example.com\n");
        else if(strcmp(c, "window") == 0) print_ui("window - show a desktop window prototype\n", "window - Desktop-Fensterprototyp anzeigen\n");
        else if(strcmp(c, "draw") == 0) print_ui("draw rectangle - draw a green rectangle at screen center\ndraw circle - draw a green circle at screen center\ndraw x y w h - draw a rectangle at the given coordinates\n", "draw rectangle - gruenes Rechteck in der Bildschirmmitte zeichnen\ndraw circle - gruenen Kreis in der Bildschirmmitte zeichnen\ndraw x y w h - Rechteck an den angegebenen Koordinaten zeichnen\n");
        else if(strcmp(c, "stat") == 0) print_ui("stat PATH - show file details\nExample: stat test\n", "stat PFAD - Dateidetails anzeigen\nBeispiel: stat test\n");
        else if(strcmp(c, "wipe") == 0) print_ui("wipe - remove every file from the filesystem\n", "wipe - alle Dateien im Dateisystem entfernen\n");
        else if(strcmp(c, "tail") == 0) print_ui("tail [-n COUNT] FILE - show the last lines of a file\n", "tail [-n ANZAHL] DATEI - letzte Zeilen einer Datei anzeigen\n");
        else if(strcmp(c, "head") == 0) print_ui("head [-n COUNT] FILE - show the first lines of a file\n", "head [-n ANZAHL] DATEI - erste Zeilen einer Datei anzeigen\n");
        else if(strcmp(c, "basename") == 0) print_ui("basename PATH - show the final path component\n", "basename PFAD - letzten Pfadbestandteil anzeigen\n");
        else if(strcmp(c, "dirname") == 0) print_ui("dirname PATH - show the parent path\n", "dirname PFAD - uebergeordneten Pfad anzeigen\n");
        else if(strcmp(c, "du") == 0) print_ui("du [PATH] - show file data size in bytes\n", "du [PFAD] - Dateidaten-Groesse in Bytes anzeigen\n");
        else if(strcmp(c, "cksum") == 0) print_ui("cksum FILE - calculate CRC-32/IEEE and file length\n", "cksum DATEI - CRC-32/IEEE und Dateilaenge berechnen\n");
        else if(strcmp(c, "clean") == 0) print_ui("clean - clear the terminal; same as clear\n", "clean - Terminal leeren; gleichbedeutend mit clear\n");
        else if(strcmp(c, "spawn") == 0) print_ui("spawn counter | spawn checksum FILE - start cooperative background jobs\n", "spawn counter | spawn checksum DATEI - kooperative Hintergrundaufgabe starten\n");
        else if(strcmp(c, "jobs") == 0) print_ui("jobs - show progress and results of cooperative tasks\n", "jobs - Fortschritt und Ergebnisse kooperativer Aufgaben anzeigen\n");
        else if(strcmp(c, "sum") == 0) print_ui("sum - calculate a legacy 16-bit checksum\nLow reliability\n", "sum - alte 16-Bit-Pruefsumme berechnen\nGeringe Zuverlaessigkeit\n");
        else if(strcmp(c, "touch") == 0) print_ui("touch FILE - create an empty file\n", "touch DATEI - leere Datei erstellen\n");
        else if(strcmp(c, "cp") == 0) print_ui("cp SOURCE DESTINATION - copy a file\n", "cp QUELLE ZIEL - Datei kopieren\n");
        else if(strcmp(c, "append") == 0) print_ui("append FILE TEXT - append text to a file\n", "append DATEI TEXT - Text an eine Datei anhaengen\n");
        else if(strcmp(c, "echo") == 0) print_ui("echo TEXT - print text\n", "echo TEXT - Text ausgeben\n");
        else if(strcmp(c, "find") == 0) print_ui("find NAME - search the filesystem for names\n", "find NAME - Dateisystem nach Namen durchsuchen\n");
        else if(strcmp(c, "search") == 0) print_ui("search TEXT - search file contents\n", "search TEXT - Dateiinhalte durchsuchen\n");
        else if(strcmp(c, "tree") == 0) print_ui("tree [PATH] - show directories and files as a tree\n", "tree [PFAD] - Verzeichnisse und Dateien als Baum anzeigen\n");
        else if(strcmp(c, "wc") == 0) print_ui("wc FILE - count lines, words, and bytes in a file\n", "wc DATEI - Zeilen, Woerter und Bytes einer Datei zaehlen\n");
        else if(strcmp(c, "seq") == 0) print_ui("seq N - count from 1 to N\nLimit: 200\n", "seq N - von 1 bis N zaehlen\nLimit: 200\n");
        else if(strcmp(c, "cat") == 0) print_ui("cat [-n] FILE - print a file; -n numbers lines\n", "cat [-n] DATEI - Datei ausgeben; -n nummeriert Zeilen\n");
        else if(strcmp(c, "write") == 0) print_ui("write FILE TEXT - write TEXT to FILE\n", "write DATEI TEXT - TEXT in DATEI schreiben\n");
        else if(strcmp(c, "calc") == 0) print_ui("calc A OP B - calculator (+ - * /)\n", "calc A OP B - Taschenrechner (+ - * /)\n");
        else if(strcmp(c, "factor") == 0) print_ui("factor N - show the prime factorization of N\n", "factor N - Primfaktorzerlegung von N anzeigen\n");
        else if(strcmp(c, "rand") == 0) print_ui("rand - generate a pseudo-random number\n", "rand - Pseudozufallszahl erzeugen\n");
        else if(strcmp(c, "ida") == 0) print_ui("ida - draw a red heart that disappears after 10 seconds\n", "ida - rotes Herz zeichnen, das nach 10 Sekunden verschwindet\n");
        else if(strcmp(c, "fib") == 0) print_ui("fib N - calculate the Nth Fibonacci number\neach number is the sum of the previous two\n", "fib N - N-te Fibonacci-Zahl berechnen\njede Zahl ist die Summe der beiden vorherigen\n");
        else if(strcmp(c, "genpass") == 0) print_ui("genpass - generate a pseudo-random password\n", "genpass - Pseudozufallspasswort erzeugen\n");
        else if(strcmp(c, "factorial") == 0) print_ui("factorial N - calculate N factorial\nExample: factorial 5 = 5x4x3x2x1 = 120\n", "factorial N - Fakultaet von N berechnen\nBeispiel: factorial 5 = 5x4x3x2x1 = 120\n");
        else if(strcmp(c, "lsblk") == 0) print_ui("lsblk - list block devices\n", "lsblk - Blockgeraete auflisten\n");
        else if(strcmp(c, "sensors") == 0) print_ui("sensors - show available CPU temperature data\n", "sensors - verfuegbare CPU-Temperaturdaten anzeigen\n");
        else if(strcmp(c, "journalctl") == 0) print_ui("journalctl - show system logs and errors\n", "journalctl - Systemprotokolle und Fehler anzeigen\n");
        else if(strcmp(c, "mouse") == 0) print_ui("mouse - show the current mouse coordinates\n", "mouse - aktuelle Mauskoordinaten anzeigen\n");
        else if(strcmp(c, "banner") == 0) print_ui("banner TEXT - show large text in the center of the screen\n", "banner TEXT - grossen Text in der Bildschirmmitte anzeigen\n");
        else if(strcmp(c, "sl") == 0) print_ui("sl - animate a train moving across the screen\n", "sl - Zug ueber den Bildschirm fahren lassen\n");
        else if(strcmp(c, "cls") == 0) print_ui("cls - clear the screen, like clear\n", "cls - Bildschirm wie mit clear leeren\n");
        else { print_ui("No manual entry for '", "Kein Handbucheintrag fuer '"); vga_print(c); vga_print("'\n"); }
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
        if(!valid_path) print_ui("tree: invalid or overlong path\n", "tree: ungueltiger oder zu langer Pfad\n");
        else if(path[0] && !fs_is_directory(root))
            print_ui("tree: directory not found\n", "tree: Verzeichnis nicht gefunden\n");
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
        if(len < 0) print_ui("cksum: file not found or unreadable\n", "cksum: Datei nicht gefunden oder unlesbar\n");
        else {
            unsigned int crc = 0xFFFFFFFFu;
            for(int i = 0; i < len; i++) {
                crc ^= (unsigned char)cmd_buf[i];
                for(int bit = 0; bit < 8; bit++)
                    crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
            }
            vga_print("CRC32 0x"); print_hex32(~crc); vga_print("  ");
            print_int(len); print_ui(" bytes\n", " Bytes\n");
        }
    }
    else if(strncmp(input_buf, "ping ", 5) == 0) {
        vga_print("PING "); vga_print(&input_buf[5]);
        print_ui(": no network stack available\n", ": kein Netzwerkprotokoll vorhanden\n");
    }
    else if(strcmp(input_buf, "ifconfig") == 0)
        print_ui("No network driver or interface is available\n",
                 "Kein Netzwerktreiber oder Netzwerkgeraet verfuegbar\n");
    else if(strcmp(input_buf, "sensors") == 0)
        print_ui("CPU temperature: unavailable (no thermal sensor driver)\n",
                 "CPU-Temperatur: nicht verfuegbar (kein Temperatursensortreiber)\n");
    else if(strcmp(input_buf, "systemctl") == 0 || strncmp(input_buf, "systemctl ", 10) == 0)
        print_ui("No service manager is implemented\n", "Kein Dienstemanager implementiert\n");
    else if(strcmp(input_buf, "journalctl") == 0) {
        print_ui("No persistent system journal is available yet\n",
                 "Noch kein dauerhaftes Systemprotokoll verfuegbar\n");
    }
    else if(strcmp(input_buf, "crontab") == 0 || strncmp(input_buf, "crontab ", 8) == 0)
        print_ui("No scheduler is available for cron jobs\n",
                 "Kein Scheduler fuer Cronjobs implementiert\n");

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
            draw_heart(cx, cy, sc, 0x000000); // aktuelle Grösse löschen
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
        // Echtzeit-Uhr: zeichnet nur bei Sekundenwechsel neu.
        int scale = 6, char_w = 8*scale, len = 8;
        int gx = (VESA_WIDTH - len*char_w) / 2, gy = (VESA_HEIGHT - 16*scale) / 2;
        clear_last_draw();
        draw_rect(gx-20, gy-20, len*char_w+40, 16*scale+40, 0x000000);
        remember_draw(gx-20, gy-20, len*char_w+40, 16*scale+40);
        print_ui("Live clock for 60s (the shell is paused, then resumes)\n",
                 "Live-Uhr fuer 60 s (die Shell pausiert und wird danach fortgesetzt)\n");
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
            if(desktop_save_preferences())
                print_ui("Theme changed and saved\n", "Design geaendert und gespeichert\n");
            else print_ui("Theme changed, but settings could not be saved\n",
                          "Design geaendert, Einstellungen konnten aber nicht gespeichert werden\n");
        }
        else print_ui("Colors: cyan blue red green purple orange\n",
                      "Farben: tuerkis blau rot gruen lila orange\n");
    }
    else if(strcmp(input_buf, "window") == 0) {
        clear_last_draw();
        draw_window(300, 200, 400, 250, "Demo-Fenster");
        remember_draw(300, 200, 400, 250);
        print_ui("Desktop window prototype (not movable yet)\n",
                 "Desktop-Fensterprototyp (noch nicht verschiebbar)\n");
    }
    else if(strcmp(input_buf, "mouse") == 0) {
        print_ui("X: ", "X: "); print_int(mouse_get_x());
        print_ui("  Y: ", "  Y: "); print_int(mouse_get_y());
        print_ui("  Left: ", "  Links: ");
        print_ui(mouse_left_pressed() ? "pressed\n" : "released\n",
                 mouse_left_pressed() ? "gedrueckt\n" : "losgelassen\n");
    }

    /* ==================== Weitere Befehle ==================== */
    else if(strncmp(input_buf, "append ", 7) == 0) {
        char* sp = &input_buf[7];
        while(*sp && *sp != ' ') sp++;
        if(*sp) {
            *sp = 0;
            char path[FS_NAME_LEN];
            if(!resolve_fs_path(&input_buf[7], path)) {
                print_ui("append: invalid or overlong path\n", "append: ungueltiger oder zu langer Pfad\n");
                goto command_done;
            }
            int old_len = fs_read(path, cmd_buf, sizeof(cmd_buf));
            if(old_len < 0) old_len = 0;
            char* add = sp+1;
            int add_len = strlen(add);
            int total = old_len + add_len;
            if(total > FS_MAX_FILE_BYTES) {
                print_ui("append: file would exceed the maximum file size\n",
                         "append: Datei wuerde die maximale Groesse ueberschreiten\n");
                goto command_done;
            }
            for(int i = 0; old_len+i < total; i++) cmd_buf[old_len+i] = add[i];
            cmd_buf[total] = 0;
            if(fs_write(path, cmd_buf, total)) vga_print("OK\n");
            else print_ui("append: write failed\n", "append: Schreiben fehlgeschlagen\n");
        } else print_ui("Usage: append FILE TEXT\n", "Aufruf: append DATEI TEXT\n");
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
        if(!any) print_ui("Nothing found\n", "Nichts gefunden\n");
    }
    else if(strncmp(input_buf, "alarm ", 6) == 0) {
        char* p = &input_buf[6];
        int secs = parse_int(&p);
        if(secs < 0) secs = 0;
        if(secs > 30) secs = 30; // Sicherheitsbegrenzung, Shell ist blockiert
        print_ui("Waiting ", "Warte "); print_int(secs); vga_print("s...\n");
        for(int s = 0; s < secs; s++)
            pit_wait_ms(1000);
        print_ui("Time's up!\n", "Zeit abgelaufen!\n");
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
        print_ui("Go!\n", "Los!\n");
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
        if(fs_save()) print_ui("Filesystem cleared\n", "Dateisystem geleert\n");
        else print_ui("Error: could not save the filesystem\n",
                      "Fehler: Dateisystem konnte nicht gespeichert werden\n");
    }
    else if(strcmp(input_buf, "desktop") == 0) {
        desktop_enter();
    }
    else if(strcmp(input_buf, "fullscreen") == 0) {
        if(desktop_active) desktop_fullscreen();
        else print_ui("Already in fullscreen mode\n", "Schon im Vollbild\n");
    }
    else if(strcmp(input_buf, "dih") == 0) {
        vga_print("          _____\n         /  |  |\n         (     )\n         |     |\n         |     |\n         |     |\n         |     |\n         |     |\n");
        vga_print("         |     |\n         |     |\n         |     |\n      __-_     _-_\n     /     `´     )\n    (              )\n");
        vga_print("     (      |      /\n      -____/|_____-\n");
    }
    else if(strcmp(input_buf,"man") == 0) {
        print_ui("man - the manual\nUse 'man' followed by a command for usage information.\n",
                 "man - das Handbuch\nNutze 'man' gefolgt von einem Befehl fuer Hinweise.\n");
    }
    else if(strcmp(input_buf, "fasfetch") == 0) {
        print_ui("Try fastfetch or neofetch!\n", "Versuche fastfetch oder neofetch!\n");
    }
    /*else if(strcmp(input_buf, "test") == 0) {
        vga_print("hej\ntest\ntest\ntest\ntest\n");
    }
    else if(strcmp(input_buf, "banane") == 0) {
        vga_print("banane gegessen!");
    }*/
    else if(input_idx > 0) {
        shell_status = 1;
        print_ui("Unknown command: ", "Unbekannter Befehl: ");
        vga_print(input_buf); vga_putc('\n');
    }
command_done:
    return;
}

static void shell_trim_command(char* command) {
    int start = 0, end = strlen(command);
    while(command[start] == ' ') start++;
    while(end > start && command[end-1] == ' ') end--;
    int length = end - start;
    for(int i = 0; i < length; i++) command[i] = command[start+i];
    command[length] = 0;
}

static int shell_parse_line(const char* line, char stages[SHELL_PIPE_STAGES][64],
                            char connectors[SHELL_PIPE_STAGES], int* stage_count,
                            char* redirect_path, int* redirect_mode) {
    int stage = 0, length = 0, target_length = 0;
    char quote = 0;
    *stage_count = 0;
    *redirect_mode = 0;
    redirect_path[0] = 0;
    for(int i = 0; line[i]; i++) {
        char c = line[i];
        if(quote) {
            if(c == quote) quote = 0;
            else {
                char* destination = *redirect_mode ? redirect_path : stages[stage];
                int* used = *redirect_mode ? &target_length : &length;
                if(*used >= 63) return 0;
                destination[(*used)++] = c;
                destination[*used] = 0;
            }
            continue;
        }
        if(c == '\'' || c == '"') {
            quote = c;
            continue;
        }
        if(c == '>' ) {
            if(*redirect_mode || stage >= SHELL_PIPE_STAGES || length == 0) return 0;
            *redirect_mode = line[i+1] == '>' ? 2 : 1;
            if(*redirect_mode == 2) i++;
            while(line[i+1] == ' ') i++;
            for(i++; line[i]; i++) {
                c = line[i];
                if(c == '\'' || c == '"') {
                    quote = c;
                    while(line[i+1] && quote) {
                        c = line[++i];
                        if(c == quote) quote = 0;
                        else {
                            if(target_length >= 63) return 0;
                            redirect_path[target_length++] = c;
                        }
                    }
                    if(quote) return 0;
                } else {
                    if(c == '|' || c == '&' || c == '>') return 0;
                    if(target_length >= 63) return 0;
                    redirect_path[target_length++] = c;
                }
            }
            redirect_path[target_length] = 0;
            shell_trim_command(redirect_path);
            if(!redirect_path[0]) return 0;
            break;
        }
        if(c == '|' || (c == '&' && line[i+1] == '&')) {
            if(length == 0 || stage >= SHELL_PIPE_STAGES-1 || *redirect_mode) return 0;
            shell_trim_command(stages[stage]);
            if(!stages[stage][0]) return 0;
            connectors[stage] = c == '|' ? '|' : '&';
            stage++;
            stages[stage][0] = 0;
            length = 0;
            if(c == '&') i++;
            while(line[i+1] == ' ') i++;
            continue;
        }
        if(c == '&') return 0;
        if(length >= 63) return 0;
        stages[stage][length++] = c;
        stages[stage][length] = 0;
    }
    if(quote) return 0;
    if(length == 0) return 0;
    shell_trim_command(stages[stage]);
    if(!stages[stage][0]) return 0;
    *stage_count = stage + 1;
    return 1;
}

static int shell_pipeline_allowed(const char* command) {
    static const char* allowed[] = {
        "echo", "printf", "cat", "ls", "pwd", "grep", "sort", "wc", "head", "tail",
        "basename", "dirname", "cksum", "xxd", "sum", "find", "du", "stat",
        "rev", "tr", "seq", "calc", "date", "time", "uptime", "uname", "hostname",
        "free", "arch", "id", "groups", "who", "ps", "jobs", "tree", "env", "history",
        "lsblk", "mount", "nproc", "man", "version", "uniq"
    };
    char name[64];
    int length = 0;
    while(command[length] && command[length] != ' ') {
        if(length >= 63) return 0;
        name[length] = command[length];
        length++;
    }
    name[length] = 0;
    for(unsigned int i = 0; i < sizeof(allowed)/sizeof(allowed[0]); i++)
        if(strcmp(name, allowed[i]) == 0) return 1;
    return 0;
}

static int shell_reads_pipe(const char* command) {
    int length = 0;
    while(command[length] && command[length] != ' ') length++;
    return (length == 3 && strncmp(command, "cat", 3) == 0) ||
           (length == 4 && strncmp(command, "grep", 4) == 0) ||
           (length == 4 && strncmp(command, "sort", 4) == 0) ||
           (length == 2 && strncmp(command, "wc", 2) == 0) ||
           (length == 4 && strncmp(command, "head", 4) == 0) ||
           (length == 4 && strncmp(command, "tail", 4) == 0) ||
           (length == 4 && strncmp(command, "uniq", 4) == 0) ||
           (length == 2 && strncmp(command, "tr", 2) == 0);
}

static int shell_store_redirect(const char* path, int mode, const char* output, int output_length) {
    char resolved[FS_NAME_LEN];
    if(!resolve_fs_path(path, resolved) || !resolved[0]) return 0;
    int existing_length = 0;
    if(mode == 2) {
        existing_length = fs_read(resolved, cmd_buf, sizeof(cmd_buf));
        if(existing_length < 0) existing_length = 0;
        if(existing_length + output_length > FS_MAX_FILE_BYTES) return 0;
        for(int i = 0; i < output_length; i++) cmd_buf[existing_length+i] = output[i];
        return fs_write(resolved, cmd_buf, existing_length + output_length);
    }
    return fs_write(resolved, output, output_length);
}

void handle_command(void) {
    char stages[SHELL_PIPE_STAGES][64];
    char connectors[SHELL_PIPE_STAGES];
    char redirect_path[64];
    int stage_count, redirect_mode;
    stages[0][0] = 0;
    input_buf[input_idx] = 0;
    if(input_idx > 0) {
        int n = input_idx < 63 ? input_idx : 63;
        for(int i = 0; i < n; i++) cmd_history[cmd_history_count % 8][i] = input_buf[i];
        cmd_history[cmd_history_count % 8][n] = 0;
        cmd_history_count++;
    }
    int parse_ok = input_idx == 0 || shell_parse_line(input_buf, stages, connectors, &stage_count,
                                                       redirect_path, &redirect_mode);
    if(input_idx == 0) {
        shell_status = 0;
    } else if(!parse_ok) {
        shell_status = 1;
        print_ui("Shell syntax error or command too long\n",
                 "Shell-Syntaxfehler oder Befehl zu lang\n");
    } else {
        int run_group = 1, group_status = 0, first = 0;
        while(first < stage_count) {
            int last = first;
            while(last < stage_count-1 && connectors[last] == '|') last++;
            if(run_group) {
                shell_status = 0;
                int constrained = last > first || (redirect_mode && last == stage_count-1);
                for(int i = first; i <= last; i++) {
                    if(constrained && (!shell_pipeline_allowed(stages[i]) ||
                       (i > first && !shell_reads_pipe(stages[i])))) {
                        shell_status = 1;
                        print_ui("Command cannot be used in this pipeline\n",
                                 "Befehl kann in dieser Pipe nicht verwendet werden\n");
                        break;
                    }
                    int length = strlen(stages[i]);
                    for(int j = 0; j <= length; j++) input_buf[j] = stages[i][j];
                    input_idx = length;
                    shell_stdin_data = i == first ? 0 : shell_pipe_buffer;
                    int capture = i < last || (redirect_mode && last == stage_count-1 && i == last);
                    if(capture) vga_capture_start(cmd_buf2, sizeof(cmd_buf2));
                    shell_execute_simple();
                    if(capture) {
                        int captured = vga_capture_end();
                        if(captured < 0) {
                            shell_status = 1;
                            print_ui("Command output exceeds the pipe buffer\n",
                                     "Befehlsausgabe ist groesser als der Pipe-Puffer\n");
                            break;
                        }
                        if(i < last) {
                            for(int j = 0; j <= captured; j++) shell_pipe_buffer[j] = cmd_buf2[j];
                        }
                        if(i == last && redirect_mode) {
                            if(!shell_store_redirect(redirect_path, redirect_mode,
                                                     cmd_buf2, captured)) {
                                shell_status = 1;
                                print_ui("Redirection failed: invalid path, full disk, or write error\n",
                                         "Umleitung fehlgeschlagen: Pfad ungueltig, Datentraeger voll oder Schreibfehler\n");
                            }
                        }
                    }
                }
                group_status = shell_status;
            }
            if(last < stage_count-1 && connectors[last] == '&') {
                run_group = group_status == 0;
            }
            first = last + 1;
        }
    }
    shell_stdin_data = 0;
    int region_x, region_y, region_w, region_h;
    vga_get_region(&region_x, &region_y, &region_w, &region_h);
    (void)region_y;
    (void)region_w;
    (void)region_h;
    if(text_x != region_x) vga_putc('\n');
    input_idx = 0;
    cursor_col = 0;
    if(desktop_active && !desktop_terminal_focused()) return;
    shell_prompt();
    prompt_x = text_x; prompt_y = text_y;
    blink_visible = 1; last_blink_tick = ticks;
    draw_cursor_bar(1);
}
