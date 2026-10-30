/**
 * @file deauth_detector_screen.c
 * @brief Deauth attack detector screen implementation
 *
 * Starts in scan mode (`deauth_detector`). The firmware prints a network CSV,
 * then `[DEAUTH] CH: <ch> | AP: <name> (<bssid>) | RSSI: <rssi>`.
 * Three frames for the same BSSID on one channel send `stop`, then
 * `deauth_detector INDEX` after WiFi has reset. Tracking shows how close
 * the frame sender is from RSSI.
 */

#include "deauth_detector_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <ctype.h>

static const char *TAG = "DEAUTH_DETECTOR";

#define MAX_SCAN_APS    64
#define MAX_HIT_BUCKETS 24
#define LOCK_FRAMES     3

enum {
    PHASE_SCAN = 0,
    PHASE_STOPPING,
    PHASE_TRACKING,
};

enum {
    TREND_NONE = 0,
    TREND_CLOSER,
    TREND_FARTHER,
    TREND_STEADY,
};

typedef struct {
    int index;
    char bssid[18];
} scan_ap_t;

typedef struct {
    int channel;
    char bssid[18];
    int count;
    bool unresolved;
} hit_bucket_t;

typedef struct {
    int channel;
    char ap_name[33];
    char bssid[18];
    int rssi;
    int detection_count;
    bool has_detection;
    bool needs_redraw;

    int phase;
    bool leaving;
    bool stop_sent;
    bool followup_ready;
    bool followup_sent;
    int lock_index;
    bool ap_missing;

    int best_channel;
    int best_count;
    int trend;
    int track_rssi;
    bool has_track_sample;

    scan_ap_t scan[MAX_SCAN_APS];
    int scan_count;
    hit_bucket_t hits[MAX_HIT_BUCKETS];
    int hit_count;
} deauth_detector_data_t;

static void draw_screen(screen_t *self);

/* Require decimal digits and detect overflow before converting to int. */
static bool parse_deauth_integer(const char *start, bool allow_negative,
                                 int *value, const char **end)
{
    const char *digits = start;
    if (allow_negative && *digits == '-') digits++;
    if (*digits < '0' || *digits > '9') return false;

    errno = 0;
    char *parsed_end;
    long parsed = strtol(start, &parsed_end, 10);
    if (errno == ERANGE || parsed < INT_MIN || parsed > INT_MAX) return false;
    *value = (int)parsed;
    *end = parsed_end;
    return true;
}

/**
 * @brief Parse a complete detection record, never the startup format example.
 * Format: [DEAUTH] CH: <ch> | AP: <name> (<bssid>) | RSSI: <rssi>
 */
static bool parse_deauth_line(const char *line, deauth_detector_data_t *data)
{
    const char *marker = "[DEAUTH] CH: ";
    if (!line || !data || strncmp(line, marker, strlen(marker)) != 0) return false;

    int channel, rssi;
    const char *end;
    if (!parse_deauth_integer(line + strlen(marker), false, &channel, &end) ||
        channel <= 0) return false;

    const char *ap_marker = " | AP: ";
    if (strncmp(end, ap_marker, strlen(ap_marker)) != 0) return false;
    const char *ap_start = end + strlen(ap_marker);

    // Use the last suffix so parentheses and delimiter-like text in SSIDs work.
    const char *rssi_marker = ") | RSSI: ";
    const char *bssid_end = NULL;
    const char *candidate = ap_start;
    while ((candidate = strstr(candidate, rssi_marker)) != NULL) {
        bssid_end = candidate;
        candidate++;
    }
    if (!bssid_end || bssid_end - ap_start < 19) return false;
    const char *bssid_start = bssid_end - 17;
    if (bssid_start[-2] != ' ' || bssid_start[-1] != '(') return false;

    size_t ap_len = (size_t)(bssid_start - 2 - ap_start);
    if (ap_len == 0 || ap_len > 32) return false;
    for (size_t i = 0; i < 17; i++) {
        if (i % 3 == 2) {
            if (bssid_start[i] != ':') return false;
        } else if (!isxdigit((unsigned char)bssid_start[i])) {
            return false;
        }
    }

    if (!parse_deauth_integer(bssid_end + strlen(rssi_marker), true, &rssi, &end) ||
        *end != '\0') return false;

    // Commit only after every field has passed validation.
    char ap_name[33];
    char bssid[18];
    memcpy(ap_name, ap_start, ap_len);
    ap_name[ap_len] = '\0';
    memcpy(bssid, bssid_start, 17);
    bssid[17] = '\0';
    memcpy(data->ap_name, ap_name, ap_len + 1);
    memcpy(data->bssid, bssid, sizeof(bssid));
    data->channel = channel;
    data->rssi = rssi;
    data->has_detection = true;
    if (data->detection_count < INT_MAX) data->detection_count++;

    ESP_LOGI(TAG, "Parsed: CH=%d AP=%s BSSID=%s RSSI=%d (total: %d)",
             data->channel, data->ap_name, data->bssid, data->rssi, data->detection_count);
    return true;
}

