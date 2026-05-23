/**
 * @file subghz_scanner_screen.c
 * @brief Sub-GHz traffic scanner ("Quick Scan")
 *
 * UART behaviour cloned from coreS3/main/screens/subghz_scanner_screen.c:
 *   start:  subghz_scanner
 *   stop:   subghz_stop
 *
 * Parses [SUBGHZ_SCAN_HIT] freq=... edges=... rssi=... into an MRU list.
 * ENTER on a frequency stops the scanner and pushes the Listen screen pre-
 * filled with that frequency, auto-starting RX.
 *
 * Repaint model is the per-row dirty model used by Listen/Hunter:
 *   - PASS pulse → only status_dirty.
 *   - MRU promote of an existing entry (existing > 0) → rows [0..existing]
 *     are all dirty; if it was already at 0, only row 0.
 *   - New entry → window_dirty (rows shifted).
 * KEY_UP/DOWN uses the network_list page-jump idiom so per-row scroll does
 * not redraw the entire window.
 */

#include "subghz_scanner_screen.h"
#include "subghz_listen_screen.h"
#include "subghz_scanner_settings_screen.h"
#include "subghz_rf_settings.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

static const char *TAG = "SUBGHZ_SCAN";

#define MAX_FREQS       16
#define VISIBLE_ITEMS   5

typedef struct {
    float freq;
    int   rssi;
    unsigned edges;
} scanner_freq_t;

typedef struct {
    bool running;
    scanner_freq_t freqs[MAX_FREQS];
    int  freq_count;
    SemaphoreHandle_t mtx;

    bool pass_pulse;
    int  pass_count;
    bool dot_bright;

    int  selected_index;
    int  scroll_offset;

    /* Per-row dirty model. */
    bool status_dirty;
    bool window_dirty;
    int  row_dirty_from;   /* -1 = none */
    int  row_dirty_to;
    bool empty_hint_drawn;

    screen_t *self;
} subghz_scanner_data_t;

static subghz_scanner_data_t *s_current = NULL;

static void draw_screen(screen_t *self);
static void redraw_freq_row(subghz_scanner_data_t *data, int idx);
static void redraw_list_window(subghz_scanner_data_t *data);
static void redraw_status_row(subghz_scanner_data_t *data);
static void redraw_empty_hint(subghz_scanner_data_t *data);
static void uart_line_cb(const char *line, void *user_data);

/* Build the scanner CLI command from NVS-persisted settings and start it.
 * Shared by screen creation and on_resume — both paths must pick up the
 * latest Scanner Settings (dwell / edges / rssi / fast). */
static void start_scanning(subghz_scanner_data_t *data)
{
    if (!data || data->running) return;

    subghz_rf_settings_t cfg;
    subghz_rf_settings_load(&cfg);
    char cmd[80];
    subghz_rf_build_scanner_cmd(&cfg, cmd, sizeof(cmd));
    if (cmd[0] == '\0') {
        snprintf(cmd, sizeof(cmd), "subghz_scanner");
    }

    uart_send_command("subghz_stop");
    data->running = true;
    uart_register_line_callback(uart_line_cb, data);
    uart_send_command(cmd);
}

static void mark_row_dirty(subghz_scanner_data_t *d, int idx)
{
    if (idx < 0) return;
    if (d->row_dirty_from < 0 || idx < d->row_dirty_from) d->row_dirty_from = idx;
    if (idx > d->row_dirty_to) d->row_dirty_to = idx;
}

static void mark_row_range_dirty(subghz_scanner_data_t *d, int from, int to)
{
    if (from < 0 || to < from) return;
    if (d->row_dirty_from < 0 || from < d->row_dirty_from) d->row_dirty_from = from;
    if (to > d->row_dirty_to) d->row_dirty_to = to;
}

static void clear_row_dirty(subghz_scanner_data_t *d)
{
    d->row_dirty_from = -1;
    d->row_dirty_to = -1;
}

