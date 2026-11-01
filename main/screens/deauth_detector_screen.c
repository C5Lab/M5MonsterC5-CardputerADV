/**
 * @file deauth_detector_screen.c
 * @brief JanOS DEAUTH, Trace, and Defence controller on the Cardputer UART.
 *
 * The user chooses Passive, Trace, Defence, or Defence + PMKID Lab. Active
 * Defence always uses a fresh JanOS scan and an exact local BSSID selection.
 */

#include "deauth_detector_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
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
#define DET_CONTENT_Y_START 19
#define DET_CONTENT_Y_END 117
#define DET_CONTENT_HEIGHT (DET_CONTENT_Y_END - DET_CONTENT_Y_START)
#define FORENSIC_DETAIL_LIMIT 16
#define FORENSIC_REASON_LIMIT 16
#define FORENSIC_BURST_GAP_US 20000000ULL

typedef enum {
    DET_TRACE_CHOICE,
    DET_PMKID_WARNING,
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
    DET_MODE_PASSIVE,
    DET_MODE_TRACE,
    DET_MODE_DEFENCE,
    DET_MODE_DEFENCE_PMKID,
    DET_MODE_COUNT
} detector_mode_t;

typedef enum {
    VIEW_DEAUTH,
    VIEW_CSA,
    VIEW_WATCH,
    VIEW_FORENSICS
} detector_view_t;

typedef enum {
    TRACE_STATUS_OFF,
    TRACE_STATUS_WAITING,
    TRACE_STATUS_ARMED,
    TRACE_STATUS_CONFIRMING,
    TRACE_STATUS_LOCKED,
    TRACE_STATUS_RELEASED,
    TRACE_STATUS_TUNE_FAILED,
    TRACE_STATUS_UNAVAILABLE
} detector_trace_status_t;

typedef enum {
    DEFENCE_STATUS_OFF,
    DEFENCE_STATUS_WAITING,
    DEFENCE_STATUS_ARMED,
    DEFENCE_STATUS_PROVISIONAL,
    DEFENCE_STATUS_RESPONDING,
    DEFENCE_STATUS_RESPONSE_OK,
    DEFENCE_STATUS_RESPONSE_ERROR,
    DEFENCE_STATUS_RELEASED,
    DEFENCE_STATUS_STOPPED,
    DEFENCE_STATUS_UNSUPPORTED
} detector_defence_status_t;

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
    char tx[18];
    char da[18];
    int rssi;
    int reason;
    bool reason_known;
    bool detailed;
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

typedef enum {
    FORENSIC_FRAME_DEAUTH,
    FORENSIC_FRAME_DISASSOC
} forensic_frame_t;

typedef enum {
    FORENSIC_SEQ_INSUFFICIENT,
    FORENSIC_SEQ_DELTA,
    FORENSIC_SEQ_RETRANSMISSION,
    FORENSIC_SEQ_REPEATED
} forensic_sequence_state_t;

typedef struct {
    forensic_frame_t frame;
    bool channel_known;
    int channel;
    char bssid[18];
    char tx[18];
    char da[18];
    bool rssi_known;
    int rssi;
    bool reason_known;
    unsigned reason;
    bool sequence_known;
    unsigned sequence;
    bool retry_known;
    unsigned retry;
    bool protected_known;
    unsigned protected_frame;
    bool fc_known;
    unsigned fc;
    uint64_t ts_us;
} forensic_row_t;

typedef struct {
    unsigned reason;
    unsigned count;
} forensic_reason_count_t;

typedef struct {
    forensic_row_t detail[FORENSIC_DETAIL_LIMIT];
    unsigned detail_count;
    unsigned accepted;
    unsigned dropped;
    unsigned unique_bssids;
    unsigned unique_das;
    unsigned bursts;
    unsigned largest_burst;
    unsigned current_burst;
    unsigned protected_count;
    unsigned unprotected_count;
    unsigned retry_count;
    /* Count every unavailable optional field, not just rows where retry or
       protection is unknown. This keeps partial evidence visibly partial. */
    unsigned unknown_count;
    unsigned unknown_rssi;
    unsigned repeated_sequences;
    forensic_reason_count_t reasons[FORENSIC_REASON_LIMIT];
    unsigned reason_count;
    bool rssi_available;
    int rssi_min;
    int rssi_max;
    int64_t rssi_sum;
    unsigned rssi_samples;
    bool last_burst_valid;
    forensic_row_t last_burst_row;
    forensic_sequence_state_t sequence_state;
    bool latest_sequence_known;
    unsigned latest_sequence;
    unsigned latest_delta;
    bool latest_retry_known;
    unsigned latest_retry;
    bool finalized;
} forensic_session_t;

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
    bool v2_requested;
    bool v2_confirmed;
    detector_mode_t mode;
    bool trace_enabled;
    detector_trace_status_t trace_status;
    char trace_bssid[18];
    int trace_first_channel;
    int trace_window_ms;
    int trace_channel;
    int trace_idle_ms;
    char trace_error[32];
    detector_defence_status_t defence_status;
    char defence_bssid[18];
    char defence_response[16];
    char defence_error[32];
    char defence_outcome[12];
    int defence_channel;
    unsigned defence_responses;
    unsigned defence_suppressed;
    unsigned defence_summary_responses;
    bool defence_summary_valid;
    bool defence_armed_latched;
    bool defence_transaction_pending;
    forensic_session_t forensic;
    bool forensic_accepting;
    bool ping_waiting;
    bool exit_ready;
    bool exit_confirm_pending;
    bool stop_pending;
    bool stop_send_failed;
    bool needs_redraw;
    bool leaving;
    bool ui_initialized;
    char rendered_title[40];
    int64_t state_started_us;
    int64_t last_ping_us;
    int64_t ping_sent_us;
    char legacy_start_command[256];
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

static void clear_trace_attempt(deauth_detector_data_t *data)
{
    data->trace_bssid[0] = '\0';
    data->trace_first_channel = 0;
    data->trace_window_ms = 0;
}

static void clear_trace_status(deauth_detector_data_t *data)
{
    data->trace_status = TRACE_STATUS_OFF;
    clear_trace_attempt(data);
    data->trace_channel = 0;
    data->trace_idle_ms = 0;
    data->trace_error[0] = '\0';
    data->needs_redraw = true;
}

static void begin_trace_run(deauth_detector_data_t *data)
{
    clear_trace_status(data);
    if (data->trace_enabled) data->trace_status = TRACE_STATUS_WAITING;
}

static void mark_trace_unavailable(deauth_detector_data_t *data)
{
    clear_trace_status(data);
    data->trace_status = TRACE_STATUS_UNAVAILABLE;
}

static bool defence_requested(const deauth_detector_data_t *data)
{
    return data->mode >= DET_MODE_DEFENCE;
}

static void clear_defence_status(deauth_detector_data_t *data)
{
    data->defence_status = DEFENCE_STATUS_OFF;
    data->defence_bssid[0] = '\0';
    data->defence_response[0] = '\0';
    data->defence_error[0] = '\0';
    data->defence_outcome[0] = '\0';
    data->defence_channel = 0;
    data->defence_responses = 0;
    data->defence_suppressed = 0;
    data->defence_summary_responses = 0;
    data->defence_summary_valid = false;
    data->defence_armed_latched = false;
    data->defence_transaction_pending = false;
    data->needs_redraw = true;
}

static void begin_defence_run(deauth_detector_data_t *data)
{
    clear_defence_status(data);
    if (defence_requested(data)) data->defence_status = DEFENCE_STATUS_WAITING;
}

