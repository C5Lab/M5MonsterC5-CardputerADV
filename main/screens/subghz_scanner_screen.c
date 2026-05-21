/**
 * @file subghz_scanner_screen.c
 * @brief Sub-GHz traffic scanner
 *
 * UART behaviour cloned from coreS3/main/screens/subghz_scanner_screen.c:
 *   start:  subghz_scanner
 *   stop:   subghz_stop
 *
 * Parses [SUBGHZ_SCAN_HIT] freq=... edges=... rssi=... into an MRU list.
 * ENTER on a frequency stops the scanner and pushes the Listen screen pre-filled
 * with that frequency, auto-starting RX.
 */

#include "subghz_scanner_screen.h"
#include "subghz_listen_screen.h"
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
    bool needs_redraw;

    screen_t *self;
} subghz_scanner_data_t;

static subghz_scanner_data_t *s_current = NULL;

static void draw_screen(screen_t *self);
static void redraw_freq_row(subghz_scanner_data_t *data, int idx);
static void redraw_list_window(subghz_scanner_data_t *data);
static void redraw_status_row(subghz_scanner_data_t *data);

static void mru_insert_locked(subghz_scanner_data_t *data, float freq, int rssi, unsigned edges)
{
    int existing = -1;
    for (int i = 0; i < data->freq_count; i++) {
        if (fabsf(data->freqs[i].freq - freq) < 0.005f) {
            existing = i;
            break;
        }
    }
    int from;
    if (existing >= 0) {
        from = existing;
    } else if (data->freq_count < MAX_FREQS) {
        from = data->freq_count;
        data->freq_count++;
    } else {
        from = MAX_FREQS - 1;
    }
    for (int i = from; i > 0; i--) {
        data->freqs[i] = data->freqs[i - 1];
    }
    data->freqs[0].freq = freq;
    data->freqs[0].rssi = rssi;
    data->freqs[0].edges = edges;
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
        data->needs_redraw = true;
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
    mru_insert_locked(data, rounded, rssi, edges);
    xSemaphoreGive(data->mtx);
    data->needs_redraw = true;
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
}

static void redraw_status_row(subghz_scanner_data_t *data)
{
    int y = 1 * 16;
    display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);

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

    if (data->freq_count == 0) {
        ui_print_center(4, "Listening...", UI_COLOR_DIMMED);
    }
    ui_draw_status("ENT:Listen UP/DN ESC:Back");
}

static void on_tick(screen_t *self)
{
    subghz_scanner_data_t *data = (subghz_scanner_data_t *)self->user_data;
    if (!data->needs_redraw) return;
    data->needs_redraw = false;

    xSemaphoreTake(data->mtx, portMAX_DELAY);
    bool pulse = data->pass_pulse;
    data->pass_pulse = false;
    xSemaphoreGive(data->mtx);

    if (pulse) data->dot_bright = !data->dot_bright;

    redraw_status_row(data);
    redraw_list_window(data);
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
            if (data->freq_count == 0) break;
            if (data->selected_index < data->freq_count - 1) {
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
        case KEY_SPACE:
            on_pick_freq(self);
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
    /* Restart scanner if user came back from a child screen (e.g. Listen) */
    if (!data->running) {
        uart_send_command("subghz_stop");
        data->running = true;
        uart_register_line_callback(uart_line_cb, data);
        uart_send_command("subghz_scanner");
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
    data->self = screen;
    s_current = data;

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;

    draw_screen(screen);

    /* Auto-start scanner */
    uart_send_command("subghz_stop");
    data->running = true;
    uart_register_line_callback(uart_line_cb, data);
    uart_send_command("subghz_scanner");

    ESP_LOGI(TAG, "Scanner screen created");
    return screen;
}