static bool bssid_equal(const char *a, const char *b)
{
    if (!a || !b) return false;
    for (int i = 0; i < 17; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca == '\0' || cb == '\0') return false;
        if (tolower(ca) != tolower(cb)) return false;
    }
    return a[17] == '\0' && b[17] == '\0';
}

static bool mac_ok(const char *s)
{
    if (!s) return false;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) {
            if (s[i] != ':') return false;
        } else if (!isxdigit((unsigned char)s[i])) {
            return false;
        }
    }
    return s[17] == '\0';
}

/* Walk one RFC 4180 quoted field. "" inside the field is one quote. */
static const char *read_quoted_field(const char *p, char *out, size_t out_len)
{
    if (!p) return NULL;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return NULL;
    p++;
    size_t n = 0;
    while (*p) {
        if (*p == '"') {
            if (p[1] == '"') {
                if (out && n + 1 < out_len) out[n++] = '"';
                p += 2;
                continue;
            }
            if (out && out_len > 0) {
                if (n >= out_len) n = out_len - 1;
                out[n] = '\0';
            }
            return p + 1;
        }
        if (out && n + 1 < out_len) out[n++] = *p;
        p++;
    }
    return NULL;
}

static const char *after_comma(const char *p)
{
    if (!p) return NULL;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ',') return NULL;
    return p + 1;
}

static bool parse_positive_int(const char *s, int *out)
{
    if (!s || !isdigit((unsigned char)*s)) return false;
    errno = 0;
    char *end = NULL;
    long value = strtol(s, &end, 10);
    if (errno == ERANGE || !end || *end != '\0' || value <= 0 || value > INT_MAX) {
        return false;
    }
    *out = (int)value;
    return true;
}

/* Scan CSV: field 0 is the JanOS index, field 3 is the BSSID. */
static bool remember_scan_ap(deauth_detector_data_t *data, const char *line)
{
    char index_buf[12];
    char bssid[32];
    const char *p = read_quoted_field(line, index_buf, sizeof(index_buf));
    p = after_comma(p);
    p = read_quoted_field(p, NULL, 0);
    p = after_comma(p);
    p = read_quoted_field(p, NULL, 0);
    p = after_comma(p);
    p = read_quoted_field(p, bssid, sizeof(bssid));
    if (!p || !mac_ok(bssid)) return false;

    int index = 0;
    if (!parse_positive_int(index_buf, &index)) return false;

    for (int i = 0; i < data->scan_count; i++) {
        if (bssid_equal(data->scan[i].bssid, bssid)) {
            data->scan[i].index = index;
            return true;
        }
    }
    if (data->scan_count >= MAX_SCAN_APS) return false;
    data->scan[data->scan_count].index = index;
    memcpy(data->scan[data->scan_count].bssid, bssid, sizeof(data->scan[0].bssid));
    data->scan_count++;
    return true;
}

static int find_scan_index(const deauth_detector_data_t *data, const char *bssid)
{
    for (int i = 0; i < data->scan_count; i++) {
        if (bssid_equal(data->scan[i].bssid, bssid)) return data->scan[i].index;
    }
    return -1;
}

static void refresh_best(deauth_detector_data_t *data)
{
    data->best_count = 0;
    data->best_channel = 0;
    for (int i = 0; i < data->hit_count; i++) {
        if (data->hits[i].unresolved) continue;
        if (data->hits[i].count > data->best_count) {
            data->best_count = data->hits[i].count;
            data->best_channel = data->hits[i].channel;
        }
    }
}

static hit_bucket_t *find_hit(deauth_detector_data_t *data, int channel, const char *bssid)
{
    for (int i = 0; i < data->hit_count; i++) {
        if (data->hits[i].channel == channel && bssid_equal(data->hits[i].bssid, bssid)) {
            return &data->hits[i];
        }
    }
    return NULL;
}

