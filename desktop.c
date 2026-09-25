#include "kernel.h"
#include "io.h"

// ==================== Desktop / Fenstermanager ====================
// Fenster haben eine Position im RAM, koennen per Titelleiste verschoben,
// per X geschlossen und per Klick nach vorne (Z-Order) geholt werden.
//
// WICHTIG zur Performance/Stabilitaet: Es wird NIE der komplette Bildschirm
// geloescht und neu gemalt (das verursachte vorher das "Blinken" beim
// Uhr-Tick und das Geflacker beim Ziehen). Stattdessen wird gezielt nur das
// betroffene Rechteck geloescht (auf den Wallpaper-Farbverlauf zurueckgesetzt)
// und neu gezeichnet. Ausnahme: ein Themenwechsel faerbt wirklich alle
// Titelleisten + Taskleiste neu ein, dafuer lohnt sich ein Komplett-Redraw.
//
// WICHTIG zur Interrupt-Sicherheit: Diese Datei wird NIE direkt aus dem
// Maus-Interrupt (IRQ12) heraus aufgerufen. desktop_handle_mouse() wird von
// der Hauptschleife in kernel.c aufgerufen (mit aktivierten Interrupts!),
// nachdem mouse.c per mouse_poll_event() ein neues Paket gemeldet hat. Die
// ISR selbst tut nur das absolut Noetige (Paket einlesen, Cursor-Sprite
// verschieben) - alles Teure (Fenster neu zeichnen) passiert ausserhalb des
// Interrupt-Kontexts. Sonst blockiert ein langer Redraw alle Interrupts
// (IDT-Gates sind hier "interrupt gates", die IF waehrend der ISR loeschen),
// der PS/2-Controller-Puffer laeuft ueber und die Maus-Pakete desynchen -
// genau das war die Ursache des "epileptischen" Rucklers beim Ziehen.

int desktop_active = 0;
static int terminal_windowed = 0;

/* ---------- Icons ---------- */
#define ICON_COUNT 9
static const char* icon_labels[ICON_COUNT] = {
    "Terminal", "Info", "Clock", "Settings", "Fullscreen",
    "Files", "Network", "Wallpaper", "Tasks"
};
static unsigned int icon_colors[ICON_COUNT] = {
    0x36B9FF, 0x5EEB9B, 0xFFB84D, 0xB58CFF,
    0xFF5C72, 0x58D6D6, 0x6FA8FF, 0xF477C8, 0xFFD166
};
#define ICON_W 120
#define ICON_H 104
#define ICON_GAP 10
#define ICON_TOP 34
#define ICON_COLS 5
#define TASKBAR_H 36

static int icon_x(int i) {
    return ICON_GAP + (i % ICON_COLS) * (ICON_W + ICON_GAP);
}

static int icon_y(int i) {
    return ICON_TOP + (i / ICON_COLS) * ICON_H;
}

/* ---------- Fenster ---------- */
#define TYPE_TERMINAL 0
#define TYPE_INFO     1
#define TYPE_CLOCK    2
#define TYPE_SETTINGS 3
#define TYPE_FILES     4
#define TYPE_NETWORK   5
#define TYPE_EDITOR    6
#define TYPE_TASKS     7

#define MAX_WINDOWS 8
#define CLOSE_BTN_SIZE 16
#define SHADOW_OFF 6
typedef struct {
    int active;
    int type;
    int x, y, w, h;
} window_t;

static window_t windows[MAX_WINDOWS];
static int zorder[MAX_WINDOWS]; // Index in windows[], zorder[0] = hinten, zorder[zcount-1] = vorne
static int zcount = 0;
static int drag_idx = -1;
static int drag_off_x = 0, drag_off_y = 0;
static unsigned int drag_last_render_tick = 0;
static int drag_outline_active = 0;
static int drag_outline_x, drag_outline_y, drag_outline_w, drag_outline_h;
static unsigned int drag_outline_pixels[2*VESA_WIDTH + 2*VESA_HEIGHT];

static const char* win_titles[8] = {
    "Terminal", "Info", "Clock", "Settings", "Files", "Network", "Editor", "Tasks"
};
static unsigned int theme_colors[6] = {0x00FFAA,0x3399FF,0xFF4444,0x55FF55,0xAA55FF,0xFFAA00};
static int wallpaper_variant = 0;
static const char* wallpaper_names[3] = {"AQUA", "OCEAN", "VIOLET"};
static const char desktop_preferences_path[] = "goonset.cfg";
static const char* preferences_status = "Changes are saved automatically";
static int files_selected = -1;
static int files_page = 0;
static char files_cwd[FS_NAME_LEN];
static char files_preview[129];
static int editor_active = 0;
static volatile int editor_dirty = 0;
static int editor_existing = 0;
static int editor_directory = 0;
static int editor_field = 0;
static int editor_name_len = 0, editor_text_len = 0;
static char editor_name[FS_NAME_LEN];
static char editor_text[128];
static int editor_cursor_visible = 1;
static unsigned int editor_last_blink_tick = 0;
static const char* editor_status = "";

// Von desktop_tick() einmal pro Sekunde aktualisiert, Format "HH:MM:SS"
static char clock_cache[9] = "--:--:--";

/* ---------- Vorwaertsdeklarationen ---------- */
static unsigned int wallpaper_color_at(int y);
static void draw_wallpaper(void);
static void draw_taskbar(void);
static void draw_icon(int i);
static void draw_icon_glyph(int i, int cx, int cy);
static void desktop_redraw_all(void);
static void erase_rect_to_desktop(int x, int y, int w, int h);
static void redraw_windows_overlapping(int x, int y, int w, int h, int except_idx);
static int  win_find_type(int type);
static int  win_occluded(int idx);
static void win_bring_front(int idx);
static int  win_alloc_slot(void);
static int  win_open(int type);
static void win_close(int idx);
static int  win_hit_test(int mx, int my);
static void win_close_btn_rect(window_t* w, int* bx, int* by, int* bs);
static int  win_hit_close(window_t* w, int mx, int my);
static int  win_hit_titlebar(window_t* w, int mx, int my);
static void draw_close_button(window_t* w);
static void settings_swatch_rect(window_t* w, int i, int* sx, int* sy, int* sw, int* sh);
static void win_render(int idx);
static void drag_move_window(int idx, int x, int y);
static void drag_outline_restore(void);
static void drag_outline_draw(int x, int y, int w, int h);
static void drag_clamp_position(window_t* w, int* x, int* y);
static void editor_draw_caret(void);
static void editor_render(void);
static int theme_color_index(void);
static void load_desktop_preferences(void);
static int save_desktop_preferences(void);
static void settings_wallpaper_rect(window_t* w, int i, int* sx, int* sy, int* sw, int* sh);

static void settings_wallpaper_rect(window_t* w, int i, int* sx, int* sy, int* sw, int* sh) {
    *sw = 82; *sh = 28;
    *sx = w->x + 20 + i*92;
    *sy = w->y + 112;
}

static int theme_color_index(void) {
    for(int i = 0; i < 6; i++)
        if(theme_colors[i] == ui_theme_color) return i;
    return -1;
}

static void load_desktop_preferences(void) {
    char config[8];
    int len = fs_read(desktop_preferences_path, config, sizeof(config));
    if(len != 6 || config[0] != '1' || config[1] != ',' ||
       config[3] != ',' || config[5] != '\n' ||
       config[2] < '0' || config[2] > '5' ||
       config[4] < '0' || config[4] > '2') {
        preferences_status = "Changes are saved automatically";
        return;
    }
    ui_theme_color = theme_colors[config[2] - '0'];
    wallpaper_variant = config[4] - '0';
    preferences_status = "Settings loaded";
}

static int save_desktop_preferences(void) {
    int color = theme_color_index();
    if(color < 0) {
        preferences_status = "Save failed: unknown accent color";
        return 0;
    }
    char config[6] = {'1', ',', (char)('0' + color), ',', (char)('0' + wallpaper_variant), '\n'};
    if(!fs_write(desktop_preferences_path, config, sizeof(config))) {
        preferences_status = "Save failed: disk full or disk error";
        return 0;
    }
    preferences_status = "Settings saved";
    return 1;
}

int desktop_save_preferences(void) {
    int saved = save_desktop_preferences();
    if(desktop_active) desktop_redraw_all();
    return saved;
}

