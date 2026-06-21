/**
 * @file subghz_listen_screen.c
 * @brief Sub-GHz Listen (RX) screen
 *
 * UART behaviour:
 *   subghz_freq <f>
 *   subghz_rx           (or subghz_rx raw)
 *   subghz_stop         on exit/stop
 *
 * Listen auto-starts on entry. A bright "LIVE 433.92 Dec" badge (green) on
 * the status row makes the running state obvious; when paused the same row
 * shows "STOPPED 433.92 Dec" in red and the body shows a centred
 * "Press SPACE to start" hint when the list is empty.
 *
 * Captured signals live in the firmware mem cache. ENTER on a selected row
 * opens a per-item action menu (Save to SD / Transmit / Cancel); SPACE
 * toggles capture start/stop. ESC with captures in the list shows a
 * leave-warning confirm (signals in mem are lost on reboot or subghz_clear).
 *
 * Repaint model: the screen runs a per-row "dirty" model — async UART events
 * (RSSI, RX, FA, …) only ever set the minimal dirty flags they need. on_tick
 * dispatches status / row-range / window repaints without ever clearing the
 * whole screen. Matches the wifi network_list_screen idiom: scroll never
 * moves by one row, it page-jumps by VISIBLE_ITEMS so per-row navigation
 * always uses the two-row redraw.
 */

#include "subghz_listen_screen.h"
#include "subghz_freq_picker_screen.h"
#include "subghz_listen_settings_screen.h"
#include "subghz_parser.h"
#include "subghz_rf_settings.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

static const char *TAG = "SUBGHZ_LISTEN";

#define VISIBLE_ITEMS       5
#define SUBGHZ_MAX_SIGNALS  128
#define TOAST_BUF_LEN       32

#define LISTEN_COLOR_LIVE     UI_COLOR_HIGHLIGHT
#define LISTEN_COLOR_STOPPED  RGB565(255, 80, 80)

typedef enum {
    LISTEN_VIEW_LIST = 0,
    LISTEN_VIEW_ACTIONS,
    LISTEN_VIEW_LEAVE_CONFIRM,
} listen_view_t;

typedef enum {
    LISTEN_ACTION_SAVE = 0,
    LISTEN_ACTION_TX,
    LISTEN_ACTION_CANCEL,
    LISTEN_ACTION_COUNT,
} listen_action_t;

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
} subghz_signal_t;

typedef struct {
    float freq_mhz;
    bool  running;
    bool  raw_mode;

    subghz_signal_t sigs[SUBGHZ_MAX_SIGNALS];
    int  sig_count;
    SemaphoreHandle_t sig_mtx;

    int   selected_index;
    int   scroll_offset;
    bool  follow_latest;

    /* Per-row dirty model. Set by async events; cleared by on_tick. */
    bool  status_dirty;
    bool  window_dirty;
    int   row_dirty_from;       /* -1 = none */
    int   row_dirty_to;         /* inclusive */
    bool  empty_hint_drawn;     /* tracks whether the centred placeholder is painted */

    listen_view_t view;
    int  action_choice;         /* 0=Save, 1=TX, 2=Cancel */
    int  confirm_choice;        /* 0=Cancel, 1=Leave anyway */
    bool was_running_pre_menu;

    /* Pushed Listen Settings? Restart RX on the next on_resume if so. */
    bool restart_on_resume;

    char toast_text[TOAST_BUF_LEN];
    bool toast_visible;

    screen_t *self;
} subghz_listen_data_t;

/* Active screen reference for UART line callback (line_callback is global). */
static subghz_listen_data_t *s_current = NULL;

/* Optional pre-fill from Scanner. The autostart flag is no longer consulted —
 * Listen always starts on entry — but the field is kept so the public API
 * stays source-compatible. */
static float s_pending_freq = 0.0f;
static bool  s_pending_autostart = false;

static void draw_screen(screen_t *self);
static void draw_list_view(screen_t *self);
static void draw_actions_view(screen_t *self);
static void draw_leave_confirm_view(screen_t *self);
static void redraw_signal_row(subghz_listen_data_t *data, int sig_idx);
static void redraw_status_row(subghz_listen_data_t *data);
static void redraw_list_window(subghz_listen_data_t *data);
static void redraw_empty_hint(subghz_listen_data_t *data);
static void start_rx(screen_t *self);
static void stop_rx(screen_t *self);

static void mark_row_dirty(subghz_listen_data_t *d, int idx)
{
    if (idx < 0) return;
    if (d->row_dirty_from < 0 || idx < d->row_dirty_from) d->row_dirty_from = idx;
    if (idx > d->row_dirty_to) d->row_dirty_to = idx;
}

