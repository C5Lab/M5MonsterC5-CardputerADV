/**
 * @file nfc_read_screen.c
 * @brief Read an NFC card and save the last read card
 */

#include "nfc_read_screen.h"
#include "nfc_parser.h"
#include "text_input_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NFC_READ_TIMEOUT_US (20000LL * 1000LL)
#define NFC_SHORT_TIMEOUT_US (5000LL * 1000LL)

typedef enum {
    NFC_READ_OP_NONE = 0,
    NFC_READ_OP_READ,
    NFC_READ_OP_SAVE,
} nfc_read_op_t;

typedef struct {
    nfc_read_op_t op;
    nfc_ui_card_t card;
    nfc_ui_card_t result;
    bool have_card;
    bool redraw;
    bool response_started;
    bool unavailable;
    int64_t deadline_us;
    char status[32];
} nfc_read_data_t;

static void print_trimmed(int row, const char *text, uint16_t color)
{
    char line[UI_COLS + 1];
    snprintf(line, sizeof(line), "%.*s", UI_COLS, text ? text : "");
    ui_print(0, row, line, color);
}

static void draw_screen(screen_t *self)
{
    nfc_read_data_t *data = self->user_data;
    char line[64];

    ui_clear();
    ui_draw_title("NFC Read");
    print_trimmed(1, data->status, data->have_card ? UI_COLOR_HIGHLIGHT : UI_COLOR_DIMMED);

    if (data->card.type[0]) print_trimmed(2, data->card.type, UI_COLOR_TEXT);
    if (data->card.uid[0]) {
        snprintf(line, sizeof(line), "UID %.26s", data->card.uid);
        print_trimmed(3, line, UI_COLOR_TEXT);
        if (strlen(data->card.uid) > 26) {
            print_trimmed(4, data->card.uid + 26, UI_COLOR_TEXT);
        }
    }
    if (data->card.have_atqa_sak) {
        snprintf(line, sizeof(line), "ATQA %02X %02X SAK %02X%s",
                 data->card.atqa[0], data->card.atqa[1], data->card.sak,
                 data->card.have_data ? " DATA" : "");
        print_trimmed(5, line, UI_COLOR_DIMMED);
    } else if (data->card.have_idm) {
        snprintf(line, sizeof(line), "IDm %s", data->card.idm);
        print_trimmed(5, line, UI_COLOR_DIMMED);
    } else if (data->card.have_data) {
        snprintf(line, sizeof(line), "Data %u bytes", data->card.data_len);
        print_trimmed(5, line, UI_COLOR_DIMMED);
    }

    print_trimmed(6, data->have_card ? "ENTER:Read  S:Save" : "ENTER:Read",
                  UI_COLOR_TEXT);
    ui_draw_status(data->unavailable ? "ESC:Back" :
                   data->op == NFC_READ_OP_NONE ?
                   "ESC:Back" : "NFC command - please wait");
}

static void finish_read(nfc_read_data_t *data)
{
    if (data->card.no_card) {
        snprintf(data->status, sizeof(data->status), "No card detected");
        data->have_card = false;
    } else if (data->card.not_detected || data->card.not_initialized) {
        snprintf(data->status, sizeof(data->status), "NFC not ready");
        data->have_card = false;
        uart_set_nfc_available(false);
    } else if (data->card.type[0] || data->card.uid[0]) {
        snprintf(data->status, sizeof(data->status), "Card read");
        data->have_card = true;
    } else {
        snprintf(data->status, sizeof(data->status), "No result");
        data->have_card = false;
    }
}

static void finish_save(nfc_read_data_t *data)
{
    if (data->result.have_saved) {
        snprintf(data->status, sizeof(data->status), "Saved");
    } else if (data->result.nothing_to_save) {
        snprintf(data->status, sizeof(data->status), "Nothing to save");
        data->have_card = false;
    } else {
        snprintf(data->status, sizeof(data->status), "Save failed");
    }
}

static void finish_operation(nfc_read_data_t *data)
{
    uart_set_nfc_transport_synced(true);
    nfc_read_op_t completed = data->op;
    data->op = NFC_READ_OP_NONE;
    data->deadline_us = 0;
    uart_clear_line_callback();

    if (completed == NFC_READ_OP_READ) finish_read(data);
    else if (completed == NFC_READ_OP_SAVE) finish_save(data);
    data->redraw = true;
}

