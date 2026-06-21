/**
 * @file antisurv_screen.c
 * @brief Anti-Surveillance (BLE follower detection) live screen.
 *
 * Protocol (board firmware unchanged; Tab5-compatible):
 *   start_antisurveillance         -> begins follower detection (sensitivity is
 *                                     pre-configured via set_antisurv_sensitivity
 *                                     on the Wardrive Setup screen)
 *   stop                           -> stops; board prints the summary line:
 *     "Anti-surveillance stopped. Devices seen: <D>, followers flagged: <F>"
 *
 * Stream lines containing "[FOLLOWER]" carry a follower MAC (after "MAC=") and
 * trigger an on-screen alert; lines mentioning "Anti-surveillance" confirm the
 * scan is live. Mutually exclusive with wardrive (shared radio).
 */

#include "antisurv_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "buzzer.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

static const char *TAG = "ANTISURV";

#define ANTISURV_START_CMD  "start_antisurveillance"  // Tab5 protocol (CRLF added by uart_send_command)
#define ALERT_FLASH_TICKS   6                          // on_tick cadence for the flash

typedef struct {
    int  follower_count;
    int  devices_seen;        // from stop summary
    int  followers_flagged;   // from stop summary
    char last_follower[20];   // last MAC seen
    bool started;
    bool start_pending;
    bool blocked;             // wardrive active -> cannot start
    bool stopping;            // stop sent, waiting for the summary line
    bool stopped;             // got the stop summary
    bool alert_on;            // flashing alert state
    int  alert_ticks;
    char status[40];
    bool needs_redraw;
    bool ui_initialized;
    screen_t *self;
} antisurv_data_t;

static void draw_screen(screen_t *self);

// Extract a 17-char MAC anywhere in the line.
static bool extract_mac(const char *line, char *out, size_t out_len)
{
    for (const char *p = line; *p; p++) {
        bool ok = true;
        for (int i = 0; i < 17; i++) {
            char c = p[i];
            if (i == 2 || i == 5 || i == 8 || i == 11 || i == 14) {
                if (c != ':') { ok = false; break; }
            } else if (!isxdigit((unsigned char)c)) {
                ok = false; break;
            }
        }
        if (ok) {
            size_t n = (out_len < 18) ? out_len - 1 : 17;
            memcpy(out, p, n);
            out[n] = '\0';
            return true;
        }
        if (!p[1]) break;
    }
    return false;
}

static void uart_line_callback(const char *line, void *user_data)
{
    antisurv_data_t *data = (antisurv_data_t *)user_data;
    if (!data || !line) return;

    if (strstr(line, "[FOLLOWER]")) {
        data->follower_count++;
        // Prefer the MAC that follows "MAC="; fall back to any MAC in the line.
        const char *src = strstr(line, "MAC=");
        src = src ? src + 4 : line;
        char mac[20];
        if (extract_mac(src, mac, sizeof(mac))) {
            snprintf(data->last_follower, sizeof(data->last_follower), "%s", mac);
        }
        data->alert_on = true;
        data->alert_ticks = 0;
        buzzer_beep_attack();
        data->needs_redraw = true;
        return;
    }

    // Stop summary (check before the generic "Anti-surveillance" match below).
    int seen = 0, flagged = 0;
    const char *p = strstr(line, "Devices seen:");
    if (p && sscanf(p, "Devices seen: %d, followers flagged: %d", &seen, &flagged) == 2) {
        data->devices_seen = seen;
        data->followers_flagged = flagged;
        data->stopped = true;
        data->stopping = false;
        data->started = false;
        uart_set_antisurv_active(false);
        snprintf(data->status, sizeof(data->status), "Stopped");
        data->needs_redraw = true;
        return;
    }

    // Any other "Anti-surveillance" line confirms the scan is live.
    if (!data->stopped && (strstr(line, "Anti-surveillance") || strstr(line, "antisurveillance"))) {
        snprintf(data->status, sizeof(data->status), "Scanning for followers...");
        data->needs_redraw = true;
    }
}

static void antisurv_stop(antisurv_data_t *data)
{
    if (data->started) {
        uart_flush_rx();
        uart_send_command("stop");
        data->started = false;
    }
    uart_set_antisurv_active(false);
}

