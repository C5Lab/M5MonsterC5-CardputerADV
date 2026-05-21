/**
 * @file subghz_manage_screen.c
 * @brief Sub-GHz "SD Signals" screen (SD library browser)
 *
 * UART behaviour:
 *   open / refresh:    "subghz_list sd"
 *                      accumulate [SUBGHZ_LIST] until [SUBGHZ_LIST_END] count=N source=sd
 *   ENTER + Rename:    push text_input_screen, then "subghz_rename <idx> <name>" + refresh
 *   ENTER + Delete:    confirm, then "subghz_delete <idx>" + refresh
 *   ENTER + Transmit:  "subghz_tx <idx> sd" (single shot)
 *   X + Yes:           iteratively "subghz_delete 1" until remaining=0 (firmware
 *                      subghz_clear only wipes mem; SD wipe must be done by the UI)
 *
 * Export / Import were removed - the SD library is the canonical store; mem-cached
 * captures from Hunter/Listen are promoted via per-item Save to SD.
 *
 * Repaint model: per-row dirty flags. on_tick never calls draw_list_view (which
 * does a ui_clear) — instead it dispatches title/status/window/rows individually.
 */

#include "subghz_manage_screen.h"
#include "subghz_parser.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "text_input_screen.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "SUBGHZ_SD";

#define VISIBLE_ITEMS       5
#define SUBGHZ_MAX_SIGNALS  128
#define STATUS_BUF_LEN      32

typedef struct {
    int   idx;
    char  type[32];
    float freq;
    char  serial[32];
    int   btn;
    int   cnt;
    char  mf[32];
    char  name[40];
    bool  is_raw;
} sd_signal_t;

typedef enum {
    SD_VIEW_LOADING = 0,
    SD_VIEW_LIST,
    SD_VIEW_ACTIONS,
    SD_VIEW_CONFIRM_DELETE,
    SD_VIEW_CONFIRM_CLEAR,
} sd_view_t;

typedef enum {
    SD_ACTION_RENAME = 0,
    SD_ACTION_DELETE,
    SD_ACTION_TRANSMIT,
    SD_ACTION_CANCEL,
    SD_ACTION_COUNT,
} sd_action_t;

typedef struct {
    sd_signal_t sigs[SUBGHZ_MAX_SIGNALS];
    int  sig_count;

    sd_view_t view;
    int  selected_index;
    int  scroll_offset;
    int  action_choice;
    int  confirm_choice;       /* 0 = Yes, 1 = Cancel */

    bool collecting_list;
    bool clearing_all;

    char status_text[STATUS_BUF_LEN];
    uint16_t status_color;
    bool status_visible;

    /* Per-row dirty model. */
    bool title_dirty;
    bool status_dirty;
    bool window_dirty;
    int  row_dirty_from;     /* -1 = none */
    int  row_dirty_to;
    bool empty_hint_drawn;
    int  last_title_count;   /* count last reflected in the title */

    SemaphoreHandle_t sig_mtx;
    screen_t *self;
} subghz_sd_data_t;

static subghz_sd_data_t *s_current = NULL;

static void draw_screen(screen_t *self);
static void draw_list_view(screen_t *self);
static void draw_actions_view(screen_t *self);
static void draw_confirm_view(screen_t *self);
static void redraw_title(subghz_sd_data_t *data);
static void redraw_status_line(subghz_sd_data_t *data);
static void redraw_list_window(subghz_sd_data_t *data);
static void redraw_signal_row(subghz_sd_data_t *data, int sig_idx);
static void redraw_empty_hint(subghz_sd_data_t *data);
static void request_list_refresh(subghz_sd_data_t *data);
static void uart_line_cb(const char *line, void *user_data);

static void mark_row_dirty(subghz_sd_data_t *d, int idx)
{
    if (idx < 0) return;
    if (d->row_dirty_from < 0 || idx < d->row_dirty_from) d->row_dirty_from = idx;
    if (idx > d->row_dirty_to) d->row_dirty_to = idx;
}

static void clear_row_dirty(subghz_sd_data_t *d)
{
    d->row_dirty_from = -1;
    d->row_dirty_to = -1;
}

