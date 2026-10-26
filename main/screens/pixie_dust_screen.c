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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "PIXIE_DUST";

#define START_TIMEOUT_US  (4000LL * 1000LL)
#define STOP_REPLY_TIMEOUT_US (7000LL * 1000LL)
#define STOP_TOTAL_TIMEOUT_US (18000LL * 1000LL)
#define STATUS_POLL_US (1000LL * 1000LL)

typedef enum {
    PIXIE_STARTING = 0,
    PIXIE_CAPTURE,
    PIXIE_SEARCH,
    PIXIE_VERIFY,
    PIXIE_DONE,
    PIXIE_ERROR,
    PIXIE_UNKNOWN,
    PIXIE_STOPPING,
    PIXIE_QUARANTINED,
} pixie_phase_t;

typedef struct {
    wifi_network_t network;
    SemaphoreHandle_t mutex;
    pixie_phase_t phase;
    char stage_label[20];
    char result_label[24];
    char pin[12];
    char psk[32];
    bool found;
    uint32_t run_id;
    bool accepted;
    bool verified_stop_contract;
    bool start_refused;
    bool exit_requested;
    bool leave_warning_shown;
    bool stop_requested;
    bool stop_sent;
    bool stop_pending;
    bool status_requested;
    bool status_pending;
    bool retry_after_status;
    bool pop_ready;
    bool reveal_pending;
    bool reveal_sent;
    bool needs_redraw;
    int64_t start_deadline_us;
    int64_t stop_started_us;
    int64_t reply_deadline_us;
    int64_t next_poll_us;
} pixie_data_t;

static bool json_copy_u32(const char *line, const char *key, uint32_t *out)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(line, pat);
    if (!p || !out) return false;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (!isdigit((unsigned char)*p)) return false;
    uint64_t value = 0;
    do {
        value = value * 10U + (unsigned)(*p++ - '0');
        if (value > UINT32_MAX) return false;
    } while (isdigit((unsigned char)*p));
    if (*p != ',' && *p != '}') return false;
    *out = (uint32_t)value;
    return true;
}

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

static bool sensitive_matches_run(const char *line, uint32_t run_id)
{
    static const char prefix[] = "SENSITIVE operation_id=";
    if (strncmp(line, prefix, sizeof(prefix) - 1) != 0) return false;
    const char *cursor = line + sizeof(prefix) - 1;
    if (!isdigit((unsigned char)*cursor)) return false;
    uint64_t value = 0;
    do {
        value = value * 10U + (unsigned)(*cursor++ - '0');
        if (value > UINT32_MAX) return false;
    } while (isdigit((unsigned char)*cursor));
    return *cursor == ' ' && value == run_id;
}

