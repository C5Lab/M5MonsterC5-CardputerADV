/**
 * @file subghz_hunter_screen.c
 * @brief Sub-GHz Hunter (auto-capture frequency analyzer) screen
 *
 * UART behaviour:
 *   start:  subghz_freq_analyzer hunt
 *   stop:   subghz_stop
 *
 * Captured signals are appended to the on-screen list (5 visible rows) and
 * live in the firmware mem cache. ENTER on a selected row opens a per-item
 * action menu (Save to SD / Transmit / Cancel); SPACE toggles capture
 * start/stop. ESC with captures in the list shows a leave-warning confirm
 * (signals in mem are lost on reboot or subghz_clear).
 *
 * Repaint model mirrors Listen: each async event sets the smallest possible
 * dirty flag (status_dirty / window_dirty / row range) and on_tick dispatches
 * only what changed. KEY_UP/DOWN page-jump like network_list_screen so per-
 * row scroll never triggers a full window redraw.
 */

#include "subghz_hunter_screen.h"
#include "subghz_parser.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

static const char *TAG = "SUBGHZ_HUNT";

#define VISIBLE_ITEMS       5
#define SUBGHZ_MAX_SIGNALS  128
#define STATUS_BUF_LEN      32

#define HUNTER_COLOR_LIVE     UI_COLOR_HIGHLIGHT
#define HUNTER_COLOR_STOPPED  RGB565(255, 80, 80)

typedef enum {
    HUNTER_STATUS_IDLE = 0,
    HUNTER_STATUS_SCAN,
    HUNTER_STATUS_CAPTURING,
    HUNTER_STATUS_TIMEOUT,
    HUNTER_STATUS_DUPLICATE,
    HUNTER_STATUS_ERROR,
    HUNTER_STATUS_STOPPED,
} hunter_status_kind_t;

typedef enum {
    HUNTER_VIEW_LIST = 0,
    HUNTER_VIEW_ACTIONS,
    HUNTER_VIEW_LEAVE_CONFIRM,
} hunter_view_t;

typedef enum {
    HUNTER_ACTION_SAVE = 0,
    HUNTER_ACTION_TX,
    HUNTER_ACTION_CANCEL,
    HUNTER_ACTION_COUNT,
} hunter_action_t;

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
} hunter_signal_t;

typedef struct {
    bool running;

    hunter_signal_t sigs[SUBGHZ_MAX_SIGNALS];
    int  sig_count;
    SemaphoreHandle_t sig_mtx;

    int  selected_index;
    int  scroll_offset;
    bool follow_latest;

    /* Per-row dirty model — same shape as Listen. */
    bool status_dirty;
    bool window_dirty;
    int  row_dirty_from;     /* -1 = none */
    int  row_dirty_to;
    bool empty_hint_drawn;

    char status_text[STATUS_BUF_LEN];
    hunter_status_kind_t status_kind;

    hunter_view_t view;
    int  action_choice;     /* 0=Save, 1=TX, 2=Cancel */
    int  confirm_choice;    /* 0=Cancel, 1=Leave anyway */
    bool was_running_pre_menu;

    char toast_text[STATUS_BUF_LEN];
    bool toast_visible;

    screen_t *self;
} subghz_hunter_data_t;

static subghz_hunter_data_t *s_current = NULL;

static void draw_screen(screen_t *self);
static void draw_list_view(screen_t *self);
static void draw_actions_view(screen_t *self);
static void draw_leave_confirm_view(screen_t *self);
static void redraw_status_row(subghz_hunter_data_t *data);
static void redraw_list_window(subghz_hunter_data_t *data);
static void redraw_signal_row(subghz_hunter_data_t *data, int sig_idx);
static void redraw_empty_hint(subghz_hunter_data_t *data);

static void mark_row_dirty(subghz_hunter_data_t *d, int idx)
{
    if (idx < 0) return;
    if (d->row_dirty_from < 0 || idx < d->row_dirty_from) d->row_dirty_from = idx;
    if (idx > d->row_dirty_to) d->row_dirty_to = idx;
}

static void clear_row_dirty(subghz_hunter_data_t *d)
{
    d->row_dirty_from = -1;
    d->row_dirty_to = -1;
}