static void fill_signal(sd_signal_t *dst, const subghz_signal_info_t *src)
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
    snprintf(dst->name, sizeof(dst->name), "%s", src->name[0] ? src->name : "");
}

/* Returns the row index of the new entry, or -1 if it was dropped. */
static int append_signal_locked(subghz_sd_data_t *data,
                                const subghz_signal_info_t *src)
{
    if (data->sig_count >= SUBGHZ_MAX_SIGNALS) {
        ESP_LOGW(TAG, "SD list cap reached at %d, dropping rest", data->sig_count);
        return -1;
    }
    if (src->idx <= 0 && !src->is_raw) return -1;
    int new_idx = data->sig_count;
    fill_signal(&data->sigs[data->sig_count++], src);
    return new_idx;
}

static void set_status(subghz_sd_data_t *data, const char *text, uint16_t color)
{
    snprintf(data->status_text, sizeof(data->status_text), "%s", text ? text : "");
    data->status_color = color;
    data->status_visible = (text != NULL && text[0] != '\0');
    data->status_dirty = true;
}

static void uart_line_cb(const char *line, void *user_data)
{
    subghz_sd_data_t *data = (subghz_sd_data_t *)user_data;
    if (!data || !line) return;

    if (data->clearing_all) {
        if (strstr(line, "[SUBGHZ_DELETE_ERR]")) {
            ESP_LOGW(TAG, "Clear-all aborted: %s", line);
            data->clearing_all = false;
            set_status(data, "Clear stopped", UI_COLOR_DIMMED);
            request_list_refresh(data);
            return;
        }
        if (strstr(line, "[SUBGHZ_DELETE]")) {
            int remaining = -1;
            const char *r = strstr(line, "remaining=");
            if (r) remaining = atoi(r + strlen("remaining="));
            if (remaining > 0) {
                uart_send_command("subghz_delete 1");
            } else {
                data->clearing_all = false;
                set_status(data, "All cleared", UI_COLOR_HIGHLIGHT);
                request_list_refresh(data);
            }
            return;
        }
        return;
    }

    if (strstr(line, "[SUBGHZ_LIST_END]")) {
        if (strstr(line, "source=sd") == NULL && strstr(line, "source=") != NULL) {
            /* Wrong source; ignore. */
            return;
        }
        data->collecting_list = false;
        bool was_loading = (data->view == SD_VIEW_LOADING);
        if (was_loading) {
            data->view = SD_VIEW_LIST;
            /* First time the list materialises: paint the whole window once. */
            data->window_dirty = true;
        }
        /* title shows "(N)" — keep it in sync on every refresh. */
        data->title_dirty = true;
        return;
    }

    if (strstr(line, "[SUBGHZ_RENAME_ERR]")) {
        set_status(data, "Rename failed", RGB565(255, 80, 80));
        return;
    }
    if (strstr(line, "[SUBGHZ_RENAME]")) {
        set_status(data, "Renamed", UI_COLOR_HIGHLIGHT);
        return;
    }

    if (!data->collecting_list) return;

    subghz_signal_info_t parsed;
    if (!subghz_parse_signal_line(line, &parsed)) return;
    if (parsed.kind != SUBGHZ_SIGNAL_KIND_LIST) return;

    xSemaphoreTake(data->sig_mtx, portMAX_DELAY);
    int new_idx = append_signal_locked(data, &parsed);
    xSemaphoreGive(data->sig_mtx);

    if (new_idx >= 0) {
        mark_row_dirty(data, new_idx);
        data->title_dirty = true;     /* "(N)" counter changed */
    }
}

static void request_list_refresh(subghz_sd_data_t *data)
{
    xSemaphoreTake(data->sig_mtx, portMAX_DELAY);
    data->sig_count = 0;
    data->selected_index = 0;
    data->scroll_offset = 0;
    xSemaphoreGive(data->sig_mtx);

    data->collecting_list = true;
    data->title_dirty = true;
    data->window_dirty = true;
    uart_send_command("subghz_list sd");
}

