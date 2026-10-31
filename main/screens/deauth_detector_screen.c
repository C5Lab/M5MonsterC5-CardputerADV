/**
 * @file deauth_detector_screen.c
 * @brief Passive JanOS DEAUTH and CSA detector on the Cardputer UART.
 *
 * The legacy all-channel command starts when this screen opens. Press S to
 * stop it and make a fresh JanOS scan for an exact BSSID watch list.
 */

#include "deauth_detector_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "DEAUTH_DETECTOR";

#define MAX_SCAN_APS 64
#define SCAN_ACK_TIMEOUT_US 5000000LL
#define SCAN_TIMEOUT_US 35000000LL
#define ALL_START_TIMEOUT_US 35000000LL
#define SELECTED_START_TIMEOUT_US 8000000LL
#define STOP_TIMEOUT_US 35000000LL
#define PING_INTERVAL_US 10000000LL
#define PING_TIMEOUT_US 5000000LL

typedef enum {
    DET_START_ALL,
    DET_ACTIVE_ALL,
    DET_STOP_FOR_SCAN,
    DET_STOP_FOR_ALL,
    DET_SCAN_WAIT_ACK,
    DET_SCAN_RUNNING,
    DET_SCAN_COLLECT,
    DET_PICK,
    DET_START_SELECTED,
    DET_ACTIVE_SELECTED,
    DET_STOP_BAD_WATCH,
    DET_STOP_EXIT,
    DET_ERROR,
    DET_DISCONNECTED
} detector_state_t;

typedef enum {
    VIEW_DEAUTH,
    VIEW_CSA,
    VIEW_WATCH
} detector_view_t;

typedef struct {
    int index;
    char ssid[33];
    char bssid[18];
    int channel;
    bool selected;
} scan_ap_t;

typedef struct {
    int channel;
    char ap_name[33];
    char bssid[18];
    int rssi;
} deauth_event_t;

typedef struct {
    int rx;
    int to;
    char bssid[18];
    char tx[18];
    int rssi;
    char via[12];
    int count;
    int mode;
    char class_name[8];
    char ssid[33];
} csa_event_t;

typedef struct {
    detector_state_t state;
    detector_view_t view;
    deauth_event_t last_deauth;
    csa_event_t last_csa;
    int deauth_count;
    int csa_count;
    scan_ap_t scan[MAX_SCAN_APS];
    int scan_count;
    int expected_scan_count;
    int cursor;
    int watch_indices[MAX_SCAN_APS];
    int watch_expected_count;
    int verified_watch_count;
    bool start_ack;
    bool ping_waiting;
    bool exit_ready;
    bool stop_send_failed;
    bool needs_redraw;
    bool leaving;
    int64_t state_started_us;
    int64_t last_ping_us;
    int64_t ping_sent_us;
    char message[64];
} deauth_detector_data_t;

static void draw_screen(screen_t *self);

static int64_t now_us(void) { return esp_timer_get_time(); }

static void set_message(deauth_detector_data_t *data, const char *text)
{
    snprintf(data->message, sizeof(data->message), "%s", text);
    data->needs_redraw = true;
}

static void set_state(deauth_detector_data_t *data, detector_state_t state)
{
    data->state = state;
    data->state_started_us = now_us();
    data->needs_redraw = true;
}

static void clear_scan(deauth_detector_data_t *data)
{
    memset(data->scan, 0, sizeof(data->scan));
    data->scan_count = 0;
    data->expected_scan_count = 0;
    data->cursor = 0;
    data->watch_expected_count = 0;
    data->verified_watch_count = 0;
    data->start_ack = false;
    data->ping_waiting = false;
    data->needs_redraw = true;
}

static void clear_events(deauth_detector_data_t *data)
{
    memset(&data->last_deauth, 0, sizeof(data->last_deauth));
    memset(&data->last_csa, 0, sizeof(data->last_csa));
    data->deauth_count = 0;
    data->csa_count = 0;
    data->needs_redraw = true;
}

static bool bssid_valid(const char *bssid)
{
    if (!bssid || strlen(bssid) != 17) return false;
    for (int i = 0; i < 17; ++i) {
        if (i % 3 == 2) {
            if (bssid[i] != ':') return false;
        } else if (!isxdigit((unsigned char)bssid[i])) {
            return false;
        }
    }
    return true;
}

static bool bssid_equal(const char *a, const char *b)
{
    if (!bssid_valid(a) || !bssid_valid(b)) return false;
    for (int i = 0; i < 17; ++i) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    }
    return true;
}

static bool parse_integer(const char *start, bool negative, int *value, const char **end)
{
    if (!start || (!isdigit((unsigned char)*start) &&
                   !(negative && *start == '-' && isdigit((unsigned char)start[1])))) {
        return false;
    }
    errno = 0;
    char *parsed_end = NULL;
    long parsed = strtol(start, &parsed_end, 10);
    if (errno == ERANGE || parsed < INT_MIN || parsed > INT_MAX) return false;
    *value = (int)parsed;
    *end = parsed_end;
    return true;
}