static void set_status(subghz_hunter_data_t *data, hunter_status_kind_t kind,
                       const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static void set_status(subghz_hunter_data_t *data, hunter_status_kind_t kind,
                       const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(data->status_text, sizeof(data->status_text), fmt, ap);
    va_end(ap);
    data->status_kind = kind;
    data->status_dirty = true;
}

static void fill_signal(hunter_signal_t *dst, const subghz_signal_info_t *src)
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

/* Returns the merged-into row index, or -1 on no merge. */
static int merge_duplicate_signal_locked(subghz_hunter_data_t *data,
                                         const subghz_signal_info_t *src)
{
    if (!src || !src->is_duplicate || data->sig_count == 0) return -1;
    int last_idx = data->sig_count - 1;
    hunter_signal_t *last = &data->sigs[last_idx];
    if (last->is_raw) return -1;

    bool match_idx = (src->idx > 0 && last->idx == src->idx);
    bool match_fields = (strcmp(last->type, src->type) == 0 &&
                         strcmp(last->serial, src->serial) == 0 &&
                         last->btn == src->btn);
    if (!match_idx && !match_fields) return -1;
    if (src->cnt > 0) last->cnt = src->cnt;
    return last_idx;
}

/* Returns the appended row index, or -2 if the ring rolled. */
static int append_signal_locked(subghz_hunter_data_t *data,
                                const subghz_signal_info_t *src)
{
    bool shifted = false;
    if (data->sig_count >= SUBGHZ_MAX_SIGNALS) {
        memmove(&data->sigs[0], &data->sigs[1],
                sizeof(data->sigs[0]) * (SUBGHZ_MAX_SIGNALS - 1));
        data->sig_count--;
        shifted = true;
    }
    int new_idx = data->sig_count;
    fill_signal(&data->sigs[data->sig_count++], src);
    return shifted ? -2 : new_idx;
}

static void parse_fa_status_line(subghz_hunter_data_t *data, const char *line)
{
    if (strstr(line, "[SUBGHZ_FA] hunt capture")) {
        set_status(data, HUNTER_STATUS_CAPTURING, "Capturing...");
        return;
    }
    if (strstr(line, "[SUBGHZ_FA] hunt timeout")) {
        set_status(data, HUNTER_STATUS_TIMEOUT, "Timeout (no burst)");
        return;
    }
    if (strstr(line, "[SUBGHZ_FA] hunt duplicate")) {
        set_status(data, HUNTER_STATUS_DUPLICATE, "Duplicate");
        return;
    }
    if (strstr(line, "[SUBGHZ_FA] hunt error")) {
        set_status(data, HUNTER_STATUS_ERROR, "Capture error");
        return;
    }
    if (strstr(line, "[SUBGHZ_FA] silent")) {
        set_status(data, HUNTER_STATUS_IDLE, "Idle (no signal)");
        return;
    }
    if (strstr(line, "[SUBGHZ_FA] freq=")) {
        float freq = 0.0f;
        int   rssi = 0;
        char  stage[8] = {0};
        const char *fields = strstr(line, "[SUBGHZ_FA]");
        if (fields &&
            sscanf(fields, "[SUBGHZ_FA] freq=%f rssi=%d stage=%7s",
                   &freq, &rssi, stage) >= 2) {
            set_status(data, HUNTER_STATUS_SCAN, "Scan %.2f@%d", freq, rssi);
        }
        return;
    }
    if (strstr(line, "[SUBGHZ_FA_START]")) {
        set_status(data, HUNTER_STATUS_SCAN, "Hunting...");
    }
}

static void uart_line_cb(const char *line, void *user_data)
{
    subghz_hunter_data_t *data = (subghz_hunter_data_t *)user_data;
    if (!data) return;

    /* FA status lines */
    if (strstr(line, "[SUBGHZ_FA")) {
        parse_fa_status_line(data, line);
        return;
    }

    int rssi_ignored;
    if (subghz_parse_rssi_line(line, &rssi_ignored)) return;

    subghz_signal_info_t parsed;
    if (!subghz_parse_signal_line(line, &parsed)) return;
    if (parsed.kind == SUBGHZ_SIGNAL_KIND_LIST) return;

    xSemaphoreTake(data->sig_mtx, portMAX_DELAY);
    int merge_idx = merge_duplicate_signal_locked(data, &parsed);
    int appended = -1;
    if (merge_idx < 0) {
        appended = append_signal_locked(data, &parsed);
    }
    int total = data->sig_count;
    bool was_follow = data->follow_latest;
    int new_scroll = data->scroll_offset;
    if (was_follow && total > 0) {
        new_scroll = total - VISIBLE_ITEMS;
        if (new_scroll < 0) new_scroll = 0;
    }
    bool scroll_changed = (new_scroll != data->scroll_offset);
    bool ring_shifted = (appended == -2);
    if (was_follow) {
        data->scroll_offset = new_scroll;
        data->selected_index = total - 1;
    }
    xSemaphoreGive(data->sig_mtx);

    data->status_dirty = true;

    if (merge_idx >= 0) {
        mark_row_dirty(data, merge_idx);
    } else if (ring_shifted || scroll_changed) {
        data->window_dirty = true;
    } else if (appended >= 0) {
        mark_row_dirty(data, appended);
    }
}

static void format_row(const hunter_signal_t *sig, char *out, size_t n)
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

static uint16_t status_color(const subghz_hunter_data_t *data)
{
    /* Stopped overrides everything — should be impossible to miss. */
    if (!data->running) return HUNTER_COLOR_STOPPED;
    switch (data->status_kind) {
        case HUNTER_STATUS_CAPTURING: return HUNTER_COLOR_LIVE;
        case HUNTER_STATUS_SCAN:      return UI_COLOR_TITLE;
        case HUNTER_STATUS_ERROR:     return RGB565(255, 80, 80);
        case HUNTER_STATUS_DUPLICATE: return UI_COLOR_TEXT;
        case HUNTER_STATUS_TIMEOUT:
        case HUNTER_STATUS_IDLE:
        case HUNTER_STATUS_STOPPED:
        default:                      return UI_COLOR_DIMMED;
    }
}

static void redraw_status_row(subghz_hunter_data_t *data)
{
    int y = 1 * 16;
    display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);

    if (data->toast_visible) {
        ui_print(0, 1, data->toast_text, UI_COLOR_HIGHLIGHT);
    } else if (!data->running) {
        /* Make "STOPPED" loud and consistent with Listen. */
        ui_print(0, 1, "STOPPED", HUNTER_COLOR_STOPPED);
    } else {
        ui_print(0, 1, data->status_text, status_color(data));
    }

    char right[16];
    snprintf(right, sizeof(right), "%c %d", data->running ? '*' : '.', data->sig_count);
    int col = UI_COLS - (int)strlen(right);
    if (col < 0) col = 0;
    ui_print(col, 1, right, UI_COLOR_DIMMED);
}