static void format_row(const sd_signal_t *sig, char *out, size_t n)
{
    int whole = (int)sig->freq;
    int frac  = ((int)(sig->freq * 100.0f + 0.5f)) % 100;

    if (sig->name[0]) {
        /* Prefer the user-renameable name as the primary label. */
        snprintf(out, n, "%d %.16s %d.%02d", sig->idx, sig->name, whole, frac);
        return;
    }

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

static void redraw_signal_row(subghz_sd_data_t *data, int sig_idx)
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

static void redraw_title(subghz_sd_data_t *data)
{
    char title[24];
    snprintf(title, sizeof(title), "SD Signals (%d)", data->sig_count);
    /* ui_draw_title repaints just the title row (row 0). */
    ui_draw_title(title);
    data->last_title_count = data->sig_count;
}

static void redraw_status_line(subghz_sd_data_t *data)
{
    /* Single-line status overlay on row 6 (above the status hint at row 7). */
    int y = 6 * 16;
    display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
    if (data->status_visible) {
        ui_print(0, 6, data->status_text, data->status_color);
    }
}

static void redraw_empty_hint(subghz_sd_data_t *data)
{
    int y = 3 * 16;
    display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
    if (data->sig_count > 0 && !data->clearing_all) {
        data->empty_hint_drawn = false;
        return;
    }
    if (data->clearing_all) {
        ui_print_center(3, "Clearing SD...", UI_COLOR_HIGHLIGHT);
    } else if (data->view == SD_VIEW_LOADING) {
        ui_print_center(3, "Loading...", UI_COLOR_DIMMED);
    } else {
        ui_print_center(3, "No signals on SD", UI_COLOR_DIMMED);
    }
    data->empty_hint_drawn = true;
}

static void redraw_list_window(subghz_sd_data_t *data)
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

    if (data->sig_count == 0 || data->clearing_all) {
        redraw_empty_hint(data);
    } else {
        data->empty_hint_drawn = false;
    }
}

static void draw_list_view(screen_t *self)
{
    subghz_sd_data_t *data = (subghz_sd_data_t *)self->user_data;

    ui_clear();
    redraw_title(data);
    redraw_list_window(data);
    redraw_status_line(data);
    ui_draw_status("ENT:Act X:Clr ESC:Back");

    data->title_dirty = false;
    data->status_dirty = false;
    data->window_dirty = false;
    clear_row_dirty(data);
}

static void draw_actions_view(screen_t *self)
{
    subghz_sd_data_t *data = (subghz_sd_data_t *)self->user_data;
    ui_clear();
    ui_draw_title("Signal Action");

    if (data->selected_index < data->sig_count) {
        const sd_signal_t *sig = &data->sigs[data->selected_index];
        char l1[40];
        int whole = (int)sig->freq;
        int frac  = ((int)(sig->freq * 100.0f + 0.5f)) % 100;
        if (sig->name[0]) {
            snprintf(l1, sizeof(l1), "#%d %.16s %d.%02d",
                     sig->idx, sig->name, whole, frac);
        } else {
            snprintf(l1, sizeof(l1), "#%d %.10s %d.%02d",
                     sig->idx, sig->type, whole, frac);
        }
        ui_print_center(2, l1, UI_COLOR_HIGHLIGHT);
    }

    ui_draw_menu_item(3, "Rename",   data->action_choice == SD_ACTION_RENAME,   false, false);
    ui_draw_menu_item(4, "Delete",   data->action_choice == SD_ACTION_DELETE,   false, false);
    ui_draw_menu_item(5, "Transmit", data->action_choice == SD_ACTION_TRANSMIT, false, false);
    ui_draw_menu_item(6, "Cancel",   data->action_choice == SD_ACTION_CANCEL,   false, false);

    ui_draw_status("UP/DN ENT:Pick ESC:Cancel");
}

