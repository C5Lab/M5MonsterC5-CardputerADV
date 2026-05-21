/**
 * @file subghz_listen_screen.c
 * @brief Sub-GHz Listen (RX) screen
 *
 * UART behaviour:
 *   subghz_freq <f>
 *   subghz_rx           (or subghz_rx raw)
 *   subghz_stop         on exit/stop
 *
 * Captured signals live in the firmware mem cache. ENTER on a selected row
 * opens a per-item action menu (Save to SD / Transmit / Cancel); SPACE
 * toggles capture start/stop. ESC with captures in the list shows a
 * leave-warning confirm (signals in mem are lost on reboot or subghz_clear).
 */

#include "subghz_listen_screen.h"
#include "subghz_freq_picker_screen.h"
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

static const char *TAG = "SUBGHZ_LISTEN";

#define VISIBLE_ITEMS       5
#define SUBGHZ_MAX_SIGNALS  128
#define TOAST_BUF_LEN       32

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
    bool  needs_redraw;
    int   prev_count_drawn;

    listen_view_t view;
    int  action_choice;     /* 0=Save, 1=TX, 2=Cancel */
    int  confirm_choice;    /* 0=Cancel, 1=Leave anyway */
    bool was_running_pre_menu;

    char toast_text[TOAST_BUF_LEN];
    bool toast_visible;

    screen_t *self;
} subghz_listen_data_t;

/* Active screen reference for UART line callback (line_callback is global). */
static subghz_listen_data_t *s_current = NULL;

/* Optional pre-fill from Scanner. */
static float s_pending_freq = 0.0f;
static bool  s_pending_autostart = false;

static void draw_screen(screen_t *self);
static void draw_list_view(screen_t *self);
static void draw_actions_view(screen_t *self);
static void draw_leave_confirm_view(screen_t *self);
static void redraw_signal_row(subghz_listen_data_t *data, int sig_idx);
static void redraw_status_row(subghz_listen_data_t *data);
static void redraw_list_window(subghz_listen_data_t *data);
static void start_rx(screen_t *self);
static void stop_rx(screen_t *self);

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

static bool merge_duplicate_signal_locked(subghz_listen_data_t *data,
                                          const subghz_signal_info_t *src)
{
    if (!src || !src->is_duplicate || data->sig_count == 0) return false;
    subghz_signal_t *last = &data->sigs[data->sig_count - 1];
    if (last->is_raw) return false;

    bool match_idx = (src->idx > 0 && last->idx == src->idx);
    bool match_fields = (strcmp(last->type, src->type) == 0 &&
                         strcmp(last->serial, src->serial) == 0 &&
                         last->btn == src->btn);
    if (!match_idx && !match_fields) return false;

    if (src->cnt > 0) last->cnt = src->cnt;
    return true;
}

static void append_signal_locked(subghz_listen_data_t *data,
                                 const subghz_signal_info_t *src)
{
    if (data->sig_count >= SUBGHZ_MAX_SIGNALS) {
        memmove(&data->sigs[0], &data->sigs[1],
                sizeof(data->sigs[0]) * (SUBGHZ_MAX_SIGNALS - 1));
        data->sig_count--;
    }
    fill_signal(&data->sigs[data->sig_count++], src);
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
    if (!merge_duplicate_signal_locked(data, &parsed)) {
        append_signal_locked(data, &parsed);
    }
    xSemaphoreGive(data->sig_mtx);

    data->needs_redraw = true;
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
        char left[20];
        int whole = (int)data->freq_mhz;
        int frac  = ((int)(data->freq_mhz * 100.0f + 0.5f)) % 100;
        snprintf(left, sizeof(left), "%d.%02d %s", whole, frac,
                 data->raw_mode ? "RAW" : "Dec");
        ui_print(0, 1, left, data->running ? UI_COLOR_TITLE : UI_COLOR_TEXT);
    }

    char right[16];
    snprintf(right, sizeof(right), "%c %d sig",
             data->running ? '*' : '.', data->sig_count);
    int col = UI_COLS - (int)strlen(right);
    if (col < 0) col = 0;
    ui_print(col, 1, right, UI_COLOR_DIMMED);
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
}

