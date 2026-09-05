/**
 * @file gitm_session_screen.c
 * @brief Running GITM / Rogue GITM session with [CGW] status polling
 */

#include "gitm_session_screen.h"
#include "cgw_parser.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "GITM_SESS";

#define STATUS_POLL_TICKS   5     /* ~2.5s (screen tick ~500ms) */
#define START_TIMEOUT_TICKS 60    /* ~30s for APSTA bring-up */
#define STOP_TIMEOUT_TICKS  10    /* ~5s wait for [PCAP_FINAL] */
#define STOP_LINGER_TICKS   2     /* ~1s show "Saved" before pop */

typedef enum {
    STATE_STARTING,
    STATE_RUNNING,
    STATE_ERROR,
    STATE_STOPPING,
} session_state_t;

typedef struct {
    gitm_mode_t mode;
    char ssid[33];
    char password[64];
    int victim_index;

    session_state_t state;
    cgw_snapshot_t snap;
    cgw_snapshot_t parse_buf;
    cgw_final_t final;
    char error[96];
    char status_msg[64];

    bool status_pending;
    bool started;
    bool pending_start;   /* defer UART start until after screen replace */
    bool needs_redraw;
    bool stop_sent;
    int poll_ticks;
    int timeout_ticks;

    screen_t *self;
} gitm_session_data_t;

static void draw_screen(screen_t *self);
static void send_start(gitm_session_data_t *data);
static void begin_stop(gitm_session_data_t *data);

static void uart_line_callback(const char *line, void *user_data)
{
    gitm_session_data_t *data = (gitm_session_data_t *)user_data;
    if (!data || !line) return;

    if (data->state == STATE_STOPPING) {
        if (cgw_parse_final_line(line, &data->final)) {
            data->needs_redraw = true;
        }
        return;
    }

    if (strstr(line, "No upstream IPv4") ||
        strstr(line, "Rogue GITM refused") ||
        strstr(line, "Capture SSID must") ||
        strstr(line, "Password length must") ||
        strstr(line, "SSID length must") ||
        strstr(line, "Capture Gateway is already active") ||
        strstr(line, "recorder failed to arm")) {
        snprintf(data->error, sizeof(data->error), "%.95s", line);
        data->state = STATE_ERROR;
        data->status_pending = false;
        data->needs_redraw = true;
        return;
    }

    if (strstr(line, "Capture Gateway ready") ||
        strstr(line, "Rogue GITM started successfully")) {
        data->started = true;
        if (data->state == STATE_STARTING) {
            data->state = STATE_RUNNING;
        }
        snprintf(data->status_msg, sizeof(data->status_msg), "Started");
        data->needs_redraw = true;
    }

    if (cgw_parse_line(line, &data->parse_buf)) {
        data->snap = data->parse_buf;
        data->status_pending = false;
        if (data->snap.active) {
            data->state = STATE_RUNNING;
            data->started = true;
        }
        data->needs_redraw = true;
        cgw_snapshot_reset(&data->parse_buf);
    }
}

static void send_start(gitm_session_data_t *data)
{
    char cmd[256];

    if (data->mode == GITM_MODE_ROGUE) {
        if (data->victim_index > 0) {
            snprintf(cmd, sizeof(cmd), "select_networks %d", data->victim_index);
            uart_send_command(cmd);
        }
        snprintf(cmd, sizeof(cmd), "start_rogue_gitm \"%s\" \"%s\"",
                 data->ssid, data->password);
        ESP_LOGI(TAG, "start_rogue_gitm \"%s\" \"***\"", data->ssid);
        uart_send_sensitive_command(cmd);
    } else {
        if (data->password[0] != '\0') {
            snprintf(cmd, sizeof(cmd), "capture_gateway start \"%s\" \"%s\"",
                     data->ssid, data->password);
        } else {
            snprintf(cmd, sizeof(cmd), "capture_gateway start \"%s\"", data->ssid);
        }
        ESP_LOGI(TAG, "capture_gateway start \"%s\"", data->ssid);
        uart_send_sensitive_command(cmd);
    }
}

static void begin_stop(gitm_session_data_t *data)
{
    if (data->stop_sent) return;
    data->stop_sent = true;
    data->state = STATE_STOPPING;
    data->timeout_ticks = 0;
    snprintf(data->status_msg, sizeof(data->status_msg), "Stopping...");
    data->needs_redraw = true;
    uart_send_command("stop");
}

