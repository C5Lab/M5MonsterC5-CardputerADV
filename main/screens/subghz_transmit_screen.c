/**
 * @file subghz_transmit_screen.c
 * @brief Sub-GHz Transmit screen
 *
 * UART behaviour cloned from coreS3/main/screens/subghz_transmit_screen.c:
 *   on open:   send "subghz_list", collect until "[SUBGHZ_LIST_END]"
 *   on ENTER:  show inline confirm, then "subghz_tx <idx>"
 *
 * Cardputer-only: inline line collection (no port of uart_start_collect),
 * the line callback accumulates [SUBGHZ_LIST] entries and snapshots the list
 * when [SUBGHZ_LIST_END] is seen.
 */

#include "subghz_transmit_screen.h"
#include "subghz_parser.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "SUBGHZ_TX";

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
} tx_signal_t;

typedef enum {
    TX_VIEW_LOADING = 0,
    TX_VIEW_LIST,
    TX_VIEW_CONFIRM,
    TX_VIEW_SENT,
} tx_view_t;

typedef struct {
    tx_signal_t sigs[SUBGHZ_MAX_SIGNALS];
    int  sig_count;
    bool list_complete;
    SemaphoreHandle_t sig_mtx;

    int  selected_index;
    int  scroll_offset;
    tx_view_t view;
    int  confirm_choice;       /* 0 = Send, 1 = Cancel */
    int  last_sent_idx;

    bool needs_redraw;
    screen_t *self;
} subghz_tx_data_t;

static subghz_tx_data_t *s_current = NULL;

static void draw_screen(screen_t *self);
static void redraw_list_window(subghz_tx_data_t *data);
static void redraw_signal_row(subghz_tx_data_t *data, int sig_idx);

static void fill_signal(tx_signal_t *dst, const subghz_signal_info_t *src)
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

static void append_signal_locked(subghz_tx_data_t *data,
                                 const subghz_signal_info_t *src)
{
    if (data->sig_count >= SUBGHZ_MAX_SIGNALS) {
        ESP_LOGW(TAG, "TX list cap reached at %d, dropping rest", data->sig_count);
        return;
    }
    if (src->idx <= 0 && !src->is_raw) return;
    fill_signal(&data->sigs[data->sig_count++], src);
}

static void uart_line_cb(const char *line, void *user_data)
{
    subghz_tx_data_t *data = (subghz_tx_data_t *)user_data;
    if (!data) return;

    /* Detect terminator before parsing the SUBGHZ_LIST line itself */
    if (strstr(line, "[SUBGHZ_LIST_END]")) {
        data->list_complete = true;
        data->needs_redraw = true;
        return;
    }

    subghz_signal_info_t parsed;
    if (!subghz_parse_signal_line(line, &parsed)) return;
    if (parsed.kind != SUBGHZ_SIGNAL_KIND_LIST) return;

    xSemaphoreTake(data->sig_mtx, portMAX_DELAY);
    append_signal_locked(data, &parsed);
    xSemaphoreGive(data->sig_mtx);
    data->needs_redraw = true;
}

static void format_row(const tx_signal_t *sig, char *out, size_t n)
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

static void redraw_signal_row(subghz_tx_data_t *data, int sig_idx)
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

static void redraw_list_window(subghz_tx_data_t *data)
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
    subghz_tx_data_t *data = (subghz_tx_data_t *)self->user_data;

    ui_clear();
    char title[24];
    snprintf(title, sizeof(title), "Transmit (%d)", data->sig_count);
    ui_draw_title(title);

    if (data->sig_count == 0) {
        ui_print_center(3, data->list_complete
                              ? "No stored signals"
                              : "Loading...", UI_COLOR_DIMMED);
    } else {
        redraw_list_window(data);
    }

    if (data->last_sent_idx > 0) {
        char buf[24];
        snprintf(buf, sizeof(buf), "Sent #%d", data->last_sent_idx);
        ui_print(0, 6, buf, UI_COLOR_HIGHLIGHT);
    }

    ui_draw_status("ENT:TX UP/DN ESC:Back");
}