static bool parse_deauth_line(const char *line, deauth_event_t *event)
{
    const char *prefix = "[DEAUTH] CH: ";
    if (!line || !event || strncmp(line, prefix, strlen(prefix)) != 0) return false;

    deauth_event_t parsed = {0};
    const char *end = NULL;
    if (!parse_integer(line + strlen(prefix), false, &parsed.channel, &end) ||
        parsed.channel < 1 || parsed.channel > 255) return false;
    const char *ap_field = " | AP: ";
    if (strncmp(end, ap_field, strlen(ap_field)) != 0) return false;
    const char *ap_start = end + strlen(ap_field);

    /* The last fixed MAC suffix wins; SSIDs may contain parentheses and pipes. */
    const char *suffix = ") | RSSI: ";
    const char *bssid_end = NULL;
    for (const char *p = ap_start; (p = strstr(p, suffix)) != NULL; ++p) bssid_end = p;
    if (!bssid_end || bssid_end - ap_start < 19) return false;
    const char *bssid_start = bssid_end - 17;
    if (bssid_start[-2] != ' ' || bssid_start[-1] != '(') return false;
    size_t ap_length = (size_t)(bssid_start - 2 - ap_start);
    if (ap_length == 0 || ap_length > 32) return false;
    memcpy(parsed.ap_name, ap_start, ap_length);
    parsed.ap_name[ap_length] = '\0';
    memcpy(parsed.bssid, bssid_start, 17);
    parsed.bssid[17] = '\0';
    if (!bssid_valid(parsed.bssid)) return false;
    if (!parse_integer(bssid_end + strlen(suffix), true, &parsed.rssi, &end) ||
        *end != '\0') return false;
    *event = parsed;
    return true;
}

static bool take_field(const char **cursor, const char *next, char *out, size_t capacity)
{
    const char *end = strstr(*cursor, next);
    if (!end || end == *cursor || (size_t)(end - *cursor) >= capacity) return false;
    size_t length = (size_t)(end - *cursor);
    memcpy(out, *cursor, length);
    out[length] = '\0';
    *cursor = end + strlen(next);
    return true;
}

static bool parse_decimal_field(const char *field, bool negative, int *value)
{
    const char *end = NULL;
    return parse_integer(field, negative, value, &end) && *end == '\0';
}

static bool parse_csa_line(const char *line, csa_event_t *event)
{
    const char *prefix = "[CSA] RX: ";
    if (!line || !event || strncmp(line, prefix, strlen(prefix)) != 0) return false;
    csa_event_t parsed = {0};
    char field[32];
    const char *p = line + strlen(prefix);
    if (!take_field(&p, " | TO: ", field, sizeof(field)) ||
        !parse_decimal_field(field, false, &parsed.rx) || parsed.rx < 0 || parsed.rx > 255 ||
        !take_field(&p, " | BSSID: ", field, sizeof(field)) ||
        !parse_decimal_field(field, false, &parsed.to) || parsed.to < 0 || parsed.to > 255 ||
        !take_field(&p, " | TX: ", parsed.bssid, sizeof(parsed.bssid)) ||
        !bssid_valid(parsed.bssid) ||
        !take_field(&p, " | RSSI: ", parsed.tx, sizeof(parsed.tx)) ||
        !bssid_valid(parsed.tx) ||
        !take_field(&p, " | VIA: ", field, sizeof(field)) ||
        !parse_decimal_field(field, true, &parsed.rssi) ||
        !take_field(&p, " | COUNT: ", parsed.via, sizeof(parsed.via)) ||
        !take_field(&p, " | MODE: ", field, sizeof(field)) ||
        !parse_decimal_field(field, false, &parsed.count) ||
        !take_field(&p, " | CLASS: ", field, sizeof(field)) ||
        !parse_decimal_field(field, false, &parsed.mode) ||
        !take_field(&p, " | SSID: ", parsed.class_name, sizeof(parsed.class_name))) {
        return false;
    }
    if (parsed.count < 0 || parsed.count > 255 ||
        parsed.mode < 0 || parsed.mode > 255 ||
        strlen(p) > 32) return false;
    snprintf(parsed.ssid, sizeof(parsed.ssid), "%s", p);
    *event = parsed;
    return true;
}

/* JanOS scan rows have exactly eight quoted fields, with RFC 4180 "" escapes. */
static const char *read_csv_field(const char *p, char *out, size_t capacity)
{
    if (!p || *p != '"') return NULL;
    ++p;
    size_t length = 0;
    while (*p) {
        if (*p == '"') {
            if (p[1] == '"') {
                if (out && length + 1 >= capacity) return NULL;
                if (out) out[length] = '"';
                ++length;
                p += 2;
                continue;
            }
            if (out) out[length] = '\0';
            return p + 1;
        }
        if (out && length + 1 >= capacity) return NULL;
        if (out) out[length] = *p;
        ++length;
        ++p;
    }
    return NULL;
}