static void draw_confirm_view(screen_t *self)
{
    subghz_sd_data_t *data = (subghz_sd_data_t *)self->user_data;
    ui_clear();
    ui_draw_title(data->view == SD_VIEW_CONFIRM_DELETE ? "Delete signal?" : "Clear ALL?");

    if (data->view == SD_VIEW_CONFIRM_DELETE &&
        data->selected_index < data->sig_count) {
        const sd_signal_t *sig = &data->sigs[data->selected_index];
        char l1[40], l2[40];
        int whole = (int)sig->freq;
        int frac  = ((int)(sig->freq * 100.0f + 0.5f)) % 100;
        if (sig->name[0]) {
            snprintf(l1, sizeof(l1), "#%d %.16s", sig->idx, sig->name);
            snprintf(l2, sizeof(l2), "%.10s %d.%02d", sig->type, whole, frac);
        } else {
            snprintf(l1, sizeof(l1), "#%d %.10s %d.%02d", sig->idx, sig->type, whole, frac);
            snprintf(l2, sizeof(l2), "%.14s %.14s",
                     sig->mf[0] ? sig->mf : "--", sig->serial[0] ? sig->serial : "--");
        }
        ui_print_center(2, l1, UI_COLOR_HIGHLIGHT);
        ui_print_center(3, l2, UI_COLOR_DIMMED);
    } else if (data->view == SD_VIEW_CONFIRM_CLEAR) {
        ui_print_center(2, "Delete ALL .sub files", UI_COLOR_HIGHLIGHT);
        ui_print_center(3, "on the SD card?", UI_COLOR_HIGHLIGHT);
    }

    ui_draw_menu_item(5, "Yes",    data->confirm_choice == 0, false, false);
    ui_draw_menu_item(6, "Cancel", data->confirm_choice == 1, false, false);

    ui_draw_status("UP/DN ENTER:Confirm ESC:Cancel");
}

static void draw_screen(screen_t *self)
{
    subghz_sd_data_t *data = (subghz_sd_data_t *)self->user_data;
    switch (data->view) {
        case SD_VIEW_LOADING:
        case SD_VIEW_LIST:
            draw_list_view(self);
            break;
        case SD_VIEW_ACTIONS:
            draw_actions_view(self);
            break;
        case SD_VIEW_CONFIRM_DELETE:
        case SD_VIEW_CONFIRM_CLEAR:
            draw_confirm_view(self);
            break;
    }
}

static void on_tick(screen_t *self)
{
    subghz_sd_data_t *data = (subghz_sd_data_t *)self->user_data;

    /* Background async updates only land on the list-style views. */
    if (data->view != SD_VIEW_LOADING && data->view != SD_VIEW_LIST) {
        data->title_dirty = false;
        data->status_dirty = false;
        data->window_dirty = false;
        clear_row_dirty(data);
        return;
    }

    if (data->title_dirty && data->last_title_count != data->sig_count) {
        redraw_title(data);
        data->title_dirty = false;
    } else if (data->title_dirty) {
        data->title_dirty = false;
    }

    if (data->status_dirty) {
        redraw_status_line(data);
        data->status_dirty = false;
    }

    bool want_hint = (data->sig_count == 0) || data->clearing_all;
    if (want_hint != data->empty_hint_drawn) {
        redraw_empty_hint(data);
    }

    if (data->window_dirty) {
        redraw_list_window(data);
        data->window_dirty = false;
        clear_row_dirty(data);
    } else if (data->row_dirty_from >= 0) {
        for (int i = data->row_dirty_from; i <= data->row_dirty_to; i++) {
            redraw_signal_row(data, i);
        }
        clear_row_dirty(data);
    }
}

static void redraw_two_rows(subghz_sd_data_t *data, int old_idx, int new_idx)
{
    if (old_idx >= 0) redraw_signal_row(data, old_idx);
    if (new_idx >= 0) redraw_signal_row(data, new_idx);
}

static void perform_delete(screen_t *self)
{
    subghz_sd_data_t *data = (subghz_sd_data_t *)self->user_data;
    if (data->selected_index >= data->sig_count) return;
    int idx = data->sigs[data->selected_index].idx;

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "subghz_delete %d", idx);
    uart_send_command(cmd);
    ESP_LOGI(TAG, "Sent: %s", cmd);

    char status[32];
    snprintf(status, sizeof(status), "Deleted #%d", idx);
    set_status(data, status, UI_COLOR_TITLE);

    data->view = SD_VIEW_LOADING;
    request_list_refresh(data);
    draw_screen(self);
}