static void editor_cursor_position(int* col, int* row) {
    *col = 0;
    *row = 0;
    for(int i = 0; i < editor_text_len; i++) {
        if(editor_text[i] == '\n') {
            *col = 0;
            (*row)++;
        } else if(++(*col) >= 76) {
            *col = 0;
            (*row)++;
        }
    }
}

static void editor_draw_caret(void) {
    int idx = win_find_type(TYPE_EDITOR);
    if(idx < 0) return;
    window_t* w = &windows[idx];
    int x, y;
    if(editor_field == 0 && !editor_existing) {
        x = w->x+80+editor_name_len*8;
        y = w->y+39;
    } else if(editor_field == 1 && !editor_directory) {
        int col, row;
        editor_cursor_position(&col, &row);
        if(row >= 18) return;
        x = w->x+22+col*8;
        y = w->y+106+row*16;
    } else return;
    draw_rect(x, y, 8, 16, 0x16283A);
    if(editor_cursor_visible) draw_rect(x, y, 2, 14, ui_theme_color);
}

static int files_make_path(const char* name, char out[FS_NAME_LEN]) {
    int base_len = strlen(files_cwd), name_len = strlen(name);
    if(!name_len) return 0;
    if(name[0] == '/') {
        if(name_len >= FS_NAME_LEN) return 0;
        for(int i = 0; i <= name_len; i++) out[i] = name[i];
        return 1;
    }
    if(base_len + (base_len ? 1 : 0) + name_len >= FS_NAME_LEN) return 0;
    int i = 0;
    while(files_cwd[i]) { out[i] = files_cwd[i]; i++; }
    if(i) out[i++] = '/';
    for(int j = 0; name[j]; j++) out[i++] = name[j];
    out[i] = 0;
    return 1;
}

static void files_go_parent(void) {
    int len = strlen(files_cwd);
    while(len > 0 && files_cwd[len-1] != '/') len--;
    if(len > 0) len--;
    files_cwd[len] = 0;
    files_selected = -1;
    files_preview[0] = 0;
    files_page = 0;
}

/* ---------- Hintergrund / Taskleiste / Icons ---------- */

static unsigned int wallpaper_color_at(int y) {
    unsigned int t = (unsigned int)y * 255 / VESA_HEIGHT;
    unsigned int r, g, b;
    if(wallpaper_variant == 1) {
        r = 8 + t / 8; g = 18 + t / 5; b = 40 + t / 2;
    } else if(wallpaper_variant == 2) {
        r = 24 + t / 4; g = 10 + t / 10; b = 38 + t / 3;
    } else {
        r = 10 + t / 5; g = 18 + t / 4; b = 32 + t / 3;
    }
    return (r << 16) | (g << 8) | b;
}

static unsigned int wallpaper_pixel_at(int x, int y) {
    unsigned int color = wallpaper_color_at(y);
    if((y % 48) == 0) color = 0x18324A;
    for(int i = 0; i < 12; i++)
        if(x == 32 + i*89) color = 0x0F2030;
    for(int i = 0; i < 9; i++) {
        int cx = 70 + i * 123;
        int cy = 300 + (i % 3) * 120;
        int dx = x - cx, dy = y - cy;
        if(dx * dx + dy * dy <= 70 * 70)
            color = (dx * dx + dy * dy <= 54 * 54)
                ? (wallpaper_variant == 1 ? 0x123D5A : 0x14243F)
                : (wallpaper_variant == 2 ? 0x321A4A : 0x102C4A);
    }
    for(int i = 0; i < 9; i++) {
        int dx = x - (70 + i*123), dy = y - (300 + (i % 3)*120);
        if(dx*dx + dy*dy <= 8*8) color = ui_theme_color;
    }
    return color;
}

static void draw_wallpaper(void) {
    for(int y = 0; y < VESA_HEIGHT; y++) {
        draw_rect(0, y, VESA_WIDTH, 1, wallpaper_color_at(y));
        if((y % 48) == 0)
            draw_rect(0, y, VESA_WIDTH, 1, 0x18324A);
    }
    for(int i = 0; i < 12; i++) {
        int x = 32 + i*89;
        draw_rect(x, 0, 1, VESA_HEIGHT-TASKBAR_H, 0x0F2030);
    }
    for(int i = 0; i < 9; i++) {
        int cx = 70 + i * 123;
        int cy = 300 + (i % 3) * 120;
        draw_circle_filled(cx, cy, 70, wallpaper_variant == 2 ? 0x321A4A : 0x102C4A);
        draw_circle_filled(cx, cy, 54, wallpaper_variant == 1 ? 0x123D5A : 0x14243F);
        draw_circle_filled(cx, cy, 8, ui_theme_color);
    }
}

static void draw_taskbar(void) {
    draw_rect(0, VESA_HEIGHT-TASKBAR_H, VESA_WIDTH, TASKBAR_H, 0x1A1A1A);
    draw_rect(0, VESA_HEIGHT-TASKBAR_H, VESA_WIDTH, 2, ui_theme_color);
    const char* label = "GoonerOS";
    for(int i = 0; label[i]; i++)
        draw_char(14+i*8, VESA_HEIGHT-TASKBAR_H+10, label[i], ui_theme_color, 0x1A1A1A);

    // Live-Uhrzeit rechts in der Taskleiste (HH:MM, von desktop_tick() gepflegt)
    int cx = VESA_WIDTH - 14 - 5*8;
    for(int i = 0; i < 5; i++)
        draw_char(cx+i*8, VESA_HEIGHT-TASKBAR_H+10, clock_cache[i] ? clock_cache[i] : ' ', 0xDDDDDD, 0x1A1A1A);
}

// Kleine, wiedererkennbare Piktogramme statt leerer Farbboxen.
static void draw_icon_glyph(int i, int cx, int cy) {
    switch(i) {
        case 0: // Terminal
            draw_rect(cx-18, cy-12, 36, 24, 0x0A0A0A);
            draw_char(cx-14, cy-8, '>', icon_colors[0], 0x0A0A0A);
            draw_char(cx-6,  cy-8, '_', icon_colors[0], 0x0A0A0A);
            break;
        case 1: // Info
            draw_circle_filled(cx, cy, 15, icon_colors[1]);
            draw_char(cx-4, cy-8, 'i', 0x000000, icon_colors[1]);
            break;
        case 2: // Uhr
            draw_circle_filled(cx, cy, 15, 0x000000);
            draw_circle_filled(cx, cy, 12, icon_colors[2]);
            draw_rect(cx-1, cy-9, 2, 9, 0x000000);
            draw_rect(cx, cy-1, 7, 2, 0x000000);
            break;
        case 3: // Optionen - drei Schieberegler
            for(int r = 0; r < 3; r++)
                draw_rect(cx-18, cy-10+r*9, 12+r*8, 3, icon_colors[3]);
            break;
        case 4: { // Vollbild - vier Eckwinkel
            int s = 10, t = 3;
            draw_rect(cx-18, cy-12, s, t, icon_colors[4]); draw_rect(cx-18, cy-12, t, s, icon_colors[4]);
            draw_rect(cx+18-s, cy-12, s, t, icon_colors[4]); draw_rect(cx+18-t, cy-12, t, s, icon_colors[4]);
            draw_rect(cx-18, cy+12-t, s, t, icon_colors[4]); draw_rect(cx-18, cy+12-s, t, s, icon_colors[4]);
            draw_rect(cx+18-s, cy+12-t, s, t, icon_colors[4]); draw_rect(cx+18-t, cy+12-s, t, s, icon_colors[4]);
            break;
        }
        case 5: // Dateien
            draw_rect(cx-17, cy-12, 34, 25, 0xD8F0FF);
            draw_rect(cx-13, cy-16, 15, 6, 0xD8F0FF);
            draw_rect(cx-12, cy-7, 24, 2, icon_colors[i]);
            draw_rect(cx-12, cy-1, 18, 2, icon_colors[i]);
            break;
        case 6: // Netzwerk
            draw_circle_filled(cx, cy-11, 5, icon_colors[i]);
            draw_circle_filled(cx-16, cy+10, 5, icon_colors[i]);
            draw_circle_filled(cx+16, cy+10, 5, icon_colors[i]);
            draw_rect(cx-2, cy-7, 4, 18, icon_colors[i]);
            draw_rect(cx-14, cy+1, 28, 3, icon_colors[i]);
            break;
        case 7: // Hintergrund
            draw_circle_filled(cx, cy, 17, icon_colors[i]);
            draw_circle_filled(cx-7, cy-6, 4, 0xFFFFFF);
            draw_circle_filled(cx+8, cy+7, 5, 0xFFFFFF);
            break;
        case 8: // Aufgaben
            draw_rect(cx-19, cy-15, 38, 30, 0x16283A);
            for(int row = 0; row < 3; row++) {
                draw_rect(cx-12, cy-9+row*8, 4, 4, icon_colors[i]);
                draw_rect(cx-4, cy-8+row*8, 16, 2, 0xFFFFFF);
            }
            break;
    }
}

