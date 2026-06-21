/**
 * @file wardrive_screen.c
 * @brief Wardrive monitor screen
 */

#include "wardrive_screen.h"
#include "wardrive_upload_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "buzzer.h"
#include "settings.h"
#include "cap_gps.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

static const char *TAG = "WARDRIVE";

#define REFRESH_INTERVAL_US 200000
#define CAP_GPS_UPDATE_TICKS 10
#define WARDRIVE_RING_SIZE 100

typedef enum {
    STATE_STARTING = 0,
    STATE_SCANNING,
    STATE_GPS_LOST,
    STATE_STOPPED
} wardrive_state_t;

typedef struct {
    char ssid[33];
    char bssid[18];
    char security[28];
    char lat[14];
    char lon[14];
    char kind[8];
} wardrive_entry_t;

typedef struct {
    wardrive_state_t state;
    wardrive_entry_t entries[WARDRIVE_RING_SIZE];
    int ring_head;
    int ring_count;

    int wardrive_wifi_count;
    int wardrive_bt_count;
    int wardrive_sat_count;
    int wardrive_relog_count;   // running re-log counter (D-UCB)
    int wardrive_best_channel;  // best channel from the D-UCB optimizer (<=0 = unknown)
    int gps_wait_elapsed;
    float wardrive_distance_m;

    char status_main[40];
    char gps_overlay[40];

    char cur_lat[14];
    char cur_lon[14];

    bool is_cap_gps;
    bool trace_enabled;
    bool wardrive_started;
    bool start_pending;
    bool no_sd_overlay;
    bool no_sd_continue_yes;
    bool antisurv_block;        // anti-surv is running -> block wardrive (shared radio)
    bool stop_confirm_overlay;
    bool stop_confirm_yes;
    int cap_tick_counter;
    bool needs_redraw;
    bool ui_initialized;

    esp_timer_handle_t refresh_timer;
    screen_t *self;
} wardrive_data_t;

static void draw_screen(screen_t *self);
extern bool is_board_sd_missing(void);

static void wardrive_send(wardrive_data_t *data, const char *cmd)
{
    (void)data;
    ESP_LOGI(TAG, "Wardrive TX > %s", cmd);
    esp_err_t ret = uart_send_command(cmd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_send_command failed for '%s': %s", cmd, esp_err_to_name(ret));
    }
}

static bool is_mac_prefix(const char *s)
{
    if (!s) return false;
    for (int i = 0; i < 17; i++) {
        if ((i + 1) % 3 == 0) {
            if (s[i] != ':') return false;
        } else {
            if (!isxdigit((unsigned char)s[i])) return false;
        }
    }
    return s[17] == ',';
}

static void trim_brackets(char *s)
{
    if (!s) return;
    size_t len = strlen(s);
    if (len >= 2 && s[0] == '[' && s[len - 1] == ']') {
        memmove(s, s + 1, len - 2);
        s[len - 2] = '\0';
    }
}

static bool parse_wardrive_csv_line(const char *line, wardrive_entry_t *entry)
{
    if (!line || !entry) return false;
    if (!(strstr(line, ",WIFI") || strstr(line, ",BLE"))) return false;
    if (!is_mac_prefix(line)) return false;

    char copy[320];
    snprintf(copy, sizeof(copy), "%s", line);

    char *fields[16] = {0};
    int field_count = 0;
    char *save = NULL;
    char *tok = strtok_r(copy, ",", &save);
    while (tok && field_count < 16) {
        fields[field_count++] = tok;
        tok = strtok_r(NULL, ",", &save);
    }
    if (field_count < 11) return false;

    snprintf(entry->bssid, sizeof(entry->bssid), "%s", fields[0]);
    snprintf(entry->ssid, sizeof(entry->ssid), "%s", fields[1][0] ? fields[1] : "<hidden>");
    snprintf(entry->security, sizeof(entry->security), "%s", fields[2][0] ? fields[2] : "-");
    snprintf(entry->lat, sizeof(entry->lat), "%s", fields[6][0] ? fields[6] : "0");
    snprintf(entry->lon, sizeof(entry->lon), "%s", fields[7][0] ? fields[7] : "0");
    snprintf(entry->kind, sizeof(entry->kind), "%s", strstr(line, ",BLE") ? "BLE" : "WIFI");

    if (strcasecmp(entry->kind, "WIFI") == 0) {
        trim_brackets(entry->security);
    }
    if (entry->ssid[0] == '\0') {
        snprintf(entry->ssid, sizeof(entry->ssid), "%s",
                 strcasecmp(entry->kind, "BLE") == 0 ? "<unnamed>" : "<hidden>");
    }
    return true;
}