static void redraw_signal_row(subghz_hunter_data_t *data, int sig_idx)
{
    int row_on_screen = sig_idx - data->scroll_offset;
    if (row_on_screen < 0 || row_on_screen >= VISIBLE_ITEMS) return;

    int ui_row = 2 + row_on_screen;
    int y = ui_row * 16;
    display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
    if (sig_idx >= data->sig_count) return;

    char buf[64];
    format_row(&data->sigs[sig_idx], buf, sizeof(buf));
    bool selected = (sig_idx == data->selected_index);
    ui_draw_menu_item(ui_row, buf, selected, false, false);
}

static void redraw_empty_hint(subghz_hunter_data_t *data)
{
    int y = 4 * 16;
    display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
    if (data->sig_count > 0) {
        data->empty_hint_drawn = false;
        return;
    }
    if (data->running) {
        ui_print_center(4, "Waiting for captures...", UI_COLOR_DIMMED);
    } else {
        ui_print_center(4, "Press SPACE to start", HUNTER_COLOR_STOPPED);
    }
    data->empty_hint_drawn = true;
}

static void redraw_list_window(subghz_hunter_data_t *data)
{
    for (int i = 0; i < VISIBLE_ITEMS; i++) {
        int sig_idx = data->scroll_offset + i;
        int ui_row = 2 + i;
        int y = ui_row * 16;
        display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
        if (sig_idx < data->sig_count) {
            char buf[64];
            format_row(&data->sigs[sig_idx], buf, sizeof(buf));
            bool selected = (sig_idx == data->selected_index);
            ui_draw_menu_item(ui_row, buf, selected, false, false);
        }
    }

    display_fill_rect(DISPLAY_WIDTH - 16, 2 * 16, 16, 16, UI_COLOR_BG);
    display_fill_rect(DISPLAY_WIDTH - 16, (2 + VISIBLE_ITEMS - 1) * 16, 16, 16, UI_COLOR_BG);
    if (data->scroll_offset > 0) {
        ui_print(UI_COLS - 2, 2, "^", UI_COLOR_DIMMED);
    }
    if (data->scroll_offset + VISIBLE_ITEMS < data->sig_count) {
        ui_print(UI_COLS - 2, 2 + VISIBLE_ITEMS - 1, "v", UI_COLOR_DIMMED);
    }

    if (data->sig_count == 0) {
        redraw_empty_hint(data);
    } else {
        data->empty_hint_drawn = false;
    }
}