static void draw_icon(int i) {
    int x = icon_x(i), y = icon_y(i), cx = x + ICON_W / 2, cy = y + 34;
    draw_circle_filled(cx+3, cy+4, 31, 0x081521);
    draw_circle_filled(cx, cy, 29, 0x122B42);
    draw_circle_filled(cx, cy, 25, icon_colors[i]);
    draw_icon_glyph(i, cx, cy);
    int len = 0; while(icon_labels[i][len]) len++;
    int lx = x + (ICON_W - len*8)/2;
    for(int c = 0; c < len; c++)
        draw_char(lx+c*8, y+ICON_H-22, icon_labels[i][c], 0xFFFFFF, wallpaper_color_at(y+ICON_H-22));
}

/* ---------- Fensterverwaltung ---------- */

static int win_find_type(int type) {
    for(int i = 0; i < MAX_WINDOWS; i++)
        if(windows[i].active && windows[i].type == type) return i;
    return -1;
}

// Ist idx gerade von einem anderen (weiter vorne liegenden) Fenster verdeckt?
static int win_occluded(int idx) {
    window_t* w = &windows[idx];
    int pos = -1;
    for(int i = 0; i < zcount; i++) if(zorder[i] == idx) { pos = i; break; }
    if(pos < 0) return 0;
    for(int i = pos+1; i < zcount; i++) {
        window_t* o = &windows[zorder[i]];
        if(w->x < o->x+o->w && w->x+w->w > o->x && w->y < o->y+o->h && w->y+w->h > o->y) return 1;
    }
    return 0;
}

static void win_bring_front(int idx) {
    int pos = -1;
    for(int i = 0; i < zcount; i++) if(zorder[i] == idx) { pos = i; break; }
    if(pos < 0) return;
    for(int i = pos; i < zcount-1; i++) zorder[i] = zorder[i+1];
    zorder[zcount-1] = idx;
}

static int win_alloc_slot(void) {
    for(int i = 0; i < MAX_WINDOWS; i++) if(!windows[i].active) return i;
    return -1;
}

// Oeffnet ein Fenster des gegebenen Typs (oder holt es nach vorne, falls
// schon offen - pro Typ existiert immer hoechstens eine Instanz, weil z.B.
// der Terminal-Zustand (input_buf, text_buffer, ...) sowieso global ist).
static int win_open(int type) {
    int existing = win_find_type(type);
    if(existing >= 0) { win_bring_front(existing); return existing; }

    int idx = win_alloc_slot();
    if(idx < 0) return -1;

    windows[idx].active = 1;
    windows[idx].type = type;
    switch(type) {
        case TYPE_TERMINAL:  windows[idx].x=140; windows[idx].y=90;  windows[idx].w=740; windows[idx].h=560; break;
        case TYPE_INFO:      windows[idx].x=260; windows[idx].y=150; windows[idx].w=460; windows[idx].h=230; break;
        case TYPE_CLOCK:     windows[idx].x=400; windows[idx].y=260; windows[idx].w=300; windows[idx].h=160; break;
        case TYPE_SETTINGS:  windows[idx].x=300; windows[idx].y=220; windows[idx].w=420; windows[idx].h=224; break;
        case TYPE_FILES:     windows[idx].x=170; windows[idx].y=130; windows[idx].w=650; windows[idx].h=450; break;
        case TYPE_NETWORK:   windows[idx].x=270; windows[idx].y=200; windows[idx].w=480; windows[idx].h=260; break;
        case TYPE_EDITOR:    windows[idx].x=180; windows[idx].y=120; windows[idx].w=660; windows[idx].h=470; break;
        case TYPE_TASKS:     windows[idx].x=250; windows[idx].y=170; windows[idx].w=520; windows[idx].h=330; break;
    }
    zorder[zcount++] = idx;

    if(type == TYPE_TERMINAL) {
        window_t* w = &windows[idx];
        terminal_windowed = 1;
        vga_set_region(w->x+16, w->y+28, w->w-32, w->h-44);
        vga_clear_region();
        vga_print("GoonerOS - Windowed terminal. Drag the title bar to move,\nclick X to close.\n> ");
        // Bugfix: frueher wurden prompt_x/prompt_y hier nie gesetzt, wodurch
        // der Cursor an der zuletzt bekannten (falschen) Position blinkte.
        prompt_x = text_x; prompt_y = text_y;
        cursor_col = 0; input_idx = 0;
    }
    return idx;
}

static void win_close(int idx) {
    if(!windows[idx].active) return;
    if(windows[idx].type == TYPE_TERMINAL) terminal_windowed = 0;
    windows[idx].active = 0;
    int pos = -1;
    for(int i = 0; i < zcount; i++) if(zorder[i] == idx) { pos = i; break; }
    if(pos >= 0) { for(int i = pos; i < zcount-1; i++) zorder[i] = zorder[i+1]; zcount--; }
}

static int win_hit_test(int mx, int my) {
    for(int k = zcount-1; k >= 0; k--) {
        int idx = zorder[k];
        window_t* w = &windows[idx];
        if(mx >= w->x && mx < w->x+w->w && my >= w->y && my < w->y+w->h) return idx;
    }
    return -1;
}

static void win_close_btn_rect(window_t* w, int* bx, int* by, int* bs) {
    *bs = CLOSE_BTN_SIZE;
    *bx = w->x + w->w - CLOSE_BTN_SIZE - 6;
    *by = w->y + 2;
}
static int win_hit_close(window_t* w, int mx, int my) {
    int bx, by, bs; win_close_btn_rect(w, &bx, &by, &bs);
    return mx >= bx && mx < bx+bs && my >= by && my < by+bs;
}
static int win_hit_titlebar(window_t* w, int mx, int my) {
    if(win_hit_close(w, mx, my)) return 0;
    return mx >= w->x && mx < w->x+w->w && my >= w->y && my < w->y+20;
}

static void draw_close_button(window_t* w) {
    int bx, by, bs; win_close_btn_rect(w, &bx, &by, &bs);
    draw_rect(bx, by, bs, bs, 0xE04040);
    for(int i = 0; i < bs-4; i++) {
        put_pixel(bx+2+i, by+2+i, 0xFFFFFF);
        put_pixel(bx+2+i, by+bs-3-i, 0xFFFFFF);
    }
}

static void settings_swatch_rect(window_t* w, int i, int* sx, int* sy, int* sw, int* sh) {
    *sw = 40; *sh = 40;
    *sx = w->x + 20 + i*(40+14);
    *sy = w->y + 56;
}

static void task_draw_text(int x, int y, const char* text, unsigned int color) {
    for(int i = 0; text[i]; i++)
        draw_char(x+i*8, y, text[i], color, 0x16283A);
}

static void task_draw_number(int x, int y, unsigned int value, unsigned int color) {
    char digits[10];
    int count = 0;
    do {
        digits[count++] = '0' + value % 10;
        value /= 10;
    } while(value && count < sizeof(digits));
    for(int i = 0; i < count; i++)
        draw_char(x+i*8, y, digits[count-i-1], color, 0x16283A);
}