static void push_entry(wardrive_data_t *data, const wardrive_entry_t *entry)
{
    if (!data || !entry) return;
    data->entries[data->ring_head] = *entry;
    data->ring_head = (data->ring_head + 1) % WARDRIVE_RING_SIZE;
    if (data->ring_count < WARDRIVE_RING_SIZE) {
        data->ring_count++;
    }

    snprintf(data->cur_lat, sizeof(data->cur_lat), "%s", entry->lat);
    snprintf(data->cur_lon, sizeof(data->cur_lon), "%s", entry->lon);
    if (strcasecmp(entry->kind, "BLE") == 0) {
        data->wardrive_bt_count++;
    } else {
        data->wardrive_wifi_count++;
    }
}

static bool parse_sat_dist(const char *line, int *sats, float *dist)
{
    const char *s = strstr(line, "sats:");
    const char *d = strstr(line, "dist:");
    if (!s || !d) return false;
    int sat_tmp = -1;
    float dist_tmp = -1.0f;
    if (sscanf(s, "sats: %d", &sat_tmp) != 1) return false;
    if (sscanf(d, "dist: %f", &dist_tmp) != 1) return false;
    *sats = sat_tmp;
    *dist = dist_tmp;
    return true;
}

static bool parse_bt_devices(const char *line, int *bt_count)
{
    const char *p = strstr(line, "BT devices");
    if (!p) return false;
    int v = -1;
    const char *s = p;
    while (s > line && !isdigit((unsigned char)*(s - 1))) s--;
    while (s > line && (isdigit((unsigned char)*(s - 1)) || *(s - 1) == ' ' || *(s - 1) == ',')) s--;
    if (sscanf(s, "%d", &v) == 1 && v >= 0) {
        *bt_count = v;
        return true;
    }
    return false;
}

// Running re-log counter: the integer that immediately precedes "relogs",
// e.g. "..., 20 relogs, D-UCB...". Read the digits directly (sscanf would
// choke on the preceding comma).
static bool parse_relogs(const char *line, int *relogs)
{
    const char *p = strstr(line, "relog");
    if (!p) return false;
    const char *e = p;
    while (e > line && e[-1] == ' ') e--;       // skip spaces before "relogs"
    const char *b = e;
    while (b > line && isdigit((unsigned char)b[-1])) b--;
    if (b == e) return false;                   // no number there
    *relogs = atoi(b);
    return true;
}

// Best channel from the D-UCB optimizer: the integer following the "best" token
// (e.g. "D-UCB best ch: 6").
static bool parse_best_channel(const char *line, int *ch)
{
    const char *p = strstr(line, "best");
    if (!p) return false;
    p += 4;
    while (*p && !isdigit((unsigned char)*p)) p++;
    int v = -1;
    if (sscanf(p, "%d", &v) == 1 && v > 0) {
        *ch = v;
        return true;
    }
    return false;
}