static void uart_line_cb(const char *line, void *user_data)
{
    nfc_read_data_t *data = user_data;
    if (!data || data->op == NFC_READ_OP_NONE || !line) return;

    bool is_end = nfc_line_is_end(line);
    if (is_end) {
        if (data->response_started) finish_operation(data);
        return;
    }

    nfc_ui_card_t *target = data->op == NFC_READ_OP_READ ?
                            &data->card : &data->result;
    nfc_parse_card_line(line, target);
    if (data->op == NFC_READ_OP_READ) {
        data->response_started = data->response_started ||
            strstr(line, "[NFC] present a card") ||
            strncmp(line, "[NFC] type: ", 12) == 0 ||
            strstr(line, "[NFC] no card detected") ||
            strstr(line, "[NFC] not detected") ||
            strstr(line, "[NFC] not initialized");
    } else {
        data->response_started = data->response_started ||
            strncmp(line, "[NFC] saved: ", 13) == 0 ||
            strstr(line, "[NFC] nothing to save") ||
            strstr(line, "[NFC] save failed") ||
            strstr(line, "Usage: nfc_save");
    }

    if (strstr(line, "[NFC] present a card")) {
        snprintf(data->status, sizeof(data->status), "Present a card...");
        data->redraw = true;
    } else if (data->op == NFC_READ_OP_READ &&
               strncmp(line, "[NFC] type: ", 12) == 0) {
        snprintf(data->status, sizeof(data->status), "Reading...");
        data->redraw = true;
    }
}

static void start_read(nfc_read_data_t *data)
{
    nfc_card_reset(&data->card);
    data->have_card = false;
    data->op = NFC_READ_OP_READ;
    data->response_started = false;
    data->deadline_us = esp_timer_get_time() + NFC_READ_TIMEOUT_US;
    snprintf(data->status, sizeof(data->status), "Present a card...");
    data->redraw = true;
    uart_register_line_callback(uart_line_cb, data);
    uart_send_command("nfc_read");
}

static void start_save(nfc_read_data_t *data, const char *name)
{
    char cmd[96];
    nfc_card_reset(&data->result);
    data->op = NFC_READ_OP_SAVE;
    data->response_started = false;
    data->deadline_us = esp_timer_get_time() + NFC_SHORT_TIMEOUT_US;
    snprintf(data->status, sizeof(data->status), "Saving...");
    data->redraw = true;
    snprintf(cmd, sizeof(cmd), "nfc_save %s", name);
    uart_register_line_callback(uart_line_cb, data);
    uart_send_command(cmd);
}

static void on_save_submit(const char *text, void *user_data)
{
    nfc_read_data_t *data = user_data;
    bool valid = nfc_name_is_valid(text);
    char name[64];
    snprintf(name, sizeof(name), "%s", text ? text : "");
    screen_manager_pop();

    if (!valid) {
        snprintf(data->status, sizeof(data->status), "Invalid name");
        draw_screen(screen_manager_get_current());
        return;
    }
    start_save(data, name);
}

static void show_save_input(nfc_read_data_t *data)
{
    text_input_params_t *params = calloc(1, sizeof(*params));
    if (!params) return;
    params->title = "Save card";
    params->hint = "a-z 0-9 _ -";
    params->on_submit = on_save_submit;
    params->user_data = data;
    params->max_length = 48;
    screen_manager_push(text_input_screen_create, params);
}

static void on_tick(screen_t *self)
{
    nfc_read_data_t *data = self->user_data;
    if (data->op != NFC_READ_OP_NONE &&
        esp_timer_get_time() >= data->deadline_us) {
        uart_clear_line_callback();
        if (data->op == NFC_READ_OP_NONE) {
            data->redraw = true;
        } else {
            nfc_read_op_t timed_out = data->op;
            data->op = NFC_READ_OP_NONE;
            snprintf(data->status, sizeof(data->status),
                     timed_out == NFC_READ_OP_READ ?
                     "Read timeout" : "Save timeout");
            if (timed_out == NFC_READ_OP_READ) data->have_card = false;
            if (!uart_resync_nfc(5000)) {
                data->unavailable = true;
                uart_set_nfc_available(false);
                snprintf(data->status, sizeof(data->status),
                         "NFC UART unavailable");
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
    nfc_read_data_t *data = self->user_data;
    if (data->unavailable) {
        if (key == KEY_ESC || key == KEY_Q || key == KEY_BACKSPACE) {
            screen_manager_pop();
        }
        return;
    }
    if (data->op != NFC_READ_OP_NONE) return;

    switch (key) {
        case KEY_ENTER:
        case KEY_R:
            start_read(data);
            break;
        case KEY_S:
            if (data->have_card) show_save_input(data);
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
    nfc_read_data_t *data = self->user_data;
    if (!uart_is_nfc_transport_synced()) {
        data->unavailable = true;
        snprintf(data->status, sizeof(data->status), "NFC UART unavailable");
    }
    draw_screen(self);
}

static void on_destroy(screen_t *self)
{
    uart_clear_line_callback();
    free(self->user_data);
}

screen_t *nfc_read_screen_create(void *params)
{
    (void)params;
    screen_t *screen = screen_alloc();
    nfc_read_data_t *data = calloc(1, sizeof(*data));
    if (!screen || !data) {
        free(screen);
        free(data);
        return NULL;
    }

    snprintf(data->status, sizeof(data->status), "Ready");
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_tick = on_tick;
    screen->on_draw = draw_screen;
    screen->on_resume = on_resume;
    screen->on_destroy = on_destroy;
    draw_screen(screen);
    return screen;
}