static void win_render(int idx) {
    window_t* w = &windows[idx];

    // Einfacher Schlagschatten (SHADOW_OFF Pixel unten/rechts sichtbar lassen)
    draw_rect(w->x+SHADOW_OFF, w->y+SHADOW_OFF, w->w, w->h, 0x0A0A0A);
    draw_window(w->x, w->y, w->w, w->h, win_titles[w->type]);
    draw_close_button(w);

    if(w->type == TYPE_TERMINAL) {
        draw_rect(w->x+2, w->y+20, w->w-4, w->h-22, 0x000000);
        vga_set_region(w->x+16, w->y+28, w->w-32, w->h-44);
        redraw_text_buffer();
        if(desktop_terminal_focused()) draw_cursor_bar(blink_visible);
    }

    else if(w->type == TYPE_INFO) {
        const char* lines[] = {
            "GoonerOS System Information",
            "",
            "Kernel: 32-bit Protected Mode",
            "Display: 1024x768 VESA framebuffer",
            "Filesystem: ATA-backed custom FS",
            "Mouse: PS/2 ready",
            "Theme: Neon Aqua",
            "Shell: gokernel shell v0.10",
            "Desktop: windowed + taskbar",
            "State: stable redraw pipeline",
            "Status: ready"
        };
        for(int i = 0; i < 11; i++)
            for(int c = 0; lines[i][c]; c++)
                draw_char(w->x+16+c*8, w->y+30+i*18, lines[i][c], 0xDDDDDD, 0x1A1A1A);
    }
    else if(w->type == TYPE_CLOCK) {
        int scale = 3, char_w = 8*scale, len = 8;
        int gx = w->x + (w->w - len*char_w)/2;
        int gy = w->y + 28 + ((w->h-28) - 16*scale)/2;
        for(int i = 0; i < len; i++)
            draw_char_scaled(gx+i*char_w, gy, clock_cache[i] ? clock_cache[i] : ' ', scale, ui_theme_color, 0x1A1A1A);
    }
    else if(w->type == TYPE_SETTINGS) {
        const char* label = "Choose an accent color:";
        for(int c = 0; label[c]; c++)
            draw_char(w->x+16+c*8, w->y+28, label[c], 0xDDDDDD, 0x1A1A1A);
        for(int i = 0; i < 6; i++) {
            int sx, sy, sw, sh; settings_swatch_rect(w, i, &sx, &sy, &sw, &sh);
            draw_rect(sx, sy, sw, sh, theme_colors[i]);
            if(theme_colors[i] == ui_theme_color) {
                draw_rect(sx-3, sy-3, sw+6, 3, 0xFFFFFF);
                draw_rect(sx-3, sy+sh, sw+6, 3, 0xFFFFFF);
                draw_rect(sx-3, sy-3, 3, sh+6, 0xFFFFFF);
                draw_rect(sx+sw, sy-3, 3, sh+6, 0xFFFFFF);
            }
        }
        const char* wallpaper_label = "Wallpaper";
        for(int c = 0; wallpaper_label[c]; c++)
            draw_char(w->x+20+c*8, w->y+100, wallpaper_label[c], 0xDDDDDD, 0x1A1A1A);
        for(int i = 0; i < 3; i++) {
            int sx, sy, sw, sh;
            settings_wallpaper_rect(w, i, &sx, &sy, &sw, &sh);
            unsigned int fill = i == wallpaper_variant ? ui_theme_color : 0x29495E;
            draw_rect(sx, sy, sw, sh, fill);
            for(int c = 0; wallpaper_names[i][c]; c++)
                draw_char(sx+8+c*8, sy+7, wallpaper_names[i][c], 0xFFFFFF, fill);
            if(i == wallpaper_variant) {
                draw_rect(sx-2, sy-2, sw+4, 2, 0xFFFFFF);
                draw_rect(sx-2, sy+sh, sw+4, 2, 0xFFFFFF);
                draw_rect(sx-2, sy-2, 2, sh+4, 0xFFFFFF);
                draw_rect(sx+sw, sy-2, 2, sh+4, 0xFFFFFF);
            }
        }
        for(int c = 0; preferences_status[c] && c < 48; c++)
            draw_char(w->x+20+c*8, w->y+164, preferences_status[c], 0x9AB3C1, 0x1A1A1A);

    }
    else if(w->type == TYPE_FILES) {
        const char* heading = "GOONEROS FILES";
        for(int c = 0; heading[c]; c++)
            draw_char(w->x+22+c*8, w->y+34, heading[c], 0xD8F0FF, 0x16283A);
        const char* location = files_cwd[0] ? files_cwd : "/";
        for(int c = 0; location[c] && c < 12; c++)
            draw_char(w->x+172+c*8, w->y+34, location[c], 0x79A9C2, 0x16283A);
        draw_rect(w->x+282, w->y+24, 36, 30, 0x29495E);
        draw_char(w->x+295, w->y+31, '<', 0xFFFFFF, 0x29495E);
        draw_rect(w->x+326, w->y+24, 154, 30, 0x29495E);
        const char* new_folder = "New Folder";
        for(int c = 0; new_folder[c]; c++)
            draw_char(w->x+332+c*8, w->y+31, new_folder[c], 0xFFFFFF, 0x29495E);
        draw_rect(w->x+490, w->y+24, 140, 30, 0x2EBD85);
        const char* create = "New File";
        for(int c = 0; create[c]; c++)
            draw_char(w->x+502+c*8, w->y+31, create[c], 0xFFFFFF, 0x2EBD85);
        draw_rect(w->x+286, w->y+58, 2, 280, 0x2A4658);
        const char* list_label = "FILE";
        const char* size_label = "SIZE";
        const char* preview_label = "PREVIEW";
        for(int c = 0; list_label[c]; c++)
            draw_char(w->x+22+c*8, w->y+60, list_label[c], 0x79A9C2, 0x16283A);
        for(int c = 0; size_label[c]; c++)
            draw_char(w->x+190+c*8, w->y+60, size_label[c], 0x79A9C2, 0x16283A);
        for(int c = 0; preview_label[c]; c++)
            draw_char(w->x+306+c*8, w->y+60, preview_label[c], 0x79A9C2, 0x16283A);
        int file_count = 0;
        for(int i = 0; i < FS_MAX_FILES; i++)
            if(fs_table[i].used && fs_is_direct_child(&fs_table[i], files_cwd)) file_count++;
        int page_count = (file_count + 7) / 8;
        if(page_count < 1) page_count = 1;
        if(files_page >= page_count) files_page = page_count - 1;
        draw_rect(w->x+22, w->y+398, 34, 30, 0x29495E);
        draw_rect(w->x+64, w->y+398, 34, 30, 0x29495E);
        draw_char(w->x+35, w->y+405, '<', 0xFFFFFF, 0x29495E);
        draw_char(w->x+77, w->y+405, '>', 0xFFFFFF, 0x29495E);
        draw_char(w->x+112, w->y+405, '0'+files_page+1, 0xD8F0FF, 0x16283A);
        draw_char(w->x+120, w->y+405, '/', 0xD8F0FF, 0x16283A);
        draw_char(w->x+128, w->y+405, '0'+page_count, 0xD8F0FF, 0x16283A);
        if(files_selected >= 0) {
            struct fs_entry* selected = &fs_table[files_selected];
            draw_rect(w->x+430, w->y+398, 92, 30, 0xB58CFF);
            const char* edit = fs_is_directory(selected) ? "Open" : "Edit";
            for(int c = 0; edit[c]; c++)
                draw_char(w->x+434+c*8, w->y+405, edit[c], 0xFFFFFF, 0xB58CFF);
            draw_rect(w->x+530, w->y+398, 96, 30, 0xA84B5A);
            const char* remove = "Delete";
            for(int c = 0; remove[c]; c++)
                draw_char(w->x+537+c*8, w->y+405, remove[c], 0xFFFFFF, 0xA84B5A);
        }
        int row = 0, file_ordinal = 0;
        for(int i = 0; i < FS_MAX_FILES && row < 8; i++) {
            if(!fs_table[i].used || !fs_is_direct_child(&fs_table[i], files_cwd)) continue;
            if(file_ordinal++ < files_page * 8) continue;
            char size[12];
            int v = (int)fs_table[i].size, digits = 0;
            if(v == 0) size[digits++] = '0';
            else {
                char rev[12]; while(v) { rev[digits++] = '0' + v % 10; v /= 10; }
                for(int j = 0; j < digits; j++) size[j] = rev[digits-1-j];
            }
            size[digits] = 0;
            int x = w->x + 22, y = w->y + 84 + row * 22;
            unsigned int fg = (files_selected == i) ? ui_theme_color : 0xD8F0FF;
            if(files_selected == i) draw_rect(w->x+18, y-2, 250, 19, 0x203C50);
            const char* basename = fs_table[i].name;
            for(int c = 0; fs_table[i].name[c]; c++) if(fs_table[i].name[c] == '/') basename = &fs_table[i].name[c+1];
            for(int c = 0; basename[c] && c < 19; c++) draw_char(x+c*8, y, basename[c], fg, files_selected == i ? 0x203C50 : 0x16283A);
            if(fs_is_directory(&fs_table[i])) draw_char(x+19*8, y, '/', fg, 0x16283A);
            for(int c = 0; size[c] && c < 8; c++) draw_char(w->x+202+c*8, y, size[c], 0x8CB5CC, 0x16283A);
            row++;
        }
        if(files_selected < 0 || files_selected >= FS_MAX_FILES || !fs_table[files_selected].used) {
            const char* empty = "Select a file on the left.";
            for(int c = 0; empty[c]; c++)
                draw_char(w->x+306+c*8, w->y+86, empty[c], 0x9AB3C1, 0x16283A);
        } else {
            if(fs_is_directory(&fs_table[files_selected])) {
                const char* line = "Directory";
                for(int c = 0; line[c]; c++)
                    draw_char(w->x+306+c*8, w->y+86, line[c], 0xD8F0FF, 0x16283A);
                const char* hint = "Open to view contents";
                for(int c = 0; hint[c]; c++)
                    draw_char(w->x+306+c*8, w->y+110, hint[c], 0x79A9C2, 0x16283A);
            } else {
                int line = 0, col = 0;
                for(int c = 0; files_preview[c] && line < 13; c++) {
                    if(files_preview[c] == '\n' || col >= 35) {
                        line++; col = 0;
                        if(files_preview[c] == '\n') continue;
                    }
                    draw_char(w->x+306+col*8, w->y+86+line*16, files_preview[c], 0xD8F0FF, 0x16283A);
                    col++;
                }
                if(files_preview[0]) {
                    const char* hint = "Edit to open the editor";
                    for(int c = 0; hint[c]; c++)
                        draw_char(w->x+306+c*8, w->y+326, hint[c], 0x79A9C2, 0x16283A);
                }
            }
        }
    }
    else if(w->type == TYPE_NETWORK) {
        const char* title = "Network status";
        for(int c = 0; title[c]; c++)
            draw_char(w->x+22+c*8, w->y+34, title[c], 0xD8F0FF, 0x16283A);
        draw_circle_filled(w->x+72, w->y+112, 20, 0xFF5C72);
        draw_circle_filled(w->x+72, w->y+112, 10, 0x16283A);
        const char* status = "No network driver";
        for(int c = 0; status[c]; c++)
            draw_char(w->x+112+c*8, w->y+106, status[c], 0xD8F0FF, 0x16283A);
        const char* detail = "Ping requires a network stack";
        for(int c = 0; detail[c]; c++)
            draw_char(w->x+22+c*8, w->y+158, detail[c], 0x91AFC2, 0x16283A);
    }
    else if(w->type == TYPE_TASKS) {
        task_draw_text(w->x+20, w->y+34, "Cooperative tasks", 0xD8F0FF);
        task_draw_text(w->x+20, w->y+56, "Kernel event loop runs; jobs share time slices.", 0x91AFC2);
        task_draw_text(w->x+20, w->y+84, "PID  TASK        STATE      PROGRESS", 0x79A9C2);
        int count = scheduler_task_count();
        for(int i = 0; i < count && i < SCHEDULER_MAX_TASKS; i++) {
            int pid, type, state;
            unsigned int steps, progress, total, result;
            if(!scheduler_get_task_info(i, &pid, &type, &state, &steps,
                                        &progress, &total, &result))
                continue;
            (void)result;
            int y = w->y+108+i*22;
            unsigned int color = state == SCHEDULER_STATE_DONE ? 0x5EEB9B : 0xD8F0FF;
            task_draw_number(w->x+20, y, (unsigned int)pid, color);
            task_draw_text(w->x+68, y, type == SCHEDULER_TASK_CHECKSUM ? "CRC32" : "Counter", color);
            task_draw_text(w->x+170, y, state == SCHEDULER_STATE_DONE ? "done" : "active", color);
            if(type == SCHEDULER_TASK_CHECKSUM) {
                task_draw_number(w->x+244, y, progress, color);
                task_draw_text(w->x+292, y, "/", color);
                task_draw_number(w->x+308, y, total, color);
                task_draw_text(w->x+372, y, "Bytes", color);
            } else {
                task_draw_number(w->x+244, y, steps, color);
                task_draw_text(w->x+308, y, "time slices", color);
            }
        }
        task_draw_text(w->x+20, w->y+300, "No preemptive scheduler; tasks have no separate stack.", 0x91AFC2);
    }
    else if(w->type == TYPE_EDITOR) {
        editor_render();
    }
}

