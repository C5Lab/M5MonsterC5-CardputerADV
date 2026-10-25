/**
 * @file nfc_settings_screen.c
 * @brief Select and probe the JanOS NFC backend
 */

#include "nfc_settings_screen.h"
#include "nfc_parser.h"
#include "settings.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

#define NFC_SETTINGS_TIMEOUT_US (5000LL * 1000LL)

typedef enum {
    NFC_SETTINGS_OP_NONE = 0,
    NFC_SETTINGS_OP_GET,
    NFC_SETTINGS_OP_SET,
    NFC_SETTINGS_OP_INIT,
} nfc_settings_op_t;

typedef struct {
    int selected;
    bool redraw;
    bool response_started;
    bool unavailable;
    atomic_int op;
    atomic_bool completion_pending;
    int64_t deadline_us;
    char status[32];
    nfc_ui_card_t result;
} nfc_settings_data_t;

static const char *const bus_labels[] = {"SPI", "I2C", "PN532"};
static const char *const bus_args[] = {"spi", "i2c", "pn532"};

static void draw_screen(screen_t *self)
{
    nfc_settings_data_t *data = self->user_data;
    ui_clear();
    ui_draw_title("NFC Mode");
    for (int i = 0; i < 3; ++i) {
        ui_draw_menu_item(i + 1, bus_labels[i], data->selected == i,
                          true, settings_get_nfc_bus_mode() == i);
    }
    ui_print_center(5, data->status,
                    atomic_load(&data->op) == NFC_SETTINGS_OP_NONE ?
                    UI_COLOR_DIMMED : UI_COLOR_HIGHLIGHT);
    ui_draw_status(data->unavailable ? "ESC:Back" :
                   atomic_load(&data->op) == NFC_SETTINGS_OP_NONE ?
                   "UP/DN ENTER:Apply ESC:Back" : "Please wait...");
}

static void uart_line_cb(const char *line, void *user_data);

static void begin_command(nfc_settings_data_t *data,
                          nfc_settings_op_t op,
                          const char *command,
                          const char *status)
{
    atomic_store(&data->op, op);
    atomic_store(&data->completion_pending, false);
    data->response_started = false;
    data->deadline_us = esp_timer_get_time() + NFC_SETTINGS_TIMEOUT_US;
    nfc_card_reset(&data->result);
    snprintf(data->status, sizeof(data->status), "%s", status);
    data->redraw = true;
    uart_register_line_callback(uart_line_cb, data);
    uart_send_command(command);
}

static void uart_line_cb(const char *line, void *user_data)
{
    nfc_settings_data_t *data = user_data;
    if (!data || !line) return;
    nfc_settings_op_t op = (nfc_settings_op_t)atomic_load(&data->op);
    if (op == NFC_SETTINGS_OP_NONE) return;

    bool is_end = nfc_line_is_end(line);
    if (!is_end && op == NFC_SETTINGS_OP_GET) {
        int mode = -1;
        if (nfc_parse_bus_line(line, &mode)) {
            data->selected = mode;
            data->response_started = true;
        }
    } else if (!is_end && op == NFC_SETTINGS_OP_SET) {
        if (strstr(line, "[NFC] bus set to")) data->response_started = true;
    } else if (!is_end && op == NFC_SETTINGS_OP_INIT) {
        nfc_parse_card_line(line, &data->result);
        data->response_started = data->response_started ||
                                 data->result.detected ||
                                 data->result.not_detected ||
                                 data->result.not_initialized;
    }

    if (is_end && data->response_started) {
        atomic_store_explicit(&data->completion_pending, true,
                              memory_order_release);
    }
}

static void complete_command(nfc_settings_data_t *data)
{
    nfc_settings_op_t op = (nfc_settings_op_t)atomic_load(&data->op);
    uart_clear_line_callback();
    uart_set_nfc_transport_synced(true);

    if (op == NFC_SETTINGS_OP_SET) {
        settings_set_nfc_bus_mode((nfc_bus_mode_t)data->selected);
        begin_command(data, NFC_SETTINGS_OP_INIT, "init_nfc", "Probing...");
        return;
    }

    if (op == NFC_SETTINGS_OP_GET) {
        settings_set_nfc_bus_mode((nfc_bus_mode_t)data->selected);
        snprintf(data->status, sizeof(data->status), "Current: %s",
                 bus_labels[data->selected]);
    } else {
        bool detected = data->result.detected && !data->result.not_detected;
        uart_set_nfc_available(detected);
        snprintf(data->status, sizeof(data->status),
                 detected ? "NFC ready" : "NFC not detected");
    }
    atomic_store(&data->op, NFC_SETTINGS_OP_NONE);
    data->deadline_us = 0;
    data->redraw = true;
}

static void apply_selected(nfc_settings_data_t *data)
{
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "set_nfc_bus %s", bus_args[data->selected]);
    begin_command(data, NFC_SETTINGS_OP_SET, cmd, "Applying...");
}

static void on_tick(screen_t *self)
{
    nfc_settings_data_t *data = self->user_data;
    if (atomic_exchange_explicit(&data->completion_pending, false,
                                 memory_order_acquire)) {
        complete_command(data);
    }

    nfc_settings_op_t op = (nfc_settings_op_t)atomic_load(&data->op);
    if (op != NFC_SETTINGS_OP_NONE &&
        esp_timer_get_time() >= data->deadline_us) {
        uart_clear_line_callback();
        if (atomic_exchange_explicit(&data->completion_pending, false,
                                     memory_order_acquire)) {
            complete_command(data);
        } else {
            bool init_timed_out = op == NFC_SETTINGS_OP_INIT;
            atomic_store(&data->op, NFC_SETTINGS_OP_NONE);
            snprintf(data->status, sizeof(data->status), "Command timeout");
            bool synced = uart_resync_nfc(5000);
            if (!synced || init_timed_out) uart_set_nfc_available(false);
            if (!synced) {
                data->unavailable = true;
                snprintf(data->status, sizeof(data->status),
                         "NFC UART unavailable");
            }
            atomic_store(&data->completion_pending, false);
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
    nfc_settings_data_t *data = self->user_data;
    if (data->unavailable) {
        if (key == KEY_ESC || key == KEY_Q || key == KEY_BACKSPACE) {
            screen_manager_pop();
        }
        return;
    }
    if (atomic_load(&data->op) != NFC_SETTINGS_OP_NONE) return;

    switch (key) {
        case KEY_UP:
        case KEY_LEFT:
            data->selected = (data->selected + 2) % 3;
            draw_screen(self);
            break;
        case KEY_DOWN:
        case KEY_RIGHT:
            data->selected = (data->selected + 1) % 3;
            draw_screen(self);
            break;
        case KEY_ENTER:
        case KEY_SPACE:
            apply_selected(data);
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

static void on_destroy(screen_t *self)
{
    uart_clear_line_callback();
    free(self->user_data);
}

screen_t *nfc_settings_screen_create(void *params)
{
    (void)params;
    screen_t *screen = screen_alloc();
    nfc_settings_data_t *data = calloc(1, sizeof(*data));
    if (!screen || !data) {
        free(screen);
        free(data);
        return NULL;
    }

    data->selected = settings_get_nfc_bus_mode();
    atomic_init(&data->op, NFC_SETTINGS_OP_NONE);
    atomic_init(&data->completion_pending, false);
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_tick = on_tick;
    screen->on_draw = draw_screen;
    screen->on_destroy = on_destroy;

    begin_command(data, NFC_SETTINGS_OP_GET, "get_nfc_bus", "Loading...");
    draw_screen(screen);
    return screen;
}