static bool parse_scan_row(const char *line, scan_ap_t *entry)
{
    if (!line || !entry) return false;
    scan_ap_t parsed = {0};
    char index[4], channel[4];
    char *outputs[] = { index, parsed.ssid, NULL, parsed.bssid, channel,
                        NULL, NULL, NULL };
    const size_t capacities[] = { sizeof(index), sizeof(parsed.ssid), 0,
                                  sizeof(parsed.bssid), sizeof(channel), 0, 0, 0 };
    const char *p = line;
    for (int i = 0; i < 8; ++i) {
        p = read_csv_field(p, outputs[i], capacities[i]);
        if (!p) return false;
        if (i < 7) {
            if (*p != ',') return false;
            ++p;
        } else if (*p != '\0') {
            return false;
        }
    }
    if (!parse_decimal_field(index, false, &parsed.index) ||
        !parse_decimal_field(channel, false, &parsed.channel) ||
        parsed.index < 1 || parsed.index > MAX_SCAN_APS ||
        parsed.channel < 1 || parsed.channel > 255 ||
        !bssid_valid(parsed.bssid)) return false;
    *entry = parsed;
    return true;
}

/* WIFI_EVENT_SCAN_DONE prints the fresh rows automatically, without the
   `Showing results ...` header used by show_scan_results. */
static bool parse_scan_retrieved(const char *line, int *count)
{
    const char *prefix = "Retrieved ";
    if (strncmp(line, prefix, strlen(prefix)) != 0) return false;
    const char *end = NULL;
    int parsed = 0;
    if (!parse_integer(line + strlen(prefix), false, &parsed, &end) ||
        strncmp(end, " network records", strlen(" network records")) != 0) {
        return false;
    }
    end += strlen(" network records");
    if (*end != '\0' && strncmp(end, " in ", 4) != 0) return false;
    *count = parsed;
    return true;
}

static bool parse_watch_line(const char *line, int *count, int *ordinal,
                             int *total, int *channel, char bssid[18])
{
    unsigned c = 0, n = 0, t = 0, ch = 0;
    char mac[18] = {0};
    if (sscanf(line,
               "[DETECTOR_WATCH] count=%u ap=%u/%u CH=%u BSSID=%17s",
               &c, &n, &t, &ch, mac) != 5 ||
        c > MAX_SCAN_APS || n > MAX_SCAN_APS || t > MAX_SCAN_APS ||
        ch > 255 || !bssid_valid(mac)) return false;
    const char *mac_field = strstr(line, " BSSID=");
    if (!mac_field || strncmp(mac_field + 7, mac, 17) != 0 ||
        strncmp(mac_field + 24, " SSID=", 6) != 0) return false;
    *count = (int)c;
    *ordinal = (int)n;
    *total = (int)t;
    *channel = (int)ch;
    memcpy(bssid, mac, 18);
    return true;
}

static void fail(deauth_detector_data_t *data, const char *message)
{
    clear_scan(data);
    set_state(data, DET_ERROR);
    set_message(data, message);
}

static void start_all(deauth_detector_data_t *data)
{
    clear_scan(data);
    clear_events(data);
    data->view = VIEW_DEAUTH;
    set_state(data, DET_START_ALL);
    set_message(data, "Waiting for JanOS start");
    if (uart_send_command("deauth_detector") != 0) {
        fail(data, "UART send failed");
    }
}

static void start_scan(deauth_detector_data_t *data)
{
    clear_scan(data);
    data->view = VIEW_DEAUTH;
    set_state(data, DET_SCAN_WAIT_ACK);
    set_message(data, "Fresh scan: waiting");
    if (uart_send_command("scan_networks") != 0) {
        fail(data, "Scan command failed");
    }
}

static void stop_then(deauth_detector_data_t *data, detector_state_t next)
{
    detector_state_t previous = data->state;
    set_state(data, next);
    set_message(data, "Waiting for stop");
    if (uart_send_command("stop") != 0) {
        if (next == DET_STOP_EXIT) {
            data->stop_send_failed = true;
            set_state(data, previous == DET_ACTIVE_ALL || previous == DET_ACTIVE_SELECTED ?
                      previous : DET_ERROR);
            set_message(data, "Stop send failed; may run");
        } else {
            fail(data, "Stop command failed");
        }
    } else {
        data->stop_send_failed = false;
    }
}

static int selected_count(const deauth_detector_data_t *data)
{
    int count = 0;
    for (int i = 0; i < data->scan_count; ++i) {
        if (data->scan[i].selected) ++count;
    }
    return count;
}