/* Returns: 0 if a new entry was inserted (rows shifted, prefer window_dirty),
 * or 1+existing-idx (1..N) if an existing entry was promoted from index N. */
static int mru_insert_locked(subghz_scanner_data_t *data, float freq,
                             int rssi, unsigned edges)
{
    int existing = -1;
    for (int i = 0; i < data->freq_count; i++) {
        if (fabsf(data->freqs[i].freq - freq) < 0.005f) {
            existing = i;
            break;
        }
    }
    int from;
    bool new_entry = false;
    if (existing >= 0) {
        from = existing;
    } else if (data->freq_count < MAX_FREQS) {
        from = data->freq_count;
        data->freq_count++;
        new_entry = true;
    } else {
        from = MAX_FREQS - 1;
        new_entry = true;
    }
    for (int i = from; i > 0; i--) {
        data->freqs[i] = data->freqs[i - 1];
    }
    data->freqs[0].freq = freq;
    data->freqs[0].rssi = rssi;
    data->freqs[0].edges = edges;

    if (new_entry) return 0;
    return 1 + existing;
}

static void uart_line_cb(const char *line, void *user_data)
{
    subghz_scanner_data_t *data = (subghz_scanner_data_t *)user_data;
    if (!data || !data->running) return;

    if (strstr(line, "[SUBGHZ_SCAN_PASS]")) {
        xSemaphoreTake(data->mtx, portMAX_DELAY);
        data->pass_pulse = true;
        data->pass_count++;
        xSemaphoreGive(data->mtx);
        /* Only the status row changes on a pass pulse. */
        data->status_dirty = true;
        return;
    }

    const char *hit = strstr(line, "[SUBGHZ_SCAN_HIT] freq=");
    if (!hit) return;

    float freq = 0.0f;
    unsigned edges = 0;
    int rssi = 0;
    if (sscanf(hit, "[SUBGHZ_SCAN_HIT] freq=%f edges=%u rssi=%d",
               &freq, &edges, &rssi) < 1 || freq <= 0.0f) return;

    float rounded = roundf(freq * 100.0f) / 100.0f;

    xSemaphoreTake(data->mtx, portMAX_DELAY);
    int kind = mru_insert_locked(data, rounded, rssi, edges);
    xSemaphoreGive(data->mtx);

    /* hits-counter on the status row needs an update. */
    data->status_dirty = true;

    if (kind == 0) {
        /* New entry shifted the whole list down. */
        data->window_dirty = true;
    } else {
        int existing_idx = kind - 1;
        if (existing_idx == 0) {
            mark_row_dirty(data, 0);
        } else {
            /* Promote: rows [0..existing_idx] all changed values. */
            mark_row_range_dirty(data, 0, existing_idx);
        }
    }
}

static void format_row(const scanner_freq_t *f, char *out, size_t n)
{
    int whole = (int)f->freq;
    int frac  = ((int)(f->freq * 100.0f + 0.5f)) % 100;
    snprintf(out, n, "%d.%02d MHz  %d dBm  %ue",
             whole, frac, f->rssi, f->edges);
}

static void redraw_freq_row(subghz_scanner_data_t *data, int idx)
{
    int row_on_screen = idx - data->scroll_offset;
    if (row_on_screen < 0 || row_on_screen >= VISIBLE_ITEMS) return;
    int ui_row = 2 + row_on_screen;
    int y = ui_row * 16;
    display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
    if (idx >= data->freq_count) return;

    char buf[40];
    format_row(&data->freqs[idx], buf, sizeof(buf));
    bool selected = (idx == data->selected_index);
    ui_draw_menu_item(ui_row, buf, selected, false, false);
}

static void redraw_empty_hint(subghz_scanner_data_t *data)
{
    int y = 4 * 16;
    display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
    if (data->freq_count > 0) {
        data->empty_hint_drawn = false;
        return;
    }
    if (data->running) {
        ui_print_center(4, "Listening...", UI_COLOR_DIMMED);
    } else {
        ui_print_center(4, "Stopped", UI_COLOR_DIMMED);
    }
    data->empty_hint_drawn = true;
}

