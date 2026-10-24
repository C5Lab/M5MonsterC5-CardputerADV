/**
 * @file sta_mac_settings_screen.c
 * @brief Set JanOS STA MAC boot policy via UART (stored in C5 NVS).
 */

#include "sta_mac_settings_screen.h"
#include "text_input_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_timer.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STA_MAC_TIMEOUT_US (5000LL * 1000LL)

#define ITEM_RANDOM  0
#define ITEM_CUSTOM  1
#define ITEM_FACTORY 2
#define ITEM_REBOOT  3
#define ITEM_COUNT   4

typedef enum {
    STA_MAC_OP_NONE = 0,
    STA_MAC_OP_STATUS,
    STA_MAC_OP_SET,
} sta_mac_op_t;

typedef struct {
    int selected;
    sta_mac_op_t op;
    bool needs_redraw;
    bool pending_reboot;
    int64_t deadline_us;
    char active_mac[18];
    char next_mode[12];
    char next_mac[18];
    char status[32];
} sta_mac_data_t;

static const char *const item_labels[] = {
    "Random",
    "Custom...",
    "Factory",
    "Reboot C5",
};

static bool json_copy_quoted(const char *line, const char *key, char *out, size_t out_len)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(line, pat);
    if (!p || out_len == 0) return false;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (strncmp(p, "null", 4) == 0) {
        out[0] = '\0';
        return true;
    }
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_len) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

static bool json_is_true(const char *line, const char *key)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(line, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ') p++;
    return strncmp(p, "true", 4) == 0;
}

static bool parse_fixed_mac(const char *text, char normalized[18])
{
    if (!text || !normalized) return false;
    while (*text && isspace((unsigned char)*text)) text++;

    uint8_t octets[6];
    const char *p = text;
    for (int i = 0; i < 6; i++) {
        if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) {
            return false;
        }
        char pair[3] = {p[0], p[1], '\0'};
        octets[i] = (uint8_t)strtoul(pair, NULL, 16);
        p += 2;
        if (i < 5) {
            if (*p != ':') return false;
            p++;
        }
    }
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '\0') return false;
    if ((octets[0] & 3U) != 2U) return false;

    snprintf(normalized, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             octets[0], octets[1], octets[2], octets[3], octets[4], octets[5]);
    return true;
}

static void begin_command(sta_mac_data_t *data, sta_mac_op_t op, const char *cmd,
                          const char *status)
{
    data->op = op;
    data->deadline_us = esp_timer_get_time() + STA_MAC_TIMEOUT_US;
    snprintf(data->status, sizeof(data->status), "%s", status);
    data->needs_redraw = true;
    uart_send_command(cmd);
}

static void uart_line_cb(const char *line, void *user_data)
{
    sta_mac_data_t *data = user_data;
    if (!data || !line || data->op == STA_MAC_OP_NONE) return;

    if (strstr(line, "\"code\":\"busy\"") || strstr(line, "radio_lease")) {
        snprintf(data->status, sizeof(data->status), "Radio busy");
        data->op = STA_MAC_OP_NONE;
        data->needs_redraw = true;
        return;
    }

    if (strstr(line, "\"record_type\":\"sta_mac_status\"")) {
        json_copy_quoted(line, "active_sta_mac", data->active_mac,
                         sizeof(data->active_mac));
        json_copy_quoted(line, "configured_mode", data->next_mode,
                         sizeof(data->next_mode));
        json_copy_quoted(line, "configured_mac", data->next_mac,
                         sizeof(data->next_mac));
        data->pending_reboot = json_is_true(line, "pending_reboot");
        if (data->pending_reboot) {
            snprintf(data->status, sizeof(data->status), "Reboot needed");
        } else {
            snprintf(data->status, sizeof(data->status), "Ready");
        }
        data->op = STA_MAC_OP_NONE;
        data->needs_redraw = true;
        return;
    }

    if (strstr(line, "\"record_type\":\"sta_mac_result\"")) {
        char result[20];
        json_copy_quoted(line, "result", result, sizeof(result));
        if (strcmp(result, "set") == 0) {
            data->pending_reboot = true;
            json_copy_quoted(line, "configured_mode", data->next_mode,
                             sizeof(data->next_mode));
            json_copy_quoted(line, "configured_mac", data->next_mac,
                             sizeof(data->next_mac));
            snprintf(data->status, sizeof(data->status), "Saved, reboot needed");
        } else if (strcmp(result, "invalid_mac") == 0) {
            snprintf(data->status, sizeof(data->status), "Invalid MAC");
        } else if (strcmp(result, "usage") == 0) {
            snprintf(data->status, sizeof(data->status), "Usage error");
        } else if (strcmp(result, "store_error") == 0) {
            snprintf(data->status, sizeof(data->status), "NVS write failed");
        } else {
            snprintf(data->status, sizeof(data->status), "%.31s", result[0] ? result : "Failed");
        }
        data->op = STA_MAC_OP_NONE;
        data->needs_redraw = true;
    }
}

