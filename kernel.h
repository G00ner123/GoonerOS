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
#define SHELL_LINE_CAPACITY 256

/* ==================== Heap ==================== */
#define HEAP_START 0x200000
#define PAGING_IDENTITY_LIMIT 0x00400000u

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
 * Bootloader liest LBA 3-510 als Kernel-Loadfenster. Das Dateisystem beginnt
 * dahinter, damit persistente Schreibvorgaenge nie Kernel
 * oder Bootcode überschreiben:
 *   LBA 512        Superblock (1 Sektor)
 *   LBA 513-514    Dateitabelle (32 Eintraege x 32 Bytes = 1024 Bytes)
 *   LBA 515+       Datenbereich, je Eintrag feste 16 Sektoren (8 KB)
 */
#define FS_MAGIC 0x474F4653u
#define FS_SUPERBLOCK_LBA 512
#define FS_TABLE_LBA 513
#define FS_TABLE_SECTORS 2
#define FS_MAX_FILES 32
#define FS_NAME_LEN 16
#define FS_DATA_START_LBA 515
#define FS_MAX_FILE_SECTORS 16
#define FS_MAX_FILE_BYTES (FS_MAX_FILE_SECTORS*512)
#define FS_DISK_SECTORS 2048
#define FS_DISK_END_LBA FS_DISK_SECTORS
#define FS_ENTRY_DIRECTORY_MARK 0xD1
#define FS_LEGACY_SUPERBLOCK_LBA 200
#define FS_LEGACY_TABLE_LBA 201
#define FS_LEGACY_DATA_START_LBA 203

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

int fs_save(void);
int fs_load(void);
struct fs_entry* fs_find(const char* name);
int fs_create(const char* name);
int fs_mkdir(const char* name);
int fs_delete(const char* name);
int fs_rmdir(const char* name);
int fs_remove_tree(const char* name);
int fs_rename(const char* old_name, const char* new_name);
int fs_is_directory(const struct fs_entry* entry);
int fs_is_direct_child(const struct fs_entry* entry, const char* parent);
int fs_write(const char* name, const char* data, int len);
int fs_read(const char* name, char* out, int max_len);
int fs_check(void);

/* ==================== ATA ==================== */
int ata_rw_sectors(unsigned int lba, unsigned short count, unsigned short* buf, int write);
void pci_list_devices(void);

/* ==================== RTC / Uhr ==================== */
unsigned char cmos_read(unsigned char reg);
unsigned char bcd_to_bin(unsigned char bcd);
void read_rtc_raw(unsigned char* h, unsigned char* m, unsigned char* s,
                   unsigned char* d, unsigned char* mo, unsigned char* y);
void print_time_date(void);

/* ==================== Heap-Allokator ==================== */
extern unsigned int heap_ptr;
void* kmalloc(unsigned int size);
void page_allocator_init(void);
void* page_alloc(void);
int page_free(void* page);
unsigned int page_free_count(void);
int paging_init(unsigned int framebuffer, unsigned int pitch, unsigned int height);
int paging_is_active(void);
unsigned int paging_directory_address(void);
int paging_create_user_space(unsigned int slot, unsigned int code_page, unsigned int stack_page);
int paging_activate_user_space(unsigned int slot);
void paging_activate_kernel_space(void);
int user_process_run(int pid, int test_fault);
void user_process_init(void);
int user_process_spawn_async(int test_fault);
int user_process_spawn_elf(const char* path);
int user_process_task_count(void);
int user_process_get_info(int index, int* pid, int* state, unsigned int* steps);
int user_process_kill(int pid);
int user_process_current_async(void);
int user_process_current_pid(void);
void user_process_async_exit(int status);
void user_process_wake_sleepers(void);
void user_process_save_stack(int index, unsigned int stack_pointer);
int user_process_runnable(int index);
unsigned int user_process_saved_stack(int index);
int user_process_activate(int index);
void user_process_reap_done(void);
int user_syscall_dispatch(unsigned int* registers);
int user_fault_dispatch(unsigned int error, unsigned int eip, unsigned int cs, unsigned int address);
void gdt_install(void);
void gdt_set_kernel_stack(unsigned int stack_top);

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
void vga_capture_start(char* buffer, unsigned int capacity);
int vga_capture_end(void);
void vga_set_text_color(unsigned int color);
void text_buffer_put(int row, int col, char c);
void redraw_text_buffer(void);
void clear_pixels_only(void);
void put_pixel(int x, int y, unsigned int color);
unsigned int get_pixel(int x, int y);
void draw_char(int x, int y, char c, unsigned int fg, unsigned int bg);
void draw_console_char(int x, int y, char c, unsigned int fg, unsigned int bg);
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
int desktop_editor_active(void);
void desktop_editor_key(char c, int special);
void desktop_editor_update(void);
int desktop_save_preferences(void);
int desktop_boot_logo_enabled(void);
int desktop_boot_animation_enabled(void);
int desktop_system_sounds_enabled(void);
int desktop_terminal_cursor_blink_enabled(void);
void play_gooneros_animation(void);
extern int mint_visible;
int mouse_get_sensitivity(void);
void mouse_set_sensitivity(int sensitivity);

