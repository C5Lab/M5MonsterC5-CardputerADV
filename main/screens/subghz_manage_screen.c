/**
 * @file subghz_manage_screen.c
 * @brief Sub-GHz Manage screen
 *
 * UART behaviour cloned from coreS3/main/screens/subghz_manage_screen.c:
 *   open:                send "subghz_list", accumulate [SUBGHZ_LIST] until [SUBGHZ_LIST_END]
 *   E:                   send "subghz_export all"
 *   I:                   send "subghz_import all", count [SUBGHZ_IMPORT] up to [SUBGHZ_IMPORT_END],
 *                        then auto refresh via subghz_list
 *   ENTER + Yes:         send "subghz_delete <idx>", refresh list
 *   X + Yes:             send "subghz_clear", empty local list
 *
 * Cardputer-only: inline line collection (no port of uart_start_collect),
 * the line callback accumulates entries from each command into a single
 * line_callback. Same idiom we use in subghz_transmit_screen.c.
 */

#include "subghz_manage_screen.h"
#include "subghz_parser.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "SUBGHZ_MGM";

#define VISIBLE_ITEMS       5
#define SUBGHZ_MAX_SIGNALS  128

typedef struct {
    int   idx;
    char  type[32];
    float freq;
    char  serial[32];
    int   btn;
    int   cnt;
    char  mf[32];
    bool  is_raw;
} mgmt_signal_t;

typedef enum {
    MGM_VIEW_LOADING = 0,
    MGM_VIEW_LIST,
    MGM_VIEW_CONFIRM_DELETE,
    MGM_VIEW_CONFIRM_CLEAR,
    MGM_VIEW_IMPORTING,
} mgm_view_t;

typedef struct {
    mgmt_signal_t sigs[SUBGHZ_MAX_SIGNALS];
    int  sig_count;

    mgm_view_t view;
    int  selected_index;
    int  scroll_offset;
    int  confirm_choice;       /* 0 = Yes, 1 = No */

    /* Async collection state - mirrors what uart_start_collect did on coreS3. */
    bool collecting_list;
    bool collecting_import;
    int  imported_count;

    /* One-line transient status (e.g. "Export sent", "Deleted #4"). */
    char status_text[32];
    uint16_t status_color;
    bool status_visible;

    bool needs_redraw;
    SemaphoreHandle_t sig_mtx;
    screen_t *self;
} subghz_mgm_data_t;

static subghz_mgm_data_t *s_current = NULL;

static void draw_screen(screen_t *self);
static void draw_list_view(screen_t *self);
static void draw_confirm_view(screen_t *self);
static void redraw_list_window(subghz_mgm_data_t *data);
static void redraw_signal_row(subghz_mgm_data_t *data, int sig_idx);
static void request_list_refresh(subghz_mgm_data_t *data);

static void fill_signal(mgmt_signal_t *dst, const subghz_signal_info_t *src)
{
    memset(dst, 0, sizeof(*dst));
    dst->idx = src->idx;
    dst->freq = src->freq;
    dst->btn = src->btn;
    dst->cnt = src->cnt;
    dst->is_raw = src->is_raw;
    snprintf(dst->type, sizeof(dst->type), "%s", src->type[0] ? src->type : "--");
    snprintf(dst->serial, sizeof(dst->serial), "%s", src->serial[0] ? src->serial : "--");
    snprintf(dst->mf, sizeof(dst->mf), "%s", src->mf[0] ? src->mf : "--");
}

/* Re-running subghz_import re-adds every .sub file on the SD card, so the
 * firmware-side list grows with each import. Hide byte-for-byte duplicates
 * (same type/freq/serial/btn/mf) from the UI; storage on JanOS is left intact
 * until the user picks Clear All. Same predicate as coreS3 version. */