static hit_bucket_t *add_hit(deauth_detector_data_t *data, int channel, const char *bssid)
{
    hit_bucket_t *slot;
    if (data->hit_count < MAX_HIT_BUCKETS) {
        slot = &data->hits[data->hit_count++];
    } else {
        int worst = 0;
        for (int i = 1; i < MAX_HIT_BUCKETS; i++) {
            if (data->hits[i].count < data->hits[worst].count) worst = i;
        }
        slot = &data->hits[worst];
    }
    memset(slot, 0, sizeof(*slot));
    slot->channel = channel;
    memcpy(slot->bssid, bssid, sizeof(slot->bssid));
    return slot;
}

static void note_hit(deauth_detector_data_t *data, int channel, const char *bssid)
{
    if (data->phase != PHASE_SCAN) return;

    hit_bucket_t *hit = find_hit(data, channel, bssid);
    if (!hit) hit = add_hit(data, channel, bssid);
    if (!hit || hit->unresolved) {
        refresh_best(data);
        return;
    }

    if (hit->count < LOCK_FRAMES) hit->count++;
    if (hit->count >= LOCK_FRAMES) {
        int index = find_scan_index(data, bssid);
        if (index > 0) {
            data->lock_index = index;
            data->phase = PHASE_STOPPING;
            ESP_LOGI(TAG, "Locking BSSID %s as index %d on CH %d", bssid, index, channel);
        } else {
            hit->unresolved = true;
            data->ap_missing = true;
            ESP_LOGI(TAG, "BSSID %s not in scan", bssid);
        }
    }
    refresh_best(data);
}

static const char *proximity_label(int rssi)
{
    if (rssi >= -35) return "HERE";
    if (rssi >= -50) return "CLOSE";
    if (rssi >= -65) return "NEAR";
    if (rssi >= -80) return "FAR";
    return "VERY FAR";
}

static int proximity_percent(int rssi)
{
    if (rssi >= -20) return 100;
    if (rssi <= -90) return 0;
    return (rssi + 90) * 100 / 70;
}

static int proximity_color(int rssi)
{
    if (rssi >= -50) return UI_COLOR_HIGHLIGHT;
    if (rssi >= -65) return UI_COLOR_TEXT;
    return UI_COLOR_DIMMED;
}

static const char *trend_text(int trend)
{
    if (trend == TREND_CLOSER) return "closer";
    if (trend == TREND_FARTHER) return "farther";
    if (trend == TREND_STEADY) return "steady";
    return "";
}

static void update_trend(deauth_detector_data_t *data, int rssi)
{
    if (data->has_track_sample) {
        int64_t current = rssi;
        int64_t previous = data->track_rssi;
        if (current >= previous + 4) data->trend = TREND_CLOSER;
        else if (current <= previous - 4) data->trend = TREND_FARTHER;
        else data->trend = TREND_STEADY;
    } else {
        data->trend = TREND_NONE;
    }
    data->track_rssi = rssi;
    data->has_track_sample = true;
}

static void uart_line_callback(const char *line, void *user_data)
{
    deauth_detector_data_t *data = (deauth_detector_data_t *)user_data;
    if (!data || !line || data->leaving) return;

    if (data->phase == PHASE_SCAN) {
        if (parse_deauth_line(line, data)) {
            note_hit(data, data->channel, data->bssid);
            data->needs_redraw = true;
        } else if (line[0] == '"') {
            remember_scan_ap(data, line);
        }
        return;
    }

    if (data->phase == PHASE_STOPPING) {
        if (data->stop_sent && strstr(line, "All operations stopped")) {
            data->followup_ready = true;
            data->needs_redraw = true;
        }
        return;
    }

    if (parse_deauth_line(line, data)) {
        update_trend(data, data->rssi);
        data->needs_redraw = true;
    }
}

static void draw_tracking(deauth_detector_data_t *data)
{
    char line[40];
    snprintf(line, sizeof(line), "%.28s", data->ap_name);
    ui_print(0, 2, line, UI_COLOR_TEXT);
    ui_print(0, 3, data->bssid, UI_COLOR_DIMMED);
    ui_print_center(4, proximity_label(data->rssi), proximity_color(data->rssi));
    ui_draw_progress(5, proximity_percent(data->rssi), NULL);
    if (data->trend == TREND_NONE) {
        snprintf(line, sizeof(line), "CH %d  %d dBm", data->channel, data->rssi);
    } else {
        snprintf(line, sizeof(line), "CH %d  %d dBm  %s",
                 data->channel, data->rssi, trend_text(data->trend));
    }
    ui_print(0, 6, line, UI_COLOR_TEXT);
}