static void start_selected(deauth_detector_data_t *data)
{
    char command[256] = "deauth_detector --only";
    size_t used = strlen(command);
    data->watch_expected_count = 0;
    data->verified_watch_count = 0;
    data->start_ack = false;
    for (int i = 0; i < data->scan_count; ++i) {
        if (!data->scan[i].selected) continue;
        int n = snprintf(command + used, sizeof(command) - used, " %d", data->scan[i].index);
        if (n < 1 || (size_t)n >= sizeof(command) - used) {
            fail(data, "Selection too large");
            return;
        }
        used += (size_t)n;
        bool already_watched = false;
        for (int j = 0; j < data->watch_expected_count; ++j) {
            if (bssid_equal(data->scan[data->watch_indices[j]].bssid,
                            data->scan[i].bssid)) {
                already_watched = true;
                break;
            }
        }
        if (already_watched) continue;
        if (data->watch_expected_count >= MAX_SCAN_APS) {
            fail(data, "Selection too large");
            return;
        }
        data->watch_indices[data->watch_expected_count++] = i;
    }
    if (data->watch_expected_count == 0) {
        set_message(data, "Select at least one AP");
        return;
    }
    data->ping_waiting = false;
    clear_events(data);
    set_state(data, DET_START_SELECTED);
    set_message(data, "Waiting for watch proof");
    if (uart_send_command(command) != 0) {
        fail(data, "UART send failed");
    }
}

static bool is_start_rejection(const char *line)
{
    static const char *const errors[] = {
        "No completed scan results available",
        "No scan results available",
        "Scan results unavailable",
        "Invalid watch index",
        "No valid channels selected",
        "Too many APs selected",
        "Detector not started",
        "Deauth detector already active",
        "Regular sniffer is active",
        "Sniffer Dog is active",
        "Failed to create Deauth Detector",
        "Detector receiver setup failed",
        "Detector initial channel",
        "Scan timed out",
        "Scan cancelled by user",
        "\"code\":\"busy\""
    };
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        if (strstr(line, errors[i])) return true;
    }
    return false;
}

static void reject_start(deauth_detector_data_t *data, const char *line)
{
    char message[64];
    snprintf(message, sizeof(message), "%.63s", line);
    fail(data, message);
}

static void reject_watch(deauth_detector_data_t *data)
{
    set_state(data, DET_STOP_BAD_WATCH);
    set_message(data, "BSSID mismatch; stopping");
    if (uart_send_command("stop") != 0) {
        fail(data, "BSSID mismatch; stop failed");
    }
}

static void accept_watch(deauth_detector_data_t *data, const char *line)
{
    int count, ordinal, total, channel;
    char bssid[18];
    if (!parse_watch_line(line, &count, &ordinal, &total, &channel, bssid) ||
        count != data->watch_expected_count || total != data->watch_expected_count ||
        ordinal != data->verified_watch_count + 1) {
        reject_watch(data);
        return;
    }
    int scan_index = data->watch_indices[data->verified_watch_count];
    scan_ap_t *expected = &data->scan[scan_index];
    /* JanOS prints the first cached channel for a BSSID even when a later
       scan row supplied the selected index. The BSSID is the watch identity. */
    int first_channel = -1;
    for (int i = 0; i < data->scan_count; ++i) {
        if (bssid_equal(data->scan[i].bssid, bssid)) {
            first_channel = data->scan[i].channel;
            break;
        }
    }
    if (!bssid_equal(expected->bssid, bssid) || channel != first_channel) {
        reject_watch(data);
        return;
    }
    ++data->verified_watch_count;
    data->needs_redraw = true;
    if (data->start_ack && data->verified_watch_count == data->watch_expected_count) {
        set_state(data, DET_ACTIVE_SELECTED);
        data->last_ping_us = now_us();
        set_message(data, "BSSID watch verified");
    }
}

static bool stopped_line(const char *line)
{
    /* JanOS prints the detector-specific line before its global Wi-Fi cleanup.
       Only this final line confirms that the next command or exit is safe. */
    return strstr(line, "All operations stopped.") != NULL;
}

static bool watched_bssid(const deauth_detector_data_t *data, const char *bssid)
{
    for (int i = 0; i < data->watch_expected_count; ++i) {
        int row = data->watch_indices[i];
        if (row >= 0 && row < data->scan_count &&
            bssid_equal(data->scan[row].bssid, bssid)) return true;
    }
    return false;
}