static void parse_lat_lon_from_text(wardrive_data_t *data, const char *line)
{
    if (!data || !line) return;
    const char *lat = strstr(line, "Lat=");
    const char *lon = strstr(line, "Lon=");
    if (!lat || !lon) return;

    lat += 4;
    lon += 4;

    size_t i = 0;
    while (lat[i] && i < sizeof(data->cur_lat) - 1) {
        char c = lat[i];
        if ((c >= '0' && c <= '9') || c == '-' || c == '+'
            || c == '.') {
            data->cur_lat[i] = c;
            i++;
        } else {
            break;
        }
    }
    data->cur_lat[i] = '\0';

    i = 0;
    while (lon[i] && i < sizeof(data->cur_lon) - 1) {
        char c = lon[i];
        if ((c >= '0' && c <= '9') || c == '-' || c == '+'
            || c == '.') {
            data->cur_lon[i] = c;
            i++;
        } else {
            break;
        }
    }
    data->cur_lon[i] = '\0';
}

static void update_status_line(wardrive_data_t *data, const char *line)
{
    if (!data || !line) return;
    parse_lat_lon_from_text(data, line);

    if (strstr(line, "GPS fix obtained")) {
        data->state = STATE_SCANNING;
        snprintf(data->gps_overlay, sizeof(data->gps_overlay), "GPS fix obtained");
    } else if (strstr(line, "GPS fix lost")) {
        data->state = STATE_GPS_LOST;
        snprintf(data->gps_overlay, sizeof(data->gps_overlay), "GPS lost...");
    } else if (strstr(line, "GPS fix recovered")) {
        data->state = STATE_SCANNING;
        snprintf(data->gps_overlay, sizeof(data->gps_overlay), "GPS fix recovered");
    } else if (strstr(line, "Still waiting for GPS fix")) {
        data->state = STATE_STARTING;
        int elapsed = 0, timeout = 0;
        if (sscanf(line, "%*[^'(](%d/%d", &elapsed, &timeout) == 2) {
            data->gps_wait_elapsed = elapsed;
        }
        snprintf(data->gps_overlay, sizeof(data->gps_overlay), "Waiting for GPS fix...");
    } else if (strstr(line, "No GPS fix obtained")) {
        data->state = STATE_GPS_LOST;
        snprintf(data->gps_overlay, sizeof(data->gps_overlay), "No GPS fix obtained");
    } else if (strstr(line, "Promiscuous wardrive started")) {
        data->state = STATE_SCANNING;
        data->wardrive_started = true;
        uart_set_wardrive_active(true);
        snprintf(data->status_main, sizeof(data->status_main), "Scanning...");
    } else if (strstr(line, "Wardrive promisc stopped")) {
        data->state = STATE_STOPPED;
        data->wardrive_started = false;
        uart_set_wardrive_active(false);
        snprintf(data->status_main, sizeof(data->status_main), "Wardrive stopped");
    } else if (strstr(line, "Flushed") && strstr(line, "networks")) {
        snprintf(data->status_main, sizeof(data->status_main), "Networks flushed");
    } else if (strstr(line, "GPS set")) {
        snprintf(data->gps_overlay, sizeof(data->gps_overlay), "GPS mode updated");
    }

    int sats = 0;
    float dist = 0.0f;
    if (parse_sat_dist(line, &sats, &dist)) {
        data->wardrive_sat_count = sats;
        data->wardrive_distance_m = dist;
        data->state = STATE_SCANNING;
    }
    int bt = 0;
    if (parse_bt_devices(line, &bt)) {
        data->wardrive_bt_count = bt;
    }
    int relogs = 0;
    if (parse_relogs(line, &relogs)) {
        data->wardrive_relog_count = relogs;
    }
    int best_ch = 0;
    if (parse_best_channel(line, &best_ch)) {
        data->wardrive_best_channel = best_ch;
    }
}