static bool is_duplicate_of_existing_locked(const subghz_mgm_data_t *data,
                                            const subghz_signal_info_t *p)
{
    int freq_x100 = (int)(p->freq * 100.0f + 0.5f);
    const char *p_type   = p->type[0]   ? p->type   : "--";
    const char *p_serial = p->serial[0] ? p->serial : "--";
    const char *p_mf     = p->mf[0]     ? p->mf     : "--";

    for (int j = 0; j < data->sig_count; j++) {
        const mgmt_signal_t *e = &data->sigs[j];
        int e_freq_x100 = (int)(e->freq * 100.0f + 0.5f);
        if (e_freq_x100 != freq_x100) continue;
        if (e->btn != p->btn) continue;
        if (strcmp(e->type, p_type) != 0) continue;
        if (strcmp(e->serial, p_serial) != 0) continue;
        if (strcmp(e->mf, p_mf) != 0) continue;
        return true;
    }
    return false;
}

static void append_signal_locked(subghz_mgm_data_t *data,
                                 const subghz_signal_info_t *src)
{
    if (data->sig_count >= SUBGHZ_MAX_SIGNALS) {
        ESP_LOGW(TAG, "Manage list cap reached at %d, dropping rest", data->sig_count);
        return;
    }
    if (src->idx <= 0 && !src->is_raw) return;
    if (is_duplicate_of_existing_locked(data, src)) return;
    fill_signal(&data->sigs[data->sig_count++], src);
}

static void set_status(subghz_mgm_data_t *data, const char *text, uint16_t color)
{
    snprintf(data->status_text, sizeof(data->status_text), "%s", text ? text : "");
    data->status_color = color;
    data->status_visible = (text != NULL && text[0] != '\0');
    data->needs_redraw = true;
}

static void uart_line_cb(const char *line, void *user_data)
{
    subghz_mgm_data_t *data = (subghz_mgm_data_t *)user_data;
    if (!data || !line) return;

    if (data->collecting_import) {
        if (strstr(line, "[SUBGHZ_IMPORT_END]")) {
            data->collecting_import = false;
            ESP_LOGI(TAG, "Import done: %d signals", data->imported_count);
            /* Chain a refresh by reusing the same callback. */
            request_list_refresh(data);
            return;
        }
        if (strstr(line, "[SUBGHZ_IMPORT] ")) {
            data->imported_count++;
        }
        return;
    }

    if (strstr(line, "[SUBGHZ_LIST_END]")) {
        data->collecting_list = false;
        if (data->view == MGM_VIEW_LOADING) {
            data->view = MGM_VIEW_LIST;
        }
        data->needs_redraw = true;
        return;
    }

    if (!data->collecting_list) return;

    subghz_signal_info_t parsed;
    if (!subghz_parse_signal_line(line, &parsed)) return;
    if (parsed.kind != SUBGHZ_SIGNAL_KIND_LIST) return;

    xSemaphoreTake(data->sig_mtx, portMAX_DELAY);
    append_signal_locked(data, &parsed);
    xSemaphoreGive(data->sig_mtx);
    data->needs_redraw = true;
}

static void request_list_refresh(subghz_mgm_data_t *data)
{
    xSemaphoreTake(data->sig_mtx, portMAX_DELAY);
    data->sig_count = 0;
    data->selected_index = 0;
    data->scroll_offset = 0;
    xSemaphoreGive(data->sig_mtx);

    data->collecting_list = true;
    data->collecting_import = false;
    uart_send_command("subghz_list");
}

static void format_row(const mgmt_signal_t *sig, char *out, size_t n)
{
    int whole = (int)sig->freq;
    int frac  = ((int)(sig->freq * 100.0f + 0.5f)) % 100;
    if (sig->is_raw) {
        snprintf(out, n, "%d RAW %d.%02d %.10s",
                 sig->idx, whole, frac, sig->mf);
    } else {
        const char *info = (sig->mf[0] && strcmp(sig->mf, "--") != 0)
                            ? sig->mf : sig->serial;
        snprintf(out, n, "%d %.6s %d.%02d %.10s",
                 sig->idx, sig->type, whole, frac, info);
    }
}

