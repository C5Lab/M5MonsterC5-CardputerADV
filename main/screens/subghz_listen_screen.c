/**
 * @file subghz_listen_screen.c
 * @brief Sub-GHz Listen (RX) screen
 *
 * UART behaviour is cloned from coreS3/main/screens/subghz_listen_screen.c:
 *   subghz_freq <f>
 *   subghz_rx           (or subghz_rx raw)
 *   subghz_stop         on exit/stop
 *
 * Captured-signal parsing uses subghz_parser.[ch] (cloned verbatim from coreS3).
 * Duplicate-merge logic mirrors merge_duplicate_signal() from coreS3's Listen.
 *
 * The captured-signal list uses the Cardputer 5-row scrollable list idiom with
 * the "two-row redraw" optimisation when the user navigates.
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

static const char *TAG = "SUBGHZ_LISTEN";

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
    screen_t *self;
} subghz_listen_data_t;

/* Active screen reference for UART line callback (line_callback is global). */
static subghz_listen_data_t *s_current = NULL;

/* Optional pre-fill from Scanner. */
static float s_pending_freq = 0.0f;
static bool  s_pending_autostart = false;

static void draw_screen(screen_t *self);
static void redraw_signal_row(subghz_listen_data_t *data, int sig_idx);
static void redraw_status_row(subghz_listen_data_t *data);
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
}

/* Mirrors merge_duplicate_signal() in coreS3 subghz_listen_screen.c */
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
        /* Drop oldest by shifting; rare. */
        memmove(&data->sigs[0], &data->sigs[1],
                sizeof(data->sigs[0]) * (SUBGHZ_MAX_SIGNALS - 1));
        data->sig_count--;
    }
    fill_signal(&data->sigs[data->sig_count++], src);
}

static void uart_line_cb(const char *line, void *user_data)
{
    subghz_listen_data_t *data = (subghz_listen_data_t *)user_data;
    if (!data || !data->running) return;

    /* RSSI flood: skip without further work */
    int rssi_ignored;
    if (subghz_parse_rssi_line(line, &rssi_ignored)) return;

    subghz_signal_info_t parsed;
    if (!subghz_parse_signal_line(line, &parsed)) return;

    /* Skip list entries (Listen does not request subghz_list) */
    if (parsed.kind == SUBGHZ_SIGNAL_KIND_LIST) return;

    /* Raw / decoded toggle behaviour mirrors coreS3 Listen */
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
    /* Compact row that fits in 30 cols including the ENTER indicator. */
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

    char left[20];
    int whole = (int)data->freq_mhz;
    int frac  = ((int)(data->freq_mhz * 100.0f + 0.5f)) % 100;
    snprintf(left, sizeof(left), "%d.%02d %s", whole, frac,
             data->raw_mode ? "RAW" : "Dec");
    ui_print(0, 1, left, data->running ? UI_COLOR_TITLE : UI_COLOR_TEXT);

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

    /* Scroll indicators */
    display_fill_rect(DISPLAY_WIDTH - 16, 2 * 16, 16, 16, UI_COLOR_BG);
    display_fill_rect(DISPLAY_WIDTH - 16, (2 + VISIBLE_ITEMS - 1) * 16, 16, 16, UI_COLOR_BG);
    if (data->scroll_offset > 0) {
        ui_print(UI_COLS - 2, 2, "^", UI_COLOR_DIMMED);
    }
    if (data->scroll_offset + VISIBLE_ITEMS < data->sig_count) {
        ui_print(UI_COLS - 2, 2 + VISIBLE_ITEMS - 1, "v", UI_COLOR_DIMMED);
    }
}

static void draw_screen(screen_t *self)
{
    subghz_listen_data_t *data = (subghz_listen_data_t *)self->user_data;

    ui_clear();
    ui_draw_title("Listen");
    redraw_status_row(data);
    redraw_list_window(data);

    if (data->sig_count == 0) {
        ui_print_center(4, "No signals captured", UI_COLOR_DIMMED);
    }

    ui_draw_status("ENT:Strt F:Frq R:Raw ESC:Back");
}

static void on_tick(screen_t *self)
{
    subghz_listen_data_t *data = (subghz_listen_data_t *)self->user_data;
    if (!data->needs_redraw) return;
    data->needs_redraw = false;

    xSemaphoreTake(data->sig_mtx, portMAX_DELAY);
    int total = data->sig_count;

    /* Follow-latest: keep cursor pinned to the newest signal. */
    if (data->follow_latest && total > 0) {
        data->selected_index = total - 1;
        int max_scroll = total - VISIBLE_ITEMS;
        if (max_scroll < 0) max_scroll = 0;
        data->scroll_offset = max_scroll;
    }
    xSemaphoreGive(data->sig_mtx);

    /* If the count changed, repaint the visible window + status row. */
    redraw_status_row(data);
    redraw_list_window(data);
    data->prev_count_drawn = total;
}

static void redraw_two_rows(subghz_listen_data_t *data, int old_idx, int new_idx)
{
    if (old_idx >= 0) redraw_signal_row(data, old_idx);
    if (new_idx >= 0) redraw_signal_row(data, new_idx);
}

static void start_rx(screen_t *self)
{
    subghz_listen_data_t *data = (subghz_listen_data_t *)self->user_data;
    if (data->running) return;

    /* Defensive stop in case any prior subghz_* task is still running. */
    uart_send_command("subghz_stop");

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "subghz_freq %.2f", data->freq_mhz);
    uart_send_command(cmd);

    data->running = true;
    uart_register_line_callback(uart_line_cb, data);

    if (data->raw_mode) uart_send_command("subghz_rx raw");
    else                uart_send_command("subghz_rx");

    ESP_LOGI(TAG, "Listen started (%.2f MHz, raw=%d)", data->freq_mhz, data->raw_mode);
    redraw_status_row(data);
}

static void stop_rx(screen_t *self)
{
    subghz_listen_data_t *data = (subghz_listen_data_t *)self->user_data;
    if (!data->running) return;

    uart_send_command("subghz_stop");
    uart_clear_line_callback();
    data->running = false;
    ESP_LOGI(TAG, "Listen stopped");
    redraw_status_row(data);
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
    /* draw via on_resume */
    (void)was_running; /* user must press Start again, mirroring coreS3 behaviour */
}

static void on_key(screen_t *self, key_code_t key)
{
    subghz_listen_data_t *data = (subghz_listen_data_t *)self->user_data;

    switch (key) {
        case KEY_ENTER:
        case KEY_SPACE:
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
                /* Snap follow-latest when we land on the newest signal. */
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
            if (data->running) {
                uart_send_command("subghz_stop");
                uart_clear_line_callback();
                data->running = false;
            }
            screen_manager_pop();
            break;

        default:
            break;
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