static void handle_scan_line(deauth_detector_data_t *data, const char *line)
{
    if (strstr(line, "Scan failed with status:")) {
        fail(data, "JanOS scan failed");
        return;
    }
    if (data->state == DET_SCAN_WAIT_ACK) {
        if (strstr(line, "Background scan started (")) {
            set_state(data, DET_SCAN_RUNNING);
            set_message(data, "Scanning JanOS...");
        } else if (strstr(line, "\"code\":\"busy\"") ||
                   strstr(line, "Scan already in progress") ||
                   strstr(line, "Wardrive is active") ||
                   strstr(line, "Failed to start scan")) {
            fail(data, "JanOS rejected scan");
        }
        return;
    }

    if (strstr(line, "No networks found in last scan.")) {
        fail(data, "Fresh scan found no AP");
        return;
    }
    int expected = 0;
    if (data->state == DET_SCAN_RUNNING && parse_scan_retrieved(line, &expected)) {
        if (expected < 1 || expected > MAX_SCAN_APS) {
            fail(data, "Scan count outside 1-64");
            return;
        }
        data->scan_count = 0;
        data->expected_scan_count = expected;
        set_state(data, DET_SCAN_COLLECT);
        return;
    }
    if (strncmp(line, "Showing results from last scan (", 32) == 0) {
        if (data->state == DET_SCAN_COLLECT) fail(data, "Overlapping scan output");
        return;
    }
    if (data->state != DET_SCAN_COLLECT) return;
    if (strcmp(line, "Scan results printed.") == 0) {
        if (data->scan_count != data->expected_scan_count) {
            fail(data, "Incomplete JanOS scan");
            return;
        }
        set_state(data, DET_PICK);
        data->last_ping_us = now_us();
        set_message(data, "Choose BSSID(s)");
        return;
    }
    if (*line != '"') return;
    scan_ap_t parsed;
    if (!parse_scan_row(line, &parsed) ||
        data->scan_count >= data->expected_scan_count ||
        parsed.index != data->scan_count + 1) {
        fail(data, "Invalid scan index/row");
        return;
    }
    data->scan[data->scan_count++] = parsed;
    data->needs_redraw = true;
}

static void uart_line_callback(const char *line, void *user_data)
{
    deauth_detector_data_t *data = user_data;
    if (!data || !line || data->leaving) return;

    /* A separate scan on this UART replaces the numbered JanOS snapshot.
       Once the list has been offered to the user, its indices cannot survive
       a new scan or another printed result set. */
    if ((data->state == DET_PICK || data->state == DET_START_SELECTED ||
         data->state == DET_ACTIVE_SELECTED) &&
        (strstr(line, "Background scan started (") ||
         strncmp(line, "Showing results from last scan (", 32) == 0 ||
         strcmp(line, "Scan results printed.") == 0)) {
        if (data->state != DET_PICK) uart_send_command("stop");
        fail(data, "Scan changed; select again");
        return;
    }

    if (strcmp(line, "pong") == 0 && data->ping_waiting) {
        data->ping_waiting = false;
        data->last_ping_us = now_us();
        return;
    }
    if ((data->state == DET_STOP_FOR_SCAN || data->state == DET_STOP_FOR_ALL ||
         data->state == DET_STOP_BAD_WATCH || data->state == DET_STOP_EXIT) &&
        stopped_line(line)) {
        detector_state_t state = data->state;
        if (state == DET_STOP_FOR_SCAN) start_scan(data);
        else if (state == DET_STOP_FOR_ALL) start_all(data);
        else if (state == DET_STOP_BAD_WATCH) fail(data, "BSSID watch mismatch");
        else data->exit_ready = true;
        return;
    }
    if (data->state == DET_SCAN_WAIT_ACK || data->state == DET_SCAN_RUNNING ||
        data->state == DET_SCAN_COLLECT) {
        handle_scan_line(data, line);
        return;
    }

    if ((data->state == DET_START_ALL || data->state == DET_START_SELECTED) &&
        is_start_rejection(line)) {
        reject_start(data, line);
        return;
    }
    if (data->state == DET_START_ALL &&
        strstr(line, "Deauth Detector started - scanning all channels")) {
        set_state(data, DET_ACTIVE_ALL);
        data->last_ping_us = now_us();
        set_message(data, "All channels active");
    } else if (data->state == DET_START_SELECTED &&
               strstr(line, "Deauth Detector started - monitoring selected channels")) {
        data->start_ack = true;
        data->needs_redraw = true;
        if (data->verified_watch_count == data->watch_expected_count) {
            set_state(data, DET_ACTIVE_SELECTED);
            data->last_ping_us = now_us();
        }
    } else if (data->state == DET_START_SELECTED &&
               strncmp(line, "[DETECTOR_WATCH]", 16) == 0 && data->start_ack) {
        accept_watch(data, line);
    } else if (data->state == DET_ACTIVE_SELECTED &&
               strncmp(line, "[DETECTOR_WATCH]", 16) == 0) {
        reject_watch(data);
    }

    /* Status and example lines are never events. */
    if (strncmp(line, "[DETECTOR_INFO]", 15) == 0 ||
        strncmp(line, "[DETECTOR_WATCH]", 16) == 0) return;

    /* JanOS can receive frames before it prints its start and WATCH proof.
       Do not display those frames while ownership is still unconfirmed. */
    if (data->state == DET_ACTIVE_ALL || data->state == DET_ACTIVE_SELECTED) {
        deauth_event_t deauth;
        csa_event_t csa;
        if (parse_deauth_line(line, &deauth)) {
            if (data->state == DET_ACTIVE_SELECTED && !watched_bssid(data, deauth.bssid)) return;
            data->last_deauth = deauth;
            if (data->deauth_count < INT_MAX) ++data->deauth_count;
            data->needs_redraw = true;
        } else if (parse_csa_line(line, &csa)) {
            if (data->state == DET_ACTIVE_SELECTED && !watched_bssid(data, csa.bssid)) return;
            data->last_csa = csa;
            if (data->csa_count < INT_MAX) ++data->csa_count;
            data->needs_redraw = true;
        }
    }
}