static void clear_row_dirty(subghz_listen_data_t *d)
{
    d->row_dirty_from = -1;
    d->row_dirty_to = -1;
}

static void fill_signal(subghz_signal_t *dst, const subghz_signal_info_t *src)
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

/* Returns the index of the merged-into row on success, -1 if no merge. */
static int merge_duplicate_signal_locked(subghz_listen_data_t *data,
                                         const subghz_signal_info_t *src)
{
    if (!src || !src->is_duplicate || data->sig_count == 0) return -1;
    int last_idx = data->sig_count - 1;
    subghz_signal_t *last = &data->sigs[last_idx];
    if (last->is_raw) return -1;

    bool match_idx = (src->idx > 0 && last->idx == src->idx);
    bool match_fields = (strcmp(last->type, src->type) == 0 &&
                         strcmp(last->serial, src->serial) == 0 &&
                         last->btn == src->btn);
    if (!match_idx && !match_fields) return -1;

    if (src->cnt > 0) last->cnt = src->cnt;
    return last_idx;
}

/* Returns the index of the newly appended row, or -1 on cap overflow drop. */
static int append_signal_locked(subghz_listen_data_t *data,
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

static void uart_line_cb(const char *line, void *user_data)
{
    subghz_listen_data_t *data = (subghz_listen_data_t *)user_data;
    if (!data) return;

    int rssi_ignored;
    if (subghz_parse_rssi_line(line, &rssi_ignored)) return;

    subghz_signal_info_t parsed;
    if (!subghz_parse_signal_line(line, &parsed)) return;

    if (parsed.kind == SUBGHZ_SIGNAL_KIND_LIST) return;

    if (!data->raw_mode && parsed.kind == SUBGHZ_SIGNAL_KIND_RAW) return;
    if (data->raw_mode && parsed.kind == SUBGHZ_SIGNAL_KIND_RX_DUP) return;

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

    /* status row always needs an update on every event (counter + LIVE state). */
    data->status_dirty = true;

    if (merge_idx >= 0) {
        mark_row_dirty(data, merge_idx);
    } else if (ring_shifted || scroll_changed) {
        data->window_dirty = true;
    } else if (appended >= 0) {
        mark_row_dirty(data, appended);
    }
}

static void format_row(const subghz_signal_t *sig, char *out, size_t n)
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

static void redraw_signal_row(subghz_listen_data_t *data, int sig_idx)
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

static void redraw_status_row(subghz_listen_data_t *data)
{
    int y = 1 * 16;
    display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);

    if (data->toast_visible) {
        ui_print(0, 1, data->toast_text, UI_COLOR_HIGHLIGHT);
    } else {
        char left[24];
        int whole = (int)data->freq_mhz;
        int frac  = ((int)(data->freq_mhz * 100.0f + 0.5f)) % 100;
        snprintf(left, sizeof(left), "%s %d.%02d %s",
                 data->running ? "LIVE" : "STOPPED",
                 whole, frac, data->raw_mode ? "RAW" : "Dec");
        ui_print(0, 1, left,
                 data->running ? LISTEN_COLOR_LIVE : LISTEN_COLOR_STOPPED);
    }

    char right[16];
    snprintf(right, sizeof(right), "%c %d sig",
             data->running ? '*' : '.', data->sig_count);
    int col = UI_COLS - (int)strlen(right);
    if (col < 0) col = 0;
    ui_print(col, 1, right, UI_COLOR_DIMMED);
}

static void redraw_empty_hint(subghz_listen_data_t *data)
{
    /* Body area (rows 2..6, 5 rows) is currently blank when sig_count==0;
     * paint a single centred row 4 hint based on running state. */
    int y = 4 * 16;
    display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
    if (data->sig_count > 0) {
        data->empty_hint_drawn = false;
        return;
    }
    if (data->running) {
        ui_print_center(4, "No signals captured", UI_COLOR_DIMMED);
    } else {
        ui_print_center(4, "Press SPACE to start", LISTEN_COLOR_STOPPED);
    }
    data->empty_hint_drawn = true;
}

static void redraw_list_window(subghz_listen_data_t *data)
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
    subghz_listen_data_t *data = (subghz_listen_data_t *)self->user_data;

    ui_clear();
    ui_draw_title("Listen");
    redraw_status_row(data);
    redraw_list_window(data);

    ui_draw_status("ENT:Act SP:Run F:Frq S:Set");

    /* On full draw all dirty flags are cleared. */
    data->status_dirty = false;
    data->window_dirty = false;
    clear_row_dirty(data);
}

