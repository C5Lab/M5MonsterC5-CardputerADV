/**
 * @file nfc_emulate_screen.c
 * @brief Start and monitor NFC-A emulation
 */

#include "nfc_emulate_screen.h"
#include "nfc_parser.h"
#include "settings.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NFC_EMULATE_TIMEOUT_US (5000LL * 1000LL)

typedef struct {
    int idx;
    bool hint_have_data;
    char hint_type[32];
    bool starting;
    bool running;
    bool start_sent;
    bool redraw;
    bool response_started;
    int64_t deadline_us;
    char status[32];
    char summary[96];
    char capability[40];
    nfc_ui_card_t result;
} nfc_emulate_data_t;

static void print_trimmed(int row, const char *text, uint16_t color)
{
    char line[UI_COLS + 1];
    snprintf(line, sizeof(line), "%.*s", UI_COLS, text ? text : "");
    ui_print(0, row, line, color);
}

static void draw_screen(screen_t *self)
{
    nfc_emulate_data_t *data = self->user_data;
    char line[48];

    ui_clear();
    ui_draw_title("NFC Emulate");
    ui_print_center(1, data->status,
                    data->running ? UI_COLOR_HIGHLIGHT : UI_COLOR_DIMMED);
    snprintf(line, sizeof(line), "Card #%d  %.18s", data->idx, data->hint_type);
    print_trimmed(2, line, UI_COLOR_TEXT);
    print_trimmed(3, data->summary, UI_COLOR_TEXT);
    print_trimmed(4, data->capability, UI_COLOR_DIMMED);
    if (nfc_type_is_classic(data->hint_type)) {
        print_trimmed(5, "Classic Crypto1 not emulated", UI_COLOR_DIMMED);
    }
    print_trimmed(6, "Back sends stop", UI_COLOR_DIMMED);
    ui_draw_status(data->starting ? "Starting - please wait" :
                   "ESC/Q:Stop and back");
}

static void set_capability(nfc_emulate_data_t *data)
{
    if (data->result.emulate_full_ul) {
        snprintf(data->capability, sizeof(data->capability),
                 "NTAG/Ultralight pages");
    } else if (data->result.emulate_uid_only ||
               settings_get_nfc_bus_mode() == NFC_BUS_MODE_I2C) {
        snprintf(data->capability, sizeof(data->capability),
                 "UID/ATQA/SAK only");
    } else if (nfc_type_is_ultralight(data->hint_type) &&
               data->hint_have_data) {
        snprintf(data->capability, sizeof(data->capability),
                 "NTAG/Ultralight pages");
    } else {
        snprintf(data->capability, sizeof(data->capability),
                 "UID/ATQA/SAK only");
    }
}

static void finish_start(nfc_emulate_data_t *data)
{
    uart_set_nfc_transport_synced(true);
    data->starting = false;
    data->deadline_us = 0;
    uart_clear_line_callback();

    if (data->result.have_emulating) {
        data->running = true;
        snprintf(data->status, sizeof(data->status), "Emulating");
        snprintf(data->summary, sizeof(data->summary), "%s",
                 data->result.emulating_summary);
        set_capability(data);
    } else if (data->result.emulate_failed ||
               data->result.no_card_loaded ||
               data->result.load_failed) {
        snprintf(data->status, sizeof(data->status), "Emulate failed");
        snprintf(data->summary, sizeof(data->summary),
                 "Only NFC-A is supported");
    } else {
        snprintf(data->status, sizeof(data->status), "Start failed");
    }
    data->redraw = true;
}

static void uart_line_cb(const char *line, void *user_data)
{
    nfc_emulate_data_t *data = user_data;
    if (!data || !data->starting || !line) return;
    bool is_end = nfc_line_is_end(line);
    if (is_end) {
        if (data->response_started) finish_start(data);
        return;
    }
    nfc_parse_card_line(line, &data->result);
    data->response_started = data->response_started ||
                             data->result.have_emulating ||
                             data->result.emulate_failed ||
                             data->result.no_card_loaded ||
                             data->result.load_failed;
}

static void on_tick(screen_t *self)
{
    nfc_emulate_data_t *data = self->user_data;
    if (data->starting && esp_timer_get_time() >= data->deadline_us) {
        uart_clear_line_callback();
        if (data->starting) {
            data->starting = false;
            snprintf(data->status, sizeof(data->status), "Start timeout");
            if (data->start_sent) {
                uart_send_command("stop");
                data->start_sent = false;
            }
            if (!uart_resync_nfc(5000)) uart_set_nfc_available(false);
            data->running = false;
            snprintf(data->status, sizeof(data->status), "Start timeout");
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
    nfc_emulate_data_t *data = self->user_data;
    if (data->starting) return;
    if (key == KEY_ESC || key == KEY_Q || key == KEY_BACKSPACE) {
        screen_manager_pop();
    }
}

static void on_destroy(screen_t *self)
{
    nfc_emulate_data_t *data = self->user_data;
    uart_clear_line_callback();
    if (data->start_sent) uart_send_command("stop");
    free(data);
}

screen_t *nfc_emulate_screen_create(void *params)
{
    nfc_emulate_params_t *input = params;
    screen_t *screen = screen_alloc();
    nfc_emulate_data_t *data = calloc(1, sizeof(*data));
    if (!screen || !data || !input) {
        free(screen);
        free(data);
        free(input);
        return NULL;
    }

    data->idx = input->idx;
    data->hint_have_data = input->have_data;
    snprintf(data->hint_type, sizeof(data->hint_type), "%s", input->type);
    free(input);

    data->starting = true;
    data->start_sent = true;
    data->response_started = false;
    data->deadline_us = esp_timer_get_time() + NFC_EMULATE_TIMEOUT_US;
    snprintf(data->status, sizeof(data->status), "Starting...");
    nfc_card_reset(&data->result);

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_tick = on_tick;
    screen->on_draw = draw_screen;
    screen->on_destroy = on_destroy;

    uart_register_line_callback(uart_line_cb, data);
    uart_send_command("start_nfc_emulate");
    draw_screen(screen);
    return screen;
}