static void editor_render(void) {
    int editor_idx = win_find_type(TYPE_EDITOR);
    if(editor_idx < 0) return;
    window_t* w = &windows[editor_idx];
    const char* name_label = "Name:";
    const char* text_label = "Content:";
    for(int c = 0; name_label[c]; c++)
        draw_char(w->x+22+c*8, w->y+38, name_label[c], 0xD8F0FF, 0x16283A);
    for(int c = 0; c < editor_name_len; c++)
        draw_char(w->x+80+c*8, w->y+38, editor_name[c], 0xFFFFFF, 0x16283A);
    if(editor_cursor_visible && editor_field == 0 && !editor_existing) {
        draw_rect(w->x+80+editor_name_len*8, w->y+39, 2, 14, ui_theme_color);
    }
    if(editor_directory) {
        const char* directory_note = "New directory in the current folder";
        for(int c = 0; directory_note[c]; c++)
            draw_char(w->x+22+c*8, w->y+82, directory_note[c], 0xD8F0FF, 0x16283A);
    } else {
        for(int c = 0; text_label[c]; c++)
            draw_char(w->x+22+c*8, w->y+82, text_label[c], 0xD8F0FF, 0x16283A);
        int col = 0, row = 0;
        for(int c = 0; c < editor_text_len; c++) {
            if(editor_text[c] == '\n') { col = 0; row++; continue; }
            if(col >= 76) { col = 0; row++; }
            draw_char(w->x+22+col*8, w->y+106+row*16, editor_text[c], 0xFFFFFF, 0x16283A);
            col++;
        }
        if(col >= 76) { col = 0; row++; }
        editor_cursor_position(&col, &row);
        if(editor_cursor_visible && editor_field == 1 && row < 18)
            draw_rect(w->x+22+col*8, w->y+106+row*16, 2, 14, ui_theme_color);
    }
    draw_rect(w->x+22, w->y+400, 120, 28, 0x2EBD85);
    draw_rect(w->x+162, w->y+400, 120, 28, 0xA84B5A);
    const char* save = "Save";
    const char* cancel = "Cancel";
    for(int c = 0; save[c]; c++) draw_char(w->x+34+c*8, w->y+406, save[c], 0xFFFFFF, 0x2EBD85);
    for(int c = 0; cancel[c]; c++) draw_char(w->x+174+c*8, w->y+406, cancel[c], 0xFFFFFF, 0xA84B5A);
    for(int c = 0; editor_status[c] && c < 42; c++)
        draw_char(w->x+22+c*8, w->y+440, editor_status[c], 0xFFB84D, 0x16283A);
}

static void drag_clamp_position(window_t* w, int* x, int* y) {
    int max_y = VESA_HEIGHT - TASKBAR_H - w->h;
    if(*x < 0) *x = 0;
    if(*y < 0) *y = 0;
    if(*x + w->w > VESA_WIDTH) *x = VESA_WIDTH - w->w;
    if(*y > max_y) *y = max_y;
}

static void drag_move_window(int idx, int x, int y) {
    window_t* w = &windows[idx];
    drag_clamp_position(w, &x, &y);
    if(x == w->x && y == w->y) return;

    int old_x = w->x, old_y = w->y;
    erase_rect_to_desktop(old_x, old_y, w->w+SHADOW_OFF, w->h+SHADOW_OFF);
    redraw_windows_overlapping(old_x, old_y, w->w+SHADOW_OFF, w->h+SHADOW_OFF, idx);
    w->x = x;
    w->y = y;
    if(w->type == TYPE_TERMINAL) {
        prompt_x += x - old_x;
        prompt_y += y - old_y;
        text_x += x - old_x;
        text_y += y - old_y;
        vga_set_region(w->x+16, w->y+28, w->w-32, w->h-44);
    }
    win_render(idx);
}

static void drag_outline_restore(void) {
    if(!drag_outline_active) return;
    int x = drag_outline_x, y = drag_outline_y;
    int w = drag_outline_w, h = drag_outline_h;
    int pos = 0;
    for(int xx = 0; xx < w; xx++) put_pixel(x+xx, y, drag_outline_pixels[pos++]);
    for(int xx = 0; xx < w; xx++) put_pixel(x+xx, y+h-1, drag_outline_pixels[pos++]);
    for(int yy = 1; yy < h-1; yy++) put_pixel(x, y+yy, drag_outline_pixels[pos++]);
    for(int yy = 1; yy < h-1; yy++) put_pixel(x+w-1, y+yy, drag_outline_pixels[pos++]);
    drag_outline_active = 0;
}