static void draw_screen(screen_t *self)
{
    antisurv_data_t *data = (antisurv_data_t *)self->user_data;

    if (!data->ui_initialized) {
        ui_clear();
        ui_draw_title("Anti-Surv");
        data->ui_initialized = true;
    }

    display_fill_rect(0, 19, DISPLAY_WIDTH, 98, UI_COLOR_BG);

    if (data->blocked) {
        ui_print_center(2, "Stop wardrive first", UI_COLOR_HIGHLIGHT);
        ui_print_center(4, "Wardrive is running", UI_COLOR_DIMMED);
        ui_draw_status("ESC:Back");
        return;
    }

    // Status line (flashes red while a follower alert is active)
    if (data->alert_on) {
        uint16_t status_color = (data->alert_ticks % 2) ? UI_COLOR_BG : RGB565(255, 68, 68);
        ui_print(0, 1, "! FOLLOWER DETECTED !", status_color);
    } else {
        ui_print(0, 1, data->status,
                 data->stopped ? UI_COLOR_DIMMED : UI_COLOR_HIGHLIGHT);
    }

    char line2[40];
    snprintf(line2, sizeof(line2), "Followers: %d", data->follower_count);
    ui_print(0, 3, line2, UI_COLOR_TEXT);

    if (data->last_follower[0]) {
        char line3[40];
        snprintf(line3, sizeof(line3), "Last: %s", data->last_follower);
        ui_print(0, 4, line3, data->alert_on ? RGB565(255, 68, 68) : UI_COLOR_DIMMED);
    }

    if (data->stopped) {
        char line5[40];
        snprintf(line5, sizeof(line5), "Seen:%d Flagged:%d",
                 data->devices_seen, data->followers_flagged);
        ui_print(0, 5, line5, UI_COLOR_DIMMED);
    }

    // ESC stops while scanning, then exits once stopped/stopping.
    ui_draw_status((data->started && !data->stopping) ? "ESC:Stop" : "ESC:Exit");
}

static void on_tick(screen_t *self)
{
    antisurv_data_t *data = (antisurv_data_t *)self->user_data;
    if (!data) return;

    if (data->start_pending) {
        data->start_pending = false;
        uart_register_line_callback(uart_line_callback, data);
        uart_flush_rx();
        uart_send_command(ANTISURV_START_CMD);
        data->started = true;
        uart_set_antisurv_active(true);
        snprintf(data->status, sizeof(data->status), "Starting: GPS+move...");
        buzzer_beep_attack();
        data->needs_redraw = true;
    }

    // Flash the alert for a short while, then clear it back to scanning.
    if (data->alert_on) {
        data->alert_ticks++;
        data->needs_redraw = true;   // keep flashing
        if (data->alert_ticks >= ALERT_FLASH_TICKS) {
            data->alert_on = false;
            if (!data->stopped) {
                snprintf(data->status, sizeof(data->status), "Scanning for followers...");
            }
        }
    }

    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

static void on_resume(screen_t *self)
{
    antisurv_data_t *data = (antisurv_data_t *)self->user_data;
    if (!data) return;
    uart_register_line_callback(uart_line_callback, data);
    data->ui_initialized = false;
    data->needs_redraw = true;
}

static void on_key(screen_t *self, key_code_t key)
{
    antisurv_data_t *data = (antisurv_data_t *)self->user_data;
    if (!data) return;

    switch (key) {
        case KEY_ESC:
        case KEY_Q:
        case KEY_BACKSPACE:
            if (data->started && !data->stopping) {
                // First ESC while scanning: stop and keep reading for the
                // summary line ("Devices seen: ..."). Second ESC exits.
                uart_send_command("stop");
                data->started = false;
                data->stopping = true;
                uart_set_antisurv_active(false);
                snprintf(data->status, sizeof(data->status), "Stopping... ESC=exit");
                data->needs_redraw = true;
            } else {
                screen_manager_pop();
            }
            break;
        default:
            break;
    }
}

static void on_destroy(screen_t *self)
{
    antisurv_data_t *data = (antisurv_data_t *)self->user_data;
    uart_clear_line_callback();
    if (data) {
        antisurv_stop(data);
        free(data);
    }
}

screen_t* antisurv_screen_create(void *params)
{
    (void)params;
    ESP_LOGI(TAG, "Creating anti-surveillance screen...");

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    antisurv_data_t *data = calloc(1, sizeof(antisurv_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }
    data->self = screen;

    // Mutual exclusion: wardrive and anti-surv share the radio.
    if (uart_is_wardrive_active()) {
        data->blocked = true;
        snprintf(data->status, sizeof(data->status), "Stop wardrive first");
    } else {
        data->start_pending = true;
        snprintf(data->status, sizeof(data->status), "Starting: GPS+move...");
    }

    screen->user_data = data;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;
    screen->on_resume = on_resume;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;

    draw_screen(screen);
    ESP_LOGI(TAG, "Anti-surveillance screen created");
    return screen;
}