/* Action menu layout: rows 4..6 for Save/TX/Cancel. */
#define LISTEN_ACTION_ROW_BASE 4

static const char *listen_action_label(int idx)
{
    switch (idx) {
        case LISTEN_ACTION_SAVE:   return "Save to SD";
        case LISTEN_ACTION_TX:     return "Transmit";
        case LISTEN_ACTION_CANCEL: return "Cancel";
        default:                   return "";
    }
}

static void redraw_action_row(subghz_listen_data_t *data, int action_idx)
{
    if (action_idx < 0 || action_idx >= LISTEN_ACTION_COUNT) return;
    int ui_row = LISTEN_ACTION_ROW_BASE + action_idx;
    int y = ui_row * 16;
    display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
    ui_draw_menu_item(ui_row, listen_action_label(action_idx),
                      data->action_choice == action_idx, false, false);
}

static void draw_actions_view(screen_t *self)
{
    subghz_listen_data_t *data = (subghz_listen_data_t *)self->user_data;
    ui_clear();
    ui_draw_title("Signal Action");

    if (data->selected_index < data->sig_count) {
        const subghz_signal_t *sig = &data->sigs[data->selected_index];
        char l1[40], l2[40];
        int whole = (int)sig->freq;
        int frac  = ((int)(sig->freq * 100.0f + 0.5f)) % 100;
        snprintf(l1, sizeof(l1), "#%d %.10s %d.%02d", sig->idx, sig->type, whole, frac);
        snprintf(l2, sizeof(l2), "%.14s %.14s",
                 sig->mf[0] ? sig->mf : "--", sig->serial[0] ? sig->serial : "--");
        ui_print_center(2, l1, UI_COLOR_HIGHLIGHT);
        ui_print_center(3, l2, UI_COLOR_DIMMED);
    }

    for (int i = 0; i < LISTEN_ACTION_COUNT; i++) redraw_action_row(data, i);

    ui_draw_status("UP/DN ENT:Pick ESC:Cancel");
}

/* Leave-confirm layout: row 5 = Cancel, row 6 = Leave anyway. */
#define LISTEN_CONFIRM_ROW_BASE 5

static const char *listen_confirm_label(int idx)
{
    return (idx == 0) ? "Cancel" : "Leave anyway";
}

static void redraw_confirm_row(subghz_listen_data_t *data, int idx)
{
    if (idx < 0 || idx > 1) return;
    int ui_row = LISTEN_CONFIRM_ROW_BASE + idx;
    int y = ui_row * 16;
    display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
    ui_draw_menu_item(ui_row, listen_confirm_label(idx),
                      data->confirm_choice == idx, false, false);
}

static void draw_leave_confirm_view(screen_t *self)
{
    subghz_listen_data_t *data = (subghz_listen_data_t *)self->user_data;
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
    subghz_listen_data_t *data = (subghz_listen_data_t *)self->user_data;
    switch (data->view) {
        case LISTEN_VIEW_LIST:          draw_list_view(self); break;
        case LISTEN_VIEW_ACTIONS:       draw_actions_view(self); break;
        case LISTEN_VIEW_LEAVE_CONFIRM: draw_leave_confirm_view(self); break;
    }
}