static void print_line(int row, const char *text, int color)
{
    ui_print(0, row, text, color);
}

static void draw_scan(const deauth_detector_data_t *data)
{
    char line[40];
    ui_print_center(2, "Fresh JanOS scan", UI_COLOR_TEXT);
    if (data->state == DET_SCAN_WAIT_ACK) {
        ui_print_center(3, "Waiting for scan ACK", UI_COLOR_HIGHLIGHT);
    } else if (data->state == DET_SCAN_COLLECT) {
        snprintf(line, sizeof(line), "Rows %d/%d", data->scan_count, data->expected_scan_count);
        ui_print_center(3, line, UI_COLOR_HIGHLIGHT);
    } else {
        ui_print_center(3, "Waiting for results", UI_COLOR_HIGHLIGHT);
    }
    ui_print_center(5, "Indices valid after marker", UI_COLOR_DIMMED);
    ui_draw_status("ESC/Q: Stop and back");
}

static void draw_pick(const deauth_detector_data_t *data)
{
    char line[64];
    snprintf(line, sizeof(line), "Wybrane sieci %d/%d", selected_count(data), data->scan_count);
    print_line(1, line, UI_COLOR_TEXT);
    int first = (data->cursor / 2) * 2;
    for (int slot = 0; slot < 2 && first + slot < data->scan_count; ++slot) {
        int i = first + slot;
        const scan_ap_t *ap = &data->scan[i];
        snprintf(line, sizeof(line), "%c%c %d %.21s",
                 i == data->cursor ? '>' : ' ',
                 ap->selected ? '*' : ' ', ap->index,
                 ap->ssid[0] ? ap->ssid : "<Hidden>");
        print_line(2 + slot * 2, line, i == data->cursor ? UI_COLOR_HIGHLIGHT : UI_COLOR_TEXT);
        snprintf(line, sizeof(line), " CH %-3d %s", ap->channel, ap->bssid);
        print_line(3 + slot * 2, line, UI_COLOR_DIMMED);
    }
    print_line(6, data->message, UI_COLOR_HIGHLIGHT);
    ui_draw_status("SP:Mark EN:Start R:Scan A:All");
}

static void draw_watch(const deauth_detector_data_t *data)
{
    char line[64];
    snprintf(line, sizeof(line), "Watching %d/%d verified",
             data->verified_watch_count, data->watch_expected_count);
    print_line(1, line, UI_COLOR_HIGHLIGHT);
    int first = (data->cursor / 2) * 2;
    for (int slot = 0; slot < 2 && first + slot < data->watch_expected_count; ++slot) {
        const scan_ap_t *ap = &data->scan[data->watch_indices[first + slot]];
        snprintf(line, sizeof(line), "%d CH %d %.20s",
                 ap->index, ap->channel, ap->ssid[0] ? ap->ssid : "<Hidden>");
        print_line(2 + slot * 2, line, UI_COLOR_TEXT);
        snprintf(line, sizeof(line), " %s", ap->bssid);
        print_line(3 + slot * 2, line, UI_COLOR_DIMMED);
    }
    ui_draw_status("UP/DN browse C:CSA S:Scan");
}

static void draw_deauth(const deauth_detector_data_t *data)
{
    char line[64];
    if (data->state == DET_ACTIVE_SELECTED) {
        snprintf(line, sizeof(line), "Wybrane sieci: %d BSSID", data->watch_expected_count);
    } else {
        snprintf(line, sizeof(line), "Wszystkie sieci / kanaly");
    }
    print_line(1, line, UI_COLOR_HIGHLIGHT);
    snprintf(line, sizeof(line), "DEAUTH %d   CSA %d", data->deauth_count, data->csa_count);
    print_line(2, line, UI_COLOR_TEXT);
    if (data->deauth_count > 0) {
        snprintf(line, sizeof(line), "CH %d  RSSI %d dBm",
                 data->last_deauth.channel, data->last_deauth.rssi);
        print_line(3, line, UI_COLOR_HIGHLIGHT);
        snprintf(line, sizeof(line), "AP %.26s", data->last_deauth.ap_name);
        print_line(4, line, UI_COLOR_TEXT);
        snprintf(line, sizeof(line), "BSSID %s", data->last_deauth.bssid);
        print_line(5, line, UI_COLOR_DIMMED);
    } else {
        ui_print_center(4, "Waiting for DEAUTH", UI_COLOR_DIMMED);
    }
    if (data->state == DET_ACTIVE_SELECTED) {
        print_line(6, "W: watched BSSIDs", UI_COLOR_TEXT);
    } else {
        print_line(6, "C: CSA notices", UI_COLOR_TEXT);
    }
    ui_draw_status("S:Scan A:All C:CSA ESC:Stop");
}