static void perform_clear(screen_t *self)
{
    subghz_sd_data_t *data = (subghz_sd_data_t *)self->user_data;

    data->clearing_all = true;
    uart_send_command("subghz_delete 1");
    ESP_LOGI(TAG, "Clear-all started (iterative subghz_delete 1)");

    set_status(data, "Clearing SD...", UI_COLOR_HIGHLIGHT);
    data->view = SD_VIEW_LOADING;
    draw_screen(self);
}

static void perform_transmit(screen_t *self)
{
    subghz_sd_data_t *data = (subghz_sd_data_t *)self->user_data;
    if (data->selected_index >= data->sig_count) return;
    int idx = data->sigs[data->selected_index].idx;

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "subghz_tx %d sd", idx);
    uart_send_command(cmd);
    ESP_LOGI(TAG, "Sent: %s", cmd);

    char status[32];
    snprintf(status, sizeof(status), "Sent #%d", idx);
    set_status(data, status, UI_COLOR_HIGHLIGHT);

    data->view = SD_VIEW_LIST;
    draw_screen(self);
}

static void on_rename_submit(const char *text, void *user_data)
{
    (void)user_data;
    if (!s_current) {
        screen_manager_pop();
        return;
    }
    subghz_sd_data_t *data = s_current;

    if (data->selected_index >= data->sig_count) {
        screen_manager_pop();
        return;
    }
    int idx = data->sigs[data->selected_index].idx;

    char cmd[80];
    snprintf(cmd, sizeof(cmd), "subghz_rename %d %s", idx, text ? text : "");
    uart_send_command(cmd);
    ESP_LOGI(TAG, "Sent: %s", cmd);

    set_status(data, "Renaming...", UI_COLOR_HIGHLIGHT);
    data->view = SD_VIEW_LOADING;
    request_list_refresh(data);

    screen_manager_pop();
}

static void enter_rename(screen_t *self)
{
    subghz_sd_data_t *data = (subghz_sd_data_t *)self->user_data;
    if (data->selected_index >= data->sig_count) return;

    text_input_params_t *p = calloc(1, sizeof(text_input_params_t));
    if (!p) return;
    p->title = "Rename Signal";
    p->hint = "a-z 0-9 _ -";
    p->on_submit = on_rename_submit;
    p->user_data = NULL;
    p->allow_empty = false;

    data->view = SD_VIEW_LIST;
    screen_manager_push(text_input_screen_create, p);
}