static void draw_list_view(screen_t *self)
{
    subghz_hunter_data_t *data = (subghz_hunter_data_t *)self->user_data;

    ui_clear();
    ui_draw_title("Hunter");
    redraw_status_row(data);
    redraw_list_window(data);

    ui_draw_status("ENT:Act SP:Run ESC:Back");

    data->status_dirty = false;
    data->window_dirty = false;
    clear_row_dirty(data);
}

#define HUNTER_ACTION_ROW_BASE 4

static const char *hunter_action_label(int idx)
{
    switch (idx) {
        case HUNTER_ACTION_SAVE:   return "Save to SD";
        case HUNTER_ACTION_TX:     return "Transmit";
        case HUNTER_ACTION_CANCEL: return "Cancel";
        default:                   return "";
    }
}

static void redraw_action_row(subghz_hunter_data_t *data, int action_idx)
{
    if (action_idx < 0 || action_idx >= HUNTER_ACTION_COUNT) return;
    int ui_row = HUNTER_ACTION_ROW_BASE + action_idx;
    int y = ui_row * 16;
    display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
    ui_draw_menu_item(ui_row, hunter_action_label(action_idx),
                      data->action_choice == action_idx, false, false);
}

static void draw_actions_view(screen_t *self)
{
    subghz_hunter_data_t *data = (subghz_hunter_data_t *)self->user_data;
    ui_clear();
    ui_draw_title("Signal Action");

    if (data->selected_index < data->sig_count) {
        const hunter_signal_t *sig = &data->sigs[data->selected_index];
        char l1[40], l2[40];
        int whole = (int)sig->freq;
        int frac  = ((int)(sig->freq * 100.0f + 0.5f)) % 100;
        snprintf(l1, sizeof(l1), "#%d %.10s %d.%02d", sig->idx, sig->type, whole, frac);
        snprintf(l2, sizeof(l2), "%.14s %.14s",
                 sig->mf[0] ? sig->mf : "--", sig->serial[0] ? sig->serial : "--");
        ui_print_center(2, l1, UI_COLOR_HIGHLIGHT);
        ui_print_center(3, l2, UI_COLOR_DIMMED);
    }

    for (int i = 0; i < HUNTER_ACTION_COUNT; i++) redraw_action_row(data, i);

    ui_draw_status("UP/DN ENT:Pick ESC:Cancel");
}

#define HUNTER_CONFIRM_ROW_BASE 5

static const char *hunter_confirm_label(int idx)
{
    return (idx == 0) ? "Cancel" : "Leave anyway";
}

static void redraw_confirm_row(subghz_hunter_data_t *data, int idx)
{
    if (idx < 0 || idx > 1) return;
    int ui_row = HUNTER_CONFIRM_ROW_BASE + idx;
    int y = ui_row * 16;
    display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
    ui_draw_menu_item(ui_row, hunter_confirm_label(idx),
                      data->confirm_choice == idx, false, false);
}

static void draw_leave_confirm_view(screen_t *self)
{
    subghz_hunter_data_t *data = (subghz_hunter_data_t *)self->user_data;
    ui_clear();
    ui_draw_title("Leave screen?");

    ui_print_center(2, "Captures live in mem.", UI_COLOR_TEXT);
    ui_print_center(3, "Save to SD first or", UI_COLOR_DIMMED);
    ui_print_center(4, "they may be lost.", UI_COLOR_DIMMED);

    redraw_confirm_row(data, 0);
    redraw_confirm_row(data, 1);

    ui_draw_status("UP/DN ENT:Confirm ESC:Cancel");
}

static void draw_screen(screen_t *self)
{
    subghz_hunter_data_t *data = (subghz_hunter_data_t *)self->user_data;
    switch (data->view) {
        case HUNTER_VIEW_LIST:          draw_list_view(self); break;
        case HUNTER_VIEW_ACTIONS:       draw_actions_view(self); break;
        case HUNTER_VIEW_LEAVE_CONFIRM: draw_leave_confirm_view(self); break;
    }
}