static void draw_screen(screen_t *self)
{
    gitm_session_data_t *data = (gitm_session_data_t *)self->user_data;
    if (!data) return;

    ui_clear();
    ui_draw_title(data->mode == GITM_MODE_ROGUE ? "Rogue GITM" : "GITM");

    char line[40];
    snprintf(line, sizeof(line), "AP: %.28s", data->ssid);
    ui_print(0, 1, line, UI_COLOR_HIGHLIGHT);

    if (data->state == STATE_ERROR) {
        ui_print(0, 3, "Error:", UI_COLOR_DIMMED);
        /* Truncate error to fit */
        char err[36];
        snprintf(err, sizeof(err), "%.35s", data->error);
        ui_print(0, 4, err, UI_COLOR_HIGHLIGHT);
        ui_draw_status("ESC:Back");
        return;
    }

    if (data->state == STATE_STARTING) {
        ui_print_center(3, "Starting gateway...", UI_COLOR_HIGHLIGHT);
        ui_draw_status("ESC:Cancel");
        return;
    }

    if (data->state == STATE_STOPPING) {
        if (data->final.valid) {
            const char *base = data->final.file;
            const char *slash = strrchr(data->final.file, '/');
            if (slash) base = slash + 1;
            snprintf(line, sizeof(line), "Saved: %.28s", base);
            ui_print(0, 3, line, UI_COLOR_HIGHLIGHT);
        } else {
            ui_print_center(3, "Stopping...", UI_COLOR_HIGHLIGHT);
        }
        ui_draw_status("Please wait");
        return;
    }

    /* RUNNING */
    const cgw_snapshot_t *s = &data->snap;
    snprintf(line, sizeof(line), "cli=%u up=%d pkts=%lu",
             s->reported_clients,
             s->upstream ? 1 : 0,
             (unsigned long)s->packets);
    ui_print(0, 2, line, UI_COLOR_TEXT);

    snprintf(line, sizeof(line), "drops=%lu ch=%u",
             (unsigned long)s->drops, s->channel);
    ui_print(0, 3, line, UI_COLOR_DIMMED);

    if (s->client_count > 0) {
        snprintf(line, sizeof(line), "%.15s %.17s",
                 s->clients[0].ip, s->clients[0].mac);
        ui_print(0, 4, line, UI_COLOR_TEXT);
    } else {
        ui_print(0, 4, "No SoftAP clients", UI_COLOR_DIMMED);
    }
    if (s->client_count > 1) {
        snprintf(line, sizeof(line), "%.15s %.17s",
                 s->clients[1].ip, s->clients[1].mac);
        ui_print(0, 5, line, UI_COLOR_TEXT);
    }

    ui_draw_status("ESC: Stop & Exit");
}

static void on_tick(screen_t *self)
{
    gitm_session_data_t *data = (gitm_session_data_t *)self->user_data;
    if (!data) return;

    /*
     * Defer UART register+start until the first tick so screen_manager_replace
     * can destroy the previous wizard (which clears the line callback) first.
     */
    if (data->pending_start) {
        data->pending_start = false;
        uart_register_line_callback(uart_line_callback, data);
        send_start(data);
        return;
    }

    if (data->state == STATE_STARTING) {
        data->timeout_ticks++;
        if (data->timeout_ticks >= START_TIMEOUT_TICKS && !data->started) {
            snprintf(data->error, sizeof(data->error), "Start timeout");
            data->state = STATE_ERROR;
            data->needs_redraw = true;
        }
    }

    if (data->state == STATE_STOPPING) {
        data->timeout_ticks++;
        if (data->needs_redraw) {
            data->needs_redraw = false;
            draw_screen(self);
        }
        if (data->final.valid && data->timeout_ticks >= STOP_LINGER_TICKS) {
            uart_clear_line_callback();
            screen_manager_pop();
            return;
        }
        if (!data->final.valid && data->timeout_ticks >= STOP_TIMEOUT_TICKS) {
            uart_clear_line_callback();
            screen_manager_pop();
            return;
        }
        return;
    }

    if (data->state == STATE_RUNNING && !data->status_pending) {
        data->poll_ticks++;
        if (data->poll_ticks >= STATUS_POLL_TICKS) {
            data->poll_ticks = 0;
            data->status_pending = true;
            cgw_snapshot_reset(&data->parse_buf);
            uart_send_command("capture_gateway status");
        }
    }

    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    gitm_session_data_t *data = (gitm_session_data_t *)self->user_data;
    if (!data) return;

    switch (key) {
        case KEY_ESC:
        case KEY_Q:
        case KEY_BACKSPACE:
            if (data->state == STATE_STOPPING) return;
            if (data->state == STATE_ERROR) {
                uart_clear_line_callback();
                screen_manager_pop();
                return;
            }
            begin_stop(data);
            break;
        default:
            break;
    }
}

static void on_destroy(screen_t *self)
{
    gitm_session_data_t *data = (gitm_session_data_t *)self->user_data;
    uart_clear_line_callback();
    if (data && !data->stop_sent &&
        (data->state == STATE_STARTING || data->state == STATE_RUNNING)) {
        uart_send_command("stop");
    }
    if (data) {
        memset(data, 0, sizeof(*data));
        free(data);
    }
}

screen_t *gitm_session_screen_create(void *params)
{
    gitm_session_params_t *in = (gitm_session_params_t *)params;
    if (!in) {
        ESP_LOGE(TAG, "Missing params");
        return NULL;
    }

    screen_t *screen = screen_alloc();
    if (!screen) {
        free(in);
        return NULL;
    }

    gitm_session_data_t *data = calloc(1, sizeof(gitm_session_data_t));
    if (!data) {
        free(screen);
        free(in);
        return NULL;
    }

    data->mode = in->mode;
    strncpy(data->ssid, in->ssid, sizeof(data->ssid) - 1);
    strncpy(data->password, in->password, sizeof(data->password) - 1);
    data->victim_index = in->victim_index;
    data->state = STATE_STARTING;
    data->pending_start = true;
    data->self = screen;
    free(in);

    cgw_snapshot_reset(&data->snap);
    cgw_snapshot_reset(&data->parse_buf);

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;

    draw_screen(screen);

    ESP_LOGI(TAG, "GITM session screen ready mode=%d ssid=%s (start deferred)",
             data->mode, data->ssid);
    return screen;
}