static void on_key_list(screen_t *self, key_code_t key)
{
    subghz_sd_data_t *data = (subghz_sd_data_t *)self->user_data;

    switch (key) {
        case KEY_UP:
            if (data->sig_count == 0) break;
            if (data->selected_index > 0) {
                int old_idx = data->selected_index;
                if (data->selected_index == data->scroll_offset &&
                    data->scroll_offset > 0) {
                    data->scroll_offset -= VISIBLE_ITEMS;
                    if (data->scroll_offset < 0) data->scroll_offset = 0;
                    data->selected_index = data->scroll_offset + VISIBLE_ITEMS - 1;
                    if (data->selected_index >= data->sig_count) {
                        data->selected_index = data->sig_count - 1;
                    }
                    redraw_list_window(data);
                } else {
                    data->selected_index = old_idx - 1;
                    redraw_two_rows(data, old_idx, data->selected_index);
                }
            }
            break;

        case KEY_DOWN:
            if (data->sig_count == 0) break;
            if (data->selected_index < data->sig_count - 1) {
                int old_idx = data->selected_index;
                if (data->selected_index == data->scroll_offset + VISIBLE_ITEMS - 1) {
                    data->scroll_offset += VISIBLE_ITEMS;
                    if (data->scroll_offset > data->sig_count - 1) {
                        data->scroll_offset = data->sig_count - 1;
                    }
                    data->selected_index = data->scroll_offset;
                    redraw_list_window(data);
                } else {
                    data->selected_index = old_idx + 1;
                    redraw_two_rows(data, old_idx, data->selected_index);
                }
            }
            break;

        case KEY_ENTER:
        case KEY_SPACE:
            if (data->sig_count == 0) break;
            data->action_choice = SD_ACTION_TRANSMIT;
            data->view = SD_VIEW_ACTIONS;
            draw_actions_view(self);
            break;

        case KEY_X:
            if (data->sig_count == 0) break;
            data->confirm_choice = 1; /* Default = Cancel */
            data->view = SD_VIEW_CONFIRM_CLEAR;
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

static void on_key_actions(screen_t *self, key_code_t key)
{
    subghz_sd_data_t *data = (subghz_sd_data_t *)self->user_data;

    switch (key) {
        case KEY_UP:
            data->action_choice = (data->action_choice + SD_ACTION_COUNT - 1)
                                   % SD_ACTION_COUNT;
            draw_actions_view(self);
            break;

        case KEY_DOWN:
            data->action_choice = (data->action_choice + 1) % SD_ACTION_COUNT;
            draw_actions_view(self);
            break;

        case KEY_ENTER:
        case KEY_SPACE:
            switch (data->action_choice) {
                case SD_ACTION_RENAME:
                    enter_rename(self);
                    break;
                case SD_ACTION_DELETE:
                    data->confirm_choice = 1; /* Default = Cancel */
                    data->view = SD_VIEW_CONFIRM_DELETE;
                    draw_confirm_view(self);
                    break;
                case SD_ACTION_TRANSMIT:
                    perform_transmit(self);
                    break;
                case SD_ACTION_CANCEL:
                default:
                    data->view = SD_VIEW_LIST;
                    draw_list_view(self);
                    break;
            }
            break;

        case KEY_ESC:
        case KEY_BACKSPACE:
        case KEY_Q:
            data->view = SD_VIEW_LIST;
            draw_list_view(self);
            break;

        default:
            break;
    }
}

static void on_key_confirm(screen_t *self, key_code_t key)
{
    subghz_sd_data_t *data = (subghz_sd_data_t *)self->user_data;

    switch (key) {
        case KEY_UP:
        case KEY_DOWN:
            data->confirm_choice = data->confirm_choice ? 0 : 1;
            draw_confirm_view(self);
            break;

        case KEY_ENTER:
        case KEY_SPACE:
            if (data->confirm_choice == 0) {
                if (data->view == SD_VIEW_CONFIRM_DELETE) perform_delete(self);
                else                                      perform_clear(self);
            } else {
                data->view = SD_VIEW_LIST;
                draw_list_view(self);
            }
            break;

        case KEY_ESC:
        case KEY_BACKSPACE:
        case KEY_Q:
            data->view = SD_VIEW_LIST;
            draw_list_view(self);
            break;

        default:
            break;
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    subghz_sd_data_t *data = (subghz_sd_data_t *)self->user_data;
    switch (data->view) {
        case SD_VIEW_ACTIONS:
            on_key_actions(self, key);
            break;
        case SD_VIEW_CONFIRM_DELETE:
        case SD_VIEW_CONFIRM_CLEAR:
            on_key_confirm(self, key);
            break;
        case SD_VIEW_LOADING:
        case SD_VIEW_LIST:
        default:
            on_key_list(self, key);
            break;
    }
}

static void on_destroy(screen_t *self)
{
    subghz_sd_data_t *data = (subghz_sd_data_t *)self->user_data;
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
    subghz_sd_data_t *data = (subghz_sd_data_t *)self->user_data;
    /* Coming back from text-input rename or any child screen - refresh. */
    if (data->view != SD_VIEW_LOADING) {
        data->view = SD_VIEW_LOADING;
    }
    uart_register_line_callback(uart_line_cb, data);
    request_list_refresh(data);
    draw_screen(self);
}

screen_t* subghz_manage_screen_create(void *params)
{
    (void)params;

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    subghz_sd_data_t *data = calloc(1, sizeof(subghz_sd_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }

    data->sig_mtx = xSemaphoreCreateMutex();
    data->view = SD_VIEW_LOADING;
    data->collecting_list = true;
    data->row_dirty_from = -1;
    data->row_dirty_to = -1;
    data->last_title_count = -1;
    data->self = screen;
    s_current = data;

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;

    draw_screen(screen);

    /* Defensive: stop any prior subghz_rx/jam/etc. and request the SD list. */
    uart_send_command("subghz_stop");
    uart_register_line_callback(uart_line_cb, data);
    uart_send_command("subghz_list sd");

    ESP_LOGI(TAG, "SD Signals screen created");
    return screen;
}