static void on_tick(screen_t *self)
{
    subghz_hunter_data_t *data = (subghz_hunter_data_t *)self->user_data;

    if (data->view != HUNTER_VIEW_LIST) {
        data->status_dirty = false;
        data->window_dirty = false;
        clear_row_dirty(data);
        return;
    }

    if (data->status_dirty) {
        redraw_status_row(data);
        data->status_dirty = false;
    }

    bool want_hint = (data->sig_count == 0);
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

static void redraw_two_rows(subghz_hunter_data_t *data, int old_idx, int new_idx)
{
    if (old_idx >= 0) redraw_signal_row(data, old_idx);
    if (new_idx >= 0) redraw_signal_row(data, new_idx);
}

static void show_toast(subghz_hunter_data_t *data, const char *fmt, ...)
                       __attribute__((format(printf, 2, 3)));

static void show_toast(subghz_hunter_data_t *data, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(data->toast_text, sizeof(data->toast_text), fmt, ap);
    va_end(ap);
    data->toast_visible = true;
}

static void clear_toast(subghz_hunter_data_t *data)
{
    data->toast_visible = false;
    data->toast_text[0] = '\0';
}

static void start_hunting(screen_t *self)
{
    subghz_hunter_data_t *data = (subghz_hunter_data_t *)self->user_data;
    if (data->running) return;

    uart_send_command("subghz_stop");
    data->running = true;
    uart_register_line_callback(uart_line_cb, data);
    uart_send_command("subghz_freq_analyzer hunt");
    set_status(data, HUNTER_STATUS_SCAN, "Hunting...");
    if (data->view == HUNTER_VIEW_LIST) {
        redraw_status_row(data);
        if (data->sig_count == 0) redraw_empty_hint(data);
    }
    ESP_LOGI(TAG, "Hunter started");
}

static void stop_hunting(screen_t *self)
{
    subghz_hunter_data_t *data = (subghz_hunter_data_t *)self->user_data;
    if (!data->running) return;
    uart_send_command("subghz_stop");
    uart_clear_line_callback();
    data->running = false;
    set_status(data, HUNTER_STATUS_STOPPED, "Stopped");
    if (data->view == HUNTER_VIEW_LIST) {
        redraw_status_row(data);
        if (data->sig_count == 0) redraw_empty_hint(data);
    }
    ESP_LOGI(TAG, "Hunter stopped");
}

static void enter_actions_view(screen_t *self)
{
    subghz_hunter_data_t *data = (subghz_hunter_data_t *)self->user_data;
    if (data->sig_count == 0) return;
    if (data->selected_index >= data->sig_count) return;

    data->was_running_pre_menu = data->running;
    if (data->running) stop_hunting(self);

    data->action_choice = HUNTER_ACTION_SAVE;
    data->view = HUNTER_VIEW_ACTIONS;
    draw_actions_view(self);
}

static void leave_menu_resume_capture(screen_t *self)
{
    subghz_hunter_data_t *data = (subghz_hunter_data_t *)self->user_data;
    data->view = HUNTER_VIEW_LIST;
    if (data->was_running_pre_menu && !data->running) {
        start_hunting(self);
    }
    data->was_running_pre_menu = false;
    draw_list_view(self);
}

static void perform_save_to_sd(screen_t *self)
{
    subghz_hunter_data_t *data = (subghz_hunter_data_t *)self->user_data;
    if (data->selected_index >= data->sig_count) return;
    int mem_idx = data->sigs[data->selected_index].idx;

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "subghz_save %d", mem_idx);
    uart_send_command(cmd);
    ESP_LOGI(TAG, "Sent: %s", cmd);

    show_toast(data, "Saved #%d to SD", mem_idx);
    leave_menu_resume_capture(self);
}

static void perform_transmit_mem(screen_t *self)
{
    subghz_hunter_data_t *data = (subghz_hunter_data_t *)self->user_data;
    if (data->selected_index >= data->sig_count) return;
    int mem_idx = data->sigs[data->selected_index].idx;

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "subghz_tx %d mem", mem_idx);
    uart_send_command(cmd);
    ESP_LOGI(TAG, "Sent: %s", cmd);

    show_toast(data, "Sent #%d", mem_idx);
    leave_menu_resume_capture(self);
}

