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
#define ICON_COUNT 5
static const char* icon_labels[ICON_COUNT] = {"Terminal", "Info", "Uhr", "Optionen", "Vollbild"};
static unsigned int icon_colors[ICON_COUNT] = {0x3399FF, 0x55FF55, 0xFFAA00, 0xAA55FF, 0xFF4444};
#define ICON_W 100
#define ICON_H 80
#define ICON_GAP 30
#define ICON_TOP 40
#define TASKBAR_H 36

static int icon_x(int i) { return ICON_GAP + i*(ICON_W+ICON_GAP); }

/* ---------- Fenster ---------- */
#define TYPE_TERMINAL 0
#define TYPE_INFO     1
#define TYPE_CLOCK    2
#define TYPE_SETTINGS 3

#define MAX_WINDOWS 4
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
static int drag_preview_active = 0;
static int drag_preview_x, drag_preview_y;
#define DRAG_PREVIEW_MAX 3000
static unsigned int drag_preview_backup[DRAG_PREVIEW_MAX];

static const char* win_titles[4] = {"Terminal", "Info", "Uhr", "Einstellungen"};
static unsigned int theme_colors[6] = {0x00FFAA,0x3399FF,0xFF4444,0x55FF55,0xAA55FF,0xFFAA00};

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
static void drag_preview_restore(void);
static void drag_preview_draw(int x, int y, int w, int h);

/* ---------- Hintergrund / Taskleiste / Icons ---------- */

static unsigned int wallpaper_color_at(int y) {
    unsigned char shade = (unsigned char)(0x10 + (y * 0x18) / VESA_HEIGHT);
    return ((unsigned int)shade << 16) | ((unsigned int)shade << 9) | (shade + 0x10);
}

static void draw_wallpaper(void) {
    for(int y = 0; y < VESA_HEIGHT; y++) draw_rect(0, y, VESA_WIDTH, 1, wallpaper_color_at(y));
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
    }
}