static void redraw_signal_row(subghz_mgm_data_t *data, int sig_idx)
{
    int row_on_screen = sig_idx - data->scroll_offset;
    if (row_on_screen < 0 || row_on_screen >= VISIBLE_ITEMS) return;
    int ui_row = 1 + row_on_screen;
    int y = ui_row * 16;
    display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
    if (sig_idx >= data->sig_count) return;

    char buf[64];
    format_row(&data->sigs[sig_idx], buf, sizeof(buf));
    bool selected = (sig_idx == data->selected_index);
    ui_draw_menu_item(ui_row, buf, selected, false, false);
}

static void redraw_list_window(subghz_mgm_data_t *data)
{
    for (int i = 0; i < VISIBLE_ITEMS; i++) {
        int sig_idx = data->scroll_offset + i;
        int ui_row = 1 + i;
        int y = ui_row * 16;
        display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
        if (sig_idx < data->sig_count) {
            char buf[64];
            format_row(&data->sigs[sig_idx], buf, sizeof(buf));
            bool selected = (sig_idx == data->selected_index);
            ui_draw_menu_item(ui_row, buf, selected, false, false);
        }
    }

    display_fill_rect(DISPLAY_WIDTH - 16, 1 * 16, 16, 16, UI_COLOR_BG);
    display_fill_rect(DISPLAY_WIDTH - 16, (1 + VISIBLE_ITEMS - 1) * 16, 16, 16, UI_COLOR_BG);
    if (data->scroll_offset > 0)
        ui_print(UI_COLS - 2, 1, "^", UI_COLOR_DIMMED);
    if (data->scroll_offset + VISIBLE_ITEMS < data->sig_count)
        ui_print(UI_COLS - 2, 1 + VISIBLE_ITEMS - 1, "v", UI_COLOR_DIMMED);
}

static void draw_list_view(screen_t *self)
{
    subghz_mgm_data_t *data = (subghz_mgm_data_t *)self->user_data;

    ui_clear();
    char title[24];
    snprintf(title, sizeof(title), "Manage (%d)", data->sig_count);
    ui_draw_title(title);

    if (data->view == MGM_VIEW_IMPORTING) {
        ui_print_center(3, "Importing from SD...", UI_COLOR_HIGHLIGHT);
    } else if (data->sig_count == 0) {
        ui_print_center(3, data->view == MGM_VIEW_LOADING
                              ? "Loading..."
                              : "No stored signals", UI_COLOR_DIMMED);
    } else {
        redraw_list_window(data);
    }

    if (data->status_visible) {
        ui_print(0, 6, data->status_text, data->status_color);
    }

    ui_draw_status("ENT:Del E:Exp I:Imp X:Clr");
}

static void draw_confirm_view(screen_t *self)
{
    subghz_mgm_data_t *data = (subghz_mgm_data_t *)self->user_data;
    ui_clear();
    ui_draw_title(data->view == MGM_VIEW_CONFIRM_DELETE ? "Delete signal?" : "Clear ALL?");

    if (data->view == MGM_VIEW_CONFIRM_DELETE &&
        data->selected_index < data->sig_count) {
        const mgmt_signal_t *sig = &data->sigs[data->selected_index];
        char l1[40], l2[40];
        int whole = (int)sig->freq;
        int frac  = ((int)(sig->freq * 100.0f + 0.5f)) % 100;
        snprintf(l1, sizeof(l1), "#%d %.10s %d.%02d", sig->idx, sig->type, whole, frac);
        snprintf(l2, sizeof(l2), "%.14s %.14s",
                 sig->mf[0] ? sig->mf : "--", sig->serial[0] ? sig->serial : "--");
        ui_print_center(2, l1, UI_COLOR_HIGHLIGHT);
        ui_print_center(3, l2, UI_COLOR_DIMMED);
    } else if (data->view == MGM_VIEW_CONFIRM_CLEAR) {
        ui_print_center(2, "Delete ALL stored", UI_COLOR_HIGHLIGHT);
        ui_print_center(3, "signals?", UI_COLOR_HIGHLIGHT);
    }

    ui_draw_menu_item(5, "Yes",    data->confirm_choice == 0, false, false);
    ui_draw_menu_item(6, "Cancel", data->confirm_choice == 1, false, false);

    ui_draw_status("UP/DN ENTER:Confirm ESC:Cancel");
}

