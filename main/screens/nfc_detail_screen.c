/**
 * @file nfc_detail_screen.c
 * @brief Load a saved NFC card and expose card actions
 */

#include "nfc_detail_screen.h"
#include "nfc_emulate_screen.h"
#include "nfc_parser.h"
#include "text_input_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NFC_DETAIL_TIMEOUT_US (5000LL * 1000LL)

typedef enum {
    NFC_DETAIL_OP_NONE = 0,
    NFC_DETAIL_OP_LOAD,
    NFC_DETAIL_OP_RENAME,
    NFC_DETAIL_OP_DELETE,
} nfc_detail_op_t;

typedef struct {
    int idx;
    char name[64];
    char filename[64];
    int action;
    int confirm_choice;
    bool confirming_delete;
    bool loaded;
    bool redraw;
    bool leave_to_list;
    bool response_started;
    bool unavailable;
    nfc_detail_op_t op;
    int64_t deadline_us;
    char status[32];
    nfc_ui_card_t card;
    nfc_ui_card_t result;
} nfc_detail_data_t;

static void print_trimmed(int row, const char *text, uint16_t color)
{
    char line[UI_COLS + 1];
    snprintf(line, sizeof(line), "%.*s", UI_COLS, text ? text : "");
    ui_print(0, row, line, color);
}

static void draw_screen(screen_t *self)
{
    nfc_detail_data_t *data = self->user_data;
    char line[64];

    ui_clear();
    if (data->confirming_delete) {
        ui_draw_title("Delete NFC card?");
        print_trimmed(2, data->name, UI_COLOR_TEXT);
        ui_draw_menu_item(4, "Delete", data->confirm_choice == 0, false, false);
        ui_draw_menu_item(5, "Cancel", data->confirm_choice == 1, false, false);
        ui_draw_status("UP/DN ENTER ESC:Cancel");
        return;
    }

    ui_draw_title("NFC Card");
    print_trimmed(1, data->status, data->loaded ? UI_COLOR_HIGHLIGHT : UI_COLOR_DIMMED);

    if (data->card.type[0]) {
        snprintf(line, sizeof(line), "%s%s", data->card.type,
                 data->card.have_data ? " [data]" : "");
        print_trimmed(2, line, UI_COLOR_TEXT);
    }
    if (data->card.uid[0]) {
        if (data->card.have_atqa_sak) {
            snprintf(line, sizeof(line), "UID %.18s SAK %02X",
                     data->card.uid, data->card.sak);
        } else {
            snprintf(line, sizeof(line), "UID %.26s", data->card.uid);
        }
        print_trimmed(3, line, UI_COLOR_TEXT);
    }

    ui_draw_menu_item(4, "Emulate", data->action == 0, false, false);
    ui_draw_menu_item(5, "Rename", data->action == 1, false, false);
    ui_draw_menu_item(6, "Delete", data->action == 2, false, false);
    ui_draw_status(data->unavailable ? "ESC:Back" :
                   data->op == NFC_DETAIL_OP_NONE ?
                   "UP/DN ENTER ESC:Back" : "Please wait...");
}

static void finish_operation(nfc_detail_data_t *data)
{
    uart_set_nfc_transport_synced(true);
    nfc_detail_op_t completed = data->op;
    data->op = NFC_DETAIL_OP_NONE;
    data->deadline_us = 0;
    uart_clear_line_callback();

    if (completed == NFC_DETAIL_OP_LOAD) {
        if (data->card.have_loaded &&
            !data->card.load_failed &&
            !data->card.not_detected &&
            !data->card.not_initialized) {
            data->loaded = true;
            nfc_path_basename(data->card.loaded_path, data->name,
                              sizeof(data->name), true);
            nfc_path_basename(data->card.loaded_path, data->filename,
                              sizeof(data->filename), false);
            snprintf(data->status, sizeof(data->status), "%.31s", data->name);
        } else {
            data->loaded = false;
            snprintf(data->status, sizeof(data->status), "Load failed");
        }
    } else if (completed == NFC_DETAIL_OP_RENAME) {
        if (data->result.have_renamed && !data->result.rename_failed) {
            data->leave_to_list = true;
        } else {
            snprintf(data->status, sizeof(data->status), "Rename failed");
        }
    } else if (completed == NFC_DETAIL_OP_DELETE) {
        if (data->result.have_deleted && !data->result.delete_failed) {
            data->leave_to_list = true;
        } else {
            snprintf(data->status, sizeof(data->status), "Delete failed");
        }
    }
    data->redraw = true;
}

