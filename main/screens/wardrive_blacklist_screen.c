/**
 * @file wardrive_blacklist_screen.c
 * @brief Wardrive 2.0 blacklist editor.
 *
 * Backed by the board commands (firmware unchanged):
 *   wardrive_blacklist list            -> current entries (one MAC per line)
 *   wardrive_blacklist add <MAC>
 *   wardrive_blacklist remove <MAC>
 *   wardrive_blacklist clear
 * The board is the source of truth; we reload the list after every edit.
 */

#include "wardrive_blacklist_screen.h"
#include "wardrive_config.h"
#include "text_input_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

static const char *TAG = "WD_BLACKLIST";

#define VISIBLE_ROWS    6
#define LOAD_TIMEOUT_TICKS 30   // on_tick cadence; ~ a couple seconds

typedef struct {
    wardrive_blacklist_t bl;
    int  selected_index;
    int  scroll_offset;
    bool loading;
    int  load_ticks;
    bool needs_redraw;
    screen_t *self;
} wd_blacklist_data_t;

static void draw_screen(screen_t *self);

// Find a 17-char "XX:XX:XX:XX:XX:XX" MAC anywhere in the line; copy to out.
static bool extract_mac(const char *line, char *out, size_t out_len)
{
    for (const char *p = line; *p; p++) {
        bool ok = true;
        for (int i = 0; i < 17; i++) {
            char c = p[i];
            if (i == 2 || i == 5 || i == 8 || i == 11 || i == 14) {
                if (c != ':') { ok = false; break; }
            } else if (!isxdigit((unsigned char)c)) {
                ok = false; break;
            }
        }
        if (ok) {
            size_t n = (out_len < 18) ? out_len - 1 : 17;
            memcpy(out, p, n);
            out[n] = '\0';
            return true;
        }
        if (!p[1]) break;
    }
    return false;
}

static void reload_list(wd_blacklist_data_t *data)
{
    wardrive_blacklist_reset(&data->bl);
    data->selected_index = 0;
    data->scroll_offset = 0;
    data->loading = true;
    data->load_ticks = 0;
    data->needs_redraw = true;
    uart_send_command("wardrive_blacklist list");
}

static void uart_line_callback(const char *line, void *user_data)
{
    wd_blacklist_data_t *data = (wd_blacklist_data_t *)user_data;
    if (!data || !line || !line[0]) return;

    // Skip the command echo and the prompt
    if (strstr(line, "wardrive_blacklist")) return;
    if (line[0] == '>' ) { data->loading = false; data->needs_redraw = true; return; }

    char mac[WARDRIVE_MAC_LEN];
    if (extract_mac(line, mac, sizeof(mac))) {
        if (wardrive_blacklist_add(&data->bl, mac)) {
            data->loading = false;
            data->needs_redraw = true;
        }
    }
}

static void draw_screen(screen_t *self)
{
    wd_blacklist_data_t *data = (wd_blacklist_data_t *)self->user_data;

    ui_clear();
    ui_draw_title("WD Blacklist");

    if (data->loading) {
        ui_print_center(3, "Loading...", UI_COLOR_DIMMED);
    } else if (data->bl.count == 0) {
        ui_print_center(3, "No entries", UI_COLOR_DIMMED);
    } else {
        int visible_end = data->scroll_offset + VISIBLE_ROWS;
        if (visible_end > data->bl.count) visible_end = data->bl.count;
        for (int i = data->scroll_offset; i < visible_end; i++) {
            int row = (i - data->scroll_offset) + 1;
            char label[31];
            snprintf(label, sizeof(label), "%s", data->bl.macs[i]);
            ui_draw_menu_item(row, label, i == data->selected_index, false, false);
        }
        if (data->scroll_offset > 0) {
            ui_print(UI_COLS - 2, 1, "^", UI_COLOR_DIMMED);
        }
        if (data->scroll_offset + VISIBLE_ROWS < data->bl.count) {
            ui_print(UI_COLS - 2, VISIBLE_ROWS, "v", UI_COLOR_DIMMED);
        }
    }

    ui_draw_status("A:Add D:Del C:Clr ESC");
}

static void redraw_entry_row(wd_blacklist_data_t *data, int idx)
{
    int row = (idx - data->scroll_offset) + 1;
    if (row < 1 || row > VISIBLE_ROWS || idx >= data->bl.count) return;
    char label[31];
    snprintf(label, sizeof(label), "%s", data->bl.macs[idx]);
    ui_draw_menu_item(row, label, idx == data->selected_index, false, false);
}

