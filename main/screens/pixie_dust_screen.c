/**
 * @file pixie_dust_screen.c
 * @brief Run wps pixie against one scanned AP and show the result.
 */

#include "pixie_dust_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "buzzer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "PIXIE_DUST";

#define START_TIMEOUT_US  (4000LL * 1000LL)

typedef enum {
    PIXIE_STARTING = 0,
    PIXIE_CAPTURE,
    PIXIE_SEARCH,
    PIXIE_VERIFY,
    PIXIE_DONE,
    PIXIE_ERROR,
} pixie_phase_t;

typedef struct {
    wifi_network_t network;
    pixie_phase_t phase;
    char stage_label[20];
    char result_label[24];
    char pin[12];
    char psk[32];
    bool found;
    bool finished;
    bool stop_sent;
    bool reveal_sent;
    bool needs_redraw;
    int64_t start_deadline_us;
} pixie_data_t;

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
        if (*p == '\\' && p[1]) {
            p++;
        }
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

static const char *map_operation_result(const char *result)
{
    if (!result || !result[0]) return "Failed";
    if (strcmp(result, "pixie_pin_and_m7_verified") == 0) return "Vulnerable";
    if (strcmp(result, "no_match") == 0 || strcmp(result, "partial_match") == 0)
        return "Not vulnerable";
    if (strcmp(result, "cancelled") == 0) return "Stopped";
    if (strncmp(result, "capture", 7) == 0 ||
        strcmp(result, "pixie_capture_failed") == 0)
        return "Capture failed";
    if (strcmp(result, "pixie_search_error") == 0) return "Search error";
    if (strcmp(result, "recovered_pin_rejected") == 0) return "PIN rejected";
    if (strcmp(result, "verification_session_failed") == 0) return "Verify failed";
    if (strcmp(result, "credential_missing") == 0) return "No vault";
    return result;
}

static void parse_sensitive_line(const char *line, pixie_data_t *data)
{
    const char *pin = strstr(line, "recovered_pin=");
    if (pin) {
        pin += strlen("recovered_pin=");
        size_t i = 0;
        while (*pin && !isspace((unsigned char)*pin) && i + 1 < sizeof(data->pin)) {
            data->pin[i++] = *pin++;
        }
        data->pin[i] = '\0';
    }
    const char *key = strstr(line, "network_key=\"");
    if (key) {
        key += strlen("network_key=\"");
        size_t i = 0;
        while (*key && *key != '"' && i + 1 < sizeof(data->psk)) {
            if (*key == '\\' && key[1]) key++;
            data->psk[i++] = *key++;
        }
        data->psk[i] = '\0';
    }
}

static void uart_line_callback(const char *line, void *user_data)
{
    pixie_data_t *data = (pixie_data_t *)user_data;
    if (!data || !line || !line[0]) return;

    if (strstr(line, "\"code\":\"busy\"") || strstr(line, "radio_lease")) {
        data->phase = PIXIE_ERROR;
        data->finished = true;
        snprintf(data->result_label, sizeof(data->result_label), "Radio busy");
        snprintf(data->stage_label, sizeof(data->stage_label), "Refused");
        data->needs_redraw = true;
        return;
    }

    if (data->phase == PIXIE_STARTING &&
        strstr(line, "Command returned non-zero")) {
        data->phase = PIXIE_ERROR;
        data->finished = true;
        snprintf(data->result_label, sizeof(data->result_label), "Refused");
        snprintf(data->stage_label, sizeof(data->stage_label), "Error");
        data->needs_redraw = true;
        return;
    }

    if (strstr(line, "\"record_type\":\"accepted\"")) {
        data->phase = PIXIE_CAPTURE;
        snprintf(data->stage_label, sizeof(data->stage_label), "Capture");
        data->needs_redraw = true;
        return;
    }

    char stage[32];
    if (json_copy_quoted(line, "stage", stage, sizeof(stage))) {
        if (strncmp(stage, "pixie_capture", 13) == 0) {
            data->phase = PIXIE_CAPTURE;
            snprintf(data->stage_label, sizeof(data->stage_label), "Capture");
        } else if (strcmp(stage, "pixie_done") == 0) {
            data->phase = PIXIE_SEARCH;
            data->found = json_is_true(line, "found");
            snprintf(data->stage_label, sizeof(data->stage_label),
                     data->found ? "Search hit" : "Search");
        } else if (strncmp(stage, "pixie_verify", 12) == 0) {
            data->phase = PIXIE_VERIFY;
            snprintf(data->stage_label, sizeof(data->stage_label), "Verify");
        }
        data->needs_redraw = true;
    }

    if (strstr(line, "\"record_type\":\"terminal\"")) {
        char result[40];
        if (!json_copy_quoted(line, "operation_result", result, sizeof(result))) {
            snprintf(result, sizeof(result), "failed");
        }
        const char *label = map_operation_result(result);
        snprintf(data->result_label, sizeof(data->result_label), "%s", label);
        data->finished = true;
        if (strcmp(result, "pixie_pin_and_m7_verified") == 0) {
            data->phase = PIXIE_DONE;
            snprintf(data->stage_label, sizeof(data->stage_label), "Done");
            if (!data->reveal_sent) {
                data->reveal_sent = true;
                uart_send_command("secret reveal");
            }
        } else if (strcmp(result, "cancelled") == 0) {
            data->phase = PIXIE_DONE;
            snprintf(data->stage_label, sizeof(data->stage_label), "Stopped");
        } else if (strcmp(result, "no_match") == 0 ||
                   strcmp(result, "partial_match") == 0) {
            data->phase = PIXIE_DONE;
            snprintf(data->stage_label, sizeof(data->stage_label), "Done");
        } else {
            data->phase = PIXIE_ERROR;
        }
        data->needs_redraw = true;
        return;
    }

    if (strncmp(line, "SENSITIVE", 9) == 0) {
        parse_sensitive_line(line, data);
        data->needs_redraw = true;
    }
}