static void on_tick(screen_t *self)
{
    subghz_listen_data_t *data = (subghz_listen_data_t *)self->user_data;
    if (data->view != LISTEN_VIEW_LIST) {
        /* Drop dirty flags collected while a sub-view was active; the next
         * return to LIST view paints fresh in leave_menu_resume_capture(). */
        data->status_dirty = false;
        data->window_dirty = false;
        clear_row_dirty(data);
        return;
    }

    if (data->status_dirty) {
        redraw_status_row(data);
        data->status_dirty = false;
    }

    /* Empty hint visibility may flip when sig_count crosses 0<->>0 or the
     * running state changes. Cheap to keep in sync via the same path. */
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

static void redraw_two_rows(subghz_listen_data_t *data, int old_idx, int new_idx)
{
    if (old_idx >= 0) redraw_signal_row(data, old_idx);
    if (new_idx >= 0) redraw_signal_row(data, new_idx);
}

static void show_toast(subghz_listen_data_t *data, const char *fmt, ...)
                       __attribute__((format(printf, 2, 3)));

static void show_toast(subghz_listen_data_t *data, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(data->toast_text, sizeof(data->toast_text), fmt, ap);
    va_end(ap);
    data->toast_visible = true;
}

static void clear_toast(subghz_listen_data_t *data)
{
    data->toast_visible = false;
    data->toast_text[0] = '\0';
}

static void start_rx(screen_t *self)
{
    subghz_listen_data_t *data = (subghz_listen_data_t *)self->user_data;
    if (data->running) return;

    uart_send_command("subghz_stop");

    char cmd[48];
    snprintf(cmd, sizeof(cmd), "subghz_freq %.2f", data->freq_mhz);
    uart_send_command(cmd);

    data->running = true;
    uart_register_line_callback(uart_line_cb, data);

    /* Apply user-configured RSSI noise gate so weak / strict captures match
     * what's set in Listen Settings (same payload as coreS3). */
    subghz_rf_settings_t cfg;
    subghz_rf_settings_load(&cfg);
    if (data->raw_mode) {
        snprintf(cmd, sizeof(cmd), "subghz_rx raw rssi=%d", (int)cfg.listen_rssi_dbm);
    } else {
        snprintf(cmd, sizeof(cmd), "subghz_rx rssi=%d", (int)cfg.listen_rssi_dbm);
    }
    uart_send_command(cmd);

    ESP_LOGI(TAG, "Listen started (%.2f MHz, raw=%d, rssi=%d)",
             data->freq_mhz, data->raw_mode, (int)cfg.listen_rssi_dbm);
    if (data->view == LISTEN_VIEW_LIST) {
        redraw_status_row(data);
        if (data->sig_count == 0) redraw_empty_hint(data);
    }
}

static void stop_rx(screen_t *self)
{
    subghz_listen_data_t *data = (subghz_listen_data_t *)self->user_data;
    if (!data->running) return;

    uart_send_command("subghz_stop");
    uart_clear_line_callback();
    data->running = false;
    ESP_LOGI(TAG, "Listen stopped");
    if (data->view == LISTEN_VIEW_LIST) {
        redraw_status_row(data);
        if (data->sig_count == 0) redraw_empty_hint(data);
    }
}

static void enter_actions_view(screen_t *self)
{
    subghz_listen_data_t *data = (subghz_listen_data_t *)self->user_data;
    if (data->sig_count == 0) return;
    if (data->selected_index >= data->sig_count) return;

    data->was_running_pre_menu = data->running;
    if (data->running) stop_rx(self);

    data->action_choice = LISTEN_ACTION_SAVE;
    data->view = LISTEN_VIEW_ACTIONS;
    draw_actions_view(self);
}

static void leave_menu_resume_capture(screen_t *self)
{
    subghz_listen_data_t *data = (subghz_listen_data_t *)self->user_data;
    data->view = LISTEN_VIEW_LIST;
    if (data->was_running_pre_menu && !data->running) {
        start_rx(self);
    }
    data->was_running_pre_menu = false;
    draw_list_view(self);
}

static void perform_save_to_sd(screen_t *self)
{
    subghz_listen_data_t *data = (subghz_listen_data_t *)self->user_data;
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
    subghz_listen_data_t *data = (subghz_listen_data_t *)self->user_data;
    if (data->selected_index >= data->sig_count) return;
    int mem_idx = data->sigs[data->selected_index].idx;

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "subghz_tx %d mem", mem_idx);
    uart_send_command(cmd);
    ESP_LOGI(TAG, "Sent: %s", cmd);

    show_toast(data, "Sent #%d", mem_idx);
    leave_menu_resume_capture(self);
}

static void on_freq_picked(float freq, void *user_data)
{
    (void)user_data;
    if (!s_current) return;
    bool was_running = s_current->running;
    if (was_running) {
        uart_send_command("subghz_stop");
        uart_clear_line_callback();
        s_current->running = false;
    }
    s_current->freq_mhz = freq;
    ESP_LOGI(TAG, "Listen freq set to %.2f MHz", freq);
    (void)was_running;
}