static void drag_outline_draw(int x, int y, int w, int h) {
    if(w <= 0 || h <= 1 || 2*w + 2*(h-2) > (int)(sizeof(drag_outline_pixels)/sizeof(drag_outline_pixels[0])))
        return;
    drag_outline_x = x; drag_outline_y = y;
    drag_outline_w = w; drag_outline_h = h;
    int pos = 0;
    for(int xx = 0; xx < w; xx++) {
        drag_outline_pixels[pos++] = get_pixel(x+xx, y);
        put_pixel(x+xx, y, 0xFFFFFF);
    }
    for(int xx = 0; xx < w; xx++) {
        drag_outline_pixels[pos++] = get_pixel(x+xx, y+h-1);
        put_pixel(x+xx, y+h-1, 0xFFFFFF);
    }
    for(int yy = 1; yy < h-1; yy++) {
        drag_outline_pixels[pos++] = get_pixel(x, y+yy);
        put_pixel(x, y+yy, 0xFFFFFF);
    }
    for(int yy = 1; yy < h-1; yy++) {
        drag_outline_pixels[pos++] = get_pixel(x+w-1, y+yy);
        put_pixel(x+w-1, y+yy, 0xFFFFFF);
    }
    drag_outline_active = 1;
}

/* ---------- Gezielte (nicht komplette) Neuzeichnungen ---------- */

// Setzt ein Rechteck auf den Wallpaper-Verlauf zurueck und malt Taskleiste/
// Icons neu, falls sie in diesem Bereich liegen. Deutlich billiger als
// clear_pixels_only()+draw_wallpaper() ueber den ganzen Bildschirm.
static void erase_rect_to_desktop(int x, int y, int w, int h) {
    if(x < 0) { w += x; x = 0; }
    if(y < 0) { h += y; y = 0; }
    if(x + w > VESA_WIDTH) w = VESA_WIDTH - x;
    if(y + h > VESA_HEIGHT) h = VESA_HEIGHT - y;
    if(w <= 0 || h <= 0) return;

    for(int yy = y; yy < y+h; yy++)
        for(int xx = x; xx < x+w; xx++)
            put_pixel(xx, yy, wallpaper_pixel_at(xx, yy));

    if(y + h > VESA_HEIGHT - TASKBAR_H) draw_taskbar();
    for(int i = 0; i < ICON_COUNT; i++) {
        int ix = icon_x(i), iy = icon_y(i);
        if(x < ix+ICON_W && x+w > ix && y < iy+ICON_H && y+h > iy) draw_icon(i);
    }
}

// Zeichnet alle (aktiven) Fenster neu, die das gegebene Rechteck ueberlappen -
// in Z-Order, "except_idx" auslassen (z.B. das gerade gezogene Fenster, das
// ohnehin gleich separat obenauf gezeichnet wird).
static void redraw_windows_overlapping(int x, int y, int w, int h, int except_idx) {
    for(int k = 0; k < zcount; k++) {
        int idx = zorder[k];
        if(idx == except_idx) continue;
        window_t* win = &windows[idx];
        if(win->x < x+w && win->x+win->w > x && win->y < y+h && win->y+win->h > y)
            win_render(idx);
    }
}

// Kompletter Redraw - bewusst NUR fuer: Desktop betreten, Themenwechsel
// (faerbt wirklich jede Titelleiste + Taskleiste neu ein). Fuer alles andere
// (Ziehen, Oeffnen/Schliessen, Uhr-Tick) werden gezielte Redraws benutzt.
static void desktop_redraw_all(void) {
    mouse_cursor_hide(); // BUGFIX: erst Cursor sauber entfernen, sonst wird er als "Hintergrund" mitgeloescht/gesichert
    clear_pixels_only();
    draw_wallpaper();
    for(int i = 0; i < ICON_COUNT; i++) draw_icon(i);
    draw_taskbar();
    for(int k = 0; k < zcount; k++) win_render(zorder[k]);
    mouse_refresh_cursor();
}

/* ==================== Oeffentliche API ==================== */

void desktop_enter(void) {
    desktop_active = 1;
    terminal_windowed = 0;
    zcount = 0;
    drag_idx = -1;
    drag_outline_restore();
    load_desktop_preferences();
    for(int i = 0; i < MAX_WINDOWS; i++) windows[i].active = 0;
    desktop_redraw_all();
}

void desktop_fullscreen(void) {
    desktop_active = 0;
    terminal_windowed = 0;
    drag_idx = -1;
    vga_set_region(0, 0, TEXT_WRAP_WIDTH, VESA_HEIGHT);
    vga_clear();
    vga_print("> ");
    // Bugfix: fehlte im Original komplett - der Eingabecursor blieb an der
    // alten Position stehen, statt hinter dem neuen Prompt zu erscheinen.
    prompt_x = text_x; prompt_y = text_y;
    cursor_col = 0; input_idx = 0;
    blink_visible = 1;
    draw_cursor_bar(1);
}

// Ob das Terminal gerade das "fokussierte Programm" ist, also Tastatur-
// eingaben bekommen soll: im Vollbild immer, auf dem Desktop nur wenn das
// Terminal-Fenster existiert UND das oberste (zuletzt angeklickte) Fenster
// ist. Wird von keyboard.c benutzt, um Tastendruecke zu verwerfen, wenn
// ein anderes Fenster obenauf liegt oder gar kein Terminal offen ist - der
// Desktop ist damit kein reiner "interaktiver Hintergrund fuers Terminal"
// mehr, sondern das Terminal ist ein Fenster wie jedes andere, das erst
// fokussiert sein muss. Steuert ausserdem, ob der Eingabecursor blinkt.
int desktop_terminal_focused(void) {
    if(!desktop_active) return 1;
    int idx = win_find_type(TYPE_TERMINAL);
    if(idx < 0) return 0;
    return zcount > 0 && zorder[zcount-1] == idx;
}

