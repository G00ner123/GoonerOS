#ifndef KERNEL_H
#define KERNEL_H

/* ==================== Bildschirm / Aufloesung ==================== */
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VESA_WIDTH 1024
#define VESA_HEIGHT 768
/* ======================================================*/
#define TEXT_WRAP_WIDTH 760
#define TEXT_COLS (TEXT_WRAP_WIDTH/8)
#define TEXT_ROWS (VESA_HEIGHT/16)

/* ==================== Heap ==================== */
#define HEAP_START 0x200000

/* ==================== ATA-Ports ==================== */
#define ATA_PRIMARY 0x1F0
#define ATA_DATA 0x1F0
#define ATA_ERROR 0x1F1
#define ATA_SECCNT 0x1F2
#define ATA_LBA0 0x1F3
#define ATA_LBA1 0x1F4
#define ATA_LBA2 0x1F5
#define ATA_DRIVE 0x1F6
#define ATA_STATUS 0x1F7
#define ATA_CMD 0x1F7
#define ATA_READ 0x20
#define ATA_WRITE 0x30

/* ==================== Dateisystem ====================
 * Layout auf der Platte (weit weg von Bootloader/Stage2/Kernel, die nur
 * die ersten ~83 Sektoren belegen):
 *   LBA 200        Superblock (1 Sektor)
 *   LBA 201-202    Dateitabelle (32 Eintraege x 32 Bytes = 1024 Bytes = 2 Sektoren)
 *   LBA 203+       Datenbereich, jede Datei bekommt feste 16 Sektoren (8 KB)
 */
#define FS_MAGIC 0x474F4653u
#define FS_SUPERBLOCK_LBA 200
#define FS_TABLE_LBA 201
#define FS_TABLE_SECTORS 2
#define FS_MAX_FILES 32
#define FS_NAME_LEN 16
#define FS_DATA_START_LBA 203
#define FS_MAX_FILE_SECTORS 16
#define FS_MAX_FILE_BYTES (FS_MAX_FILE_SECTORS*512)

struct fs_entry {
    char name[FS_NAME_LEN];
    unsigned int size;
    unsigned int start_lba;
    unsigned char used;
    unsigned char reserved[7];
} __attribute__((packed));

struct fs_superblock {
    unsigned int magic;
    unsigned int file_count;
    unsigned int next_free_lba;
    unsigned char reserved[500];
} __attribute__((packed));

extern struct fs_entry fs_table[FS_MAX_FILES];
extern struct fs_superblock fs_sb;

void fs_save(void);
void fs_load(void);
struct fs_entry* fs_find(const char* name);
int fs_create(const char* name);
int fs_delete(const char* name);
int fs_write(const char* name, const char* data, int len);
int fs_read(const char* name, char* out, int max_len);

/* ==================== ATA ==================== */
int ata_rw_sectors(unsigned int lba, unsigned short count, unsigned short* buf, int write);

/* ==================== RTC / Uhr ==================== */
unsigned char cmos_read(unsigned char reg);
unsigned char bcd_to_bin(unsigned char bcd);
void read_rtc_raw(unsigned char* h, unsigned char* m, unsigned char* s,
                   unsigned char* d, unsigned char* mo, unsigned char* y);
void print_time_date(void);

/* ==================== Heap-Allokator ==================== */
extern unsigned int heap_ptr;
void* kmalloc(unsigned int size);

/* ==================== VGA / Grafik ==================== */
extern int text_x, text_y;

void vga_init(unsigned int fb_addr, unsigned int pitch, unsigned int bpp);
unsigned int vga_get_pitch(void);
unsigned int vga_get_bpp(void);
void vga_set_region(int ox, int oy, int w, int h);
void vga_get_region(int* ox, int* oy, int* w, int* h);
void vga_clear(void);
void vga_clear_region(void);
void vga_putc(char c);
void vga_print(const char* s);
void text_buffer_put(int row, int col, char c);
void redraw_text_buffer(void);
void clear_pixels_only(void);
void put_pixel(int x, int y, unsigned int color);
unsigned int get_pixel(int x, int y);
void draw_char(int x, int y, char c, unsigned int fg, unsigned int bg);
void draw_char_scaled(int x, int y, char c, int scale, unsigned int fg, unsigned int bg);
void draw_rect(int x, int y, int w, int h, unsigned int color);
void draw_triangle(int x0,int y0,int x1,int y1,int x2,int y2, unsigned int color);
void draw_mouse(int x, int y, unsigned int color);
void draw_circle_filled(int cx, int cy, int r, unsigned int color);
void draw_arch_logo(int cx, int top_y, int height, int half_width, unsigned int color);
void draw_mint_logo(int show);
void draw_heart(int cx, int cy, int scale, unsigned int color);

/* ==================== Desktop-Vorkehrungen ==================== */
extern unsigned int ui_theme_color;
void draw_window(int x, int y, int w, int h, const char* title);

/* ==================== Desktop ==================== */
extern int desktop_active;
void desktop_enter(void);
void desktop_fullscreen(void);
void desktop_handle_mouse(int x, int y, int left_down);
void desktop_tick(void);
int desktop_terminal_visible(void);
int desktop_terminal_focused(void);
void play_gooneros_animation(void);
extern int mint_visible;

/* ==================== Interrupts / PIT ==================== */
extern volatile unsigned int ticks;
void pit_init(void);
void pit_wait_ms(unsigned int ms);
void idt_install(void);
void remap_pic(void);
void irq_ack(unsigned char irq);
void irq0_handler(void);

/* ==================== Tastatur / PS2 ==================== */
void ps2_init(void);
void irq1_handler(void);
void draw_cursor_bar(int on);
void redraw_input_line(void);

/* ==================== Maus ==================== */
void mouse_init(void);
void irq12_handler(void);
int mouse_get_x(void);
int mouse_get_y(void);
int mouse_left_pressed(void);
void mouse_refresh_cursor(void);
void mouse_cursor_hide(void); // BUGFIX: vor jedem Redraw aufrufen, der Pixel unter dem Cursor ueberschreiben koennte
int mouse_poll_event(int* x, int* y, int* left);

/* ==================== Shell ==================== */
extern char input_buf[64];
extern int input_idx;
extern int cursor_col;
extern int prompt_x, prompt_y;
extern unsigned int last_blink_tick;
extern int blink_visible;

void handle_command(void);
void print_int(int n);
int parse_int(char** p);
void clear_last_draw(void);
void remember_draw(int x, int y, int w, int h);
int get_ip(char* out);
void reboot(void);
void beep(void);

#endif