// Redraw only the list rows + scroll arrows (title/status untouched).
static void redraw_content(wd_blacklist_data_t *data)
{
    for (int r = 0; r < VISIBLE_ROWS; r++) {
        int idx = data->scroll_offset + r;
        int row = r + 1;
        if (idx < data->bl.count) {
            char label[31];
            snprintf(label, sizeof(label), "%s", data->bl.macs[idx]);
            ui_draw_menu_item(row, label, idx == data->selected_index, false, false);
        } else {
            display_fill_rect(0, row * 16, DISPLAY_WIDTH, 16, UI_COLOR_BG);
        }
    }
    if (data->scroll_offset > 0) {
        ui_print(UI_COLS - 2, 1, "^", UI_COLOR_DIMMED);
    }
    if (data->scroll_offset + VISIBLE_ROWS < data->bl.count) {
        ui_print(UI_COLS - 2, VISIBLE_ROWS, "v", UI_COLOR_DIMMED);
    }
}

static void on_tick(screen_t *self)
{
    wd_blacklist_data_t *data = (wd_blacklist_data_t *)self->user_data;
    if (!data) return;

    if (data->loading) {
        data->load_ticks++;
        if (data->load_ticks >= LOAD_TIMEOUT_TICKS) {
            data->loading = false;     // no (more) entries arrived
            data->needs_redraw = true;
        }
    }
    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

static void on_mac_added(const char *text, void *user_data)
{
    (void)user_data;
    char mac[WARDRIVE_MAC_LEN];
    // Accept whatever the user typed; trim to a valid MAC if present.
    if (!extract_mac(text, mac, sizeof(mac))) {
        snprintf(mac, sizeof(mac), "%.17s", text);
    }
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "wardrive_blacklist add %s", mac);
    uart_send_command(cmd);
    screen_manager_pop();   // back to the list; on_resume reloads
}

static void on_key(screen_t *self, key_code_t key)
{
    wd_blacklist_data_t *data = (wd_blacklist_data_t *)self->user_data;
    if (!data) return;

    switch (key) {
        case KEY_UP:
        case KEY_DOWN:
            if (data->bl.count > 0) {
                int prev = data->selected_index;
                int old_scroll = data->scroll_offset;
                if (key == KEY_UP)
                    data->selected_index = (data->selected_index > 0)
                        ? data->selected_index - 1 : data->bl.count - 1;
                else
                    data->selected_index = (data->selected_index < data->bl.count - 1)
                        ? data->selected_index + 1 : 0;
                data->scroll_offset = (data->selected_index / VISIBLE_ROWS) * VISIBLE_ROWS;
                if (data->scroll_offset != old_scroll) {
                    redraw_content(data);           // page changed: content only
                } else {
                    redraw_entry_row(data, prev);   // partial: deselect + select
                    redraw_entry_row(data, data->selected_index);
                }
            }
            break;
        case KEY_A: {
            text_input_params_t *p = calloc(1, sizeof(text_input_params_t));
            if (p) {
                p->title = "Add MAC";
                p->hint = "AA:BB:CC:DD:EE:FF";
                p->on_submit = on_mac_added;
                p->user_data = NULL;
                p->allow_empty = false;
                screen_manager_push(text_input_screen_create, p);
            }
            break;
        }
        case KEY_D:
            if (data->bl.count > 0 && data->selected_index < data->bl.count) {
                char cmd[48];
                snprintf(cmd, sizeof(cmd), "wardrive_blacklist remove %s",
                         data->bl.macs[data->selected_index]);
                uart_send_command(cmd);
                reload_list(data);
                draw_screen(self);
            }
            break;
        case KEY_C:
            uart_send_command("wardrive_blacklist clear");
            reload_list(data);
            draw_screen(self);
            break;
        case KEY_ESC:
        case KEY_Q:
        case KEY_BACKSPACE:
            screen_manager_pop();
            break;
        default:
            break;
    }
}

static void on_resume(screen_t *self)
{
    wd_blacklist_data_t *data = (wd_blacklist_data_t *)self->user_data;
    if (!data) return;
    uart_register_line_callback(uart_line_callback, data);
    reload_list(data);
    draw_screen(self);
}

static void on_destroy(screen_t *self)
{
    uart_clear_line_callback();
    if (self->user_data) free(self->user_data);
}

screen_t* wardrive_blacklist_screen_create(void *params)
{
    (void)params;
    ESP_LOGI(TAG, "Creating wardrive blacklist screen...");

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    wd_blacklist_data_t *data = calloc(1, sizeof(wd_blacklist_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }
    data->self = screen;

    screen->user_data = data;
    screen->on_draw = draw_screen;
    screen->on_key = on_key;
    screen->on_tick = on_tick;
    screen->on_resume = on_resume;
    screen->on_destroy = on_destroy;

    uart_register_line_callback(uart_line_callback, data);
    reload_list(data);
    draw_screen(screen);

    ESP_LOGI(TAG, "Wardrive blacklist screen created");
    return screen;
}