// Wird NICHT mehr aus dem Maus-Interrupt aufgerufen, sondern von kernel.c's
// Hauptschleife, sobald mouse_poll_event() ein neues Paket meldet - siehe
// Erklaerung oben im Datei-Header. Erkennt Klick-Uebergaenge selbst.
void desktop_handle_mouse(int mx, int my, int left_down) {
    static int prev_left = 0;
    if(!desktop_active) {
        prev_left = left_down;
        drag_outline_restore();
        drag_idx = -1;
        mouse_refresh_cursor();
        return;
    }

    int pressed = left_down && !prev_left;
    int released = !left_down && prev_left;
    prev_left = left_down;

    // ---- Aktives Ziehen eines Fensters ----
    if(drag_idx >= 0) {
        if(released || !left_down) {
            mouse_cursor_hide();
            drag_outline_restore();
            drag_move_window(drag_idx, mx-drag_off_x, my-drag_off_y);
            mouse_refresh_cursor();
            drag_idx = -1;
            return;
        }
        if(ticks - drag_last_render_tick >= 16) {
            int preview_x = mx-drag_off_x, preview_y = my-drag_off_y;
            drag_clamp_position(&windows[drag_idx], &preview_x, &preview_y);
            mouse_cursor_hide();
            drag_outline_restore();
            drag_outline_draw(preview_x, preview_y, windows[drag_idx].w, windows[drag_idx].h);
            drag_last_render_tick = ticks;
            mouse_refresh_cursor();
        } else {
            mouse_refresh_cursor();
        }
        return;
    }

    if(!pressed) {
        mouse_refresh_cursor();
        return;
    }

    // ---- Klick auf ein Fenster ----
    int idx = win_hit_test(mx, my);
    if(idx >= 0) {
        window_t* w = &windows[idx];
        int was_front = (zcount > 0 && zorder[zcount-1] == idx);
        win_bring_front(idx);

        if(win_hit_close(w, mx, my)) {
            int cx = w->x, cy = w->y, cw = w->w, ch = w->h;
            if(w->type == TYPE_EDITOR) { editor_active = 0; editor_directory = 0; }
            win_close(idx);
            mouse_cursor_hide(); // BUGFIX: siehe mouse.c
            erase_rect_to_desktop(cx, cy, cw+SHADOW_OFF, ch+SHADOW_OFF);
            redraw_windows_overlapping(cx, cy, cw+SHADOW_OFF, ch+SHADOW_OFF, -1);
            mouse_refresh_cursor();
            return;
        }
        if(win_hit_titlebar(w, mx, my)) {
            drag_idx = idx;
            drag_off_x = mx - w->x;
            drag_off_y = my - w->y;
            drag_last_render_tick = ticks;
            drag_outline_active = 0;
            if(!was_front) { mouse_cursor_hide(); win_render(idx); mouse_refresh_cursor(); }
            return;
        }
        if(w->type == TYPE_EDITOR) {
            if(mx >= w->x+22 && mx < w->x+142 && my >= w->y+400 && my < w->y+428) {
                if(editor_name_len == 0) {
                    editor_status = "Enter a file name.";
                    mouse_cursor_hide(); win_render(idx); mouse_refresh_cursor();
                    return;
                }
                char save_path[FS_NAME_LEN];
                int path_ok = 1;
                if(editor_existing) {
                    int len = strlen(editor_name);
                    if(len >= FS_NAME_LEN) path_ok = 0;
                    else for(int i = 0; i <= len; i++) save_path[i] = editor_name[i];
                } else path_ok = files_make_path(editor_name, save_path);
                if(!path_ok) {
                    editor_status = "Path is too long or invalid.";
                    mouse_cursor_hide(); win_render(idx); mouse_refresh_cursor();
                    return;
                }
                if(!editor_existing && fs_find(save_path)) {
                    editor_status = "That name already exists.";
                    mouse_cursor_hide(); win_render(idx); mouse_refresh_cursor();
                    return;
                }
                int saved = editor_directory ? fs_mkdir(save_path)
                                             : fs_write(save_path, editor_text, editor_text_len);
                if(!saved) {
                    editor_status = editor_directory
                        ? "Could not create directory (parent missing or disk full)."
                        : "Save failed (check disk space).";
                    mouse_cursor_hide(); win_render(idx); mouse_refresh_cursor();
                    return;
                }
                editor_status = "";
                struct fs_entry* saved_file = fs_find(save_path);
                if(saved_file) {
                    files_selected = (int)(saved_file - fs_table);
                    int ordinal = 0;
                    for(int i = 0; i < files_selected; i++)
                        if(fs_table[i].used && fs_is_direct_child(&fs_table[i], files_cwd)) ordinal++;
                    files_page = ordinal / 8;
                    files_preview[0] = 0;
                    if(!editor_directory) {
                        char data[sizeof(files_preview)];
                        int n = fs_read(save_path, data, sizeof(data));
                        int j = 0;
                        if(n > 0) while(j < n && j < (int)sizeof(files_preview)-1) { files_preview[j] = data[j]; j++; }
                        files_preview[j] = 0;
                        if(n < 0) {
                            const char* error = "Could not read the preview.";
                            for(j = 0; error[j] && j < (int)sizeof(files_preview)-1; j++) files_preview[j] = error[j];
                            files_preview[j] = 0;
                        }
                    }
                }
                int ex = w->x, ey = w->y, ew = w->w, eh = w->h;
                win_close(idx); editor_active = 0; editor_directory = 0;
                mouse_cursor_hide();
                erase_rect_to_desktop(ex, ey, ew+SHADOW_OFF, eh+SHADOW_OFF);
                redraw_windows_overlapping(ex, ey, ew+SHADOW_OFF, eh+SHADOW_OFF, -1);
                mouse_refresh_cursor();
                return;
            }
            if(mx >= w->x+162 && mx < w->x+282 && my >= w->y+400 && my < w->y+428) {
                int ex = w->x, ey = w->y, ew = w->w, eh = w->h;
                win_close(idx); editor_active = 0; editor_directory = 0;
                mouse_cursor_hide();
                erase_rect_to_desktop(ex, ey, ew+SHADOW_OFF, eh+SHADOW_OFF);
                redraw_windows_overlapping(ex, ey, ew+SHADOW_OFF, eh+SHADOW_OFF, -1);
                mouse_refresh_cursor();
                return;
            }
            if(my >= w->y+28 && my < w->y+64) editor_field = 0;
            else if(my >= w->y+72 && my < w->y+390) editor_field = 1;
            editor_cursor_visible = 1; editor_last_blink_tick = ticks;
            mouse_cursor_hide(); win_render(idx); mouse_refresh_cursor();
            return;
        }
        if(w->type == TYPE_FILES) {
            if(files_selected < 0 || files_selected >= FS_MAX_FILES || !fs_table[files_selected].used) {
                files_selected = -1;
                files_preview[0] = 0;
            }
            if(mx >= w->x+282 && mx < w->x+318 && my >= w->y+24 && my < w->y+54) {
                if(files_cwd[0]) files_go_parent();
                mouse_cursor_hide(); win_render(idx); mouse_refresh_cursor();
                return;
            }
            if(mx >= w->x+326 && mx < w->x+480 && my >= w->y+24 && my < w->y+54) {
                editor_active = 1; editor_existing = 0; editor_directory = 1; editor_field = 0;
                editor_name_len = 0; editor_text_len = 0;
                editor_name[0] = 0; editor_text[0] = 0; editor_status = "";
                editor_cursor_visible = 1; editor_last_blink_tick = ticks;
                int edit = win_open(TYPE_EDITOR);
                if(edit >= 0) { mouse_cursor_hide(); win_render(edit); mouse_refresh_cursor(); }
                return;
            }
            if(mx >= w->x+490 && mx < w->x+630 && my >= w->y+24 && my < w->y+54) {
                editor_active = 1; editor_existing = 0; editor_directory = 0; editor_field = 0;
                editor_name_len = 0; editor_text_len = 0;
                editor_name[0] = 0; editor_text[0] = 0;
                editor_status = "";
                editor_cursor_visible = 1; editor_last_blink_tick = ticks;
                int edit = win_open(TYPE_EDITOR);
                if(edit >= 0) { mouse_cursor_hide(); win_render(edit); mouse_refresh_cursor(); }
                return;
            }
            if(my >= w->y+398 && my < w->y+428 &&
               ((mx >= w->x+22 && mx < w->x+56) || (mx >= w->x+64 && mx < w->x+98))) {
                int count = 0;
                for(int i = 0; i < FS_MAX_FILES; i++)
                    if(fs_table[i].used && fs_is_direct_child(&fs_table[i], files_cwd)) count++;
                int pages = (count + 7) / 8;
                if(pages < 1) pages = 1;
                int old_page = files_page;
                if(mx < w->x+56 && files_page > 0) files_page--;
                if(mx >= w->x+64 && files_page + 1 < pages) files_page++;
                if(files_page != old_page) {
                    files_selected = -1;
                    files_preview[0] = 0;
                }
                mouse_cursor_hide(); win_render(idx); mouse_refresh_cursor();
                return;
            }
            if(files_selected >= 0 &&
               mx >= w->x+430 && mx < w->x+522 &&
               my >= w->y+398 && my < w->y+428) {
                if(fs_is_directory(&fs_table[files_selected])) {
                    int path_len = strlen(fs_table[files_selected].name);
                    for(int k = 0; k <= path_len; k++) files_cwd[k] = fs_table[files_selected].name[k];
                    files_selected = -1; files_page = 0; files_preview[0] = 0;
                    mouse_cursor_hide(); win_render(idx); mouse_refresh_cursor();
                    return;
                }
                editor_active = 1; editor_existing = 1; editor_directory = 0; editor_field = 1;
                editor_name_len = strlen(fs_table[files_selected].name);
                for(int k = 0; k < editor_name_len; k++) editor_name[k] = fs_table[files_selected].name[k];
                editor_name[editor_name_len] = 0;
                editor_text_len = 0;
                char data[128];
                int n = fs_read(editor_name, data, sizeof(data));
                if(n > 0) {
                    editor_text_len = n;
                    for(int k = 0; k < n; k++) editor_text[k] = data[k];
                    editor_status = "";
                } else editor_status = "Could not read the file.";
                editor_text[editor_text_len] = 0;
                editor_cursor_visible = 1; editor_last_blink_tick = ticks;
                int edit = win_open(TYPE_EDITOR);
                if(edit >= 0) { mouse_cursor_hide(); win_render(edit); mouse_refresh_cursor(); }
                return;
            }
            if(files_selected >= 0 && mx >= w->x+530 && mx < w->x+626 &&
               my >= w->y+398 && my < w->y+428) {
                int removed = fs_is_directory(&fs_table[files_selected])
                    ? fs_rmdir(fs_table[files_selected].name)
                    : fs_delete(fs_table[files_selected].name);
                if(removed) {
                    files_selected = -1;
                    files_preview[0] = 0;
                } else {
                    const char* error = "Delete failed.";
                    int j = 0;
                    while(error[j] && j < (int)sizeof(files_preview)-1) { files_preview[j] = error[j]; j++; }
                    files_preview[j] = 0;
                }
                mouse_cursor_hide(); win_render(idx); mouse_refresh_cursor();
                return;
            }
            int row = (my - (w->y + 84)) / 22;
            if(mx < w->x+286 && row >= 0 && row < 8) {
                int found = -1, current = 0;
                int target = files_page * 8 + row;
                for(int i = 0; i < FS_MAX_FILES; i++) {
                    if(!fs_table[i].used || !fs_is_direct_child(&fs_table[i], files_cwd)) continue;
                    if(current++ == target) { found = i; break; }
                }
                if(found >= 0) {
                    files_selected = found;
                    files_page = (current - 1) / 8;
                    files_preview[0] = 0;
                    char data[sizeof(files_preview)];
                    int n = fs_read(fs_table[found].name, data, sizeof(data));
                    int j = 0;
                    if(n > 0) while(j < n && j < (int)sizeof(files_preview)-1) { files_preview[j] = data[j]; j++; }
                    files_preview[j] = 0;
                    if(n < 0) {
                        const char* error = "Could not read the preview.";
                        for(j = 0; error[j] && j < (int)sizeof(files_preview)-1; j++) files_preview[j] = error[j];
                        files_preview[j] = 0;
                    }
                    if(fs_is_directory(&fs_table[found])) {
                        const char* directory = "Directory - open to view contents";
                        for(j = 0; directory[j] && j < (int)sizeof(files_preview)-1; j++) files_preview[j] = directory[j];
                        files_preview[j] = 0;
                    }
                    mouse_cursor_hide(); win_render(idx); mouse_refresh_cursor();
                    return;
                }
            }
        }
        if(w->type == TYPE_SETTINGS) {
            for(int i = 0; i < 6; i++) {
                int sx, sy, sw, sh; settings_swatch_rect(w, i, &sx, &sy, &sw, &sh);
                if(mx >= sx && mx < sx+sw && my >= sy && my < sy+sh) {
                    ui_theme_color = theme_colors[i];
                    desktop_save_preferences();
                    return;
                }
            }
            for(int i = 0; i < 3; i++) {
                int sx, sy, sw, sh;
                settings_wallpaper_rect(w, i, &sx, &sy, &sw, &sh);
                if(mx >= sx && mx < sx+sw && my >= sy && my < sy+sh) {
                    wallpaper_variant = i;
                    desktop_save_preferences();
                    return;
                }
            }
        }
        if(!was_front) { mouse_cursor_hide(); win_render(idx); mouse_refresh_cursor(); }
        return;
    }

    // ---- Kein Fenster getroffen -> vielleicht ein Icon ----
    for(int i = 0; i < ICON_COUNT; i++) {
        int ix = icon_x(i), iy = icon_y(i);
        if(mx >= ix && mx < ix+ICON_W && my >= iy && my < iy+ICON_H) {
            int idx2 = -1;
            if(i == 0) idx2 = win_open(TYPE_TERMINAL);
            else if(i == 1) idx2 = win_open(TYPE_INFO);
            else if(i == 2) idx2 = win_open(TYPE_CLOCK);
            else if(i == 3) idx2 = win_open(TYPE_SETTINGS);
            else if(i == 4) { desktop_fullscreen(); return; }
            else if(i == 5) idx2 = win_open(TYPE_FILES);
            else if(i == 6) idx2 = win_open(TYPE_NETWORK);
            else if(i == 7) {
                wallpaper_variant = (wallpaper_variant + 1) % 3;
                desktop_save_preferences();
                return;
            }
            else if(i == 8) idx2 = win_open(TYPE_TASKS);
            if(idx2 >= 0) { mouse_cursor_hide(); win_render(idx2); mouse_refresh_cursor(); }
            return;
        }
    }
}