static void draw_screen(screen_t *self)
{
    subghz_mgm_data_t *data = (subghz_mgm_data_t *)self->user_data;
    switch (data->view) {
        case MGM_VIEW_LOADING:
        case MGM_VIEW_LIST:
        case MGM_VIEW_IMPORTING:
            draw_list_view(self);
            break;
        case MGM_VIEW_CONFIRM_DELETE:
        case MGM_VIEW_CONFIRM_CLEAR:
            draw_confirm_view(self);
            break;
    }
}

static void on_tick(screen_t *self)
{
    subghz_mgm_data_t *data = (subghz_mgm_data_t *)self->user_data;
    if (!data->needs_redraw) return;
    data->needs_redraw = false;

    if (data->view == MGM_VIEW_IMPORTING && !data->collecting_import) {
        /* Import finished - request_list_refresh moved us back to LOADING. */
        data->view = MGM_VIEW_LOADING;
        char buf[32];
        snprintf(buf, sizeof(buf), "Imported %d",
                 data->imported_count);
        set_status(data, buf, UI_COLOR_HIGHLIGHT);
    }

    if (data->view != MGM_VIEW_CONFIRM_DELETE &&
        data->view != MGM_VIEW_CONFIRM_CLEAR) {
        draw_list_view(self);
    }
}

static void redraw_two_rows(subghz_mgm_data_t *data, int old_idx, int new_idx)
{
    if (old_idx >= 0) redraw_signal_row(data, old_idx);
    if (new_idx >= 0) redraw_signal_row(data, new_idx);
}

static void perform_delete(screen_t *self)
{
    subghz_mgm_data_t *data = (subghz_mgm_data_t *)self->user_data;
    if (data->selected_index >= data->sig_count) return;
    int idx = data->sigs[data->selected_index].idx;

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "subghz_delete %d", idx);
    uart_send_command(cmd);
    ESP_LOGI(TAG, "Sent subghz_delete %d", idx);

    char status[32];
    snprintf(status, sizeof(status), "Deleted #%d", idx);
    set_status(data, status, UI_COLOR_TITLE);

    data->view = MGM_VIEW_LOADING;
    request_list_refresh(data);
    draw_screen(self);
}

static void perform_clear(screen_t *self)
{
    subghz_mgm_data_t *data = (subghz_mgm_data_t *)self->user_data;
    uart_send_command("subghz_clear");
    ESP_LOGI(TAG, "Sent subghz_clear");

    xSemaphoreTake(data->sig_mtx, portMAX_DELAY);
    data->sig_count = 0;
    data->selected_index = 0;
    data->scroll_offset = 0;
    xSemaphoreGive(data->sig_mtx);

    set_status(data, "All cleared", UI_COLOR_HIGHLIGHT);
    data->view = MGM_VIEW_LIST;
    draw_screen(self);
}