static void draw_stopping(deauth_detector_data_t *data)
{
    char line[40];
    ui_print_center(3, "Switching...", UI_COLOR_HIGHLIGHT);
    snprintf(line, sizeof(line), "%.28s", data->ap_name);
    ui_print_center(4, line, UI_COLOR_TEXT);
    snprintf(line, sizeof(line), "deauth_detector %d", data->lock_index);
    ui_print_center(5, line, UI_COLOR_DIMMED);
}

static void draw_scan_hit(deauth_detector_data_t *data)
{
    char line[40];
    ui_print(1, 2, "Last Detection:", UI_COLOR_TEXT);
    snprintf(line, sizeof(line), "CH: %d  RSSI: %d dBm", data->channel, data->rssi);
    ui_print(1, 3, line, UI_COLOR_HIGHLIGHT);
    snprintf(line, sizeof(line), "AP: %.24s", data->ap_name);
    ui_print(1, 4, line, UI_COLOR_TEXT);
    snprintf(line, sizeof(line), "BSSID: %s", data->bssid);
    ui_print(1, 5, line, UI_COLOR_DIMMED);

    if (data->ap_missing && data->best_count > 0) {
        snprintf(line, sizeof(line), "Not in scan  CH %d %d/%d",
                 data->best_channel, data->best_count, LOCK_FRAMES);
    } else if (data->ap_missing) {
        snprintf(line, sizeof(line), "Not in scan");
    } else if (data->best_count > 0) {
        snprintf(line, sizeof(line), "CH %d  %d/%d   Tot %d",
                 data->best_channel, data->best_count, LOCK_FRAMES, data->detection_count);
    } else {
        snprintf(line, sizeof(line), "Total: %d detections", data->detection_count);
    }
    ui_print(1, 6, line, UI_COLOR_TEXT);
}

static void draw_screen(screen_t *self)
{
    deauth_detector_data_t *data = (deauth_detector_data_t *)self->user_data;

    ui_clear();
    ui_draw_title("DEAUTH DETECTOR");

    if (data->phase == PHASE_TRACKING) {
        draw_tracking(data);
    } else if (data->phase == PHASE_STOPPING) {
        draw_stopping(data);
    } else if (data->has_detection) {
        draw_scan_hit(data);
    } else {
        ui_print_center(3, "Scanning for deauth", UI_COLOR_TEXT);
        ui_print_center(4, "attacks...", UI_COLOR_TEXT);
        ui_print_center(6, "Waiting for data", UI_COLOR_DIMMED);
    }

    ui_draw_status("ESC: Stop");
}

static void on_tick(screen_t *self)
{
    deauth_detector_data_t *data = (deauth_detector_data_t *)self->user_data;
    if (!data || data->leaving) return;

    if (data->phase == PHASE_STOPPING && !data->stop_sent) {
        data->stop_sent = true;
        ESP_LOGI(TAG, "Stopping scan to lock index %d", data->lock_index);
        uart_send_command("stop");
        data->needs_redraw = true;
    }

    if (data->phase == PHASE_STOPPING && data->followup_ready && !data->followup_sent) {
        char cmd[40];
        data->followup_sent = true;
        data->phase = PHASE_TRACKING;
        data->trend = TREND_NONE;
        data->has_track_sample = data->has_detection;
        data->track_rssi = data->rssi;
        snprintf(cmd, sizeof(cmd), "deauth_detector %d", data->lock_index);
        ESP_LOGI(TAG, "Starting selected detector: %s", cmd);
        uart_send_command(cmd);
        data->needs_redraw = true;
    }

    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    deauth_detector_data_t *data = (deauth_detector_data_t *)self->user_data;
    if (!data) return;

    switch (key) {
        case KEY_ESC:
        case KEY_Q:
            data->leaving = true;
            data->followup_ready = false;
            ESP_LOGI(TAG, "Stopping deauth detector");
            uart_send_command("stop");
            screen_manager_pop();
            break;

        default:
            break;
    }
}

static void on_destroy(screen_t *self)
{
    uart_clear_line_callback();

    if (self->user_data) {
        free(self->user_data);
    }
}

screen_t* deauth_detector_screen_create(void *params)
{
    (void)params;

    ESP_LOGI(TAG, "Creating deauth detector screen...");

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    deauth_detector_data_t *data = calloc(1, sizeof(deauth_detector_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }

    data->phase = PHASE_SCAN;

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;

    uart_register_line_callback(uart_line_callback, data);
    uart_send_command("deauth_detector");
    draw_screen(screen);

    ESP_LOGI(TAG, "Deauth detector screen created");
    return screen;
}