static void redraw_list_window(subghz_scanner_data_t *data)
{
    for (int i = 0; i < VISIBLE_ITEMS; i++) {
        int idx = data->scroll_offset + i;
        int ui_row = 2 + i;
        int y = ui_row * 16;
        display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
        if (idx < data->freq_count) {
            char buf[40];
            format_row(&data->freqs[idx], buf, sizeof(buf));
            bool selected = (idx == data->selected_index);
            ui_draw_menu_item(ui_row, buf, selected, false, false);
        }
    }

    display_fill_rect(DISPLAY_WIDTH - 16, 2 * 16, 16, 16, UI_COLOR_BG);
    display_fill_rect(DISPLAY_WIDTH - 16, (2 + VISIBLE_ITEMS - 1) * 16, 16, 16, UI_COLOR_BG);
    if (data->scroll_offset > 0)
        ui_print(UI_COLS - 2, 2, "^", UI_COLOR_DIMMED);
    if (data->scroll_offset + VISIBLE_ITEMS < data->freq_count)
        ui_print(UI_COLS - 2, 2 + VISIBLE_ITEMS - 1, "v", UI_COLOR_DIMMED);

    if (data->freq_count == 0) {
        redraw_empty_hint(data);
    } else {
        data->empty_hint_drawn = false;
    }
}

static void redraw_status_row(subghz_scanner_data_t *data)
{
    int y = 1 * 16;
    display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);

    /* Burn the pulse: toggle the dot once per pulse, then forget it. */
    xSemaphoreTake(data->mtx, portMAX_DELAY);
    bool pulse = data->pass_pulse;
    data->pass_pulse = false;
    xSemaphoreGive(data->mtx);
    if (pulse) data->dot_bright = !data->dot_bright;

    char left[24];
    snprintf(left, sizeof(left), "%s  passes:%d",
             data->running ? (data->dot_bright ? "Scanning*" : "Scanning ")
                           : "Stopped",
             data->pass_count);
    ui_print(0, 1, left, data->running ? UI_COLOR_TITLE : UI_COLOR_DIMMED);

    char right[12];
    snprintf(right, sizeof(right), "%d hits", data->freq_count);
    int col = UI_COLS - (int)strlen(right);
    if (col < 0) col = 0;
    ui_print(col, 1, right, UI_COLOR_DIMMED);
}

static void draw_screen(screen_t *self)
{
    subghz_scanner_data_t *data = (subghz_scanner_data_t *)self->user_data;

    ui_clear();
    ui_draw_title("Quick Scan");
    redraw_status_row(data);
    redraw_list_window(data);

    ui_draw_status("ENT:Listen UP/DN S:Set");

    data->status_dirty = false;
    data->window_dirty = false;
    clear_row_dirty(data);
}

static void on_tick(screen_t *self)
{
    subghz_scanner_data_t *data = (subghz_scanner_data_t *)self->user_data;

    if (data->status_dirty) {
        redraw_status_row(data);
        data->status_dirty = false;
    }

    bool want_hint = (data->freq_count == 0);
    if (want_hint != data->empty_hint_drawn) {
        redraw_empty_hint(data);
    }

    if (data->window_dirty) {
        redraw_list_window(data);
        data->window_dirty = false;
        clear_row_dirty(data);
    } else if (data->row_dirty_from >= 0) {
        for (int i = data->row_dirty_from; i <= data->row_dirty_to; i++) {
            redraw_freq_row(data, i);
        }
        clear_row_dirty(data);
    }
}

static void redraw_two_rows(subghz_scanner_data_t *data, int old_idx, int new_idx)
{
    if (old_idx >= 0) redraw_freq_row(data, old_idx);
    if (new_idx >= 0) redraw_freq_row(data, new_idx);
}