static void draw_list_view(screen_t *self)
{
    subghz_listen_data_t *data = (subghz_listen_data_t *)self->user_data;

    ui_clear();
    ui_draw_title("Listen");
    redraw_status_row(data);
    redraw_list_window(data);

    if (data->sig_count == 0) {
        ui_print_center(4, "No signals captured", UI_COLOR_DIMMED);
    }

    ui_draw_status("ENT:Act SP:Run F:Frq R:Raw");
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

    ui_draw_menu_item(4, "Save to SD", data->action_choice == LISTEN_ACTION_SAVE,   false, false);
    ui_draw_menu_item(5, "Transmit",   data->action_choice == LISTEN_ACTION_TX,     false, false);
    ui_draw_menu_item(6, "Cancel",     data->action_choice == LISTEN_ACTION_CANCEL, false, false);

    ui_draw_status("UP/DN ENT:Pick ESC:Cancel");
}

static void draw_leave_confirm_view(screen_t *self)
{
    subghz_listen_data_t *data = (subghz_listen_data_t *)self->user_data;
    ui_clear();
    ui_draw_title("Leave screen?");

    ui_print_center(2, "Captures live in mem.", UI_COLOR_TEXT);
    ui_print_center(3, "Save to SD first or", UI_COLOR_DIMMED);
    ui_print_center(4, "they may be lost.", UI_COLOR_DIMMED);

    ui_draw_menu_item(5, "Cancel",       data->confirm_choice == 0, false, false);
    ui_draw_menu_item(6, "Leave anyway", data->confirm_choice == 1, false, false);

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
        data->needs_redraw = false;
        return;
    }
    if (!data->needs_redraw) return;
    data->needs_redraw = false;

    xSemaphoreTake(data->sig_mtx, portMAX_DELAY);
    int total = data->sig_count;

    if (data->follow_latest && total > 0) {
        data->selected_index = total - 1;
        int max_scroll = total - VISIBLE_ITEMS;
        if (max_scroll < 0) max_scroll = 0;
        data->scroll_offset = max_scroll;
    }
    xSemaphoreGive(data->sig_mtx);

    redraw_status_row(data);
    redraw_list_window(data);
    data->prev_count_drawn = total;
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

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "subghz_freq %.2f", data->freq_mhz);
    uart_send_command(cmd);

    data->running = true;
    uart_register_line_callback(uart_line_cb, data);

    if (data->raw_mode) uart_send_command("subghz_rx raw");
    else                uart_send_command("subghz_rx");

    ESP_LOGI(TAG, "Listen started (%.2f MHz, raw=%d)", data->freq_mhz, data->raw_mode);
    if (data->view == LISTEN_VIEW_LIST) redraw_status_row(data);
}

static void stop_rx(screen_t *self)
{
    subghz_listen_data_t *data = (subghz_listen_data_t *)self->user_data;
    if (!data->running) return;

    uart_send_command("subghz_stop");
    uart_clear_line_callback();
    data->running = false;
    ESP_LOGI(TAG, "Listen stopped");
    if (data->view == LISTEN_VIEW_LIST) redraw_status_row(data);
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

        case KEY_UP:
            if (data->sig_count == 0) break;
            data->follow_latest = false;
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
                if (new_idx == data->sig_count - 1) {
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
        case KEY_UP:
            data->action_choice = (data->action_choice + LISTEN_ACTION_COUNT - 1)
                                   % LISTEN_ACTION_COUNT;
            draw_actions_view(self);
            break;

        case KEY_DOWN:
            data->action_choice = (data->action_choice + 1) % LISTEN_ACTION_COUNT;
            draw_actions_view(self);
            break;

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
        case KEY_DOWN:
            data->confirm_choice = data->confirm_choice ? 0 : 1;
            draw_leave_confirm_view(self);
            break;

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
    draw_screen(self);
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
    data->sig_mtx = xSemaphoreCreateMutex();
    data->self = screen;

    bool autostart = s_pending_autostart;
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

    if (autostart) {
        start_rx(screen);
    }
    return screen;
}

/* Used by Scanner to jump directly to Listen on a detected freq. */
void subghz_listen_screen_set_pending(float freq_mhz, bool autostart)
{
    s_pending_freq = freq_mhz;
    s_pending_autostart = autostart;
}