static void uart_line_callback(const char *line, void *user_data)
{
    pixie_data_t *data = (pixie_data_t *)user_data;
    if (!data || !line || !line[0]) return;

    xSemaphoreTake(data->mutex, portMAX_DELAY);

    char record[32] = {0};
    (void)json_copy_quoted(line, "record_type", record, sizeof(record));

    if (strcmp(record, "wps_stop") == 0 && data->stop_sent) {
        char result[24] = {0};
        if (json_copy_quoted(line, "result", result, sizeof(result))) {
            data->stop_sent = false;
            if (strcmp(result, "stopped") == 0) {
                data->stop_pending = false;
                if (data->verified_stop_contract) {
                    data->phase = PIXIE_DONE;
                    data->pop_ready = data->exit_requested;
                    snprintf(data->stage_label, sizeof(data->stage_label), "Stopped");
                } else {
                    /* JanOS 1.7.6 can say stopped while its WPS worker is
                     * still in the epilogue.  Never imply green/idle here. */
                    data->phase = PIXIE_UNKNOWN;
                    snprintf(data->stage_label, sizeof(data->stage_label),
                             "Update JanOS");
                    snprintf(data->result_label, sizeof(data->result_label),
                             "Stop unconfirmed");
                }
            } else if (strcmp(result, "stop_pending") == 0) {
                data->stop_pending = true;
                data->phase = PIXIE_STOPPING;
                data->next_poll_us = esp_timer_get_time() + STATUS_POLL_US;
                snprintf(data->stage_label, sizeof(data->stage_label), "Stopping");
            } else if (strcmp(result, "quarantined") == 0) {
                data->stop_pending = false;
                data->phase = PIXIE_QUARANTINED;
                snprintf(data->stage_label, sizeof(data->stage_label), "Radio fault");
                snprintf(data->result_label, sizeof(data->result_label), "Monster needs recovery");
            }
            data->needs_redraw = true;
        }
        goto done;
    }

    if (strcmp(record, "wps_status") == 0 && data->status_pending) {
        uint32_t run_id = 0;
        char phase[24] = {0};
        data->status_pending = false;
        if (!json_copy_u32(line, "run_id", &run_id) ||
            !json_copy_quoted(line, "phase", phase, sizeof(phase)) ||
            (data->accepted && run_id != data->run_id)) {
            data->stop_pending = false;
            data->phase = PIXIE_UNKNOWN;
            snprintf(data->stage_label, sizeof(data->stage_label), "Other WPS run");
            snprintf(data->result_label, sizeof(data->result_label), "Stop unconfirmed");
        } else if (strcmp(phase, "quarantined") == 0) {
            data->stop_pending = false;
            data->phase = PIXIE_QUARANTINED;
            snprintf(data->stage_label, sizeof(data->stage_label), "Radio fault");
            snprintf(data->result_label, sizeof(data->result_label), "Monster needs recovery");
        } else if (data->retry_after_status) {
            data->retry_after_status = false;
            data->stop_pending = false;
            data->stop_requested = true;
        } else if (strcmp(phase, "idle") == 0 && data->stop_pending) {
            /* The service can report idle before the worker epilogue ends.
             * A second stop response is the completion barrier. */
            data->stop_requested = true;
            data->stop_pending = false;
        } else if (data->stop_pending) {
            data->next_poll_us = esp_timer_get_time() + STATUS_POLL_US;
        }
        data->needs_redraw = true;
        goto done;
    }

    if (!data->accepted &&
        (strstr(line, "\"code\":\"busy\"") || strstr(line, "radio_lease"))) {
        data->phase = PIXIE_ERROR;
        data->start_refused = true;
        snprintf(data->result_label, sizeof(data->result_label), "Radio busy");
        snprintf(data->stage_label, sizeof(data->stage_label), "Refused");
        data->needs_redraw = true;
        goto done;
    }

    if (!data->accepted && data->phase == PIXIE_STARTING &&
        strstr(line, "Command returned non-zero")) {
        data->phase = PIXIE_ERROR;
        data->start_refused = true;
        snprintf(data->result_label, sizeof(data->result_label), "Refused");
        snprintf(data->stage_label, sizeof(data->stage_label), "Error");
        data->needs_redraw = true;
        goto done;
    }

    if (strcmp(record, "accepted") == 0) {
        uint32_t run_id = 0;
        char operation[24] = {0};
        if (json_copy_u32(line, "run_id", &run_id) && run_id != 0 &&
            json_copy_quoted(line, "operation", operation, sizeof(operation)) &&
            strcmp(operation, "wps_pixie") == 0 && !data->accepted) {
            data->accepted = true;
            data->run_id = run_id;
            data->start_refused = false;
            if (data->exit_requested) {
                data->phase = PIXIE_STOPPING;
                if (!data->stop_sent && !data->stop_pending &&
                    !data->status_pending && !data->pop_ready)
                    data->stop_requested = true;
            } else {
                data->phase = PIXIE_CAPTURE;
                snprintf(data->stage_label, sizeof(data->stage_label), "Capture");
            }
            data->needs_redraw = true;
        }
        goto done;
    }

    char stage[32];
    char stage_type[24];
    uint32_t stage_operation_id = 0;
    if (data->accepted &&
        !data->exit_requested &&
        json_copy_u32(line, "operation_id", &stage_operation_id) &&
        stage_operation_id == data->run_id &&
        json_copy_quoted(line, "type", stage_type, sizeof(stage_type)) &&
        strcmp(stage_type, "wps_stage") == 0 &&
        json_copy_quoted(line, "stage", stage, sizeof(stage))) {
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

    uint32_t terminal_run_id = 0;
    if (strcmp(record, "terminal") == 0 && data->accepted &&
        json_copy_u32(line, "run_id", &terminal_run_id) &&
        terminal_run_id == data->run_id) {
        bool waiting_for_stop = data->exit_requested &&
            (data->stop_sent || data->stop_pending || data->status_pending ||
             data->stop_requested);
        char result[40];
        char cleanup[32] = {0};
        char lease[32] = {0};
        if (!json_copy_quoted(line, "operation_result", result, sizeof(result))) {
            snprintf(result, sizeof(result), "failed");
        }
        const char *label = map_operation_result(result);
        snprintf(data->result_label, sizeof(data->result_label), "%s", label);
        bool restored = json_copy_quoted(line, "cleanup_result", cleanup, sizeof(cleanup)) &&
                        strcmp(cleanup, "restored") == 0 &&
                        json_copy_quoted(line, "lease_state", lease, sizeof(lease)) &&
                        strcmp(lease, "released") == 0;
        if (!restored) {
            data->phase = PIXIE_QUARANTINED;
            snprintf(data->stage_label, sizeof(data->stage_label), "Radio fault");
            snprintf(data->result_label, sizeof(data->result_label), "Cleanup unconfirmed");
        } else if (strcmp(result, "pixie_pin_and_m7_verified") == 0) {
            data->phase = PIXIE_DONE;
            snprintf(data->stage_label, sizeof(data->stage_label), "Done");
            if (!data->reveal_sent) {
                data->reveal_pending = true;
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
        if (waiting_for_stop && restored) {
            /* A terminal record precedes the worker epilogue.  The stop
             * response is still the barrier for leaving this screen. */
            data->phase = PIXIE_STOPPING;
            snprintf(data->stage_label, sizeof(data->stage_label), "Finishing");
            snprintf(data->result_label, sizeof(data->result_label), "Stop pending");
        }
        data->needs_redraw = true;
        goto done;
    }

    if (data->reveal_sent && data->accepted &&
        sensitive_matches_run(line, data->run_id)) {
        parse_sensitive_line(line, data);
        data->needs_redraw = true;
    }
done:
    xSemaphoreGive(data->mutex);
}

static void draw_screen(screen_t *self)
{
    pixie_data_t *data = (pixie_data_t *)self->user_data;
    pixie_data_t snapshot;
    xSemaphoreTake(data->mutex, portMAX_DELAY);
    snapshot = *data;
    xSemaphoreGive(data->mutex);
    data = &snapshot;

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

    if (data->phase == PIXIE_STOPPING) {
        ui_draw_status("Stopping... wait");
    } else if (data->phase == PIXIE_UNKNOWN || data->phase == PIXIE_ERROR ||
               data->phase == PIXIE_QUARANTINED) {
        ui_draw_status("ENT:Stop ESC:Back");
    } else {
        ui_draw_status("ESC:Stop/Back");
    }
}

static void on_tick(screen_t *self)
{
    pixie_data_t *data = (pixie_data_t *)self->user_data;
    int64_t now = esp_timer_get_time();
    bool send_stop = false, send_status = false, send_reveal = false;
    bool redraw = false, pop = false;

    xSemaphoreTake(data->mutex, portMAX_DELAY);
    if (data->phase == PIXIE_STARTING && now >= data->start_deadline_us) {
        data->phase = PIXIE_UNKNOWN;
        snprintf(data->result_label, sizeof(data->result_label), "Monster state unknown");
        snprintf(data->stage_label, sizeof(data->stage_label), "No reply");
        data->needs_redraw = true;
    }
    if (data->stop_sent && now >= data->reply_deadline_us) {
        data->stop_sent = false;
        data->stop_pending = false;
        data->phase = PIXIE_UNKNOWN;
        snprintf(data->result_label, sizeof(data->result_label), "Stop unconfirmed");
        snprintf(data->stage_label, sizeof(data->stage_label), "No stop reply");
        data->needs_redraw = true;
    }
    if (data->status_pending && now >= data->reply_deadline_us) {
        data->status_pending = false;
        data->stop_pending = false;
        data->retry_after_status = false;
        data->phase = PIXIE_UNKNOWN;
        snprintf(data->result_label, sizeof(data->result_label), "Stop unconfirmed");
        snprintf(data->stage_label, sizeof(data->stage_label), "No status reply");
        data->needs_redraw = true;
    }
    if (data->stop_pending && data->stop_started_us > 0 &&
        now - data->stop_started_us >= STOP_TOTAL_TIMEOUT_US) {
        data->stop_pending = false;
        data->status_requested = false;
        data->phase = PIXIE_UNKNOWN;
        snprintf(data->result_label, sizeof(data->result_label), "Stop unconfirmed");
        snprintf(data->stage_label, sizeof(data->stage_label), "Still busy");
        data->needs_redraw = true;
    }
    if (data->stop_pending && !data->status_pending &&
        now >= data->next_poll_us) {
        data->status_requested = true;
    }
    if (data->stop_requested && !data->stop_sent && !data->status_pending) {
        data->stop_requested = false;
        data->stop_sent = true;
        data->stop_pending = false;
        data->phase = PIXIE_STOPPING;
        if (data->stop_started_us == 0) data->stop_started_us = now ? now : 1;
        data->reply_deadline_us = now + STOP_REPLY_TIMEOUT_US;
        data->needs_redraw = true;
        send_stop = true;
    } else if (data->status_requested && !data->status_pending && !data->stop_sent) {
        data->status_requested = false;
        data->status_pending = true;
        data->reply_deadline_us = now + STOP_REPLY_TIMEOUT_US;
        send_status = true;
    }
    if (data->reveal_pending && !data->reveal_sent) {
        data->reveal_pending = false;
        data->reveal_sent = true;
        send_reveal = true;
    }
    redraw = data->needs_redraw;
    data->needs_redraw = false;
    pop = data->pop_ready;
    data->pop_ready = false;
    xSemaphoreGive(data->mutex);

    if (send_stop && uart_send_command("wps stop") != ESP_OK) {
        xSemaphoreTake(data->mutex, portMAX_DELAY);
        data->stop_sent = false;
        data->phase = PIXIE_ERROR;
        snprintf(data->result_label, sizeof(data->result_label), "Stop send failed");
        snprintf(data->stage_label, sizeof(data->stage_label), "Retry with ENTER");
        data->needs_redraw = true;
        xSemaphoreGive(data->mutex);
        redraw = true;
    }
    if (send_status && uart_send_command("wps status") != ESP_OK) {
        xSemaphoreTake(data->mutex, portMAX_DELAY);
        data->status_pending = false;
        data->stop_pending = false;
        data->phase = PIXIE_ERROR;
        snprintf(data->result_label, sizeof(data->result_label), "Status send failed");
        data->needs_redraw = true;
        xSemaphoreGive(data->mutex);
        redraw = true;
    }
    if (send_reveal) uart_send_command("secret reveal");
    if (redraw) draw_screen(self);
    if (pop) screen_manager_pop();
}

static void on_key(screen_t *self, key_code_t key)
{
    pixie_data_t *data = (pixie_data_t *)self->user_data;
    xSemaphoreTake(data->mutex, portMAX_DELAY);
    switch (key) {
        case KEY_ESC:
        case KEY_Q:
        case KEY_BACKSPACE:
            if (data->phase == PIXIE_STOPPING) break;
            if (data->start_refused) {
                data->pop_ready = true;
            } else if (data->accepted && !data->exit_requested) {
                data->exit_requested = true;
                data->stop_requested = true;
                data->phase = PIXIE_STOPPING;
            } else if (!data->accepted && !data->leave_warning_shown) {
                data->exit_requested = true;
                data->leave_warning_shown = true;
                data->phase = PIXIE_UNKNOWN;
                snprintf(data->result_label, sizeof(data->result_label),
                         "Monster state unknown");
            } else {
                /* A second Back after an explicit warning leaves recovery to
                 * the operator; it never claims the Monster stopped. */
                data->pop_ready = true;
            }
            data->needs_redraw = true;
            break;
        case KEY_ENTER:
            if (data->phase == PIXIE_UNKNOWN || data->phase == PIXIE_ERROR ||
                data->phase == PIXIE_QUARANTINED) {
                data->exit_requested = true;
                data->stop_started_us = 0; /* Explicit retry gets its own bound. */
                if (data->accepted) {
                    data->retry_after_status = true;
                    data->status_requested = true;
                } else {
                    /* Explicit operator choice: wps stop is global without a
                     * run id, so never issue it on a missing ACK automatically. */
                    data->stop_requested = true;
                }
                data->phase = PIXIE_STOPPING;
                data->needs_redraw = true;
            }
            break;
        default:
            break;
    }
    xSemaphoreGive(data->mutex);
}

static void on_destroy(screen_t *self)
{
    pixie_data_t *data = (pixie_data_t *)self->user_data;
    uart_clear_line_callback();
    vSemaphoreDelete(data->mutex);
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

    data->mutex = xSemaphoreCreateMutex();
    if (!data->mutex) {
        free(screen);
        free(data);
        free(pixie_params);
        return NULL;
    }

    data->network = pixie_params->network;
    data->verified_stop_contract = uart_janos_version_at_least(1, 7, 7);
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
    if (uart_send_command(cmd) != ESP_OK) {
        xSemaphoreTake(data->mutex, portMAX_DELAY);
        data->phase = PIXIE_ERROR;
        data->start_refused = true;
        snprintf(data->result_label, sizeof(data->result_label), "Start send failed");
        data->needs_redraw = true;
        xSemaphoreGive(data->mutex);
    }
    buzzer_beep_attack();

    draw_screen(screen);
    return screen;
}
