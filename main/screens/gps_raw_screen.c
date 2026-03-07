/**
 * @file gps_raw_screen.c
 * @brief GPS raw output screen implementation
 */

#include "gps_raw_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "keyboard.h"
#include "settings.h"
#include "cap_gps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "font8x16.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "GPS_RAW_SCREEN";

#define RAW_LINE_COUNT 2
#define RAW_LINE_LEN   48
#define TIME_LEN       12
#define COORD_LEN      20
#define GPS_NO_DATA_TIMEOUT_US (2000000)

typedef enum {
    GPS_STATUS_NO_GPS = 0,
    GPS_STATUS_WAITING,
    GPS_STATUS_FIX
} gps_status_t;

// Screen user data
typedef struct {
    char raw_lines[RAW_LINE_COUNT][RAW_LINE_LEN];
    int raw_index;
    int sat_count;
    bool fix;
    char lat[COORD_LEN];
    char lon[COORD_LEN];
    char alt[COORD_LEN];
    char time_str[TIME_LEN];
    char pending_nmea[128];
    bool pending_active;
    int64_t last_gps_us;
    gps_status_t last_status;
    bool needs_redraw;
    bool is_cap_gps;
    bool cap_inited;
} gps_raw_data_t;

static void draw_screen(screen_t *self);

static const char *skip_prefix(const char *line)
{
    const char *p = strstr(line, "[GPS RAW]");
    if (p) {
        p += strlen("[GPS RAW]");
        while (*p == ' ' || *p == '\t') p++;
        return p;
    }
    return line;
}

static void push_raw_line(gps_raw_data_t *data, const char *line)
{
    const char *text = skip_prefix(line);
    char *dst = data->raw_lines[data->raw_index];

    snprintf(dst, RAW_LINE_LEN, "%.30s", text);
    data->raw_index = (data->raw_index + 1) % RAW_LINE_COUNT;
}

static void format_time(char *out, size_t out_size, const char *t)
{
    if (!t || strlen(t) < 6) {
        out[0] = '\0';
        return;
    }

    char hh[3] = {t[0], t[1], '\0'};
    char mm[3] = {t[2], t[3], '\0'};
    char ss[3] = {t[4], t[5], '\0'};

    snprintf(out, out_size, "%s:%s:%s", hh, mm, ss);
}

/**
 * @brief Convert NMEA coordinate (DDMM.MMMM) to decimal degrees
 */
static double nmea_coord_to_decimal(const char *coord, char hemisphere)
{
    if (!coord || !coord[0]) return 0.0;

    double raw = strtod(coord, NULL);
    int degrees = (int)(raw / 100.0);
    double minutes = raw - (degrees * 100.0);
    double decimal = degrees + (minutes / 60.0);

    if (hemisphere == 'S' || hemisphere == 'W') {
        decimal = -decimal;
    }
    return decimal;
}

static void parse_gga(gps_raw_data_t *data, const char *nmea)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", nmea);

    char *save = NULL;
    char *tok = strtok_r(buf, ",", &save);
    int field = 0;

    char time_raw[16] = {0};
    char lat[16] = {0};
    char ns[2] = {0};
    char lon[16] = {0};
    char ew[2] = {0};
    int fix_quality = -1;
    int sats = -1;
    char alt[16] = {0};

    while (tok) {
        switch (field) {
            case 1:
                snprintf(time_raw, sizeof(time_raw), "%s", tok);
                break;
            case 2:
                snprintf(lat, sizeof(lat), "%s", tok);
                break;
            case 3:
                snprintf(ns, sizeof(ns), "%s", tok);
                break;
            case 4:
                snprintf(lon, sizeof(lon), "%s", tok);
                break;
            case 5:
                snprintf(ew, sizeof(ew), "%s", tok);
                break;
            case 6:
                fix_quality = atoi(tok);
                break;
            case 7:
                sats = atoi(tok);
                break;
            case 9:
                snprintf(alt, sizeof(alt), "%s", tok);
                break;
            default:
                break;
        }
        tok = strtok_r(NULL, ",", &save);
        field++;
    }

    if (time_raw[0]) {
        format_time(data->time_str, sizeof(data->time_str), time_raw);
    }

    if (lat[0] && ns[0]) {
        double dec = nmea_coord_to_decimal(lat, ns[0]);
        snprintf(data->lat, sizeof(data->lat), "%.7f", dec);
    }
    if (lon[0] && ew[0]) {
        double dec = nmea_coord_to_decimal(lon, ew[0]);
        snprintf(data->lon, sizeof(data->lon), "%.7f", dec);
    }
    if (alt[0]) {
        snprintf(data->alt, sizeof(data->alt), "%sm", alt);
    }

    if (sats >= 0) {
        data->sat_count = sats;
    }
    if (fix_quality >= 0) {
        data->fix = (fix_quality > 0);
    }
}