static void draw_csa(const deauth_detector_data_t *data)
{
    char line[64];
    snprintf(line, sizeof(line), "CSA #%d: AP switch unconfirmed", data->csa_count);
    print_line(1, line, UI_COLOR_HIGHLIGHT);
    if (data->csa_count > 0) {
        snprintf(line, sizeof(line), "SSID %.24s", data->last_csa.ssid);
        print_line(2, line, UI_COLOR_TEXT);
        snprintf(line, sizeof(line), "BSSID %s", data->last_csa.bssid);
        print_line(3, line, UI_COLOR_DIMMED);
        snprintf(line, sizeof(line), "RX %d -> TO %d  %d dBm",
                 data->last_csa.rx, data->last_csa.to, data->last_csa.rssi);
        print_line(4, line, UI_COLOR_HIGHLIGHT);
        snprintf(line, sizeof(line), "TX %s", data->last_csa.tx);
        print_line(5, line, UI_COLOR_DIMMED);
        snprintf(line, sizeof(line), "VIA %s CNT %d M%d CL %s",
                 data->last_csa.via, data->last_csa.count,
                 data->last_csa.mode, data->last_csa.class_name);
        print_line(6, line, UI_COLOR_TEXT);
    } else {
        ui_print_center(3, "No CSA announcement yet", UI_COLOR_DIMMED);
        ui_print_center(5, "AP switch unconfirmed", UI_COLOR_DIMMED);
    }
    ui_draw_status("C:DEAUTH W:Watch ESC:Stop");
}

static void draw_screen(screen_t *self)
{
    deauth_detector_data_t *data = self->user_data;
    if (!data) return;
    ui_clear();
    ui_draw_title(data->view == VIEW_CSA ? "CSA ANNOUNCEMENTS" : "DEAUTH DETECTOR");
    if (data->state == DET_SCAN_WAIT_ACK || data->state == DET_SCAN_RUNNING ||
        data->state == DET_SCAN_COLLECT) {
        draw_scan(data);
    } else if (data->state == DET_PICK) {
        draw_pick(data);
    } else if (data->state == DET_ACTIVE_ALL || data->state == DET_ACTIVE_SELECTED) {
        if (data->view == VIEW_CSA) draw_csa(data);
        else if (data->view == VIEW_WATCH && data->state == DET_ACTIVE_SELECTED) draw_watch(data);
        else draw_deauth(data);
    } else {
        ui_print_center(2, data->message, UI_COLOR_HIGHLIGHT);
        if (data->state == DET_START_SELECTED) {
            char line[40];
            snprintf(line, sizeof(line), "WATCH %d/%d",
                     data->verified_watch_count, data->watch_expected_count);
            ui_print_center(4, line, UI_COLOR_TEXT);
        } else if (data->state == DET_ERROR || data->state == DET_DISCONNECTED) {
            ui_print_center(4, "Detector not confirmed", UI_COLOR_TEXT);
            ui_print_center(5, "R: fresh scan   A: all", UI_COLOR_DIMMED);
        }
        ui_draw_status("ESC/Q: Stop and back");
    }
    if (data->stop_send_failed) {
        ui_print(0, 6, data->message, UI_COLOR_HIGHLIGHT);
        ui_draw_status("ESC/Q: Retry stop");
    }
}

static void check_ping(deauth_detector_data_t *data, int64_t now)
{
    if (data->state != DET_PICK && data->state != DET_ACTIVE_ALL &&
        data->state != DET_ACTIVE_SELECTED) return;
    if (data->ping_waiting && now - data->ping_sent_us >= PING_TIMEOUT_US) {
        clear_scan(data);
        data->ping_waiting = false;
        set_state(data, DET_DISCONNECTED);
        set_message(data, "JanOS connection lost");
        return;
    }
    if (!data->ping_waiting && now - data->last_ping_us >= PING_INTERVAL_US) {
        if (uart_send_command("ping") != 0) {
            clear_scan(data);
            set_state(data, DET_DISCONNECTED);
            set_message(data, "UART connection lost");
            return;
        }
        data->ping_waiting = true;
        data->ping_sent_us = now;
    }
}

