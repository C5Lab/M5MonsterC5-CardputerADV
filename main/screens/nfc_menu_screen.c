/**
 * @file nfc_menu_screen.c
 * @brief NFC hub with hardware probe
 */

#include "nfc_menu_screen.h"
#include "nfc_read_screen.h"
#include "nfc_list_screen.h"
#include "nfc_parser.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_timer.h"
#include <stdlib.h>

#define NFC_INIT_TIMEOUT_US (3000LL * 1000LL)

typedef struct {
    int selected;
    bool probing;
    bool response_started;
    bool ready;
    bool unavailable;
    bool redraw;
    int64_t deadline_us;
    nfc_ui_card_t probe_result;
} nfc_menu_data_t;

static void draw_screen(screen_t *self)
{
    nfc_menu_data_t *data = self->user_data;
    ui_clear();
    ui_draw_title("NFC");

    ui_draw_menu_item(2, data->ready ? "Read" : "Read (unavailable)",
                      data->selected == 0, false, false);
    ui_draw_menu_item(3, "List", data->selected == 1, false, false);

    if (data->unavailable) {
        ui_print_center(5, "NFC UART unavailable", RGB565(255, 80, 80));
    } else if (data->probing) {
        ui_print_center(5, "Probing...", UI_COLOR_DIMMED);
    } else if (data->ready) {
        ui_print_center(5, "NFC ready", UI_COLOR_HIGHLIGHT);
    } else {
        ui_print_center(5, "NFC not detected", RGB565(255, 80, 80));
    }
    ui_draw_status(data->unavailable ? "ESC:Back" :
                   data->probing ? "Please wait..." :
                   "UP/DN ENTER:Open ESC:Back");
}

static void finish_probe(nfc_menu_data_t *data, bool ready)
{
    data->probing = false;
    data->ready = ready;
    data->redraw = true;
    data->deadline_us = 0;
    uart_clear_line_callback();
    uart_set_nfc_available(ready);
}

static void uart_line_cb(const char *line, void *user_data)
{
    nfc_menu_data_t *data = user_data;
    if (!data || !data->probing || !line) return;

    bool is_end = nfc_line_is_end(line);
    if (!is_end) {
        nfc_parse_card_line(line, &data->probe_result);
        data->response_started = data->probe_result.detected ||
                                 data->probe_result.not_detected ||
                                 data->probe_result.not_initialized;
    }
    if (is_end && data->response_started) {
        uart_set_nfc_transport_synced(true);
        finish_probe(data, data->probe_result.detected &&
                           !data->probe_result.not_detected);
    }
}

static void start_probe(nfc_menu_data_t *data)
{
    if (!uart_is_nfc_transport_synced()) {
        data->probing = false;
        data->ready = false;
        data->unavailable = true;
        data->redraw = true;
        return;
    }
    nfc_card_reset(&data->probe_result);
    data->probing = true;
    data->unavailable = false;
    data->response_started = false;
    data->ready = false;
    data->deadline_us = esp_timer_get_time() + NFC_INIT_TIMEOUT_US;
    data->redraw = true;
    uart_register_line_callback(uart_line_cb, data);
    uart_send_command("init_nfc");
}

static void on_tick(screen_t *self)
{
    nfc_menu_data_t *data = self->user_data;
    if (data->probing && esp_timer_get_time() >= data->deadline_us) {
        uart_clear_line_callback();
        if (!data->probing) return;
        data->probing = false;
        bool synced = uart_resync_nfc(5000);
        if (!synced) {
            data->unavailable = true;
            uart_set_nfc_available(false);
        }
        finish_probe(data, false);
    }
    if (data->redraw) {
        data->redraw = false;
        draw_screen(self);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    nfc_menu_data_t *data = self->user_data;
    if (data->unavailable) {
        if (key == KEY_ESC || key == KEY_Q || key == KEY_BACKSPACE) {
            screen_manager_pop();
        }
        return;
    }
    if (data->probing) return;

    switch (key) {
        case KEY_UP:
        case KEY_DOWN:
            data->selected = 1 - data->selected;
            draw_screen(self);
            break;
        case KEY_ENTER:
        case KEY_SPACE:
            if (data->selected == 0 && data->ready) {
                screen_manager_push(nfc_read_screen_create, NULL);
            } else if (data->selected == 1) {
                screen_manager_push(nfc_list_screen_create, NULL);
            }
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
    nfc_menu_data_t *data = self->user_data;
    start_probe(data);
    draw_screen(self);
}

static void on_destroy(screen_t *self)
{
    uart_clear_line_callback();
    free(self->user_data);
}

screen_t *nfc_menu_screen_create(void *params)
{
    (void)params;
    screen_t *screen = screen_alloc();
    nfc_menu_data_t *data = calloc(1, sizeof(*data));
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

    start_probe(data);
    draw_screen(screen);
    return screen;
}
