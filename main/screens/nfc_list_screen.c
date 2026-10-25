/**
 * @file nfc_list_screen.c
 * @brief Saved NFC card library
 */

#include "nfc_list_screen.h"
#include "nfc_detail_screen.h"
#include "nfc_parser.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NFC_LIST_CAP 128
#define NFC_LIST_VISIBLE 5
#define NFC_LIST_TIMEOUT_US (5000LL * 1000LL)

typedef struct {
    nfc_list_entry_t entries[NFC_LIST_CAP];
    int count;
    int selected;
    int scroll;
    bool loading;
    bool redraw;
    bool response_started;
    bool unavailable;
    int64_t deadline_us;
    char status[32];
} nfc_list_data_t;

static void draw_screen(screen_t *self)
{
    nfc_list_data_t *data = self->user_data;
    char line[UI_COLS + 1];

    ui_clear();
    ui_draw_title("NFC List");

    if (!data->loading && data->count == 0) {
        ui_print_center(3, "No saved cards", UI_COLOR_DIMMED);
    } else {
        int end = data->scroll + NFC_LIST_VISIBLE;
        if (end > data->count) end = data->count;
        for (int i = data->scroll; i < end; ++i) {
            snprintf(line, sizeof(line), "%d %.25s",
                     data->entries[i].idx, data->entries[i].name);
            ui_draw_menu_item((i - data->scroll) + 1, line,
                              i == data->selected, false, false);
        }
    }

    if (data->loading) {
        ui_print_center(6, "Loading...", UI_COLOR_DIMMED);
    } else {
        ui_print_center(6, data->status, UI_COLOR_DIMMED);
    }
    ui_draw_status(data->unavailable ? "ESC:Back" :
                   data->loading ? "Please wait..." :
                   "UP/DN ENTER:Open R:Reload");
}

static void finish_list(nfc_list_data_t *data)
{
    uart_set_nfc_transport_synced(true);
    data->loading = false;
    data->deadline_us = 0;
    uart_clear_line_callback();
    if (data->selected >= data->count) {
        data->selected = data->count > 0 ? data->count - 1 : 0;
    }
    if (data->count == 0) {
        data->scroll = 0;
        snprintf(data->status, sizeof(data->status), "No saved cards");
    } else {
        snprintf(data->status, sizeof(data->status), "%d card(s)", data->count);
    }
    data->redraw = true;
}

static void uart_line_cb(const char *line, void *user_data)
{
    nfc_list_data_t *data = user_data;
    if (!data || !data->loading || !line) return;

    bool is_end = nfc_line_is_end(line);
    if (is_end) {
        if (data->response_started) finish_list(data);
        return;
    }

    nfc_list_entry_t entry;
    if (nfc_parse_list_entry(line, &entry)) {
        data->response_started = true;
        if (data->count < NFC_LIST_CAP) {
            data->entries[data->count++] = entry;
        }
    } else {
        int reported = 0;
        if (nfc_parse_card_count(line, &reported) ||
            strstr(line, "[NFC] no saved cards")) {
            data->response_started = true;
        }
    }
}

static void request_list(nfc_list_data_t *data)
{
    data->count = 0;
    data->selected = 0;
    data->scroll = 0;
    data->loading = true;
    data->response_started = false;
    data->deadline_us = esp_timer_get_time() + NFC_LIST_TIMEOUT_US;
    snprintf(data->status, sizeof(data->status), "Loading...");
    data->redraw = true;
    uart_register_line_callback(uart_line_cb, data);
    uart_send_command("nfc_list");
}

static void on_tick(screen_t *self)
{
    nfc_list_data_t *data = self->user_data;
    if (data->loading && esp_timer_get_time() >= data->deadline_us) {
        uart_clear_line_callback();
        if (data->loading) {
            data->loading = false;
            snprintf(data->status, sizeof(data->status), "List timeout");
            if (!uart_resync_nfc(5000)) {
                data->unavailable = true;
                uart_set_nfc_available(false);
                snprintf(data->status, sizeof(data->status), "NFC UART unavailable");
            }
            data->redraw = true;
        }
    }
    if (data->redraw) {
        data->redraw = false;
        draw_screen(self);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    nfc_list_data_t *data = self->user_data;
    if (data->unavailable) {
        if (key == KEY_ESC || key == KEY_Q || key == KEY_BACKSPACE) {
            screen_manager_pop();
        }
        return;
    }
    if (data->loading) return;

    switch (key) {
        case KEY_UP:
            if (data->count > 0) {
                data->selected = (data->selected + data->count - 1) % data->count;
                if (data->selected < data->scroll ||
                    data->selected >= data->scroll + NFC_LIST_VISIBLE) {
                    data->scroll = (data->selected / NFC_LIST_VISIBLE) * NFC_LIST_VISIBLE;
                }
                draw_screen(self);
            }
            break;
        case KEY_DOWN:
            if (data->count > 0) {
                data->selected = (data->selected + 1) % data->count;
                if (data->selected < data->scroll ||
                    data->selected >= data->scroll + NFC_LIST_VISIBLE) {
                    data->scroll = (data->selected / NFC_LIST_VISIBLE) * NFC_LIST_VISIBLE;
                }
                draw_screen(self);
            }
            break;
        case KEY_ENTER:
        case KEY_SPACE:
            if (data->count > 0) {
                nfc_detail_params_t *params = calloc(1, sizeof(*params));
                if (!params) break;
                params->idx = data->entries[data->selected].idx;
                snprintf(params->name, sizeof(params->name), "%s",
                         data->entries[data->selected].name);
                screen_manager_push(nfc_detail_screen_create, params);
            }
            break;
        case KEY_R:
            request_list(data);
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

static void on_resume(screen_t *self)
{
    nfc_list_data_t *data = self->user_data;
    if (!uart_is_nfc_transport_synced()) {
        data->loading = false;
        data->unavailable = true;
        snprintf(data->status, sizeof(data->status), "NFC UART unavailable");
        draw_screen(self);
        return;
    }
    request_list(data);
    draw_screen(self);
}

static void on_destroy(screen_t *self)
{
    uart_clear_line_callback();
    free(self->user_data);
}

screen_t *nfc_list_screen_create(void *params)
{
    (void)params;
    screen_t *screen = screen_alloc();
    nfc_list_data_t *data = calloc(1, sizeof(*data));
    if (!screen || !data) {
        free(screen);
        free(data);
        return NULL;
    }

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_tick = on_tick;
    screen->on_draw = draw_screen;
    screen->on_resume = on_resume;
    screen->on_destroy = on_destroy;
    request_list(data);
    draw_screen(screen);
    return screen;
}