static void on_key_list(screen_t *self, key_code_t key)
{
    subghz_mgm_data_t *data = (subghz_mgm_data_t *)self->user_data;

    switch (key) {
        case KEY_UP:
            if (data->sig_count == 0) break;
            if (data->selected_index > 0) {
                int old = data->selected_index;
                int new_idx = old - 1;
                if (new_idx < data->scroll_offset) {
                    data->scroll_offset = new_idx;
                    data->selected_index = new_idx;
                    redraw_list_window(data);
                } else {
                    data->selected_index = new_idx;
                    redraw_two_rows(data, old, new_idx);
                }
            }
            break;

        case KEY_DOWN:
            if (data->sig_count == 0) break;
            if (data->selected_index < data->sig_count - 1) {
                int old = data->selected_index;
                int new_idx = old + 1;
                if (new_idx >= data->scroll_offset + VISIBLE_ITEMS) {
                    data->scroll_offset = new_idx - VISIBLE_ITEMS + 1;
                    data->selected_index = new_idx;
                    redraw_list_window(data);
                } else {
                    data->selected_index = new_idx;
                    redraw_two_rows(data, old, new_idx);
                }
            }
            break;

        case KEY_ENTER:
            if (data->sig_count == 0) break;
            data->confirm_choice = 1; /* Default to Cancel - delete is destructive */
            data->view = MGM_VIEW_CONFIRM_DELETE;
            draw_confirm_view(self);
            break;

        case KEY_E:
            uart_send_command("subghz_export all");
            ESP_LOGI(TAG, "Sent subghz_export all");
            set_status(data, "Export sent", UI_COLOR_TITLE);
            draw_list_view(self);
            break;

        case KEY_I:
            if (data->collecting_import) break;
            data->imported_count = 0;
            data->collecting_import = true;
            data->collecting_list = false;
            data->view = MGM_VIEW_IMPORTING;
            uart_send_command("subghz_import all");
            ESP_LOGI(TAG, "Sent subghz_import all");
            set_status(data, "Importing...", UI_COLOR_HIGHLIGHT);
            draw_list_view(self);
            break;

        case KEY_X:
            if (data->sig_count == 0) break;
            data->confirm_choice = 1; /* Default to Cancel */
            data->view = MGM_VIEW_CONFIRM_CLEAR;
            draw_confirm_view(self);
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

static void on_key_confirm(screen_t *self, key_code_t key)
{
    subghz_mgm_data_t *data = (subghz_mgm_data_t *)self->user_data;

    switch (key) {
        case KEY_UP:
        case KEY_DOWN:
            data->confirm_choice = data->confirm_choice ? 0 : 1;
            draw_confirm_view(self);
            break;

        case KEY_ENTER:
        case KEY_SPACE:
            if (data->confirm_choice == 0) {
                if (data->view == MGM_VIEW_CONFIRM_DELETE) perform_delete(self);
                else                                       perform_clear(self);
            } else {
                data->view = MGM_VIEW_LIST;
                draw_list_view(self);
            }
            break;

        case KEY_ESC:
        case KEY_BACKSPACE:
        case KEY_Q:
            data->view = MGM_VIEW_LIST;
            draw_list_view(self);
            break;

        default:
            break;
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    subghz_mgm_data_t *data = (subghz_mgm_data_t *)self->user_data;
    if (data->view == MGM_VIEW_CONFIRM_DELETE ||
        data->view == MGM_VIEW_CONFIRM_CLEAR) {
        on_key_confirm(self, key);
    } else {
        on_key_list(self, key);
    }
}

static void on_destroy(screen_t *self)
{
    subghz_mgm_data_t *data = (subghz_mgm_data_t *)self->user_data;
    if (data) {
        uart_clear_line_callback();
        if (data->sig_mtx) {
            vSemaphoreDelete(data->sig_mtx);
            data->sig_mtx = NULL;
        }
        if (s_current == data) s_current = NULL;
        free(data);
        self->user_data = NULL;
    }
}

static void on_resume(screen_t *self)
{
    draw_screen(self);
}

screen_t* subghz_manage_screen_create(void *params)
{
    (void)params;

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    subghz_mgm_data_t *data = calloc(1, sizeof(subghz_mgm_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }

    data->sig_mtx = xSemaphoreCreateMutex();
    data->view = MGM_VIEW_LOADING;
    data->collecting_list = true;
    data->self = screen;
    s_current = data;

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;

    draw_screen(screen);

    /* Defensive: stop any prior subghz_rx/jam/etc. and request the list. */
    uart_send_command("subghz_stop");
    uart_register_line_callback(uart_line_cb, data);
    uart_send_command("subghz_list");

    ESP_LOGI(TAG, "Manage screen created");
    return screen;
}