static void draw_confirm_view(screen_t *self)
{
    subghz_tx_data_t *data = (subghz_tx_data_t *)self->user_data;
    ui_clear();
    ui_draw_title("Confirm TX");

    if (data->selected_index < data->sig_count) {
        const tx_signal_t *sig = &data->sigs[data->selected_index];
        char l1[40], l2[40];
        int whole = (int)sig->freq;
        int frac  = ((int)(sig->freq * 100.0f + 0.5f)) % 100;
        snprintf(l1, sizeof(l1), "#%d %.10s %d.%02d", sig->idx, sig->type, whole, frac);
        snprintf(l2, sizeof(l2), "%.14s %.14s",
                 sig->mf[0] ? sig->mf : "--", sig->serial[0] ? sig->serial : "--");
        ui_print_center(2, l1, UI_COLOR_HIGHLIGHT);
        ui_print_center(3, l2, UI_COLOR_DIMMED);
    }

    ui_draw_menu_item(5, "Send",   data->confirm_choice == 0, false, false);
    ui_draw_menu_item(6, "Cancel", data->confirm_choice == 1, false, false);

    ui_draw_status("UP/DN ENTER:Confirm ESC:Cancel");
}

static void draw_screen(screen_t *self)
{
    subghz_tx_data_t *data = (subghz_tx_data_t *)self->user_data;
    switch (data->view) {
        case TX_VIEW_LOADING:
        case TX_VIEW_LIST:
        case TX_VIEW_SENT:
            draw_list_view(self);
            break;
        case TX_VIEW_CONFIRM:
            draw_confirm_view(self);
            break;
    }
}

static void on_tick(screen_t *self)
{
    subghz_tx_data_t *data = (subghz_tx_data_t *)self->user_data;
    if (!data->needs_redraw) return;
    data->needs_redraw = false;

    if (data->view == TX_VIEW_LOADING && data->list_complete) {
        data->view = TX_VIEW_LIST;
        if (data->sig_count > 0) data->selected_index = 0;
    }
    if (data->view != TX_VIEW_CONFIRM) {
        draw_list_view(self);
    }
}

static void redraw_two_rows(subghz_tx_data_t *data, int old_idx, int new_idx)
{
    if (old_idx >= 0) redraw_signal_row(data, old_idx);
    if (new_idx >= 0) redraw_signal_row(data, new_idx);
}

static void perform_tx(screen_t *self)
{
    subghz_tx_data_t *data = (subghz_tx_data_t *)self->user_data;
    if (data->selected_index >= data->sig_count) return;
    int idx = data->sigs[data->selected_index].idx;

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "subghz_tx %d", idx);
    uart_send_command(cmd);
    data->last_sent_idx = idx;
    data->view = TX_VIEW_SENT;
    ESP_LOGI(TAG, "Sent subghz_tx %d", idx);
    draw_list_view(self);
}

static void on_key_list(screen_t *self, key_code_t key)
{
    subghz_tx_data_t *data = (subghz_tx_data_t *)self->user_data;

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
        case KEY_SPACE:
            if (data->sig_count == 0) break;
            data->confirm_choice = 0;
            data->view = TX_VIEW_CONFIRM;
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
    subghz_tx_data_t *data = (subghz_tx_data_t *)self->user_data;

    switch (key) {
        case KEY_UP:
        case KEY_DOWN:
            data->confirm_choice = data->confirm_choice ? 0 : 1;
            draw_confirm_view(self);
            break;

        case KEY_ENTER:
        case KEY_SPACE:
            if (data->confirm_choice == 0) {
                perform_tx(self);
            } else {
                data->view = TX_VIEW_LIST;
                draw_list_view(self);
            }
            break;

        case KEY_ESC:
        case KEY_BACKSPACE:
        case KEY_Q:
            data->view = TX_VIEW_LIST;
            draw_list_view(self);
            break;

        default:
            break;
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    subghz_tx_data_t *data = (subghz_tx_data_t *)self->user_data;
    if (data->view == TX_VIEW_CONFIRM) on_key_confirm(self, key);
    else                                on_key_list(self, key);
}

static void on_destroy(screen_t *self)
{
    subghz_tx_data_t *data = (subghz_tx_data_t *)self->user_data;
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

screen_t* subghz_transmit_screen_create(void *params)
{
    (void)params;

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    subghz_tx_data_t *data = calloc(1, sizeof(subghz_tx_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }

    data->sig_mtx = xSemaphoreCreateMutex();
    data->view = TX_VIEW_LOADING;
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

    ESP_LOGI(TAG, "Transmit screen created");
    return screen;
}