static void mark_defence_unsupported(deauth_detector_data_t *data)
{
    clear_defence_status(data);
    data->defence_status = DEFENCE_STATUS_UNSUPPORTED;
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

static bool parse_u64_field(const char *field, uint64_t *value)
{
    if (!field || !isdigit((unsigned char)*field)) return false;
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(field, &end, 10);
    if (errno == ERANGE || *end != '\0') return false;
    *value = (uint64_t)parsed;
    return true;
}

static bool parse_unsigned_field(const char *field, unsigned *value)
{
    uint64_t parsed = 0;
    if (!parse_u64_field(field, &parsed) || parsed > UINT_MAX) return false;
    *value = (unsigned)parsed;
    return true;
}

static bool status_token_valid(const char *token)
{
    size_t length = token ? strlen(token) : 0;
    if (length == 0 || length > 31) return false;
    for (const unsigned char *ch = (const unsigned char *)token; *ch; ++ch) {
        if (!isalnum(*ch) && *ch != '_' && *ch != '-' && *ch != '.') return false;
    }
    return true;
}

static bool mac_or_dash_valid(const char *value)
{
    return strcmp(value, "-") == 0 || bssid_valid(value);
}

static void increment_saturated(unsigned *value)
{
    if (*value < UINT_MAX) ++*value;
}

static void reset_forensic_session(deauth_detector_data_t *data)
{
    memset(&data->forensic, 0, sizeof(data->forensic));
    data->forensic.sequence_state = FORENSIC_SEQ_INSUFFICIENT;
    data->forensic_accepting = data->v2_requested;
    data->needs_redraw = true;
}

static bool forensic_same_stream(const forensic_row_t *a,
                                 const forensic_row_t *b)
{
    return a->frame == b->frame && bssid_equal(a->bssid, b->bssid) &&
           bssid_equal(a->tx, b->tx) && bssid_equal(a->da, b->da);
}

static void recalculate_forensic_bounded_counts(forensic_session_t *session)
{
    session->unique_bssids = 0;
    session->unique_das = 0;
    session->reason_count = 0;
    memset(session->reasons, 0, sizeof(session->reasons));
    for (unsigned i = 0; i < session->detail_count; ++i) {
        bool bssid_seen = false;
        bool da_seen = false;
        for (unsigned j = 0; j < i; ++j) {
            if (bssid_equal(session->detail[i].bssid, session->detail[j].bssid)) {
                bssid_seen = true;
            }
            if (bssid_equal(session->detail[i].da, session->detail[j].da)) {
                da_seen = true;
            }
        }
        if (!bssid_seen) increment_saturated(&session->unique_bssids);
        if (!da_seen) increment_saturated(&session->unique_das);
        if (!session->detail[i].reason_known) continue;
        unsigned slot = session->reason_count;
        for (unsigned j = 0; j < session->reason_count; ++j) {
            if (session->reasons[j].reason == session->detail[i].reason) {
                slot = j;
                break;
            }
        }
        if (slot == session->reason_count) {
            if (session->reason_count >= FORENSIC_REASON_LIMIT) continue;
            session->reasons[slot].reason = session->detail[i].reason;
            ++session->reason_count;
        }
        increment_saturated(&session->reasons[slot].count);
    }
}

static void accept_forensic_row(deauth_detector_data_t *data,
                                const forensic_row_t *row)
{
    forensic_session_t *session = &data->forensic;
    session->sequence_state = FORENSIC_SEQ_INSUFFICIENT;
    session->latest_sequence_known = row->sequence_known;
    session->latest_sequence = row->sequence;
    session->latest_retry_known = row->retry_known;
    session->latest_retry = row->retry;
    session->latest_delta = 0;
    if (row->sequence_known) {
        for (unsigned i = session->detail_count; i > 0; --i) {
            const forensic_row_t *previous = &session->detail[i - 1];
            if (!previous->sequence_known || !forensic_same_stream(previous, row)) continue;
            if (previous->sequence == row->sequence) {
                increment_saturated(&session->repeated_sequences);
                if (row->retry_known && row->retry == 1U) {
                    session->sequence_state = FORENSIC_SEQ_RETRANSMISSION;
                } else if (row->retry_known && row->retry == 0U) {
                    session->sequence_state = FORENSIC_SEQ_REPEATED;
                } else {
                    session->sequence_state = FORENSIC_SEQ_DELTA;
                }
            } else {
                session->sequence_state = FORENSIC_SEQ_DELTA;
            }
            session->latest_delta = (row->sequence + 4096U - previous->sequence) % 4096U;
            break;
        }
    }

    bool same_burst = session->last_burst_valid &&
        forensic_same_stream(&session->last_burst_row, row) &&
        row->ts_us >= session->last_burst_row.ts_us &&
        row->ts_us - session->last_burst_row.ts_us <= FORENSIC_BURST_GAP_US;
    if (same_burst) {
        increment_saturated(&session->current_burst);
    } else {
        increment_saturated(&session->bursts);
        session->current_burst = 1;
    }
    if (session->current_burst > session->largest_burst) {
        session->largest_burst = session->current_burst;
    }
    session->last_burst_row = *row;
    session->last_burst_valid = true;

    increment_saturated(&session->accepted);
    if (row->protected_known) {
        if (row->protected_frame) increment_saturated(&session->protected_count);
        else increment_saturated(&session->unprotected_count);
    }
    if (row->retry_known && row->retry) increment_saturated(&session->retry_count);
    if (!row->channel_known) increment_saturated(&session->unknown_count);
    if (!row->rssi_known) {
        increment_saturated(&session->unknown_count);
        increment_saturated(&session->unknown_rssi);
    }
    if (!row->reason_known) increment_saturated(&session->unknown_count);
    if (!row->sequence_known) increment_saturated(&session->unknown_count);
    if (!row->retry_known) increment_saturated(&session->unknown_count);
    if (!row->protected_known) increment_saturated(&session->unknown_count);
    if (!row->fc_known) increment_saturated(&session->unknown_count);
    if (row->rssi_known) {
        if (!session->rssi_available) {
            session->rssi_available = true;
            session->rssi_min = session->rssi_max = row->rssi;
        } else {
            if (row->rssi < session->rssi_min) session->rssi_min = row->rssi;
            if (row->rssi > session->rssi_max) session->rssi_max = row->rssi;
        }
        if (session->rssi_samples < UINT_MAX) {
            session->rssi_sum += row->rssi;
            ++session->rssi_samples;
        }
    }

    if (session->detail_count == FORENSIC_DETAIL_LIMIT) {
        memmove(&session->detail[0], &session->detail[1],
                (FORENSIC_DETAIL_LIMIT - 1U) * sizeof(session->detail[0]));
        --session->detail_count;
        increment_saturated(&session->dropped);
    }
    session->detail[session->detail_count++] = *row;
    recalculate_forensic_bounded_counts(session);
    data->needs_redraw = true;
}

static bool parse_optional_decimal(const char *field, int minimum, int maximum,
                                   bool *known, int *value)
{
    if (strcmp(field, "-") == 0) {
        *known = false;
        *value = 0;
        return true;
    }
    int parsed = 0;
    if (!parse_decimal_field(field, minimum < 0, &parsed) ||
        parsed < minimum || parsed > maximum) return false;
    *known = true;
    *value = parsed;
    return true;
}

static bool parse_optional_unsigned(const char *field, unsigned maximum,
                                    bool *known, unsigned *value)
{
    if (strcmp(field, "-") == 0) {
        *known = false;
        *value = 0;
        return true;
    }
    unsigned parsed = 0;
    if (!parse_unsigned_field(field, &parsed) || parsed > maximum) return false;
    *known = true;
    *value = parsed;
    return true;
}

static bool parse_optional_fc(const char *field, bool *known, unsigned *value)
{
    if (strcmp(field, "-") == 0) {
        *known = false;
        *value = 0;
        return true;
    }
    if (strlen(field) != 6 || field[0] != '0' || field[1] != 'x') return false;
    for (int i = 2; i < 6; ++i) {
        if (!isxdigit((unsigned char)field[i])) return false;
    }
    char *end = NULL;
    unsigned long parsed = strtoul(field + 2, &end, 16);
    if (*end != '\0' || parsed > 0xffffUL) return false;
    *known = true;
    *value = (unsigned)parsed;
    return true;
}

static bool parse_forensic_line(const char *line, forensic_row_t *row)
{
    const char *prefix = "[DEAUTH_FORENSIC/1] FRAME=";
    if (!line || !row || strncmp(line, prefix, strlen(prefix)) != 0) return false;
    forensic_row_t parsed = {0};
    char frame[12], field[32];
    const char *p = line + strlen(prefix);
    if (!take_field(&p, " CH=", frame, sizeof(frame)) ||
        !take_field(&p, " BSSID=", field, sizeof(field)) ||
        !parse_optional_decimal(field, 1, 233, &parsed.channel_known, &parsed.channel) ||
        !take_field(&p, " TX=", parsed.bssid, sizeof(parsed.bssid)) ||
        !bssid_valid(parsed.bssid) ||
        !take_field(&p, " DA=", parsed.tx, sizeof(parsed.tx)) ||
        !bssid_valid(parsed.tx) ||
        !take_field(&p, " RSSI=", parsed.da, sizeof(parsed.da)) ||
        !bssid_valid(parsed.da) ||
        !take_field(&p, " REASON=", field, sizeof(field)) ||
        !parse_optional_decimal(field, -127, 0, &parsed.rssi_known, &parsed.rssi) ||
        !take_field(&p, " SEQ=", field, sizeof(field)) ||
        !parse_optional_unsigned(field, 65535U, &parsed.reason_known, &parsed.reason) ||
        !take_field(&p, " RETRY=", field, sizeof(field)) ||
        !parse_optional_unsigned(field, 4095U, &parsed.sequence_known, &parsed.sequence) ||
        !take_field(&p, " PROTECTED=", field, sizeof(field)) ||
        !parse_optional_unsigned(field, 1U, &parsed.retry_known, &parsed.retry) ||
        !take_field(&p, " FC=", field, sizeof(field)) ||
        !parse_optional_unsigned(field, 1U, &parsed.protected_known,
                                 &parsed.protected_frame) ||
        !take_field(&p, " TS_US=", field, sizeof(field)) ||
        !parse_optional_fc(field, &parsed.fc_known, &parsed.fc) ||
        !parse_u64_field(p, &parsed.ts_us)) return false;
    if (strcmp(frame, "deauth") == 0) parsed.frame = FORENSIC_FRAME_DEAUTH;
    else if (strcmp(frame, "disassoc") == 0) parsed.frame = FORENSIC_FRAME_DISASSOC;
    else return false;
    *row = parsed;
    return true;
}

typedef struct {
    char frame[12];
    char bssid[18];
    char sta[18];
    char decision[16];
    char reason[32];
    int channel;
} defence_trigger_t;

typedef struct {
    char response[16];
    char result[8];
    char error[32];
    unsigned frames;
    uint64_t latency_us;
} defence_tx_t;

static bool parse_defence_armed_line(const char *line, unsigned *targets,
                                     bool *pmkid_decoy)
{
    const char *prefix = "[DETECTOR_DEFENCE] armed targets=";
    if (strncmp(line, prefix, strlen(prefix)) != 0) return false;
    char field[24];
    const char *p = line + strlen(prefix);
    if (!take_field(&p, " trace=fast pmkid=", field, sizeof(field)) ||
        !parse_unsigned_field(field, targets)) return false;
    if (strcmp(p, "monitor") == 0) *pmkid_decoy = false;
    else if (strcmp(p, "decoy") == 0) *pmkid_decoy = true;
    else return false;
    return true;
}

static bool parse_defence_trigger_line(const char *line, defence_trigger_t *trigger)
{
    const char *prefix = "[DETECTOR_DEFENCE] trigger=";
    if (strncmp(line, prefix, strlen(prefix)) != 0) return false;
    defence_trigger_t parsed = {0};
    char field[24];
    const char *p = line + strlen(prefix);
    if (!take_field(&p, " BSSID=", parsed.frame, sizeof(parsed.frame)) ||
        !take_field(&p, " STA=", parsed.bssid, sizeof(parsed.bssid)) ||
        !take_field(&p, " RX=", parsed.sta, sizeof(parsed.sta)) ||
        !take_field(&p, " decision=", field, sizeof(field)) ||
        !parse_decimal_field(field, false, &parsed.channel) ||
        !take_field(&p, " reason=", parsed.decision, sizeof(parsed.decision)) ||
        strlen(p) >= sizeof(parsed.reason)) return false;
    snprintf(parsed.reason, sizeof(parsed.reason), "%s", p);
    if ((strcmp(parsed.frame, "deauth") != 0 &&
         strcmp(parsed.frame, "disassoc") != 0 &&
         strcmp(parsed.frame, "csa") != 0 &&
         strcmp(parsed.frame, "pmkid_probe") != 0) ||
        !bssid_valid(parsed.bssid) || !mac_or_dash_valid(parsed.sta) ||
        parsed.channel < 1 || parsed.channel > 255 ||
        (strcmp(parsed.decision, "decoy_4way") != 0 &&
         strcmp(parsed.decision, "decoy_pmkid") != 0 &&
         strcmp(parsed.decision, "observe") != 0) ||
        !status_token_valid(parsed.reason)) return false;
    *trigger = parsed;
    return true;
}

static bool parse_defence_provisional_line(const char *line, char bssid[18],
                                           int *channel)
{
    const char *prefix = "[DETECTOR_DEFENCE] provisional_bssid=";
    if (strncmp(line, prefix, strlen(prefix)) != 0) return false;
    char field[24];
    const char *p = line + strlen(prefix);
    if (!take_field(&p, " channel=", bssid, 18) || !bssid_valid(bssid) ||
        strlen(p) >= sizeof(field)) return false;
    snprintf(field, sizeof(field), "%s", p);
    return parse_decimal_field(field, false, channel) &&
           *channel >= 1 && *channel <= 255;
}

static bool parse_defence_tx_line(const char *line, defence_tx_t *tx)
{
    const char *prefix = "[DETECTOR_DEFENCE] tx=";
    if (strncmp(line, prefix, strlen(prefix)) != 0) return false;
    defence_tx_t parsed = {0};
    char stage[16], field[32];
    const char *p = line + strlen(prefix);
    if (!take_field(&p, " stage=", parsed.response, sizeof(parsed.response)) ||
        !take_field(&p, " result=", stage, sizeof(stage)) ||
        !take_field(&p, " frames=", parsed.result, sizeof(parsed.result)) ||
        !take_field(&p, " latency_us=", field, sizeof(field)) ||
        !parse_unsigned_field(field, &parsed.frames) ||
        !take_field(&p, " error=", field, sizeof(field)) ||
        !parse_u64_field(field, &parsed.latency_us) ||
        strlen(p) >= sizeof(parsed.error)) return false;
    snprintf(parsed.error, sizeof(parsed.error), "%s", p);
    bool pair_ok = strcmp(parsed.response, "decoy_4way") == 0 &&
                   strcmp(stage, "m1_m2") == 0;
    bool pmkid_ok = strcmp(parsed.response, "decoy_pmkid") == 0 &&
                    strcmp(stage, "m1_pmkid") == 0;
    if ((!pair_ok && !pmkid_ok) ||
        (strcmp(parsed.result, "ok") != 0 && strcmp(parsed.result, "error") != 0) ||
        parsed.frames > 2 || !status_token_valid(parsed.error)) return false;
    *tx = parsed;
    return true;
}

static bool parse_defence_suppressed_line(const char *line, unsigned *count,
                                          char bssid[18])
{
    const char *prefix = "[DETECTOR_DEFENCE] suppressed=";
    if (strncmp(line, prefix, strlen(prefix)) != 0) return false;
    char field[32], reason[32];
    const char *p = line + strlen(prefix);
    if (!take_field(&p, " reason=", field, sizeof(field)) ||
        !parse_unsigned_field(field, count) ||
        !take_field(&p, " BSSID=", reason, sizeof(reason)) ||
        !status_token_valid(reason) ||
        !take_field(&p, " window_ms=", bssid, 18) || !bssid_valid(bssid) ||
        strcmp(p, "1000") != 0) return false;
    return true;
}

static bool parse_defence_released_line(const char *line, char bssid[18],
                                        int *channel, char response[16],
                                        unsigned *suppressed)
{
    const char *prefix = "[DETECTOR_DEFENCE] released_bssid=";
    if (strncmp(line, prefix, strlen(prefix)) != 0) return false;
    char field[32];
    const char *p = line + strlen(prefix);
    if (!take_field(&p, " channel=", bssid, 18) || !bssid_valid(bssid) ||
        !take_field(&p, " reason=idle idle_ms=20000 response=", field, sizeof(field)) ||
        !parse_decimal_field(field, false, channel) || *channel < 1 || *channel > 255 ||
        !take_field(&p, " suppressed=", response, 16) ||
        (strcmp(response, "decoy_4way") != 0 &&
         strcmp(response, "decoy_pmkid") != 0) ||
        !parse_unsigned_field(p, suppressed)) return false;
    return true;
}

static bool parse_defence_stopped_line(const char *line, unsigned *responses,
                                       char outcome[12])
{
    const char *prefix = "[DETECTOR_DEFENCE] stopped observations=";
    if (strncmp(line, prefix, strlen(prefix)) != 0) return false;
    const char *separators[] = {
        " tx_pairs=", " pmkid_m1=", " responses=", " satisfied=",
        " errors=", " dropped=", " outcome="
    };
    char field[32];
    unsigned values[7] = {0};
    const char *p = line + strlen(prefix);
    for (int i = 0; i < 7; ++i) {
        if (!take_field(&p, separators[i], field, sizeof(field)) ||
            !parse_unsigned_field(field, &values[i])) return false;
    }
    if (strlen(p) >= 12 ||
        (strcmp(p, "responded") != 0 && strcmp(p, "observed") != 0 &&
         strcmp(p, "idle") != 0)) return false;
    *responses = values[3];
    snprintf(outcome, 12, "%s", p);
    return true;
}

static const char *detector_record_line(const char *line)
{
    if (line[0] == '>' && line[1] == '[') return line + 1;
    if (line[0] == '>' && line[1] == ' ' && line[2] == '[') return line + 2;
    return line;
}

static bool parse_trace_locked_line(const char *line, int *channel)
{
    const char *prefix = "[DETECTOR_TRACE] locked_channel=";
    if (strncmp(line, prefix, strlen(prefix)) != 0) return false;
    const char *value = line + strlen(prefix);
    const char *end = NULL;
    int parsed = 0;
    if (!isdigit((unsigned char)*value) ||
        !parse_integer(value, false, &parsed, &end) || *end != '\0' ||
        parsed < 1 || parsed > 255) return false;
    *channel = parsed;
    return true;
}

static bool parse_trace_confirming_line(const char *line, char *bssid,
                                        size_t bssid_capacity, int *channel,
                                        int *window_ms)
{
    const char *prefix = "[DETECTOR_TRACE] confirming_bssid=";
    if (strncmp(line, prefix, strlen(prefix)) != 0) return false;

    const char *cursor = line + strlen(prefix);
    if (!take_field(&cursor, " first_channel=", bssid, bssid_capacity) ||
        !bssid_valid(bssid)) return false;

    const char *end = NULL;
    int parsed_channel = 0;
    if (!isdigit((unsigned char)*cursor) ||
        !parse_integer(cursor, false, &parsed_channel, &end) ||
        parsed_channel < 1 || parsed_channel > 255 ||
        strcmp(end, " window_ms=1000") != 0) return false;

    *channel = parsed_channel;
    *window_ms = 1000;
    return true;
}

static bool parse_trace_failed_line(const char *line, int *channel,
                                    char *error, size_t error_capacity)
{
    const char *prefix = "[DETECTOR_TRACE] lock_failed_channel=";
    if (strncmp(line, prefix, strlen(prefix)) != 0) return false;
    const char *value = line + strlen(prefix);
    const char *end = NULL;
    int parsed = 0;
    if (!isdigit((unsigned char)*value) ||
        !parse_integer(value, false, &parsed, &end) || parsed < 1 || parsed > 255 ||
        strncmp(end, " error=", 7) != 0) return false;
    const char *error_text = end + 7;
    size_t length = strlen(error_text);
    if (length == 0 || length >= error_capacity) return false;
    for (const unsigned char *ch = (const unsigned char *)error_text; *ch; ++ch) {
        if (*ch < 0x20 || *ch > 0x7e) return false;
    }
    *channel = parsed;
    memcpy(error, error_text, length + 1);
    return true;
}

static bool parse_trace_released_line(const char *line, int *channel,
                                      int *idle_ms)
{
    const char *prefix = "[DETECTOR_TRACE] released_channel=";
    if (strncmp(line, prefix, strlen(prefix)) != 0) return false;
    const char *value = line + strlen(prefix);
    const char *end = NULL;
    int parsed = 0;
    if (!isdigit((unsigned char)*value) ||
        !parse_integer(value, false, &parsed, &end) ||
        parsed < 1 || parsed > 255 ||
        strcmp(end, " reason=idle idle_ms=20000") != 0) return false;
    *channel = parsed;
    *idle_ms = 20000;
    return true;
}

static bool trace_attempt_may_finish(detector_trace_status_t status)
{
    return status == TRACE_STATUS_WAITING || status == TRACE_STATUS_ARMED ||
           status == TRACE_STATUS_CONFIRMING ||
           status == TRACE_STATUS_TUNE_FAILED;
}

static bool handle_trace_status_line(deauth_detector_data_t *data, const char *line)
{
    const char *prefix = "[DETECTOR_TRACE]";
    if (strncmp(line, prefix, strlen(prefix)) != 0) return false;

    int channel = 0;
    int idle_ms = 0;
    int window_ms = 0;
    char bssid[sizeof(data->trace_bssid)];
    char error[sizeof(data->trace_error)];
    if (data->trace_enabled && data->v2_requested &&
        data->trace_status == TRACE_STATUS_WAITING &&
        strcmp(line, "[DETECTOR_TRACE] armed") == 0) {
        data->trace_status = TRACE_STATUS_ARMED;
        clear_trace_attempt(data);
        data->trace_channel = 0;
        data->trace_idle_ms = 0;
        data->trace_error[0] = '\0';
        data->needs_redraw = true;
    } else if (data->trace_enabled && data->v2_requested &&
               (data->trace_status == TRACE_STATUS_ARMED ||
                data->trace_status == TRACE_STATUS_TUNE_FAILED ||
                data->trace_status == TRACE_STATUS_RELEASED) &&
               parse_trace_confirming_line(line, bssid, sizeof(bssid),
                                           &channel, &window_ms)) {
        data->trace_status = TRACE_STATUS_CONFIRMING;
        snprintf(data->trace_bssid, sizeof(data->trace_bssid), "%s", bssid);
        data->trace_first_channel = channel;
        data->trace_window_ms = window_ms;
        data->trace_channel = 0;
        data->trace_idle_ms = 0;
        data->trace_error[0] = '\0';
        data->needs_redraw = true;
    } else if (data->trace_enabled && data->v2_requested &&
               parse_trace_locked_line(line, &channel) &&
               (trace_attempt_may_finish(data->trace_status) ||
                (data->trace_status == TRACE_STATUS_LOCKED &&
                 channel == data->trace_channel))) {
        data->trace_status = TRACE_STATUS_LOCKED;
        clear_trace_attempt(data);
        data->trace_channel = channel;
        data->trace_idle_ms = 0;
        data->trace_error[0] = '\0';
        data->needs_redraw = true;
    } else if (data->trace_enabled && data->v2_requested &&
               trace_attempt_may_finish(data->trace_status) &&
               parse_trace_failed_line(line, &channel, error, sizeof(error))) {
        data->trace_status = TRACE_STATUS_TUNE_FAILED;
        clear_trace_attempt(data);
        data->trace_channel = channel;
        data->trace_idle_ms = 0;
        snprintf(data->trace_error, sizeof(data->trace_error), "%s", error);
        data->needs_redraw = true;
    } else if (data->trace_enabled && data->v2_requested &&
               data->trace_status == TRACE_STATUS_LOCKED &&
               parse_trace_released_line(line, &channel, &idle_ms) &&
               channel == data->trace_channel) {
        data->trace_status = TRACE_STATUS_RELEASED;
        clear_trace_attempt(data);
        data->trace_idle_ms = idle_ms;
        data->trace_error[0] = '\0';
        data->needs_redraw = true;
    }
    /* Malformed or unsolicited trace status is still a status line, not an event. */
    return true;
}

static bool parse_deauth_v2_line(const char *line, deauth_event_t *event)
{
    const char *prefix = "[DEAUTH_V2] CH: ";
    if (!line || !event || strncmp(line, prefix, strlen(prefix)) != 0) return false;

    deauth_event_t parsed = {0};
    char field[32];
    const char *p = line + strlen(prefix);
    if (!take_field(&p, " | BSSID: ", field, sizeof(field)) ||
        !parse_decimal_field(field, false, &parsed.channel) ||
        parsed.channel < 1 || parsed.channel > 255 ||
        !take_field(&p, " | TX: ", parsed.bssid, sizeof(parsed.bssid)) ||
        !bssid_valid(parsed.bssid) ||
        !take_field(&p, " | DA: ", parsed.tx, sizeof(parsed.tx)) ||
        !bssid_valid(parsed.tx) ||
        !take_field(&p, " | RSSI: ", parsed.da, sizeof(parsed.da)) ||
        !bssid_valid(parsed.da) ||
        !take_field(&p, " | REASON: ", field, sizeof(field)) ||
        !parse_decimal_field(field, true, &parsed.rssi) ||
        !take_field(&p, " | SSID: ", field, sizeof(field)) ||
        strlen(p) > 32) {
        return false;
    }
    if (strcmp(field, "-") == 0) {
        /* Only a protected individually addressed frame may hide the reason. */
        if (((unsigned)strtoul(parsed.da, NULL, 16) & 1U) != 0) return false;
        parsed.reason_known = false;
    } else {
        if (!parse_decimal_field(field, false, &parsed.reason) ||
            parsed.reason < 0 || parsed.reason > 65535) return false;
        parsed.reason_known = true;
    }
    for (const unsigned char *ch = (const unsigned char *)p; *ch; ++ch) {
        if (*ch < 0x20 || *ch == 0x7f) return false;
    }
    snprintf(parsed.ap_name, sizeof(parsed.ap_name), "%s", p);
    parsed.detailed = true;
    *event = parsed;
    return true;
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
    data->exit_confirm_pending = false;
    data->forensic_accepting = false;
    clear_scan(data);
    set_state(data, DET_ERROR);
    set_message(data, message);
}

static bool send_start_command(deauth_detector_data_t *data)
{
    char command[256];
    const char *legacy_name = "deauth_detector";
    const char *suffix = data->legacy_start_command + strlen(legacy_name);
    const char *trace = data->v2_requested && data->trace_enabled ? " trace" : "";
    const char *defence = data->v2_requested && data->mode >= DET_MODE_DEFENCE ?
                           " defence" : "";
    const char *pmkid = data->v2_requested && data->mode == DET_MODE_DEFENCE_PMKID ?
                         " pmkid-decoy" : "";
    int length = snprintf(command, sizeof(command), "%s%s%s%s%s",
                          data->v2_requested ? "deauth_detector_v2" : legacy_name,
                          suffix, trace, defence, pmkid);
    if (length < 0 || (size_t)length >= sizeof(command)) {
        fail(data, "Selection too large");
        return false;
    }
    if (uart_send_command(command) != 0) {
        fail(data, "UART send failed");
        return false;
    }
    return true;
}

static void start_all(deauth_detector_data_t *data)
{
    if (defence_requested(data)) {
        fail(data, "Defence requires BSSID");
        return;
    }
    clear_scan(data);
    clear_events(data);
    data->view = VIEW_DEAUTH;
    snprintf(data->legacy_start_command, sizeof(data->legacy_start_command), "deauth_detector");
    data->v2_requested = true;
    data->v2_confirmed = false;
    begin_trace_run(data);
    clear_defence_status(data);
    set_state(data, DET_START_ALL);
    set_message(data, "Waiting for JanOS start");
    send_start_command(data);
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

static bool send_stop_command(deauth_detector_data_t *data)
{
    data->forensic_accepting = false;
    if (uart_send_command("stop") != 0) return false;
    data->stop_pending = true;
    data->stop_send_failed = false;
    return true;
}

static void fail_after_safety_stop(deauth_detector_data_t *data,
                                   const char *message)
{
    if (!send_stop_command(data)) {
        data->stop_send_failed = true;
        fail(data, "Stop send failed; may run");
        return;
    }
    fail(data, message);
}

static void stop_then(deauth_detector_data_t *data, detector_state_t next)
{
    detector_state_t previous = data->state;
    set_state(data, next);
    set_message(data, "Waiting for stop");
    if (!send_stop_command(data)) {
        data->stop_send_failed = true;
        if (next == DET_STOP_EXIT) {
            set_state(data, previous == DET_ACTIVE_ALL || previous == DET_ACTIVE_SELECTED ?
                      previous : DET_ERROR);
            set_message(data, "Stop send failed; may run");
        } else {
            fail(data, "Stop send failed; may run");
        }
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
    data->v2_requested = true;
    data->v2_confirmed = false;
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
    begin_trace_run(data);
    begin_defence_run(data);
    snprintf(data->legacy_start_command, sizeof(data->legacy_start_command), "%s", command);
    set_state(data, DET_START_SELECTED);
    set_message(data, "Waiting for watch proof");
    send_start_command(data);
}

static void maybe_activate(deauth_detector_data_t *data)
{
    if (!data->start_ack) return;
    if (data->v2_requested && !data->v2_confirmed) {
        set_message(data, "Waiting for V2 format ACK");
        return;
    }
    if (data->state == DET_START_ALL) {
        reset_forensic_session(data);
        set_state(data, DET_ACTIVE_ALL);
        data->last_ping_us = now_us();
        set_message(data, "All channels active");
    } else if (data->state == DET_START_SELECTED && defence_requested(data) &&
               !data->defence_armed_latched) {
        set_message(data, "Waiting for Defence arm");
    } else if (data->state == DET_START_SELECTED && !defence_requested(data) &&
               data->verified_watch_count < data->watch_expected_count) {
        set_message(data, "Waiting for watch proof");
    } else if (data->state == DET_START_SELECTED) {
        reset_forensic_session(data);
        set_state(data, DET_ACTIVE_SELECTED);
        data->last_ping_us = now_us();
        set_message(data, "BSSID watch verified");
    }
}

static void retry_legacy_start(deauth_detector_data_t *data)
{
    detector_state_t state = data->state;
    data->v2_requested = false;
    data->v2_confirmed = false;
    data->start_ack = false;
    data->verified_watch_count = 0;
    if (data->trace_enabled) {
        mark_trace_unavailable(data);
    }
    set_state(data, state);
    set_message(data, data->trace_enabled ?
                "Legacy; Trace unavailable" : "JanOS legacy start pending");
    send_start_command(data);
}

static bool is_start_rejection(const char *line)
{
    static const char *const errors[] = {
        "Invalid detector option",
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

static bool is_trace_rejection(const deauth_detector_data_t *data,
                               const char *line)
{
    if (!data->trace_enabled || !data->v2_requested) return false;
    return strcmp(line,
                  "Invalid detector option; use --only before scan indices.") == 0 ||
           strstr(line, "Invalid detector index 'trace'") != NULL ||
           strstr(line, "Invalid watch index 'trace'") != NULL;
}

static void reject_start(deauth_detector_data_t *data, const char *line)
{
    char message[64];
    snprintf(message, sizeof(message), "%.63s", line);
    /* A start or format proof means the receiver may already be running. */
    if (data->start_ack || data->v2_confirmed) {
        fail_after_safety_stop(data, message);
        return;
    }
    fail(data, message);
}

static void reject_watch(deauth_detector_data_t *data)
{
    set_state(data, DET_STOP_BAD_WATCH);
    set_message(data, "BSSID mismatch; stopping");
    if (!send_stop_command(data)) {
        data->stop_send_failed = true;
        fail(data, "Stop send failed; may run");
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
    maybe_activate(data);
}

static bool stopped_line(const char *line)
{
    /* JanOS prints the detector-specific line before its global Wi-Fi cleanup.
       Only this final line confirms that the next command or exit is safe. */
    return strcmp(line, "All operations stopped.") == 0;
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

static bool defence_can_update(const deauth_detector_data_t *data)
{
    return defence_requested(data) && data->v2_requested &&
           data->state == DET_ACTIVE_SELECTED;
}

static bool handle_defence_status_line(deauth_detector_data_t *data,
                                       const char *line)
{
    const char *prefix = "[DETECTOR_DEFENCE]";
    if (strncmp(line, prefix, strlen(prefix)) != 0) return false;

    unsigned targets = 0;
    bool pmkid_decoy = false;
    if (parse_defence_armed_line(line, &targets, &pmkid_decoy)) {
        bool arm_state = data->state == DET_START_SELECTED ||
                         data->state == DET_ACTIVE_SELECTED;
        if (defence_requested(data) && data->v2_requested && arm_state &&
            targets == (unsigned)data->watch_expected_count &&
            pmkid_decoy == (data->mode == DET_MODE_DEFENCE_PMKID) &&
            data->defence_status == DEFENCE_STATUS_WAITING) {
            data->defence_status = DEFENCE_STATUS_ARMED;
            data->defence_armed_latched = true;
            data->needs_redraw = true;
            maybe_activate(data);
        }
        return true;
    }

    defence_trigger_t trigger;
    if (parse_defence_trigger_line(line, &trigger)) {
        bool trigger_ready = data->defence_status == DEFENCE_STATUS_ARMED ||
                             data->defence_status == DEFENCE_STATUS_RELEASED ||
                             data->defence_status == DEFENCE_STATUS_RESPONSE_ERROR;
        if (defence_can_update(data) && watched_bssid(data, trigger.bssid) &&
            trigger_ready &&
            !data->defence_transaction_pending) {
            bool response_allowed = strcmp(trigger.decision, "decoy_4way") == 0 ||
                (data->mode == DET_MODE_DEFENCE_PMKID &&
                 strcmp(trigger.decision, "decoy_pmkid") == 0);
            if (response_allowed) {
                snprintf(data->defence_bssid, sizeof(data->defence_bssid), "%s",
                         trigger.bssid);
                data->defence_channel = trigger.channel;
                data->defence_error[0] = '\0';
                data->defence_status = DEFENCE_STATUS_RESPONDING;
                data->defence_transaction_pending = true;
                snprintf(data->defence_response, sizeof(data->defence_response), "%s",
                         trigger.decision);
                data->needs_redraw = true;
            }
        }
        return true;
    }

    char bssid[18];
    int channel = 0;
    if (parse_defence_provisional_line(line, bssid, &channel)) {
        if (defence_can_update(data) && watched_bssid(data, bssid) &&
            data->defence_transaction_pending &&
            data->defence_status == DEFENCE_STATUS_RESPONDING &&
            bssid_equal(data->defence_bssid, bssid)) {
            data->defence_status = DEFENCE_STATUS_PROVISIONAL;
            data->defence_channel = channel;
            data->needs_redraw = true;
        }
        return true;
    }

    defence_tx_t tx;
    if (parse_defence_tx_line(line, &tx)) {
        if (defence_can_update(data) && data->defence_transaction_pending &&
            (data->defence_status == DEFENCE_STATUS_RESPONDING ||
             data->defence_status == DEFENCE_STATUS_PROVISIONAL) &&
            strcmp(data->defence_response, tx.response) == 0) {
            snprintf(data->defence_error, sizeof(data->defence_error), "%s", tx.error);
            if (strcmp(tx.result, "ok") == 0) {
                data->defence_status = DEFENCE_STATUS_RESPONSE_OK;
                if (data->defence_responses < UINT_MAX) ++data->defence_responses;
            } else {
                data->defence_status = DEFENCE_STATUS_RESPONSE_ERROR;
            }
            data->defence_transaction_pending = false;
            data->needs_redraw = true;
        }
        return true;
    }

    unsigned suppressed = 0;
    if (parse_defence_suppressed_line(line, &suppressed, bssid)) {
        if (defence_can_update(data) && watched_bssid(data, bssid)) {
            data->defence_suppressed = suppressed;
            data->needs_redraw = true;
        }
        return true;
    }

    char response[16];
    if (parse_defence_released_line(line, bssid, &channel, response, &suppressed)) {
        if (defence_can_update(data) && watched_bssid(data, bssid) &&
            !data->defence_transaction_pending &&
            data->defence_status == DEFENCE_STATUS_RESPONSE_OK &&
            bssid_equal(data->defence_bssid, bssid) &&
            strcmp(data->defence_response, response) == 0) {
            data->defence_status = DEFENCE_STATUS_RELEASED;
            snprintf(data->defence_bssid, sizeof(data->defence_bssid), "%s", bssid);
            snprintf(data->defence_response, sizeof(data->defence_response), "%s", response);
            data->defence_channel = channel;
            data->defence_suppressed = suppressed;
            data->needs_redraw = true;
        }
        return true;
    }

    unsigned responses = 0;
    char outcome[12];
    if (parse_defence_stopped_line(line, &responses, outcome)) {
        if (defence_requested(data) && data->stop_pending &&
            data->defence_status != DEFENCE_STATUS_OFF) {
            data->defence_transaction_pending = false;
            data->defence_status = DEFENCE_STATUS_STOPPED;
            data->defence_summary_responses = responses;
            data->defence_summary_valid = true;
            snprintf(data->defence_outcome, sizeof(data->defence_outcome), "%s", outcome);
            data->needs_redraw = true;
        }
        return true;
    }

    /* Every exact marker prefix is status traffic, even when malformed. */
    return true;
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

    const char *record_line = detector_record_line(line);
    const char *forensic_marker = "[DEAUTH_FORENSIC/1]";
    if (strncmp(record_line, forensic_marker, strlen(forensic_marker)) == 0) {
        forensic_row_t forensic;
        if (data->forensic_accepting && parse_forensic_line(record_line, &forensic) &&
            (data->state == DET_ACTIVE_ALL ||
             (data->state == DET_ACTIVE_SELECTED &&
              watched_bssid(data, forensic.bssid)))) {
            accept_forensic_row(data, &forensic);
        }
        return;
    }
    const char *defence_marker = "[DETECTOR_DEFENCE]";
    if (strncmp(record_line, defence_marker, strlen(defence_marker)) == 0) {
        handle_defence_status_line(data, record_line);
        return;
    }
    const char *trace_marker = "[DETECTOR_TRACE]";
    if (strncmp(record_line, trace_marker, strlen(trace_marker)) == 0) {
        if (data->state == DET_START_ALL || data->state == DET_START_SELECTED ||
            data->state == DET_ACTIVE_ALL || data->state == DET_ACTIVE_SELECTED) {
            handle_trace_status_line(data, record_line);
        }
        return;
    }

    /* A separate scan on this UART replaces the numbered JanOS snapshot.
       Once the list has been offered to the user, its indices cannot survive
       a new scan or another printed result set. */
    if ((data->state == DET_PICK || data->state == DET_START_SELECTED ||
         data->state == DET_ACTIVE_SELECTED) &&
        (strstr(line, "Background scan started (") ||
         strncmp(line, "Showing results from last scan (", 32) == 0 ||
         strcmp(line, "Scan results printed.") == 0)) {
        if (data->state != DET_PICK) {
            fail_after_safety_stop(data, "Scan changed; select again");
        } else {
            fail(data, "Scan changed; select again");
        }
        return;
    }

    if (strcmp(line, "pong") == 0 && data->ping_waiting) {
        data->ping_waiting = false;
        data->last_ping_us = now_us();
        return;
    }
    if (strncmp(line, "Output:", 7) == 0) return;
    if (data->stop_pending && stopped_line(line)) {
        detector_state_t state = data->state;
        data->stop_pending = false;
        data->forensic.finalized = true;
        clear_trace_status(data);
        clear_defence_status(data);
        if (state == DET_STOP_FOR_SCAN) start_scan(data);
        else if (state == DET_STOP_FOR_ALL) start_all(data);
        else if (state == DET_STOP_BAD_WATCH) fail(data, "BSSID watch mismatch");
        else if (state == DET_STOP_EXIT) data->exit_ready = true;
        return;
    }
    if (data->state == DET_TRACE_CHOICE) return;
    if (data->state == DET_SCAN_WAIT_ACK || data->state == DET_SCAN_RUNNING ||
        data->state == DET_SCAN_COLLECT) {
        handle_scan_line(data, line);
        return;
    }

    if (data->state == DET_START_ALL || data->state == DET_START_SELECTED) {
        if (is_trace_rejection(data, line)) {
            if (defence_requested(data)) {
                mark_defence_unsupported(data);
                reject_start(data, "Defence unsupported");
            } else {
                mark_trace_unavailable(data);
                reject_start(data, "Trace unavailable on JanOS");
            }
            return;
        }
        if (strcmp(line, "Unrecognized command") == 0) {
            if (defence_requested(data)) {
                mark_defence_unsupported(data);
                reject_start(data, "Defence unsupported");
            } else if (data->v2_requested && !data->start_ack && !data->v2_confirmed) {
                retry_legacy_start(data);
            } else {
                reject_start(data, line);
            }
            return;
        }
        if (is_start_rejection(line)) {
            if (defence_requested(data) &&
                (strstr(line, "Defence") || strstr(line, "defence") ||
                 strstr(line, "pmkid") || strstr(line, "Invalid detector option"))) {
                mark_defence_unsupported(data);
                reject_start(data, "Defence unsupported");
                return;
            }
            reject_start(data, line);
            return;
        }
        if (data->v2_requested &&
            strcmp(line, "[DETECTOR_INFO] format=deauth_v2") == 0) {
            data->v2_confirmed = true;
            maybe_activate(data);
            return;
        }
    }
    if (data->state == DET_START_ALL &&
        strstr(line, "Deauth Detector started - scanning all channels")) {
        data->start_ack = true;
        maybe_activate(data);
    } else if (data->state == DET_START_SELECTED &&
               strstr(line, "Deauth Detector started - monitoring selected channels")) {
        data->start_ack = true;
        data->needs_redraw = true;
        maybe_activate(data);
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
        if ((data->v2_requested ? parse_deauth_v2_line(record_line, &deauth) :
             parse_deauth_line(record_line, &deauth))) {
            if (data->state == DET_ACTIVE_SELECTED && !watched_bssid(data, deauth.bssid)) return;
            data->last_deauth = deauth;
            if (data->deauth_count < INT_MAX) ++data->deauth_count;
            data->needs_redraw = true;
        } else if (parse_csa_line(record_line, &csa)) {
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

static bool trace_status_text(const deauth_detector_data_t *data,
                              char *line, size_t capacity)
{
    switch (data->trace_status) {
        case TRACE_STATUS_WAITING:
            snprintf(line, capacity, "Trace requested: wait arm");
            return true;
        case TRACE_STATUS_ARMED:
            snprintf(line, capacity, "Trace armed: wait DEAUTH");
            return true;
        case TRACE_STATUS_CONFIRMING:
            snprintf(line, capacity, "Conf %s CH%d",
                     data->trace_bssid, data->trace_first_channel);
            return true;
        case TRACE_STATUS_LOCKED:
            snprintf(line, capacity, "Trace locked on CH %d", data->trace_channel);
            return true;
        case TRACE_STATUS_RELEASED:
            snprintf(line, capacity, "CH%d released after idle", data->trace_channel);
            return true;
        case TRACE_STATUS_TUNE_FAILED:
            snprintf(line, capacity, "Fail %d %.21s",
                     data->trace_channel, data->trace_error);
            return true;
        case TRACE_STATUS_UNAVAILABLE:
            snprintf(line, capacity, "Trace unavailable (legacy)");
            return true;
        case TRACE_STATUS_OFF:
        default:
            return false;
    }
}

static void draw_trace_choice(const deauth_detector_data_t *data)
{
    static const char *const names[] = {
        "Passive", "Trace", "Defence", "Defence + PMKID Lab"
    };
    ui_print_center(2, "Detector mode", UI_COLOR_TEXT);
    ui_print_center(3, names[data->mode], UI_COLOR_HIGHLIGHT);
    if (data->mode == DET_MODE_PASSIVE) {
        ui_print_center(5, "Receive only", UI_COLOR_DIMMED);
    } else if (data->mode == DET_MODE_TRACE) {
        ui_print_center(5, "JanOS confirms channel", UI_COLOR_DIMMED);
        ui_print_center(6, "Passive receive only", UI_COLOR_DIMMED);
    } else {
        ui_print_center(5, "Fresh BSSID scan required", UI_COLOR_DIMMED);
        ui_print_center(6, "Controlled networks only", UI_COLOR_DIMMED);
    }
    ui_draw_status("SP:Next EN:Start Q:Exit");
}

static void draw_pmkid_warning(void)
{
    ui_print_center(1, "PMKID LAB WARNING", UI_COLOR_HIGHLIGHT);
    ui_print_center(3, "May interfere with", UI_COLOR_TEXT);
    ui_print_center(4, "legitimate client joins", UI_COLOR_TEXT);
    ui_print_center(6, "Controlled RF lab only", UI_COLOR_DIMMED);
    ui_draw_status("ENTER:Accept ESC/Q:Back");
}

static void draw_trace_status(const deauth_detector_data_t *data, int row)
{
    char line[40];
    if (!trace_status_text(data, line, sizeof(line))) return;
    print_line(row, line, data->trace_status == TRACE_STATUS_TUNE_FAILED ?
               UI_COLOR_HIGHLIGHT : UI_COLOR_TEXT);
    if (data->trace_status == TRACE_STATUS_TUNE_FAILED && row < 6) {
        print_line(row + 1, "hopping continues", UI_COLOR_HIGHLIGHT);
    } else if (data->trace_status == TRACE_STATUS_RELEASED && row < 6) {
        print_line(row + 1, "Trace rearmed / hopping", UI_COLOR_TEXT);
    }
}

static bool trace_footer_text(const deauth_detector_data_t *data,
                              const char *return_key, char *line, size_t capacity)
{
    switch (data->trace_status) {
        case TRACE_STATUS_WAITING:
            snprintf(line, capacity, "Trace wait arm %s", return_key);
            return true;
        case TRACE_STATUS_ARMED:
            snprintf(line, capacity, "Armed wait DEAUTH %s", return_key);
            return true;
        case TRACE_STATUS_CONFIRMING:
            snprintf(line, capacity, "confirm CH%d %dms %s",
                     data->trace_first_channel, data->trace_window_ms, return_key);
            return true;
        case TRACE_STATUS_LOCKED:
            snprintf(line, capacity, "Trace CH %d %s", data->trace_channel, return_key);
            return true;
        case TRACE_STATUS_RELEASED:
            snprintf(line, capacity, "rearmed/hopping %s", return_key);
            return true;
        case TRACE_STATUS_TUNE_FAILED:
            snprintf(line, capacity, "hopping continues %s", return_key);
            return true;
        case TRACE_STATUS_UNAVAILABLE:
            snprintf(line, capacity, "Trace unavailable %s", return_key);
            return true;
        case TRACE_STATUS_OFF:
        default:
            return false;
    }
}

static bool trace_compact_text(const deauth_detector_data_t *data,
                               char *line, size_t capacity)
{
    switch (data->trace_status) {
        case TRACE_STATUS_WAITING:
            snprintf(line, capacity, "Trace wait");
            return true;
        case TRACE_STATUS_ARMED:
            snprintf(line, capacity, "Trace armed");
            return true;
        case TRACE_STATUS_CONFIRMING:
            snprintf(line, capacity, "Trace conf CH%d", data->trace_first_channel);
            return true;
        case TRACE_STATUS_LOCKED:
            snprintf(line, capacity, "Trace CH%d", data->trace_channel);
            return true;
        case TRACE_STATUS_RELEASED:
            snprintf(line, capacity, "Trace rearmed CH%d", data->trace_channel);
            return true;
        case TRACE_STATUS_TUNE_FAILED:
            snprintf(line, capacity, "Trace fail CH%d", data->trace_channel);
            return true;
        case TRACE_STATUS_UNAVAILABLE:
            snprintf(line, capacity, "Trace unavailable");
            return true;
        case TRACE_STATUS_OFF:
        default:
            return false;
    }
}

static bool defence_status_text(const deauth_detector_data_t *data,
                                char *line, size_t capacity)
{
    switch (data->defence_status) {
        case DEFENCE_STATUS_WAITING:
            snprintf(line, capacity, "Defence: wait arm");
            return true;
        case DEFENCE_STATUS_ARMED:
            snprintf(line, capacity, "Defence armed");
            return true;
        case DEFENCE_STATUS_PROVISIONAL:
            snprintf(line, capacity, "Provisional CH%d", data->defence_channel);
            return true;
        case DEFENCE_STATUS_RESPONDING:
            snprintf(line, capacity, "Responding %.18s", data->defence_response);
            return true;
        case DEFENCE_STATUS_RESPONSE_OK:
            snprintf(line, capacity, "Response OK %.17s", data->defence_response);
            return true;
        case DEFENCE_STATUS_RESPONSE_ERROR:
            snprintf(line, capacity, "Response error");
            return true;
        case DEFENCE_STATUS_RELEASED:
            snprintf(line, capacity, "Released/rearmed CH%d", data->defence_channel);
            return true;
        case DEFENCE_STATUS_STOPPED:
            snprintf(line, capacity, "Stopped Resp %u",
                     data->defence_summary_responses);
            return true;
        case DEFENCE_STATUS_UNSUPPORTED:
            snprintf(line, capacity, "Defence unsupported");
            return true;
        case DEFENCE_STATUS_OFF:
        default:
            return false;
    }
}

static void draw_defence_status(const deauth_detector_data_t *data, int row)
{
    char line[40];
    if (!defence_status_text(data, line, sizeof(line))) return;
    print_line(row, line,
               data->defence_status == DEFENCE_STATUS_RESPONSE_ERROR ||
               data->defence_status == DEFENCE_STATUS_UNSUPPORTED ?
               UI_COLOR_HIGHLIGHT : UI_COLOR_TEXT);
    if (row < 6) {
        snprintf(line, sizeof(line), "Responses %u", data->defence_responses);
        print_line(row + 1, line, UI_COLOR_TEXT);
    }
    if (row < 5 && data->defence_suppressed > 0) {
        snprintf(line, sizeof(line), "Suppressed %u", data->defence_suppressed);
        print_line(row + 2, line, UI_COLOR_DIMMED);
    }
}

static void compact_count(unsigned value, char *text, size_t capacity)
{
    if (value > 9999U) snprintf(text, capacity, "9999+");
    else snprintf(text, capacity, "%u", value);
}

static int forensic_average_rssi(const forensic_session_t *session)
{
    if (!session->rssi_available || session->rssi_samples == 0) return 0;
    return (int)((session->rssi_sum - (int64_t)(session->rssi_samples / 2U)) /
                 (int64_t)session->rssi_samples);
}

static void format_forensic_rssi(const forensic_session_t *session,
                                 char *line, size_t capacity)
{
    char missing[8] = {0};
    if (session->unknown_rssi > 0) {
        compact_count(session->unknown_rssi, missing, sizeof(missing));
    }
    if (session->rssi_available) {
        snprintf(line, capacity, session->unknown_rssi > 0 ?
                 "RSSI %d/%d/%d ?%s" : "RSSI %d/%d/%d",
                 session->rssi_min, forensic_average_rssi(session),
                 session->rssi_max, missing);
    } else {
        snprintf(line, capacity, session->unknown_rssi > 0 ?
                 "RSSI -/-/- ?%s" : "RSSI -/-/-", missing);
    }
}

static bool forensic_remote_uncertain(const deauth_detector_data_t *data)
{
    if (data->forensic.finalized) return false;
    return data->stop_pending || data->stop_send_failed ||
           data->state == DET_STOP_FOR_SCAN || data->state == DET_STOP_FOR_ALL ||
           data->state == DET_STOP_BAD_WATCH || data->state == DET_STOP_EXIT ||
           data->state == DET_ERROR || data->state == DET_DISCONNECTED;
}

static void draw_forensic_uncertainty(const deauth_detector_data_t *data)
{
    char line[UI_COLS + 1];
    if (data->state == DET_ERROR || data->state == DET_DISCONNECTED ||
        data->stop_send_failed) {
        snprintf(line, sizeof(line), "%.30s", data->message);
    } else {
        snprintf(line, sizeof(line), "Stop ACK pending");
    }
    print_line(6, line, UI_COLOR_HIGHLIGHT);
    ui_draw_status("Remote state unknown");
}

static void draw_forensics(const deauth_detector_data_t *data)
{
    const forensic_session_t *session = &data->forensic;
    char line[64];
    if (session->accepted == 0) {
        ui_print_center(3, "Forensics unavailable", UI_COLOR_DIMMED);
        ui_print_center(5, "Optional JanOS v1 marker", UI_COLOR_DIMMED);
        ui_print_center(6, "Evidence only", UI_COLOR_TEXT);
        if (forensic_remote_uncertain(data)) draw_forensic_uncertainty(data);
        else ui_draw_status("W:Back C:CSA S:Scan");
        return;
    }

    char rows_count[8], bursts[8], largest[8], dropped[8];
    char protected_count[8], unprotected[8], retry[8], unknown[8];
    compact_count(session->accepted, rows_count, sizeof(rows_count));
    compact_count(session->bursts, bursts, sizeof(bursts));
    compact_count(session->largest_burst, largest, sizeof(largest));
    compact_count(session->dropped, dropped, sizeof(dropped));
    compact_count(session->protected_count, protected_count, sizeof(protected_count));
    compact_count(session->unprotected_count, unprotected, sizeof(unprotected));
    compact_count(session->retry_count, retry, sizeof(retry));
    compact_count(session->unknown_count, unknown, sizeof(unknown));
    snprintf(line, UI_COLS + 1, "Rows %s Bursts %s", rows_count, bursts);
    print_line(1, line, UI_COLOR_TEXT);

    if (session->finalized) {
        snprintf(line, UI_COLS + 1, "Largest %s Drop %s", largest, dropped);
        print_line(2, line, UI_COLOR_TEXT);
        format_forensic_rssi(session, line, UI_COLS + 1);
        print_line(3, line, UI_COLOR_TEXT);
        snprintf(line, UI_COLS + 1, "Prot %s Unprot %s", protected_count, unprotected);
        print_line(4, line, UI_COLOR_TEXT);
        snprintf(line, UI_COLS + 1, "Retry %s Unknown %s", retry, unknown);
        print_line(5, line, UI_COLOR_TEXT);
        print_line(6, "Evidence only", UI_COLOR_TEXT);
        ui_draw_status(data->state == DET_PICK ? "W:Back to scan" : "W:Back C:CSA");
        return;
    }

    format_forensic_rssi(session, line, UI_COLS + 1);
    print_line(2, line, UI_COLOR_TEXT);
    switch (session->sequence_state) {
        case FORENSIC_SEQ_DELTA:
            snprintf(line, UI_COLS + 1, "Seq %u d+%u R%s", session->latest_sequence,
                     session->latest_delta, session->latest_retry_known ?
                     (session->latest_retry ? "1" : "0") : "-");
            break;
        case FORENSIC_SEQ_RETRANSMISSION:
            snprintf(line, UI_COLS + 1, "Seq %u retransmission",
                     session->latest_sequence);
            break;
        case FORENSIC_SEQ_REPEATED:
            snprintf(line, UI_COLS + 1, "Seq %u repeated sequence",
                     session->latest_sequence);
            break;
        case FORENSIC_SEQ_INSUFFICIENT:
        default:
            snprintf(line, UI_COLS + 1, "insufficient sequence data");
            break;
    }
    print_line(3, line, UI_COLOR_TEXT);
    snprintf(line, UI_COLS + 1, "Unprot %s Retry %s", unprotected, retry);
    print_line(4, line, UI_COLOR_TEXT);
    snprintf(line, UI_COLS + 1, "Prot %s Unknown %s", protected_count, unknown);
    print_line(5, line, UI_COLOR_TEXT);
    snprintf(line, UI_COLS + 1, "Largest %s Drop %s", largest, dropped);
    print_line(6, line, UI_COLOR_TEXT);
    if (forensic_remote_uncertain(data)) draw_forensic_uncertainty(data);
    else ui_draw_status("Evidence only W:Back");
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
    ui_draw_status("ESC/Q: Exit");
}

static void draw_pick(const deauth_detector_data_t *data)
{
    char line[64];
    snprintf(line, sizeof(line), "Selected networks %d/%d", selected_count(data), data->scan_count);
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
    ui_draw_status(data->forensic.finalized && data->forensic.accepted > 0 ?
                   "SP:Mark EN:Run W:Evidence" :
                   "SP:Mark EN:Run R:Scan A:All");
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
    ui_draw_status("UP/DN W:Forensic C:CSA");
}

static void draw_deauth(const deauth_detector_data_t *data)
{
    char line[64];
    if (defence_requested(data)) {
        draw_defence_status(data, 1);
        char trace_line[24] = {0};
        if (trace_compact_text(data, trace_line, sizeof(trace_line))) {
            if (data->defence_suppressed > 0) {
                char count[8];
                compact_count(data->defence_suppressed, count, sizeof(count));
                snprintf(line, UI_COLS + 1, "%.19s Sup%.5s", trace_line, count);
            } else {
                snprintf(line, UI_COLS + 1, "%s", trace_line);
            }
            print_line(3, line, UI_COLOR_TEXT);
        } else if (data->defence_suppressed > 0) {
            snprintf(line, UI_COLS + 1, "Suppressed %u", data->defence_suppressed);
            print_line(3, line, UI_COLOR_DIMMED);
        }
        unsigned shown_deauth = data->deauth_count < 0 ? 0U :
            data->deauth_count > 9999 ? 9999U : (unsigned)data->deauth_count;
        unsigned shown_csa = data->csa_count < 0 ? 0U :
            data->csa_count > 9999 ? 9999U : (unsigned)data->csa_count;
        snprintf(line, UI_COLS + 1, "DEAUTH %u%s CSA %u%s",
                 shown_deauth, data->deauth_count > 9999 ? "+" : "",
                 shown_csa, data->csa_count > 9999 ? "+" : "");
        print_line(4, line, UI_COLOR_TEXT);
        if (data->deauth_count > 0) {
            snprintf(line, sizeof(line), "CH %d RSSI %d", data->last_deauth.channel,
                     data->last_deauth.rssi);
            print_line(5, line, UI_COLOR_HIGHLIGHT);
            snprintf(line, UI_COLS + 1, "Claimed TX %s", data->last_deauth.tx);
            print_line(6, line, UI_COLOR_TEXT);
        } else {
            ui_print_center(6, "Waiting for observations", UI_COLOR_DIMMED);
        }
        ui_draw_status("S:Scan W:Watch C:CSA Q:Exit");
        return;
    }
    if (data->trace_status != TRACE_STATUS_OFF) {
        trace_status_text(data, line, sizeof(line));
        print_line(1, line, data->trace_status == TRACE_STATUS_TUNE_FAILED ?
                   UI_COLOR_HIGHLIGHT : UI_COLOR_TEXT);
        if (data->deauth_count > 0 && data->last_deauth.detailed) {
            snprintf(line, UI_COLS + 1, "SSID %.25s", data->last_deauth.ap_name[0] ?
                     data->last_deauth.ap_name : "<Hidden>");
            print_line(2, line, UI_COLOR_TEXT);
            if (data->last_deauth.reason_known) {
                snprintf(line, UI_COLS + 1, "Reason %d CH %d RSSI %d",
                         data->last_deauth.reason, data->last_deauth.channel,
                         data->last_deauth.rssi);
            } else {
                snprintf(line, UI_COLS + 1, "Reason - (PMF) CH%d RSSI%d",
                         data->last_deauth.channel, data->last_deauth.rssi);
            }
            print_line(3, line, UI_COLOR_HIGHLIGHT);
            snprintf(line, UI_COLS + 1, "BSSID %s", data->last_deauth.bssid);
            print_line(4, line, UI_COLOR_DIMMED);
            snprintf(line, UI_COLS + 1, "Claimed TX %s", data->last_deauth.tx);
            print_line(5, line, UI_COLOR_TEXT);
            if (bssid_equal(data->last_deauth.da, "FF:FF:FF:FF:FF:FF")) {
                print_line(6, "DA broadcast: no client ID", UI_COLOR_TEXT);
            } else {
                snprintf(line, UI_COLS + 1, "DA %s", data->last_deauth.da);
                print_line(6, line, UI_COLOR_TEXT);
            }
        } else {
            snprintf(line, sizeof(line), data->state == DET_ACTIVE_SELECTED ?
                     "Selected BSSIDs: %d" : "All networks / all channels",
                     data->watch_expected_count);
            print_line(2, line, UI_COLOR_HIGHLIGHT);
            snprintf(line, sizeof(line), "DEAUTH %d   CSA %d",
                     data->deauth_count, data->csa_count);
            print_line(3, line, UI_COLOR_TEXT);
            if (data->deauth_count > 0) {
                snprintf(line, sizeof(line), "CH %d  RSSI %d dBm",
                         data->last_deauth.channel, data->last_deauth.rssi);
                print_line(4, line, UI_COLOR_HIGHLIGHT);
                snprintf(line, sizeof(line), "AP %.26s", data->last_deauth.ap_name);
                print_line(5, line, UI_COLOR_TEXT);
                snprintf(line, sizeof(line), "BSSID %s", data->last_deauth.bssid);
                print_line(6, line, UI_COLOR_DIMMED);
            } else {
                ui_print_center(5, "Waiting for DEAUTH", UI_COLOR_DIMMED);
                if (data->state == DET_ACTIVE_SELECTED) {
                    print_line(6, "W: watched BSSIDs", UI_COLOR_TEXT);
                }
            }
        }
        if (data->trace_status == TRACE_STATUS_TUNE_FAILED) {
            ui_draw_status("hopping continues S:Scan ESC");
        } else if (data->trace_status == TRACE_STATUS_RELEASED) {
            ui_draw_status("Trace rearmed/hopping S:Scan");
        } else {
            ui_draw_status(data->state == DET_ACTIVE_SELECTED ?
                           "S:Scan W:Watch C:CSA Q:Exit" :
                           "S:Scan W:Forensic C:CSA" );
        }
        return;
    }
    if (data->deauth_count > 0 && data->last_deauth.detailed) {
        snprintf(line, UI_COLS + 1, "SSID %.25s", data->last_deauth.ap_name[0] ?
                 data->last_deauth.ap_name : "<Hidden>");
        print_line(1, line, UI_COLOR_TEXT);
        if (data->last_deauth.reason_known) {
            snprintf(line, UI_COLS + 1, "Reason %d CH %d RSSI %d",
                     data->last_deauth.reason, data->last_deauth.channel,
                     data->last_deauth.rssi);
        } else {
            snprintf(line, UI_COLS + 1, "Reason - (PMF) CH%d RSSI%d",
                     data->last_deauth.channel, data->last_deauth.rssi);
        }
        print_line(2, line, UI_COLOR_HIGHLIGHT);
        snprintf(line, UI_COLS + 1, "BSSID %s", data->last_deauth.bssid);
        print_line(3, line, UI_COLOR_DIMMED);
        snprintf(line, UI_COLS + 1, "Claimed TX %s", data->last_deauth.tx);
        print_line(4, line, UI_COLOR_TEXT);
        print_line(5, "Frame recipient / DA", UI_COLOR_TEXT);
        if (bssid_equal(data->last_deauth.da, "FF:FF:FF:FF:FF:FF")) {
            print_line(6, "Broadcast: no single recipient", UI_COLOR_TEXT);
        } else {
            print_line(6, data->last_deauth.da, UI_COLOR_TEXT);
        }
        ui_draw_status(data->state == DET_ACTIVE_SELECTED ?
                       "S:Scan W:Watch C:CSA Q:Exit" : "S:Scan W:Forensic C:CSA");
        return;
    }
    if (data->state == DET_ACTIVE_SELECTED) {
        snprintf(line, sizeof(line), "Selected BSSIDs: %d", data->watch_expected_count);
    } else {
        snprintf(line, sizeof(line), "All networks / all channels");
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
    ui_draw_status("S:Scan A:All W:Forensic");
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
    if (trace_footer_text(data, "C:DEAUTH", line, sizeof(line))) {
        ui_draw_status(line);
    } else {
        ui_draw_status("C:DEAUTH W:Watch ESC:Exit");
    }
}

static void draw_screen(screen_t *self)
{
    deauth_detector_data_t *data = self->user_data;
    if (!data) return;
    char detail_title[40];
    const char *title = data->exit_confirm_pending ? "EXIT DEAUTH DETECTOR?" :
                        data->view == VIEW_FORENSICS ?
                            (data->forensic.finalized ? "FORENSICS SUMMARY" : "FORENSICS v1") :
                        data->view == VIEW_CSA ? "CSA ANNOUNCEMENTS" : "DEAUTH DETECTOR";
    if (!data->exit_confirm_pending && data->view == VIEW_DEAUTH &&
        (data->state == DET_ACTIVE_ALL || data->state == DET_ACTIVE_SELECTED) &&
        data->deauth_count > 0 && data->last_deauth.detailed) {
        int shown_deauth = data->deauth_count < 0 ? 0 :
                           data->deauth_count > 9999 ? 9999 : data->deauth_count;
        int shown_csa = data->csa_count < 0 ? 0 :
                        data->csa_count > 9999 ? 9999 : data->csa_count;
        snprintf(detail_title, sizeof(detail_title), "DEAUTH %d%s CSA%d%s",
                 shown_deauth, data->deauth_count > 9999 ? "+" : "",
                 shown_csa, data->csa_count > 9999 ? "+" : "");
        title = detail_title;
    }
    if (!data->ui_initialized) {
        ui_clear();
        data->ui_initialized = true;
        data->rendered_title[0] = '\0';
    }
    if (strcmp(data->rendered_title, title) != 0) {
        ui_draw_title(title);
        snprintf(data->rendered_title, sizeof(data->rendered_title), "%s", title);
    }
    display_fill_rect(0, DET_CONTENT_Y_START, DISPLAY_WIDTH, DET_CONTENT_HEIGHT, UI_COLOR_BG);
    if (data->exit_confirm_pending) {
        ui_print_center(2, "Stop detector and exit?", UI_COLOR_TEXT);
        ui_print_center(4, "Wait for JanOS stop ACK", UI_COLOR_DIMMED);
        ui_draw_status("ENTER:Yes ESC/Q:No");
        return;
    }
    if (data->state == DET_TRACE_CHOICE) {
        draw_trace_choice(data);
    } else if (data->state == DET_PMKID_WARNING) {
        draw_pmkid_warning();
    } else if (data->state == DET_SCAN_WAIT_ACK || data->state == DET_SCAN_RUNNING ||
        data->state == DET_SCAN_COLLECT) {
        draw_scan(data);
    } else if (data->state == DET_PICK && data->view == VIEW_FORENSICS &&
               data->forensic.finalized && data->forensic.accepted > 0) {
        draw_forensics(data);
    } else if (data->state == DET_PICK) {
        draw_pick(data);
    } else if (data->state == DET_ACTIVE_ALL || data->state == DET_ACTIVE_SELECTED) {
        if (data->view == VIEW_FORENSICS) draw_forensics(data);
        else if (data->view == VIEW_CSA) draw_csa(data);
        else if (data->view == VIEW_WATCH && data->state == DET_ACTIVE_SELECTED) draw_watch(data);
        else draw_deauth(data);
    } else if (data->view == VIEW_FORENSICS && data->forensic.accepted > 0 &&
               (data->state == DET_STOP_FOR_SCAN || data->state == DET_STOP_FOR_ALL ||
                data->state == DET_STOP_BAD_WATCH || data->state == DET_STOP_EXIT ||
                data->state == DET_ERROR || data->state == DET_DISCONNECTED)) {
        draw_forensics(data);
    } else {
        ui_print_center(2, data->message, UI_COLOR_HIGHLIGHT);
        if (data->state == DET_START_SELECTED) {
            char line[40];
            snprintf(line, sizeof(line), "WATCH %d/%d",
                     data->verified_watch_count, data->watch_expected_count);
            ui_print_center(4, line, UI_COLOR_TEXT);
            draw_trace_status(data, 5);
        } else if (data->state == DET_START_ALL) {
            draw_trace_status(data, 4);
        } else if (!defence_requested(data) &&
                   (data->state == DET_STOP_FOR_SCAN ||
                   data->state == DET_STOP_FOR_ALL ||
                   data->state == DET_STOP_BAD_WATCH ||
                   data->state == DET_STOP_EXIT)) {
            draw_trace_status(data, 4);
        } else if (data->state == DET_ERROR || data->state == DET_DISCONNECTED) {
            if (!defence_requested(data) &&
                (data->stop_pending || data->stop_send_failed) &&
                data->trace_status != TRACE_STATUS_OFF) {
                draw_trace_status(data, 4);
                ui_print_center(6, "R: fresh scan   A: all", UI_COLOR_DIMMED);
            } else {
                ui_print_center(4, "Detector not confirmed", UI_COLOR_TEXT);
                ui_print_center(5, "R: fresh scan   A: all", UI_COLOR_DIMMED);
            }
        }
        if (defence_requested(data) && data->defence_status != DEFENCE_STATUS_OFF) {
            draw_defence_status(data, 3);
            char trace_line[40];
            if (trace_compact_text(data, trace_line, sizeof(trace_line))) {
                print_line(5, trace_line, UI_COLOR_TEXT);
            }
        }
        ui_draw_status((data->stop_pending ||
                        data->state == DET_STOP_FOR_SCAN ||
                        data->state == DET_STOP_FOR_ALL ||
                        data->state == DET_STOP_BAD_WATCH ||
                        data->state == DET_STOP_EXIT) ?
                       "Waiting for JanOS stop ACK" : "ESC/Q: Exit");
    }
    if (data->stop_send_failed) {
        ui_print(0, 6, data->message, UI_COLOR_HIGHLIGHT);
        ui_draw_status("ESC/Q: Confirm exit");
    }
}

static void on_resume(screen_t *self)
{
    deauth_detector_data_t *data = self->user_data;
    if (!data) return;
    data->ui_initialized = false;
    data->needs_redraw = true;
}

static void check_ping(deauth_detector_data_t *data, int64_t now)
{
    if (data->state != DET_PICK && data->state != DET_ACTIVE_ALL &&
        data->state != DET_ACTIVE_SELECTED) return;
    if (data->ping_waiting && now - data->ping_sent_us >= PING_TIMEOUT_US) {
        data->exit_confirm_pending = false;
        clear_scan(data);
        data->ping_waiting = false;
        data->forensic_accepting = false;
        set_state(data, DET_DISCONNECTED);
        set_message(data, "JanOS connection lost");
        return;
    }
    if (!data->ping_waiting && now - data->last_ping_us >= PING_INTERVAL_US) {
        if (uart_send_command("ping") != 0) {
            data->exit_confirm_pending = false;
            clear_scan(data);
            data->forensic_accepting = false;
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
        fail_after_safety_stop(data, "No JanOS start response");
    } else if (data->state == DET_START_SELECTED &&
               now - data->state_started_us >= SELECTED_START_TIMEOUT_US) {
        fail_after_safety_stop(data, data->start_ack ?
                               "WATCH proof timed out" :
                               "No JanOS start response");
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
    if (!data || data->leaving) return;
    if (data->state == DET_TRACE_CHOICE) {
        if (key == KEY_SPACE) {
            data->mode = (detector_mode_t)((data->mode + 1) % DET_MODE_COUNT);
            data->trace_enabled = data->mode != DET_MODE_PASSIVE;
            clear_trace_status(data);
        } else if (key == KEY_ENTER) {
            if (data->mode == DET_MODE_DEFENCE_PMKID) {
                set_state(data, DET_PMKID_WARNING);
                set_message(data, "Confirm PMKID Lab warning");
            } else if (data->mode == DET_MODE_DEFENCE) {
                stop_then(data, DET_STOP_FOR_SCAN);
            } else {
                start_all(data);
            }
        } else if (key == KEY_ESC || key == KEY_Q) {
            data->leaving = true;
            screen_manager_pop();
        }
        return;
    }
    if (data->state == DET_PMKID_WARNING) {
        if (key == KEY_ENTER) {
            stop_then(data, DET_STOP_FOR_SCAN);
        } else if (key == KEY_ESC || key == KEY_Q) {
            set_state(data, DET_TRACE_CHOICE);
            set_message(data, "Choose detector mode");
        }
        return;
    }
    if (data->exit_confirm_pending) {
        if (key == KEY_ESC || key == KEY_Q) {
            data->exit_confirm_pending = false;
            data->needs_redraw = true;
        } else if (key == KEY_ENTER) {
            data->exit_confirm_pending = false;
            data->needs_redraw = true;
            if (data->state == DET_STOP_FOR_SCAN ||
                data->state == DET_STOP_FOR_ALL ||
                data->state == DET_STOP_BAD_WATCH) {
                /* Reuse the pending stop and its original timeout. */
                data->state = DET_STOP_EXIT;
                set_message(data, "Waiting for JanOS stop");
            } else if (data->state != DET_STOP_EXIT) {
                stop_then(data, DET_STOP_EXIT);
            }
        }
        return;
    }
    if (key == KEY_ESC || key == KEY_Q) {
        if (data->state != DET_STOP_EXIT) {
            data->exit_confirm_pending = true;
            data->needs_redraw = true;
        }
        return;
    }
    if (data->state == DET_PICK && data->view == VIEW_FORENSICS) {
        if (key == KEY_W) {
            data->view = VIEW_DEAUTH;
            data->needs_redraw = true;
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
        if (data->state == DET_ACTIVE_SELECTED && !defence_requested(data)) {
            stop_then(data, DET_STOP_FOR_ALL);
        }
        return;
    }
    if (key == KEY_A && data->state == DET_PICK && data->mode < DET_MODE_DEFENCE) {
        start_all(data);
        return;
    }
    if (key == KEY_A && data->mode < DET_MODE_DEFENCE &&
        (data->state == DET_ERROR || data->state == DET_DISCONNECTED)) {
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
        if (key == KEY_W && data->forensic.finalized &&
            data->forensic.accepted > 0) {
            data->view = data->view == VIEW_FORENSICS ? VIEW_DEAUTH : VIEW_FORENSICS;
        } else if (data->view == VIEW_FORENSICS) {
            return;
        } else if (key == KEY_UP && data->cursor > 0) --data->cursor;
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
    } else if (key == KEY_W) {
        if (data->state == DET_ACTIVE_SELECTED) {
            data->view = data->view == VIEW_WATCH ? VIEW_FORENSICS :
                         data->view == VIEW_FORENSICS ? VIEW_WATCH : VIEW_WATCH;
            data->cursor = 0;
        } else {
            data->view = data->view == VIEW_FORENSICS ? VIEW_DEAUTH : VIEW_FORENSICS;
        }
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
    screen->on_resume = on_resume;
    screen->on_tick = on_tick;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    uart_register_line_callback(uart_line_callback, data);
    data->mode = DET_MODE_PASSIVE;
    set_state(data, DET_TRACE_CHOICE);
    set_message(data, "Choose detector mode");
    clear_trace_status(data);
    draw_screen(screen);
    ESP_LOGI(TAG, "Detector screen opened");
    return screen;
}