static void draw_screen(screen_t *self)
{
    pixie_data_t *data = (pixie_data_t *)self->user_data;

    ui_clear();
    ui_draw_title("Pixie Dust");

    char line[32];
    if (data->network.ssid[0]) {
        snprintf(line, sizeof(line), "%.29s", data->network.ssid);
    } else {
        snprintf(line, sizeof(line), "[Hidden]");
    }
    ui_print(0, 1, line, UI_COLOR_HIGHLIGHT);
    snprintf(line, sizeof(line), "%.29s", data->network.bssid);
    ui_print(0, 2, line, UI_COLOR_TEXT);

    snprintf(line, sizeof(line), "Stage: %.22s", data->stage_label);
    ui_print(0, 3, line, UI_COLOR_TEXT);

    if (data->result_label[0]) {
        snprintf(line, sizeof(line), "%.29s", data->result_label);
        ui_print(0, 4, line, UI_COLOR_HIGHLIGHT);
    }

    if (data->pin[0]) {
        snprintf(line, sizeof(line), "PIN: %.24s", data->pin);
        ui_print(0, 5, line, UI_COLOR_HIGHLIGHT);
    }
    if (data->psk[0]) {
        snprintf(line, sizeof(line), "Key: %.24s", data->psk);
        ui_print(0, 6, line, UI_COLOR_TEXT);
    }

    ui_draw_status(data->finished ? "ESC:Back" : "ESC:Stop");
}

static void send_stop_if_needed(pixie_data_t *data)
{
    if (data->stop_sent || data->finished) return;
    data->stop_sent = true;
    uart_send_command("wps stop");
}

static void on_tick(screen_t *self)
{
    pixie_data_t *data = (pixie_data_t *)self->user_data;
    if (data->phase == PIXIE_STARTING && !data->finished &&
        esp_timer_get_time() >= data->start_deadline_us) {
        data->phase = PIXIE_ERROR;
        data->finished = true;
        snprintf(data->result_label, sizeof(data->result_label), "No response");
        snprintf(data->stage_label, sizeof(data->stage_label), "Timeout");
        data->needs_redraw = true;
    }
    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    pixie_data_t *data = (pixie_data_t *)self->user_data;

    switch (key) {
        case KEY_ESC:
        case KEY_Q:
        case KEY_BACKSPACE:
            send_stop_if_needed(data);
            screen_manager_pop();
            break;
        default:
            break;
    }
}

static void on_destroy(screen_t *self)
{
    pixie_data_t *data = (pixie_data_t *)self->user_data;
    send_stop_if_needed(data);
    uart_clear_line_callback();
    free(data);
}

screen_t *pixie_dust_screen_create(void *params)
{
    pixie_dust_screen_params_t *pixie_params = (pixie_dust_screen_params_t *)params;
    if (!pixie_params) {
        ESP_LOGE(TAG, "Invalid parameters");
        return NULL;
    }

    screen_t *screen = screen_alloc();
    pixie_data_t *data = calloc(1, sizeof(*data));
    if (!screen || !data) {
        free(screen);
        free(data);
        free(pixie_params);
        return NULL;
    }

    data->network = pixie_params->network;
    free(pixie_params);

    snprintf(data->stage_label, sizeof(data->stage_label), "Starting");
    data->start_deadline_us = esp_timer_get_time() + START_TIMEOUT_US;

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_tick = on_tick;
    screen->on_draw = draw_screen;
    screen->on_destroy = on_destroy;

    uart_register_line_callback(uart_line_callback, data);

    char cmd[40];
    snprintf(cmd, sizeof(cmd), "wps pixie %s", data->network.bssid);
    uart_send_command(cmd);
    buzzer_beep_attack();

    draw_screen(screen);
    return screen;
}