int desktop_editor_active(void) {
    int idx = win_find_type(TYPE_EDITOR);
    return editor_active && idx >= 0 && zcount > 0 && zorder[zcount-1] == idx;
}

void desktop_editor_update(void) {
    if(!desktop_editor_active()) return;
    int redraw = editor_dirty;
    int blink = 0;
    if(ticks - editor_last_blink_tick >= 300) {
        editor_last_blink_tick = ticks;
        editor_cursor_visible = !editor_cursor_visible;
        blink = 1;
    }
    if(redraw) {
        editor_dirty = 0;
        int idx = win_find_type(TYPE_EDITOR);
        mouse_cursor_hide(); win_render(idx); mouse_refresh_cursor();
    } else if(blink) {
        mouse_cursor_hide(); editor_draw_caret(); mouse_refresh_cursor();
    }
}

void desktop_editor_key(char c, int special) {
    if(!desktop_editor_active()) return;
    if(special == 1) {
        if(editor_field == 0 && !editor_existing && editor_name_len > 0) editor_name[--editor_name_len] = 0;
        if(editor_field == 1 && editor_text_len > 0) editor_text[--editor_text_len] = 0;
    } else if(special == 2) {
        if(editor_field == 0) editor_field = 1;
        else if(!editor_directory && editor_text_len < (int)sizeof(editor_text)-1) {
            int col, row;
            editor_cursor_position(&col, &row);
            if(row >= 17) return;
            editor_text[editor_text_len++] = '\n';
            editor_text[editor_text_len] = 0;
        }
    } else if(c >= 32 && c < 127) {
        if(editor_field == 0 && !editor_existing && editor_name_len < FS_NAME_LEN-1) {
            editor_name[editor_name_len++] = c; editor_name[editor_name_len] = 0;
        } else if(editor_field == 1 && editor_text_len < (int)sizeof(editor_text)-1) {
            int col, row;
            editor_cursor_position(&col, &row);
            if(col >= 75 && row >= 17) return;
            editor_text[editor_text_len++] = c; editor_text[editor_text_len] = 0;
        }
    }
    editor_cursor_visible = 1;
    editor_last_blink_tick = ticks;
    editor_dirty = 1;
}

// Von kernel_main einmal pro Sekunde aufgerufen: liest die RTC, aktualisiert
// den Uhr-Cache und malt NUR die Taskleiste + ein evtl. offenes (nicht
// verdecktes) Uhr-Fenster neu - kein Komplett-Redraw mehr (das war der
// Grund fuer das sekuendliche "Blinken").
void desktop_tick(void) {
    unsigned char s,m,h,d,mo,y, s2,m2,h2,d2,mo2,y2;
    do {
        read_rtc_raw(&h,&m,&s,&d,&mo,&y);
        read_rtc_raw(&h2,&m2,&s2,&d2,&mo2,&y2);
    } while(h!=h2 || m!=m2 || s!=s2 || d!=d2 || mo!=mo2 || y!=y2);

    unsigned char regB = cmos_read(0x0B);
    if(!(regB & 0x04)) {
        s = bcd_to_bin(s); m = bcd_to_bin(m);
        h = bcd_to_bin(h & 0x7F) | (h & 0x80);
    }
    if(!(regB & 0x02) && (h & 0x80)) h = ((h & 0x7F) + 12) % 24;
    else h &= 0x7F;

    clock_cache[0]='0'+h/10; clock_cache[1]='0'+h%10; clock_cache[2]=':';
    clock_cache[3]='0'+m/10; clock_cache[4]='0'+m%10; clock_cache[5]=':';
    clock_cache[6]='0'+s/10; clock_cache[7]='0'+s%10; clock_cache[8]=0;

    if(!desktop_active || drag_idx >= 0) return;

    // BUGFIX: das war die Hauptquelle des "Einbrennens" - dieser Tick laeuft
    // jede Sekunde, egal ob die Maus sich bewegt hat oder nicht. Ohne den
    // Hide-Aufruf hat mouse_refresh_cursor() weiter unten regelmaessig den
    // noch dort stehenden Cursor-Pfeil selbst als "Hintergrund" gesichert.
    mouse_cursor_hide();
    draw_taskbar();
    redraw_windows_overlapping(0, VESA_HEIGHT-TASKBAR_H, VESA_WIDTH, TASKBAR_H, -1);
    int ci = win_find_type(TYPE_CLOCK);
    if(ci >= 0 && !win_occluded(ci)) win_render(ci);
    int ti = win_find_type(TYPE_TASKS);
    if(ti >= 0 && !win_occluded(ti)) win_render(ti);
    mouse_refresh_cursor();
}