static void uart_line_cb(const char *line, void *user_data)
{
    nfc_detail_data_t *data = user_data;
    if (!data || data->op == NFC_DETAIL_OP_NONE || !line) return;

    bool is_end = nfc_line_is_end(line);
    if (is_end) {
        if (data->response_started) finish_operation(data);
        return;
    }

    nfc_ui_card_t *target = data->op == NFC_DETAIL_OP_LOAD ?
                            &data->card : &data->result;
    nfc_parse_card_line(line, target);
    if (data->op == NFC_DETAIL_OP_LOAD) {
        data->response_started = data->response_started ||
                                 target->have_loaded ||
                                 target->load_failed ||
                                 target->not_detected ||
                                 target->not_initialized;
    } else if (data->op == NFC_DETAIL_OP_RENAME) {
        data->response_started = data->response_started ||
                                 target->have_renamed ||
                                 target->rename_failed ||
                                 target->load_failed;
    } else {
        data->response_started = data->response_started ||
                                 target->have_deleted ||
                                 target->delete_failed ||
                                 target->load_failed;
    }
}

static void start_operation(nfc_detail_data_t *data, nfc_detail_op_t op,
                            const char *cmd, const char *status)
{
    if (op == NFC_DETAIL_OP_LOAD) nfc_card_reset(&data->card);
    else nfc_card_reset(&data->result);
    data->op = op;
    data->response_started = false;
    data->deadline_us = esp_timer_get_time() + NFC_DETAIL_TIMEOUT_US;
    snprintf(data->status, sizeof(data->status), "%s", status);
    data->redraw = true;
    uart_register_line_callback(uart_line_cb, data);
    uart_send_command(cmd);
}

static void start_load(nfc_detail_data_t *data)
{
    char cmd[96];
    data->loaded = false;
    snprintf(cmd, sizeof(cmd), "nfc_load %s", data->filename);
    start_operation(data, NFC_DETAIL_OP_LOAD, cmd, "Loading...");
}

static void on_rename_submit(const char *text, void *user_data)
{
    nfc_detail_data_t *data = user_data;
    bool valid = nfc_name_is_valid(text);
    char new_name[64];
    snprintf(new_name, sizeof(new_name), "%s", text ? text : "");
    screen_manager_pop();

    if (!valid) {
        snprintf(data->status, sizeof(data->status), "Invalid name");
        draw_screen(screen_manager_get_current());
        return;
    }

    char cmd[144];
    snprintf(cmd, sizeof(cmd), "nfc_rename %s %s", data->filename, new_name);
    start_operation(data, NFC_DETAIL_OP_RENAME, cmd, "Renaming...");
}

static void show_rename_input(nfc_detail_data_t *data)
{
    text_input_params_t *params = calloc(1, sizeof(*params));
    if (!params) return;
    params->title = "Rename card";
    params->hint = "a-z 0-9 _ -";
    params->initial_text = data->name;
    params->on_submit = on_rename_submit;
    params->user_data = data;
    params->max_length = 48;
    screen_manager_push(text_input_screen_create, params);
}

static void start_delete(nfc_detail_data_t *data)
{
    char cmd[96];
    data->confirming_delete = false;
    snprintf(cmd, sizeof(cmd), "nfc_delete %s", data->filename);
    start_operation(data, NFC_DETAIL_OP_DELETE, cmd, "Deleting...");
}