static void draw_icon(int i) {
    int x = icon_x(i), y = ICON_TOP;
    draw_rect(x, y, ICON_W, ICON_H-24, 0x222222);
    draw_rect(x, y, ICON_W, 3, icon_colors[i]);
    draw_rect(x, y, 3, ICON_H-24, icon_colors[i]);
    draw_rect(x+ICON_W-3, y, 3, ICON_H-24, icon_colors[i]);
    draw_rect(x, y+ICON_H-27, ICON_W, 3, icon_colors[i]);
    draw_icon_glyph(i, x+ICON_W/2, y+(ICON_H-24)/2);
    int len = 0; while(icon_labels[i][len]) len++;
    int lx = x + (ICON_W - len*8)/2;
    for(int c = 0; c < len; c++)
        draw_char(lx+c*8, y+ICON_H-18, icon_labels[i][c], 0xFFFFFF, 0x000000);
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
        case TYPE_SETTINGS:  windows[idx].x=300; windows[idx].y=220; windows[idx].w=420; windows[idx].h=180; break;
    }
    zorder[zcount++] = idx;

    if(type == TYPE_TERMINAL) {
        window_t* w = &windows[idx];
        terminal_windowed = 1;
        vga_set_region(w->x+16, w->y+28, w->w-32, w->h-44);
        vga_clear_region();
        vga_print("GoonerOS - Terminal im Fenster. Titelleiste zum Verschieben,\nX zum Schliessen.\n> ");
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

static void win_render(int idx) {
    window_t* w = &windows[idx];

    // Einfacher Schlagschatten (SHADOW_OFF Pixel unten/rechts sichtbar lassen)
    draw_rect(w->x+SHADOW_OFF, w->y+SHADOW_OFF, w->w, w->h, 0x0A0A0A);
    draw_window(w->x, w->y, w->w, w->h, win_titles[w->type]);
    draw_close_button(w);

    if(w->type == TYPE_TERMINAL) {
        vga_set_region(w->x+16, w->y+28, w->w-32, w->h-44);
        redraw_text_buffer();
    }

    else if(w->type == TYPE_INFO) {
        const char* lines[] = {
            "GoonerOS Desktop",
            "",
            "Fenster per Titelleiste verschiebbar,",
            "per X oben rechts schliessbar, per",
            "Klick ins Fenster nach vorne holbar.",
            "",
            "Icons: Terminal, Info, Uhr,",
            "Optionen (Theme-Farbe), Vollbild.",
            "",
            "Kein echtes Multitasking - Fenster",
            "sind trotzdem unabhaengig positionierbar."
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
        const char* label = "Theme-Farbe waehlen:";
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

    }
}

static void screen_copy_rect(int src_x, int src_y, int dst_x, int dst_y, int w, int h) {
    if(w <= 0 || h <= 0) return;
    if(src_x < 0) { w += src_x; src_x = 0; }
    if(src_y < 0) { h += src_y; src_y = 0; }
    if(dst_x < 0) { w += dst_x; dst_x = 0; }
    if(dst_y < 0) { h += dst_y; dst_y = 0; }
    if(src_x + w > VESA_WIDTH) w = VESA_WIDTH - src_x;
    if(src_y + h > VESA_HEIGHT) h = VESA_HEIGHT - src_y;
    if(dst_x + w > VESA_WIDTH) w = VESA_WIDTH - dst_x;
    if(dst_y + h > VESA_HEIGHT) h = VESA_HEIGHT - dst_y;
    if(w <= 0 || h <= 0) return;

    if(dst_x >= src_x && dst_y >= src_y) {
        for(int y = 0; y < h; y++)
            for(int x = 0; x < w; x++)
                put_pixel(dst_x + x, dst_y + y, get_pixel(src_x + x, src_y + y));
    } else {
        for(int y = h - 1; y >= 0; y--)
            for(int x = w - 1; x >= 0; x--)
                put_pixel(dst_x + x, dst_y + y, get_pixel(src_x + x, src_y + y));
    }
}

static void drag_preview_restore(void) {
    drag_preview_active = 0;
}

static void drag_preview_draw(int x, int y, int w, int h) {
    (void)w; (void)h;
    drag_preview_x = x;
    drag_preview_y = y;
    drag_preview_active = 1;
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

    for(int yy = y; yy < y+h; yy++) draw_rect(x, yy, w, 1, wallpaper_color_at(yy));

    if(y + h > VESA_HEIGHT - TASKBAR_H) draw_taskbar();
    for(int i = 0; i < ICON_COUNT; i++) {
        int ix = icon_x(i), iy = ICON_TOP;
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
    if(!desktop_active) { prev_left = left_down; drag_idx = -1; return; }

    int pressed = left_down && !prev_left;
    int released = !left_down && prev_left;
    prev_left = left_down;

    // ---- Aktives Ziehen eines Fensters ----
    if(drag_idx >= 0) {
        window_t* w = &windows[drag_idx];
        if(released || !left_down) {
            mouse_cursor_hide();
            drag_preview_restore();
            int old_x = w->x, old_y = w->y;
            int nx = mx - drag_off_x, ny = my - drag_off_y;
            if(nx < 0) nx = 0;
            if(ny < 0) ny = 0;
            if(nx + w->w > VESA_WIDTH) nx = VESA_WIDTH - w->w;
            if(ny + w->h > VESA_HEIGHT) ny = VESA_HEIGHT - w->h;
            w->x = nx; w->y = ny;
            if(w->type == TYPE_TERMINAL) {
                prompt_x += nx - old_x; prompt_y += ny - old_y;
                text_x += nx - old_x; text_y += ny - old_y;
                vga_set_region(w->x+16, w->y+28, w->w-32, w->h-44);
            }
            erase_rect_to_desktop(old_x, old_y, w->w+SHADOW_OFF, w->h+SHADOW_OFF);
            redraw_windows_overlapping(old_x, old_y, w->w+SHADOW_OFF, w->h+SHADOW_OFF, drag_idx);
            win_render(drag_idx);
            mouse_refresh_cursor();
            drag_idx = -1;
            return;
        }
        int nx = mx - drag_off_x;
        int ny = my - drag_off_y;
        if(nx < 0) nx = 0;
        if(ny < 0) ny = 0;
        if(nx + w->w > VESA_WIDTH) nx = VESA_WIDTH - w->w;
        if(ny + w->h > VESA_HEIGHT) ny = VESA_HEIGHT - w->h;

        if(nx != w->x || ny != w->y) {
            int old_x = w->x, old_y = w->y;
            mouse_cursor_hide();
            erase_rect_to_desktop(old_x, old_y, w->w + SHADOW_OFF, w->h + SHADOW_OFF);
            redraw_windows_overlapping(old_x, old_y, w->w + SHADOW_OFF, w->h + SHADOW_OFF, drag_idx);
            screen_copy_rect(old_x, old_y, nx, ny, w->w, w->h);
            redraw_windows_overlapping(nx, ny, w->w + SHADOW_OFF, w->h + SHADOW_OFF, drag_idx);
            w->x = nx; w->y = ny;
            if(w->type == TYPE_TERMINAL) {
                prompt_x += nx - old_x; prompt_y += ny - old_y;
                text_x += nx - old_x; text_y += ny - old_y;
                vga_set_region(w->x+16, w->y+28, w->w-32, w->h-44);
            }
            win_render(drag_idx);
            mouse_refresh_cursor();
        }
        return;
    }

    if(!pressed) return;

    // ---- Klick auf ein Fenster ----
    int idx = win_hit_test(mx, my);
    if(idx >= 0) {
        window_t* w = &windows[idx];
        int was_front = (zcount > 0 && zorder[zcount-1] == idx);
        win_bring_front(idx);

        if(win_hit_close(w, mx, my)) {
            int cx = w->x, cy = w->y, cw = w->w, ch = w->h;
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
            if(!was_front) { mouse_cursor_hide(); win_render(idx); mouse_refresh_cursor(); }
            mouse_cursor_hide();
            drag_preview_draw(w->x, w->y, w->w, w->h);
            mouse_refresh_cursor();
            return;
        }
        if(w->type == TYPE_SETTINGS) {
            for(int i = 0; i < 6; i++) {
                int sx, sy, sw, sh; settings_swatch_rect(w, i, &sx, &sy, &sw, &sh);
                if(mx >= sx && mx < sx+sw && my >= sy && my < sy+sh) {
                    ui_theme_color = theme_colors[i];
                    // Theme faerbt ALLE Titelleisten + die Taskleiste neu -
                    // dafuer ist ein Komplett-Redraw hier tatsaechlich richtig.
                    desktop_redraw_all();
                    return;
                }
            }
        }
        if(!was_front) { mouse_cursor_hide(); win_render(idx); mouse_refresh_cursor(); }
        return;
    }

    // ---- Kein Fenster getroffen -> vielleicht ein Icon ----
    for(int i = 0; i < ICON_COUNT; i++) {
        int ix = icon_x(i), iy = ICON_TOP;
        if(mx >= ix && mx < ix+ICON_W && my >= iy && my < iy+ICON_H) {
            int idx2 = -1;
            if(i == 0) idx2 = win_open(TYPE_TERMINAL);
            else if(i == 1) idx2 = win_open(TYPE_INFO);
            else if(i == 2) idx2 = win_open(TYPE_CLOCK);
            else if(i == 3) idx2 = win_open(TYPE_SETTINGS);
            else if(i == 4) { desktop_fullscreen(); return; }
            if(idx2 >= 0) { mouse_cursor_hide(); win_render(idx2); mouse_refresh_cursor(); }
            return;
        }
    }
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
    mouse_refresh_cursor();
}
