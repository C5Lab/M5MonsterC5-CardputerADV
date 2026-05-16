/**
 * @file subghz_hunter_screen.c
 * @brief Sub-GHz Hunter (auto-capture frequency analyzer) screen
 *
 * UART behaviour cloned from coreS3/main/screens/subghz_hunter_screen.c:
 *   start:  subghz_freq_analyzer hunt
 *   stop:   subghz_stop
 *
 * Status line is driven by [SUBGHZ_FA*] events (Scan / Capturing / Timeout /
 * Duplicate / Error / Idle), captured signals come in as [SUBGHZ_RX], [SUBGHZ_RX_DUP]
 * or [SUBGHZ_RAW] and are appended to the on-screen list (5 visible rows,
 * two-row redraw on cursor navigation).
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

typedef enum {
    HUNTER_STATUS_IDLE = 0,
    HUNTER_STATUS_SCAN,
    HUNTER_STATUS_CAPTURING,
    HUNTER_STATUS_TIMEOUT,
    HUNTER_STATUS_DUPLICATE,
    HUNTER_STATUS_ERROR,
    HUNTER_STATUS_STOPPED,
} hunter_status_kind_t;

typedef struct {
    int   idx;
    char  type[32];
    float freq;
    char  serial[32];
    int   btn;
    int   cnt;
    char  mf[32];
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
    bool needs_redraw;
    bool status_dirty;

    char status_text[STATUS_BUF_LEN];
    hunter_status_kind_t status_kind;

    screen_t *self;
} subghz_hunter_data_t;

static subghz_hunter_data_t *s_current = NULL;

static void draw_screen(screen_t *self);
static void redraw_status_row(subghz_hunter_data_t *data);
static void redraw_list_window(subghz_hunter_data_t *data);
static void redraw_signal_row(subghz_hunter_data_t *data, int sig_idx);

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
}

static bool merge_duplicate_signal_locked(subghz_hunter_data_t *data,
                                          const subghz_signal_info_t *src)
{
    if (!src || !src->is_duplicate || data->sig_count == 0) return false;
    hunter_signal_t *last = &data->sigs[data->sig_count - 1];
    if (last->is_raw) return false;

    bool match_idx = (src->idx > 0 && last->idx == src->idx);
    bool match_fields = (strcmp(last->type, src->type) == 0 &&
                         strcmp(last->serial, src->serial) == 0 &&
                         last->btn == src->btn);
    if (!match_idx && !match_fields) return false;
    if (src->cnt > 0) last->cnt = src->cnt;
    return true;
}

static void append_signal_locked(subghz_hunter_data_t *data,
                                 const subghz_signal_info_t *src)
{
    if (data->sig_count >= SUBGHZ_MAX_SIGNALS) {
        memmove(&data->sigs[0], &data->sigs[1],
                sizeof(data->sigs[0]) * (SUBGHZ_MAX_SIGNALS - 1));
        data->sig_count--;
    }
    fill_signal(&data->sigs[data->sig_count++], src);
}

/* Mirrors parse_fa_status_line() in coreS3 subghz_hunter_screen.c */
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
    if (!data || !data->running) return;

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
    if (!merge_duplicate_signal_locked(data, &parsed)) {
        append_signal_locked(data, &parsed);
    }
    xSemaphoreGive(data->sig_mtx);

    data->needs_redraw = true;
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

static uint16_t status_color(hunter_status_kind_t kind)
{
    switch (kind) {
        case HUNTER_STATUS_CAPTURING: return UI_COLOR_HIGHLIGHT;
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

    ui_print(0, 1, data->status_text, status_color(data->status_kind));

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
}

static void draw_screen(screen_t *self)
{
    subghz_hunter_data_t *data = (subghz_hunter_data_t *)self->user_data;

    ui_clear();
    ui_draw_title("Hunter");
    redraw_status_row(data);
    redraw_list_window(data);

    if (data->sig_count == 0) {
        ui_print_center(4, "Waiting for captures...", UI_COLOR_DIMMED);
    }

    ui_draw_status("ENT:Stp/Strt ESC:Back");
}

static void on_tick(screen_t *self)
{
    subghz_hunter_data_t *data = (subghz_hunter_data_t *)self->user_data;
    bool did_status = false;

    if (data->status_dirty) {
        data->status_dirty = false;
        redraw_status_row(data);
        did_status = true;
    }

    if (!data->needs_redraw) {
        (void)did_status;
        return;
    }
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

    if (!did_status) redraw_status_row(data);
    redraw_list_window(data);
}

static void redraw_two_rows(subghz_hunter_data_t *data, int old_idx, int new_idx)
{
    if (old_idx >= 0) redraw_signal_row(data, old_idx);
    if (new_idx >= 0) redraw_signal_row(data, new_idx);
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
    redraw_status_row(data);
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
    redraw_status_row(data);
    ESP_LOGI(TAG, "Hunter stopped");
}

static void on_key(screen_t *self, key_code_t key)
{
    subghz_hunter_data_t *data = (subghz_hunter_data_t *)self->user_data;

    switch (key) {
        case KEY_ENTER:
        case KEY_SPACE:
            if (data->running) stop_hunting(self);
            else               start_hunting(self);
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
                if (new_idx == data->sig_count - 1) data->follow_latest = true;
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

    /* Auto-start hunt like coreS3 */
    start_hunting(screen);

    return screen;
}