static void on_tick(screen_t *self)
{
    deauth_detector_data_t *data = self->user_data;
    if (!data || data->leaving) return;
    if (data->state == DET_STOP_EXIT && data->exit_ready) {
        data->leaving = true;
        screen_manager_pop();
        return;
    }
    int64_t now = now_us();
    if (data->state == DET_START_ALL &&
        now - data->state_started_us >= ALL_START_TIMEOUT_US) {
        /* The receiver may have started even though its ACK was lost. */
        uart_send_command("stop");
        fail(data, "No JanOS start response");
    } else if (data->state == DET_START_SELECTED &&
               now - data->state_started_us >= SELECTED_START_TIMEOUT_US) {
        uart_send_command("stop");
        fail(data, data->start_ack ? "WATCH proof timed out" : "No JanOS start response");
    } else if (data->state == DET_SCAN_WAIT_ACK &&
               now - data->state_started_us >= SCAN_ACK_TIMEOUT_US) {
        fail(data, "No JanOS scan response");
    } else if ((data->state == DET_SCAN_RUNNING || data->state == DET_SCAN_COLLECT) &&
               now - data->state_started_us >= SCAN_TIMEOUT_US) {
        fail(data, "JanOS scan timed out");
    } else if ((data->state == DET_STOP_FOR_SCAN || data->state == DET_STOP_FOR_ALL ||
                data->state == DET_STOP_BAD_WATCH || data->state == DET_STOP_EXIT) &&
               now - data->state_started_us >= STOP_TIMEOUT_US) {
        fail(data, "No JanOS stop response");
    }
    check_ping(data, now);
    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    deauth_detector_data_t *data = self->user_data;
    if (!data) return;
    if (key == KEY_ESC || key == KEY_Q) {
        if (data->state == DET_PICK || data->state == DET_DISCONNECTED) {
            data->leaving = true;
            screen_manager_pop();
        } else if (data->state == DET_STOP_FOR_SCAN ||
                   data->state == DET_STOP_FOR_ALL ||
                   data->state == DET_STOP_BAD_WATCH) {
            set_state(data, DET_STOP_EXIT);
            set_message(data, "Waiting for stop");
        } else if (data->state != DET_STOP_EXIT) {
            stop_then(data, DET_STOP_EXIT);
        }
        return;
    }
    if (key == KEY_S &&
        (data->state == DET_START_ALL || data->state == DET_START_SELECTED ||
         data->state == DET_ACTIVE_ALL || data->state == DET_ACTIVE_SELECTED)) {
        stop_then(data, DET_STOP_FOR_SCAN);
        return;
    }
    if (key == KEY_A &&
        (data->state == DET_ACTIVE_ALL || data->state == DET_ACTIVE_SELECTED)) {
        if (data->state == DET_ACTIVE_SELECTED) stop_then(data, DET_STOP_FOR_ALL);
        return;
    }
    if (key == KEY_A && data->state == DET_PICK) {
        start_all(data);
        return;
    }
    if (key == KEY_A && (data->state == DET_ERROR || data->state == DET_DISCONNECTED)) {
        stop_then(data, DET_STOP_FOR_ALL);
        return;
    }
    if (key == KEY_R && data->state == DET_PICK) {
        start_scan(data);
        return;
    }
    if (key == KEY_R && (data->state == DET_ERROR || data->state == DET_DISCONNECTED)) {
        stop_then(data, DET_STOP_FOR_SCAN);
        return;
    }
    if (data->state == DET_PICK) {
        if (key == KEY_UP && data->cursor > 0) --data->cursor;
        else if (key == KEY_DOWN && data->cursor + 1 < data->scan_count) ++data->cursor;
        else if (key == KEY_SPACE && data->scan_count > 0) {
            data->scan[data->cursor].selected = !data->scan[data->cursor].selected;
        } else if (key == KEY_ENTER) {
            start_selected(data);
        }
        data->needs_redraw = true;
        return;
    }
    if (data->state != DET_ACTIVE_ALL && data->state != DET_ACTIVE_SELECTED) return;
    if (key == KEY_C) {
        data->view = data->view == VIEW_CSA ? VIEW_DEAUTH : VIEW_CSA;
        data->needs_redraw = true;
    } else if (key == KEY_W && data->state == DET_ACTIVE_SELECTED) {
        data->view = VIEW_WATCH;
        data->cursor = 0;
        data->needs_redraw = true;
    } else if (data->view == VIEW_WATCH && key == KEY_UP && data->cursor > 0) {
        --data->cursor;
        data->needs_redraw = true;
    } else if (data->view == VIEW_WATCH && key == KEY_DOWN &&
               data->cursor + 1 < data->watch_expected_count) {
        ++data->cursor;
        data->needs_redraw = true;
    }
}

static void on_destroy(screen_t *self)
{
    uart_clear_line_callback();
    free(self->user_data);
}

screen_t *deauth_detector_screen_create(void *params)
{
    (void)params;
    screen_t *screen = screen_alloc();
    if (!screen) return NULL;
    deauth_detector_data_t *data = calloc(1, sizeof(*data));
    if (!data) {
        free(screen);
        return NULL;
    }
    screen->user_data = data;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    uart_register_line_callback(uart_line_callback, data);
    start_all(data);
    draw_screen(screen);
    ESP_LOGI(TAG, "Detector screen opened");
    return screen;
}