static void parse_rmc(gps_raw_data_t *data, const char *nmea)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", nmea);

    char *save = NULL;
    char *tok = strtok_r(buf, ",", &save);
    int field = 0;

    char time_raw[16] = {0};
    char status = '\0';
    char lat[16] = {0};
    char ns[2] = {0};
    char lon[16] = {0};
    char ew[2] = {0};

    while (tok) {
        switch (field) {
            case 1:
                snprintf(time_raw, sizeof(time_raw), "%s", tok);
                break;
            case 2:
                status = tok[0];
                break;
            case 3:
                snprintf(lat, sizeof(lat), "%s", tok);
                break;
            case 4:
                snprintf(ns, sizeof(ns), "%s", tok);
                break;
            case 5:
                snprintf(lon, sizeof(lon), "%s", tok);
                break;
            case 6:
                snprintf(ew, sizeof(ew), "%s", tok);
                break;
            default:
                break;
        }
        tok = strtok_r(NULL, ",", &save);
        field++;
    }

    if (time_raw[0]) {
        format_time(data->time_str, sizeof(data->time_str), time_raw);
    }

    if (lat[0] && ns[0]) {
        double dec = nmea_coord_to_decimal(lat, ns[0]);
        snprintf(data->lat, sizeof(data->lat), "%.7f", dec);
    }
    if (lon[0] && ew[0]) {
        double dec = nmea_coord_to_decimal(lon, ew[0]);
        snprintf(data->lon, sizeof(data->lon), "%.7f", dec);
    }

    if (status == 'A' || status == 'V') {
        data->fix = (status == 'A');
    }
}

static void handle_nmea_line(gps_raw_data_t *data, const char *line)
{
    const char *nmea = strchr(line, '$');
    if (!nmea) return;

    if (strstr(nmea, "GGA") != NULL) {
        parse_gga(data, nmea);
    } else if (strstr(nmea, "RMC") != NULL) {
        parse_rmc(data, nmea);
    }
}

static gps_status_t compute_status(gps_raw_data_t *data)
{
    if (data->is_cap_gps) {
        if (!data->cap_inited) return GPS_STATUS_NO_GPS;
        int sats = cap_gps_get_satellites();
        if (sats < 0) return GPS_STATUS_NO_GPS;
        return cap_gps_has_fix() ? GPS_STATUS_FIX : GPS_STATUS_WAITING;
    }

    int64_t now = esp_timer_get_time();
    if (data->last_gps_us == 0 || (now - data->last_gps_us) > GPS_NO_DATA_TIMEOUT_US) {
        return GPS_STATUS_NO_GPS;
    }
    return data->fix ? GPS_STATUS_FIX : GPS_STATUS_WAITING;
}

static bool nmea_has_checksum(const char *line)
{
    return (strchr(line, '*') != NULL);
}

static void try_handle_line(gps_raw_data_t *data, const char *line)
{
    if (!line || !line[0]) return;

    if (line[0] == '$') {
        if (nmea_has_checksum(line)) {
            handle_nmea_line(data, line);
            return;
        }
        snprintf(data->pending_nmea, sizeof(data->pending_nmea), "%s", line);
        data->pending_active = true;
        return;
    }

    if (data->pending_active) {
        size_t cur_len = strlen(data->pending_nmea);
        snprintf(data->pending_nmea + cur_len,
                 sizeof(data->pending_nmea) - cur_len,
                 "%s",
                 line);
        if (nmea_has_checksum(data->pending_nmea)) {
            handle_nmea_line(data, data->pending_nmea);
            data->pending_active = false;
            data->pending_nmea[0] = '\0';
        }
    }
}

// UART response callback - runs in UART RX task context, DO NOT call display functions!
static void on_uart_response(const char *line, void *user_data)
{
    screen_t *self = (screen_t *)user_data;
    gps_raw_data_t *data = (gps_raw_data_t *)self->user_data;

    if (strstr(line, "[GPS RAW]") != NULL) {
        data->last_gps_us = esp_timer_get_time();
        push_raw_line(data, line);

        const char *text = skip_prefix(line);
        if (text[0] == '$') {
            data->pending_active = false;
            data->pending_nmea[0] = '\0';
        }
        try_handle_line(data, text);
        data->needs_redraw = true;
    }
}