static void on_key_list(screen_t *self, key_code_t key)
{
    subghz_listen_data_t *data = (subghz_listen_data_t *)self->user_data;

    switch (key) {
        case KEY_ENTER:
            if (data->sig_count > 0) {
                clear_toast(data);
                enter_actions_view(self);
            } else {
                if (data->running) stop_rx(self);
                else               start_rx(self);
            }
            break;

        case KEY_SPACE:
            clear_toast(data);
            if (data->running) stop_rx(self);
            else               start_rx(self);
            break;

        case KEY_F:
            if (!data->running) {
                subghz_freq_picker_params_t *p = calloc(1, sizeof(subghz_freq_picker_params_t));
                if (p) {
                    p->initial_freq = data->freq_mhz;
                    p->on_pick = on_freq_picked;
                    p->user_data = NULL;
                    screen_manager_push(subghz_freq_picker_screen_create, p);
                }
            }
            break;

        case KEY_R:
            if (!data->running) {
                data->raw_mode = !data->raw_mode;
                redraw_status_row(data);
            }
            break;

        case KEY_S:
            /* Open Listen Settings. Stop RX first so the UART line callback
             * is freed; the settings screen does its own UART work and the
             * Hunter/Listen restart pattern in coreS3 is "restart on back". */
            data->restart_on_resume = data->running;
            if (data->running) stop_rx(self);
            screen_manager_push(subghz_listen_settings_screen_create, NULL);
            break;

        case KEY_UP:
            if (data->sig_count == 0) break;
            data->follow_latest = false;
            if (data->selected_index > 0) {
                int old_idx = data->selected_index;
                /* network_list_screen idiom: page-jump from the top edge of
                 * the current page; single-step otherwise. */
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
                data->view = LISTEN_VIEW_LEAVE_CONFIRM;
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
    subghz_listen_data_t *data = (subghz_listen_data_t *)self->user_data;

    switch (key) {
        case KEY_UP: {
            int old = data->action_choice;
            data->action_choice = (old + LISTEN_ACTION_COUNT - 1) % LISTEN_ACTION_COUNT;
            redraw_action_row(data, old);
            redraw_action_row(data, data->action_choice);
            break;
        }

        case KEY_DOWN: {
            int old = data->action_choice;
            data->action_choice = (old + 1) % LISTEN_ACTION_COUNT;
            redraw_action_row(data, old);
            redraw_action_row(data, data->action_choice);
            break;
        }

        case KEY_ENTER:
        case KEY_SPACE:
            switch (data->action_choice) {
                case LISTEN_ACTION_SAVE:   perform_save_to_sd(self); break;
                case LISTEN_ACTION_TX:     perform_transmit_mem(self); break;
                case LISTEN_ACTION_CANCEL:
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
    subghz_listen_data_t *data = (subghz_listen_data_t *)self->user_data;

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
                data->view = LISTEN_VIEW_LIST;
                draw_list_view(self);
            }
            break;

        case KEY_ESC:
        case KEY_BACKSPACE:
        case KEY_Q:
            data->view = LISTEN_VIEW_LIST;
            draw_list_view(self);
            break;

        default:
            break;
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    subghz_listen_data_t *data = (subghz_listen_data_t *)self->user_data;
    switch (data->view) {
        case LISTEN_VIEW_ACTIONS:       on_key_actions(self, key); break;
        case LISTEN_VIEW_LEAVE_CONFIRM: on_key_leave_confirm(self, key); break;
        case LISTEN_VIEW_LIST:
        default:                        on_key_list(self, key); break;
    }
}

static void on_destroy(screen_t *self)
{
    subghz_listen_data_t *data = (subghz_listen_data_t *)self->user_data;
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
    subghz_listen_data_t *data = (subghz_listen_data_t *)self->user_data;
    draw_screen(self);
    /* Auto-restart RX if we stopped it to push Listen Settings (coreS3
     * pattern). Always clear the flag so subsequent resumes don't restart. */
    if (data && data->restart_on_resume) {
        data->restart_on_resume = false;
        if (!data->running) start_rx(self);
    }
}

screen_t* subghz_listen_screen_create(void *params)
{
    (void)params;

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    subghz_listen_data_t *data = calloc(1, sizeof(subghz_listen_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }

    data->freq_mhz = (s_pending_freq > 0.0f) ? s_pending_freq : 433.92f;
    data->running = false;
    data->raw_mode = false;
    data->follow_latest = true;
    data->view = LISTEN_VIEW_LIST;
    data->row_dirty_from = -1;
    data->row_dirty_to = -1;
    data->sig_mtx = xSemaphoreCreateMutex();
    data->self = screen;

    s_pending_freq = 0.0f;
    s_pending_autostart = false;

    s_current = data;

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;

    draw_screen(screen);
    ESP_LOGI(TAG, "Listen screen created");

    /* Listen always auto-starts on entry — the LIVE/STOPPED badge keeps the
     * user from doubting whether RX is on. SPACE pauses, SPACE resumes. */
    start_rx(screen);
    return screen;
}

/* Used by Scanner to jump directly to Listen on a detected freq. The
 * autostart flag is now ignored — Listen always starts — but the public
 * API is kept stable. */
void subghz_listen_screen_set_pending(float freq_mhz, bool autostart)
{
    s_pending_freq = freq_mhz;
    s_pending_autostart = autostart;
}