static void uart_line_callback(const char *line, void *user_data)
{
    wardrive_data_t *data = (wardrive_data_t *)user_data;
    if (!data) return;

    wardrive_entry_t entry;
    if (parse_wardrive_csv_line(line, &entry)) {
        push_entry(data, &entry);
        data->state = STATE_SCANNING;
        snprintf(data->status_main, sizeof(data->status_main), "Scanning...");
        data->needs_redraw = true;
        return;
    }

    if (strstr(line, "tab_gps_read")) {
        char reply[48];
        if (data->cur_lat[0] && data->cur_lon[0]) {
            snprintf(reply, sizeof(reply), "%s,%s", data->cur_lat, data->cur_lon);
        } else {
            snprintf(reply, sizeof(reply), "%s", "No GPS fix");
        }
        uart_send_command(reply);
    }

    update_status_line(data, line);
    data->needs_redraw = true;
}

static void refresh_timer_callback(void *arg)
{
    wardrive_data_t *data = (wardrive_data_t *)arg;
    if (!data || !data->self) return;

    if (data->is_cap_gps) {
        data->cap_tick_counter++;
        if (data->cap_tick_counter >= CAP_GPS_UPDATE_TICKS) {
            data->cap_tick_counter = 0;
            if (cap_gps_has_fix()) {
                double lat, lon, alt, hdop;
                if (cap_gps_get_position(&lat, &lon, &alt, &hdop)) {
                    char cmd[80];
                    snprintf(cmd, sizeof(cmd), "set_gps_position %.7f %.7f", lat, lon);
                    uart_send_command(cmd);
                    snprintf(data->cur_lat, sizeof(data->cur_lat), "%.7f", lat);
                    snprintf(data->cur_lon, sizeof(data->cur_lon), "%.7f", lon);
                    (void)alt;
                    (void)hdop;
                }
            } else {
                uart_send_command("set_gps_position");
            }
        }
    }

    (void)data->self;
}