static void on_tick(screen_t *self)
{
    gps_raw_data_t *data = (gps_raw_data_t *)self->user_data;

    // CAP GPS: poll position from driver
    if (data->is_cap_gps && data->cap_inited) {
        data->sat_count = cap_gps_get_satellites();
        data->fix = cap_gps_has_fix();
        if (data->fix) {
            double lat, lon, alt, hdop;
            if (cap_gps_get_position(&lat, &lon, &alt, &hdop)) {
                snprintf(data->lat, sizeof(data->lat), "%.7f", lat);
                snprintf(data->lon, sizeof(data->lon), "%.7f", lon);
                snprintf(data->alt, sizeof(data->alt), "%.1fm", alt);
            }
        }
        data->needs_redraw = true;
    }

    gps_status_t status = compute_status(data);
    if (status != data->last_status) {
        data->last_status = status;
        data->needs_redraw = true;
    }

    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

static void draw_screen(screen_t *self)
{
    gps_raw_data_t *data = (gps_raw_data_t *)self->user_data;

    ui_clear();

    // Draw title
    const char *title = data->is_cap_gps ? "CAP GPS" : "GPS Read";
    ui_draw_title(title);

    gps_status_t status = compute_status(data);

    const char *status_text = "Status: No GPS";
    if (status == GPS_STATUS_WAITING) {
        status_text = "Status: Waiting for Fix";
    } else if (status == GPS_STATUS_FIX) {
        status_text = "Status: Fix";
    }
    ui_draw_text(0, FONT_HEIGHT + 4, status_text, UI_COLOR_HIGHLIGHT, UI_COLOR_BG);

    if (status != GPS_STATUS_NO_GPS && data->sat_count >= 0) {
        char sats_line[32];
        snprintf(sats_line, sizeof(sats_line), "Satellites: %d", data->sat_count);
        ui_print(0, 2, sats_line, UI_COLOR_DIMMED);
    }

    if (status != GPS_STATUS_NO_GPS && data->time_str[0]) {
        char time_line[24];
        snprintf(time_line, sizeof(time_line), "Time: %s", data->time_str);
        ui_print(0, 3, time_line, UI_COLOR_DIMMED);
    }

    if (status == GPS_STATUS_FIX && data->lat[0] && data->lon[0]) {
        char lat_line[32], lon_line[32];
        snprintf(lat_line, sizeof(lat_line), "Lat: %s", data->lat);
        snprintf(lon_line, sizeof(lon_line), "Lon: %s", data->lon);
        ui_print(0, 4, lat_line, UI_COLOR_TEXT);
        ui_print(0, 5, lon_line, UI_COLOR_TEXT);
        if (data->alt[0]) {
            char alt_line[32];
            snprintf(alt_line, sizeof(alt_line), "Alt: %s", data->alt);
            ui_print(0, 6, alt_line, UI_COLOR_TEXT);
        }
    }

    // Draw status bar
    ui_draw_status("ESC: Stop & Exit");
}

static void on_key(screen_t *self, key_code_t key)
{
    switch (key) {
        case KEY_ESC:
        case KEY_Q:
        case KEY_BACKSPACE:
        {
            gps_raw_data_t *data = (gps_raw_data_t *)self->user_data;
            if (!data->is_cap_gps) {
                uart_send_command("stop");
            }
            screen_manager_pop();
            break;
        }

        default:
            break;
    }
}

static void on_destroy(screen_t *self)
{
    gps_raw_data_t *data = (gps_raw_data_t *)self->user_data;

    if (data->is_cap_gps) {
        if (data->cap_inited) {
            cap_gps_deinit();
        }
    } else {
        uart_send_command("stop");
        uart_clear_line_callback();
    }

    if (self->user_data) {
        free(self->user_data);
    }
}

screen_t* gps_raw_screen_create(void *params)
{
    (void)params;

    ESP_LOGI(TAG, "Creating GPS raw screen...");

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    gps_raw_data_t *data = calloc(1, sizeof(gps_raw_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }

    data->raw_index = 0;
    data->sat_count = -1;
    data->fix = false;
    data->lat[0] = '\0';
    data->lon[0] = '\0';
    data->alt[0] = '\0';
    data->time_str[0] = '\0';
    data->pending_nmea[0] = '\0';
    data->pending_active = false;
    data->last_gps_us = 0;
    data->last_status = GPS_STATUS_NO_GPS;
    data->needs_redraw = false;
    data->is_cap_gps = (settings_get_gps_type() == GPS_TYPE_CAP);
    data->cap_inited = false;

    for (int i = 0; i < RAW_LINE_COUNT; i++) {
        data->raw_lines[i][0] = '\0';
    }

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;

    if (data->is_cap_gps) {
        ESP_LOGI(TAG, "CAP GPS mode - reading directly from LoRa CAP");
        esp_err_t ret = cap_gps_init();
        if (ret == ESP_OK) {
            data->cap_inited = true;
        } else {
            ESP_LOGE(TAG, "Failed to init CAP GPS: %s", esp_err_to_name(ret));
        }
    } else {
        uart_register_line_callback(on_uart_response, screen);
        uart_send_command("start_gps_raw");
    }

    draw_screen(screen);

    ESP_LOGI(TAG, "GPS raw screen created");
    return screen;
}