static void on_key_list(screen_t *self, key_code_t key)
{
    subghz_hunter_data_t *data = (subghz_hunter_data_t *)self->user_data;

    switch (key) {
        case KEY_ENTER:
            if (data->sig_count > 0) {
                clear_toast(data);
                enter_actions_view(self);
            } else {
                if (data->running) stop_hunting(self);
                else               start_hunting(self);
            }
            break;

        case KEY_SPACE:
            clear_toast(data);
            if (data->running) stop_hunting(self);
            else               start_hunting(self);
            break;

        case KEY_UP:
            if (data->sig_count == 0) break;
            data->follow_latest = false;
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
                if (data->selected_index == data->sig_count - 1) {
                    data->follow_latest = true;
                }
            } else {
                data->follow_latest = true;
            }
            break;

        case KEY_ESC:
        case KEY_Q:
        case KEY_BACKSPACE:
            if (data->sig_count > 0) {
                data->confirm_choice = 0;
                data->view = HUNTER_VIEW_LEAVE_CONFIRM;
                draw_leave_confirm_view(self);
            } else {
                if (data->running) {
                    uart_send_command("subghz_stop");
                    uart_clear_line_callback();
                    data->running = false;
                }
                screen_manager_pop();
            }
            break;

        default:
            break;
    }
}

static void on_key_actions(screen_t *self, key_code_t key)
{
    subghz_hunter_data_t *data = (subghz_hunter_data_t *)self->user_data;

    switch (key) {
        case KEY_UP: {
            int old = data->action_choice;
            data->action_choice = (old + HUNTER_ACTION_COUNT - 1) % HUNTER_ACTION_COUNT;
            redraw_action_row(data, old);
            redraw_action_row(data, data->action_choice);
            break;
        }

        case KEY_DOWN: {
            int old = data->action_choice;
            data->action_choice = (old + 1) % HUNTER_ACTION_COUNT;
            redraw_action_row(data, old);
            redraw_action_row(data, data->action_choice);
            break;
        }

        case KEY_ENTER:
        case KEY_SPACE:
            switch (data->action_choice) {
                case HUNTER_ACTION_SAVE:   perform_save_to_sd(self); break;
                case HUNTER_ACTION_TX:     perform_transmit_mem(self); break;
                case HUNTER_ACTION_CANCEL:
                default:                   leave_menu_resume_capture(self); break;
            }
            break;

        case KEY_ESC:
        case KEY_BACKSPACE:
        case KEY_Q:
            leave_menu_resume_capture(self);
            break;

        default:
            break;
    }
}

static void on_key_leave_confirm(screen_t *self, key_code_t key)
{
    subghz_hunter_data_t *data = (subghz_hunter_data_t *)self->user_data;

    switch (key) {
        case KEY_UP:
        case KEY_DOWN: {
            int old = data->confirm_choice;
            data->confirm_choice = old ? 0 : 1;
            redraw_confirm_row(data, old);
            redraw_confirm_row(data, data->confirm_choice);
            break;
        }

        case KEY_ENTER:
        case KEY_SPACE:
            if (data->confirm_choice == 1) {
                if (data->running) {
                    uart_send_command("subghz_stop");
                    uart_clear_line_callback();
                    data->running = false;
                }
                screen_manager_pop();
            } else {
                data->view = HUNTER_VIEW_LIST;
                draw_list_view(self);
            }
            break;

        case KEY_ESC:
        case KEY_BACKSPACE:
        case KEY_Q:
            data->view = HUNTER_VIEW_LIST;
            draw_list_view(self);
            break;

        default:
            break;
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    subghz_hunter_data_t *data = (subghz_hunter_data_t *)self->user_data;
    switch (data->view) {
        case HUNTER_VIEW_ACTIONS:       on_key_actions(self, key); break;
        case HUNTER_VIEW_LEAVE_CONFIRM: on_key_leave_confirm(self, key); break;
        case HUNTER_VIEW_LIST:
        default:                        on_key_list(self, key); break;
    }
}

static void on_destroy(screen_t *self)
{
    subghz_hunter_data_t *data = (subghz_hunter_data_t *)self->user_data;
    if (data) {
        if (data->running) {
            uart_send_command("subghz_stop");
            uart_clear_line_callback();
            data->running = false;
        }
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

screen_t* subghz_hunter_screen_create(void *params)
{
    (void)params;

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    subghz_hunter_data_t *data = calloc(1, sizeof(subghz_hunter_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }

    data->sig_mtx = xSemaphoreCreateMutex();
    data->follow_latest = true;
    data->self = screen;
    data->view = HUNTER_VIEW_LIST;
    data->row_dirty_from = -1;
    data->row_dirty_to = -1;
    snprintf(data->status_text, sizeof(data->status_text), "Starting hunter...");
    data->status_kind = HUNTER_STATUS_SCAN;

    s_current = data;

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;

    draw_screen(screen);
    ESP_LOGI(TAG, "Hunter screen created");

    start_hunting(screen);

    return screen;
}