static void on_tick(screen_t *self)
{
    wardrive_data_t *data = (wardrive_data_t *)self->user_data;
    if (!data) return;

    if (data->start_pending) {
        data->start_pending = false;
        ESP_LOGI(TAG, "Starting wardrive session...");
        uart_register_line_callback(uart_line_callback, data);
        ESP_LOGI(TAG, "Wardrive callback registered");
        wardrive_send(data, "unselect_networks");
        if (settings_get_gps_type() == GPS_TYPE_M5) {
            wardrive_send(data, "gps_set m5");
            snprintf(data->gps_overlay, sizeof(data->gps_overlay), "GPS set: m5");
        } else if (settings_get_gps_type() == GPS_TYPE_ATGM) {
            wardrive_send(data, "gps_set atgm");
            snprintf(data->gps_overlay, sizeof(data->gps_overlay), "GPS set: atgm");
        } else {
            wardrive_send(data, "gps_set external");
            snprintf(data->gps_overlay, sizeof(data->gps_overlay), "GPS set: external");
        }
        if (data->is_cap_gps) {
            cap_gps_init();
        }
        wardrive_send(data, data->trace_enabled ? "start_wardrive_promisc_trace"
                                                : "start_wardrive_promisc");
        data->wardrive_started = true;
        uart_set_wardrive_active(true);
        buzzer_beep_attack();
        data->needs_redraw = true;
    }

    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

static void on_resume(screen_t *self)
{
    wardrive_data_t *data = (wardrive_data_t *)self->user_data;
    if (!data) return;
    uart_register_line_callback(uart_line_callback, data);
    data->ui_initialized = false; // force full redraw after returning from sub-screen
    data->needs_redraw = true;
}

#define WD_CONTENT_Y_START  19
#define WD_CONTENT_Y_END    117
#define WD_CONTENT_HEIGHT   (WD_CONTENT_Y_END - WD_CONTENT_Y_START)

static void draw_screen(screen_t *self)
{
    wardrive_data_t *data = (wardrive_data_t *)self->user_data;

    if (!data->ui_initialized) {
        ui_clear();
        ui_draw_title("Wardrive");
        data->ui_initialized = true;
    }

    // Clear content area only
    display_fill_rect(0, WD_CONTENT_Y_START, DISPLAY_WIDTH, WD_CONTENT_HEIGHT, UI_COLOR_BG);

    if (data->antisurv_block) {
        ui_print_center(2, "Stop Anti-Surv first", UI_COLOR_HIGHLIGHT);
        ui_print_center(4, "Anti-Surv is running", UI_COLOR_DIMMED);
        ui_draw_status("ESC:Back");
        return;
    }

    if (data->no_sd_overlay) {
        ui_print_center(2, "No SD card!", UI_COLOR_HIGHLIGHT);
        ui_print_center(3, "Logs won't be saved.", UI_COLOR_TEXT);
        ui_print_center(4, "Continue anyway?", UI_COLOR_TEXT);
        ui_print_center(5, data->no_sd_continue_yes ? "No   [Yes]" : "[No]   Yes", UI_COLOR_DIMMED);
        ui_draw_status("LEFT/RIGHT OK BACK");
        return;
    }

    if (data->stop_confirm_overlay) {
        ui_print_center(3, "Stop wardrive?", UI_COLOR_HIGHLIGHT);
        ui_print_center(4, data->stop_confirm_yes ? "No   [Yes]" : "[No]   Yes", UI_COLOR_TEXT);
        ui_draw_status("LEFT/RIGHT OK BACK");
        return;
    }

    const char *state_text = "Starting...";
    if (data->state == STATE_SCANNING) state_text = "Scanning...";
    if (data->state == STATE_GPS_LOST) state_text = "GPS lost...";
    if (data->state == STATE_STOPPED) state_text = "Wardrive stopped";

    if (data->state == STATE_STARTING && data->gps_wait_elapsed > 0) {
        char wait_line[40];
        snprintf(wait_line, sizeof(wait_line), "Waiting fix: %ds", data->gps_wait_elapsed);
        ui_print(0, 1, wait_line, UI_COLOR_HIGHLIGHT);
    } else if (data->status_main[0]) {
        ui_print(0, 1, data->status_main, UI_COLOR_HIGHLIGHT);
    } else {
        ui_print(0, 1, state_text, UI_COLOR_HIGHLIGHT);
    }

    char counter[40];
    float km = data->wardrive_distance_m / 1000.0f;
    snprintf(counter, sizeof(counter), "WiFi:%d BT:%d SAT:%d",
             data->wardrive_wifi_count, data->wardrive_bt_count, data->wardrive_sat_count);
    ui_print(0, 2, counter, UI_COLOR_DIMMED);

    char metrics[40];
    char ch_str[12];
    if (data->wardrive_best_channel > 0) {
        snprintf(ch_str, sizeof(ch_str), "%d", data->wardrive_best_channel);
    } else {
        snprintf(ch_str, sizeof(ch_str), "-");
    }
    snprintf(metrics, sizeof(metrics), "relog:%d ch:%s %.2fkm",
             data->wardrive_relog_count, ch_str, km);
    ui_print(0, 3, metrics, UI_COLOR_TEXT);

    if (data->cur_lat[0] && data->cur_lon[0]) {
        char gps_line[40];
        snprintf(gps_line, sizeof(gps_line), "GPS: %.7s, %.7s", data->cur_lat, data->cur_lon);
        ui_print(0, 4, gps_line, UI_COLOR_TEXT);
    } else {
        ui_print(0, 4, "GPS: no fix", UI_COLOR_DIMMED);
    }

    if (data->gps_overlay[0]) {
        ui_print(0, 5, data->gps_overlay, UI_COLOR_DIMMED);
    }

    ui_draw_status("ESC:Stop S:Send");
}

static void on_key(screen_t *self, key_code_t key)
{
    wardrive_data_t *data = (wardrive_data_t *)self->user_data;
    if (!data) return;

    if (data->antisurv_block) {
        if (key == KEY_ESC || key == KEY_Q || key == KEY_BACKSPACE) {
            screen_manager_pop();
        }
        return;
    }

    if (data->no_sd_overlay) {
        if (key == KEY_LEFT || key == KEY_RIGHT) {
            data->no_sd_continue_yes = !data->no_sd_continue_yes;
            data->needs_redraw = true;
            return;
        }
        if (key == KEY_ENTER || key == KEY_SPACE) {
            if (data->no_sd_continue_yes) {
                data->no_sd_overlay = false;
                data->start_pending = true;
                data->needs_redraw = true;
            } else {
                screen_manager_pop();
            }
            return;
        }
        if (key == KEY_ESC || key == KEY_BACKSPACE) {
            screen_manager_pop();
            return;
        }
    }

    if (data->stop_confirm_overlay) {
        if (key == KEY_LEFT || key == KEY_RIGHT) {
            data->stop_confirm_yes = !data->stop_confirm_yes;
            data->needs_redraw = true;
            return;
        }
        if (key == KEY_ENTER || key == KEY_SPACE) {
            if (data->stop_confirm_yes) {
                uart_flush_rx();
                wardrive_send(data, "stop");
                data->state = STATE_STOPPED;
                data->wardrive_started = false;
                uart_set_wardrive_active(false);
                screen_manager_pop();
            } else {
                data->stop_confirm_overlay = false;
                data->needs_redraw = true;
            }
            return;
        }
        if (key == KEY_ESC || key == KEY_BACKSPACE) {
            data->stop_confirm_overlay = false;
            data->needs_redraw = true;
            return;
        }
    }

    switch (key) {
        case KEY_ESC:
        case KEY_Q:
        case KEY_BACKSPACE:
            data->stop_confirm_overlay = true;
            data->stop_confirm_yes = false;
            data->needs_redraw = true;
            break;
        case KEY_S:
            screen_manager_push(wardrive_upload_screen_create, NULL);
            break;
        default:
            break;
    }
}

static void on_destroy(screen_t *self)
{
    wardrive_data_t *data = (wardrive_data_t *)self->user_data;
    uart_clear_line_callback();

    if (data) {
        if (data->refresh_timer) {
            esp_timer_stop(data->refresh_timer);
            esp_timer_delete(data->refresh_timer);
        }
        if (data->is_cap_gps) {
            cap_gps_deinit();
        }
        if (data->wardrive_started) {
            uart_flush_rx();
            uart_send_command("stop");
        }
        uart_set_wardrive_active(false);
        free(data);
    }
}

screen_t* wardrive_screen_create(void *params)
{
    ESP_LOGI(TAG, "Creating wardrive screen...");
    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    wardrive_data_t *data = calloc(1, sizeof(wardrive_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }

    data->self = screen;
    data->state = STATE_STARTING;
    data->is_cap_gps = (settings_get_gps_type() == GPS_TYPE_CAP);
    data->trace_enabled = true;
    wardrive_run_params_t *in = (wardrive_run_params_t *)params;
    if (in) {
        data->trace_enabled = in->trace;
        free(in);
    }
    data->antisurv_block = uart_is_antisurv_active();
    data->no_sd_overlay = !data->antisurv_block && is_board_sd_missing();
    data->no_sd_continue_yes = false;
    data->start_pending = !data->antisurv_block && !data->no_sd_overlay;
    snprintf(data->status_main, sizeof(data->status_main), "Starting...");
    snprintf(data->gps_overlay, sizeof(data->gps_overlay), "Waiting for GPS");

    screen->user_data = data;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;
    screen->on_resume = on_resume;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;

    esp_timer_create_args_t timer_args = {
        .callback = refresh_timer_callback,
        .arg = data,
        .name = "wardrive_refresh"
    };
    if (esp_timer_create(&timer_args, &data->refresh_timer) == ESP_OK) {
        esp_timer_start_periodic(data->refresh_timer, REFRESH_INTERVAL_US);
    }

    draw_screen(screen);
    ESP_LOGI(TAG, "Wardrive screen created");
    return screen;
}