/* ==================== Interrupts / PIT ==================== */
extern volatile unsigned int ticks;
void pit_init(void);
void pit_wait_ms(unsigned int ms);
void idt_install(void);
void remap_pic(void);
void irq_ack(unsigned char irq);
unsigned int irq0_handler(unsigned int stack_pointer);

/* ==================== Tastatur / PS2 ==================== */
#define KEYBOARD_LAYOUT_DE 0
#define KEYBOARD_LAYOUT_EN 1
void ps2_init(void);
void irq1_handler(void);
void keyboard_poll(void);
int shell_menuconfig_active(void);
void shell_menuconfig_key(int key);
int shell_history_move(int direction);
void redraw_input_line(void);
void keyboard_load_layout(void);
int keyboard_set_layout(int layout);
int keyboard_get_layout(void);
int keyboard_save_layout(void);
static inline const char* ui_text(const char* english, const char* german) {
    return keyboard_get_layout() == KEYBOARD_LAYOUT_EN ? english : german;
}
void desktop_language_changed(void);
void draw_cursor_bar(int on);
void redraw_input_line(void);

/* ==================== Maus ==================== */
int mouse_init(void);
void irq12_handler(void);
int mouse_get_x(void);
int mouse_get_y(void);
int mouse_left_pressed(void);
void mouse_refresh_cursor(void);
void mouse_cursor_hide(void); // BUGFIX: vor jedem Redraw aufrufen, der Pixel unter dem Cursor überschreiben könnte
void mouse_set_cursor_shape(int shape);
int mouse_poll_event(int* x, int* y, int* left);

/* ==================== Shell ==================== */
extern char input_buf[SHELL_LINE_CAPACITY];
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
void kernel_panic(const char* message);
unsigned int kernel_exception_dispatch(unsigned int vector, unsigned int error,
                                       unsigned int eip, unsigned int cs,
                                       unsigned int address);
void beep(void);

/* ==================== Kooperativer Scheduler ==================== */
#define SCHEDULER_MAX_TASKS 8
#define USER_PROCESS_LIMIT 2
#define SCHEDULER_TASK_COUNTER 0
#define SCHEDULER_TASK_CHECKSUM 1
#define SCHEDULER_TASK_USER 2
#define SCHEDULER_STATE_RUNNABLE 0
#define SCHEDULER_STATE_DONE 1
#define SCHEDULER_STATE_SLEEPING 2
#define SCHEDULER_STATE_WAITING 3
void scheduler_init(void);
int scheduler_spawn_counter(void);
int scheduler_spawn_checksum(const char* path);
int scheduler_kill(int pid);
unsigned int scheduler_timer_switch(unsigned int stack_pointer);
int scheduler_task_count(void);
int scheduler_get_task(int index, int* pid, unsigned int* steps);
int scheduler_get_task_info(int index, int* pid, int* type, int* state,
                            unsigned int* steps, unsigned int* progress,
                            unsigned int* total, unsigned int* result);

#endif