static void on_pick_freq(screen_t *self)
{
    subghz_scanner_data_t *data = (subghz_scanner_data_t *)self->user_data;
    if (data->selected_index >= data->freq_count) return;

    float freq = data->freqs[data->selected_index].freq;

    /* Stop scanner cleanly before handing off the radio to Listen. */
    if (data->running) {
        uart_send_command("subghz_stop");
        uart_clear_line_callback();
        data->running = false;
    }

    ESP_LOGI(TAG, "Handing off to Listen at %.2f MHz", freq);
    subghz_listen_screen_set_pending(freq, true);
    screen_manager_push(subghz_listen_screen_create, NULL);
}

static void on_key(screen_t *self, key_code_t key)
{
    subghz_scanner_data_t *data = (subghz_scanner_data_t *)self->user_data;

    switch (key) {
        case KEY_UP:
            if (data->freq_count == 0) break;
            if (data->selected_index > 0) {
                int old_idx = data->selected_index;
                if (data->selected_index == data->scroll_offset &&
                    data->scroll_offset > 0) {
                    data->scroll_offset -= VISIBLE_ITEMS;
                    if (data->scroll_offset < 0) data->scroll_offset = 0;
                    data->selected_index = data->scroll_offset + VISIBLE_ITEMS - 1;
                    if (data->selected_index >= data->freq_count) {
                        data->selected_index = data->freq_count - 1;
                    }
                    redraw_list_window(data);
                } else {
                    data->selected_index = old_idx - 1;
                    redraw_two_rows(data, old_idx, data->selected_index);
                }
            }
            break;

        case KEY_DOWN:
            if (data->freq_count == 0) break;
            if (data->selected_index < data->freq_count - 1) {
                int old_idx = data->selected_index;
                if (data->selected_index == data->scroll_offset + VISIBLE_ITEMS - 1) {
                    data->scroll_offset += VISIBLE_ITEMS;
                    if (data->scroll_offset > data->freq_count - 1) {
                        data->scroll_offset = data->freq_count - 1;
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
            on_pick_freq(self);
            break;

        case KEY_S:
            /* Open Scanner Settings; on_resume restarts the scanner with
             * the freshly saved cfg via start_scanning(). */
            if (data->running) {
                uart_send_command("subghz_stop");
                uart_clear_line_callback();
                data->running = false;
            }
            screen_manager_push(subghz_scanner_settings_screen_create, NULL);
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
    subghz_scanner_data_t *data = (subghz_scanner_data_t *)self->user_data;
    if (data) {
        if (data->running) {
            uart_send_command("subghz_stop");
            uart_clear_line_callback();
            data->running = false;
        }
        if (data->mtx) {
            vSemaphoreDelete(data->mtx);
            data->mtx = NULL;
        }
        if (s_current == data) s_current = NULL;
        free(data);
        self->user_data = NULL;
    }
}

static void on_resume(screen_t *self)
{
    subghz_scanner_data_t *data = (subghz_scanner_data_t *)self->user_data;
    /* Restart scanner if user came back from a child screen (Listen,
     * Scanner Settings). start_scanning() picks up any freshly-saved
     * settings from NVS. */
    if (!data->running) {
        start_scanning(data);
    }
    draw_screen(self);
}

screen_t* subghz_scanner_screen_create(void *params)
{
    (void)params;

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    subghz_scanner_data_t *data = calloc(1, sizeof(subghz_scanner_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }

    data->mtx = xSemaphoreCreateMutex();
    data->dot_bright = true;
    data->row_dirty_from = -1;
    data->row_dirty_to = -1;
    data->self = screen;
    s_current = data;

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;

    draw_screen(screen);

    /* Auto-start scanner using NVS-persisted Scanner Settings. */
    start_scanning(data);

    ESP_LOGI(TAG, "Scanner screen created");
    return screen;
}