static void open_emulate(nfc_detail_data_t *data)
{
    nfc_emulate_params_t *params = calloc(1, sizeof(*params));
    if (!params) return;
    params->idx = data->idx;
    params->have_data = data->card.have_data;
    snprintf(params->type, sizeof(params->type), "%s", data->card.type);
    screen_manager_push(nfc_emulate_screen_create, params);
}

static void activate_action(nfc_detail_data_t *data)
{
    if (!data->loaded) return;
    if (data->action == 0) {
        open_emulate(data);
    } else if (data->action == 1) {
        show_rename_input(data);
    } else {
        data->confirming_delete = true;
        data->confirm_choice = 1;
        draw_screen(screen_manager_get_current());
    }
}

static void on_tick(screen_t *self)
{
    nfc_detail_data_t *data = self->user_data;
    if (data->op != NFC_DETAIL_OP_NONE &&
        esp_timer_get_time() >= data->deadline_us) {
        uart_clear_line_callback();
        if (data->op != NFC_DETAIL_OP_NONE) {
            data->op = NFC_DETAIL_OP_NONE;
            snprintf(data->status, sizeof(data->status), "Command timeout");
            if (!uart_resync_nfc(5000)) {
                data->unavailable = true;
                uart_set_nfc_available(false);
                snprintf(data->status, sizeof(data->status), "NFC UART unavailable");
            }
            data->redraw = true;
        }
    }
    if (data->leave_to_list) {
        data->leave_to_list = false;
        screen_manager_pop();
        return;
    }
    if (data->redraw) {
        data->redraw = false;
        draw_screen(self);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    nfc_detail_data_t *data = self->user_data;
    if (data->unavailable) {
        if (key == KEY_ESC || key == KEY_Q || key == KEY_BACKSPACE) {
            screen_manager_pop();
        }
        return;
    }
    if (data->op != NFC_DETAIL_OP_NONE) return;

    if (data->confirming_delete) {
        if (key == KEY_UP || key == KEY_DOWN) {
            data->confirm_choice = 1 - data->confirm_choice;
            draw_screen(self);
        } else if (key == KEY_ENTER || key == KEY_SPACE) {
            if (data->confirm_choice == 0) start_delete(data);
            else {
                data->confirming_delete = false;
                draw_screen(self);
            }
        } else if (key == KEY_ESC || key == KEY_Q || key == KEY_BACKSPACE) {
            data->confirming_delete = false;
            draw_screen(self);
        }
        return;
    }

    switch (key) {
        case KEY_UP:
            data->action = (data->action + 2) % 3;
            draw_screen(self);
            break;
        case KEY_DOWN:
            data->action = (data->action + 1) % 3;
            draw_screen(self);
            break;
        case KEY_ENTER:
        case KEY_SPACE:
            activate_action(data);
            break;
        case KEY_E:
            if (data->loaded) open_emulate(data);
            break;
        case KEY_R:
            if (data->loaded) show_rename_input(data);
            break;
        case KEY_D:
            if (data->loaded) {
                data->confirming_delete = true;
                data->confirm_choice = 1;
                draw_screen(self);
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
    nfc_detail_data_t *data = self->user_data;
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

screen_t *nfc_detail_screen_create(void *params)
{
    nfc_detail_params_t *input = params;
    screen_t *screen = screen_alloc();
    nfc_detail_data_t *data = calloc(1, sizeof(*data));
    if (!screen || !data || !input) {
        free(screen);
        free(data);
        free(input);
        return NULL;
    }

    data->idx = input->idx;
    nfc_path_basename(input->name, data->name, sizeof(data->name), true);
    nfc_path_basename(input->name, data->filename, sizeof(data->filename), false);
    free(input);

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_tick = on_tick;
    screen->on_draw = draw_screen;
    screen->on_resume = on_resume;
    screen->on_destroy = on_destroy;
    start_load(data);
    draw_screen(screen);
    return screen;
}