static void draw_screen(screen_t *self)
{
    sta_mac_data_t *data = self->user_data;
    ui_clear();
    ui_draw_title("STA MAC");

    char line[32];
    if (data->active_mac[0]) {
        snprintf(line, sizeof(line), "Now %.25s", data->active_mac);
    } else {
        snprintf(line, sizeof(line), "Now (unknown)");
    }
    ui_print(0, 1, line, UI_COLOR_DIMMED);

    if (data->next_mac[0]) {
        snprintf(line, sizeof(line), "Next %.25s", data->next_mac);
    } else if (data->next_mode[0]) {
        snprintf(line, sizeof(line), "Next %s", data->next_mode);
    } else {
        snprintf(line, sizeof(line), "Next (unset)");
    }
    ui_print(0, 2, line, UI_COLOR_TEXT);

    for (int i = 0; i < ITEM_COUNT; i++) {
        ui_draw_menu_item(3 + i, item_labels[i], data->selected == i, false, false);
    }

    if (data->op != STA_MAC_OP_NONE) {
        ui_draw_status(data->status);
    } else if (data->status[0]) {
        ui_draw_status(data->status);
    } else {
        ui_draw_status("UP/DN ENTER ESC");
    }
}

static void on_mac_submitted(const char *text, void *user_data)
{
    sta_mac_data_t *data = user_data;
    char mac[18];
    bool ok = parse_fixed_mac(text, mac);
    if (!ok) {
        snprintf(data->status, sizeof(data->status), "Need local unicast");
        screen_manager_pop();
        return;
    }

    screen_manager_pop();
    char cmd[40];
    snprintf(cmd, sizeof(cmd), "sta_mac set fixed %s", mac);
    begin_command(data, STA_MAC_OP_SET, cmd, "Saving...");
}

static void push_custom_input(sta_mac_data_t *data)
{
    text_input_params_t *params = calloc(1, sizeof(*params));
    if (!params) return;
    params->title = "STA MAC";
    params->hint = "02:xx:xx:xx:xx:xx";
    params->on_submit = on_mac_submitted;
    params->user_data = data;
    params->allow_empty = false;
    params->max_length = 17;
    if (data->next_mac[0]) {
        params->initial_text = data->next_mac;
    }
    screen_manager_push(text_input_screen_create, params);
}

static void on_tick(screen_t *self)
{
    sta_mac_data_t *data = self->user_data;
    if (data->op != STA_MAC_OP_NONE &&
        esp_timer_get_time() >= data->deadline_us) {
        data->op = STA_MAC_OP_NONE;
        snprintf(data->status, sizeof(data->status), "Command timeout");
        data->needs_redraw = true;
    }
    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    sta_mac_data_t *data = self->user_data;
    if (data->op != STA_MAC_OP_NONE) {
        if (key == KEY_ESC || key == KEY_Q || key == KEY_BACKSPACE) {
            screen_manager_pop();
        }
        return;
    }

    switch (key) {
        case KEY_UP:
        case KEY_LEFT:
            data->selected = (data->selected + ITEM_COUNT - 1) % ITEM_COUNT;
            draw_screen(self);
            break;
        case KEY_DOWN:
        case KEY_RIGHT:
            data->selected = (data->selected + 1) % ITEM_COUNT;
            draw_screen(self);
            break;
        case KEY_ENTER:
        case KEY_SPACE:
            if (data->selected == ITEM_RANDOM) {
                begin_command(data, STA_MAC_OP_SET, "sta_mac set random", "Saving...");
            } else if (data->selected == ITEM_CUSTOM) {
                push_custom_input(data);
            } else if (data->selected == ITEM_FACTORY) {
                begin_command(data, STA_MAC_OP_SET, "sta_mac set factory", "Saving...");
            } else if (data->selected == ITEM_REBOOT) {
                snprintf(data->status, sizeof(data->status), "Rebooting C5...");
                data->needs_redraw = true;
                uart_send_command("reboot");
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
    sta_mac_data_t *data = self->user_data;
    uart_register_line_callback(uart_line_cb, data);
    draw_screen(self);
}

static void on_destroy(screen_t *self)
{
    uart_clear_line_callback();
    free(self->user_data);
}

screen_t *sta_mac_settings_screen_create(void *params)
{
    (void)params;
    screen_t *screen = screen_alloc();
    sta_mac_data_t *data = calloc(1, sizeof(*data));
    if (!screen || !data) {
        free(screen);
        free(data);
        return NULL;
    }

    snprintf(data->status, sizeof(data->status), "Loading...");
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_tick = on_tick;
    screen->on_draw = draw_screen;
    screen->on_resume = on_resume;
    screen->on_destroy = on_destroy;

    uart_register_line_callback(uart_line_cb, data);
    begin_command(data, STA_MAC_OP_STATUS, "sta_mac status", "Loading...");
    draw_screen(screen);
    return screen;
}
