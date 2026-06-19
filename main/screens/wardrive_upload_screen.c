/**
 * @file wardrive_upload_screen.c
 * @brief Wardrive upload screen (WiGLE/WDGWars)
 */

#include "wardrive_upload_screen.h"
#include "wifi_connect_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

static const char *TAG = "WARDRIVE_UPLOAD";

#define MAX_UPLOAD_FILES 48
#define MAX_UPLOAD_HISTORY 48
#define MAX_SCAN_DIRS 4
#define VISIBLE_ITEMS 4

typedef enum {
    STATE_LOADING_FILES = 0,
    STATE_PICK_FILES,
    STATE_WAIT_WIFI,
    STATE_UPLOADING,
    STATE_ARCHIVING,
    STATE_DONE,
    STATE_ERROR
} upload_state_t;

typedef enum {
    UPLOAD_MODE_SELECTED = 0,
    UPLOAD_MODE_PENDING,
    UPLOAD_MODE_ALL,
} upload_mode_t;

typedef struct {
    char path[160];
    char name[96];
    char hash[17];
    char wigle_status[16];
    char wdgwars_status[16];
    long size_bytes;
    int wifi_rows;
    int ble_rows;
    int bt_rows;
    int bad_rows;
    bool selected;
    void *checkbox_ptr;
} upload_file_t;

typedef struct {
    char service[12];
    char filename[96];
    char hash[17];
    char status[16];
    long size_bytes;
    int wifi_rows;
    int ble_rows;
    int bt_rows;
    int bad_rows;
} upload_history_t;

typedef struct {
    upload_state_t state;
    wardrive_upload_target_t target;
    upload_file_t files[MAX_UPLOAD_FILES];
    upload_history_t history[MAX_UPLOAD_HISTORY];
    int file_count;
    int history_count;
    int selected_index;
    int history_index;
    int scroll_offset;
    int history_scroll;
    int selected_count;
    bool show_history;

    volatile bool needs_redraw;
    volatile bool closing;

    volatile bool file_scan_running;
    volatile bool file_scan_stop;
    volatile bool loading_wardrive_files;
    volatile bool loading_upload_state;
    volatile bool wardrive_files_end_seen;
    volatile bool upload_state_end_seen;
    TaskHandle_t file_scan_task;
    char active_dir[48];
    char status_line[64];

    volatile bool task_running;
    volatile bool cancel_requested;
    TaskHandle_t upload_task;
    upload_mode_t upload_mode;
    int upload_uploaded;
    int upload_failed;
    int upload_skipped;
    int upload_rate_limited;
    int upload_total;
    int upload_index;

    volatile bool waiting_file_done;
    volatile bool waiting_key_check;
    volatile bool key_missing;
    volatile bool key_checked;
    volatile bool upload_fatal;
    char fatal_reason[64];

    int cur_ok;
    int cur_skip;
    int cur_fail;
    bool cur_accounted;

    volatile bool cleanup_running;
    volatile bool cleanup_end_seen;
    volatile bool cleanup_unsupported;
    volatile bool cleanup_move_ready;
    bool cleanup_move;
    TaskHandle_t cleanup_task;
    int cleanup_scanned;
    int cleanup_matched;
    int cleanup_moved;
    int cleanup_failed;
    char cleanup_last[64];
} wardrive_upload_data_t;

static const char *k_scan_dirs[MAX_SCAN_DIRS] = {
    "/sdcard/lab/wardrives",
    "/sdcard/lab/wardrive",
    "/sdcard/lab",
    "/sdcard"
};

static void draw_screen(screen_t *self);

static bool contains_nocase(const char *text, const char *needle)
{
    if (!text || !needle) return false;
    size_t nlen = strlen(needle);
    if (nlen == 0) return true;
    for (const char *p = text; *p; p++) {
        if (tolower((unsigned char)*p) == tolower((unsigned char)needle[0])) {
            if (strncasecmp(p, needle, nlen) == 0) return true;
        }
    }
    return false;
}

static bool get_token_value(const char *line, const char *key, char *out, size_t out_len)
{
    if (out && out_len) out[0] = '\0';
    if (!line || !key || !out || out_len == 0) return false;

    size_t key_len = strlen(key);
    const char *p = line;
    while ((p = strstr(p, key)) != NULL) {
        bool left_ok = (p == line) || p[-1] == ' ' || p[-1] == '[';
        if (left_ok && p[key_len] == '=') {
            const char *v = p + key_len + 1;
            size_t i = 0;
            while (v[i] && v[i] != ' ' && v[i] != '\r' && v[i] != '\n' && i < out_len - 1) {
                out[i] = v[i];
                i++;
            }
            out[i] = '\0';
            return i > 0;
        }
        p += key_len;
    }
    return false;
}

static int get_token_int(const char *line, const char *key, int fallback)
{
    char buf[24];
    if (!get_token_value(line, key, buf, sizeof(buf))) return fallback;
    char *end = NULL;
    long v = strtol(buf, &end, 0);
    return (end == buf) ? fallback : (int)v;
}

static long get_token_long(const char *line, const char *key, long fallback)
{
    char buf[24];
    if (!get_token_value(line, key, buf, sizeof(buf))) return fallback;
    char *end = NULL;
    long v = strtol(buf, &end, 0);
    return (end == buf) ? fallback : v;
}

static bool has_upload_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return strcasecmp(dot, ".csv") == 0 ||
           strcasecmp(dot, ".txt") == 0 ||
           strcasecmp(dot, ".log") == 0;
}

static bool is_skip_line(const char *line)
{
    if (!line || !line[0]) return true;
    if ((line[0] == 'I' || line[0] == 'W' || line[0] == 'E' || line[0] == 'D') &&
        line[1] == ' ' && line[2] == '(') return true;
    if (strstr(line, "[MEM]") != NULL) return true;
    if (strstr(line, "Files in ") != NULL) return true;
    if (strstr(line, "Found ") != NULL) return true;
    if (strncmp(line, "list_dir", 8) == 0) return true;
    return false;
}

static void trim_token(char *s)
{
    if (!s) return;
    while (*s == ' ' || *s == '\t' || *s == '"' || *s == '\'' || *s == ',') {
        memmove(s, s + 1, strlen(s));
    }
    size_t len = strlen(s);
    while (len > 0) {
        char c = s[len - 1];
        if (c == ' ' || c == '\t' || c == '"' || c == '\'' || c == ',' || c == '\r' || c == '\n') {
            s[--len] = '\0';
        } else {
            break;
        }
    }
}

static const char *file_basename(const char *path)
{
    if (!path) return "";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool status_is_done(const char *status)
{
    return status && (strcasecmp(status, "done") == 0 ||
                      strcasecmp(status, "ok") == 0 ||
                      strcasecmp(status, "uploaded") == 0);
}

static const char *target_short_name(wardrive_upload_target_t target)
{
    return target == WARDRIVE_UPLOAD_TARGET_WIGLE ? "WIG" : "WDG";
}

static const char *file_target_status(const upload_file_t *file, wardrive_upload_target_t target)
{
    if (!file) return "";
    return target == WARDRIVE_UPLOAD_TARGET_WIGLE ? file->wigle_status : file->wdgwars_status;
}

static bool file_needs_target_upload(const upload_file_t *file, wardrive_upload_target_t target)
{
    return file && !status_is_done(file_target_status(file, target));
}

static int target_pending_count(wardrive_upload_data_t *data)
{
    int n = 0;
    for (int i = 0; i < data->file_count; i++) {
        if (file_needs_target_upload(&data->files[i], data->target)) n++;
    }
    return n;
}

static int target_done_count(wardrive_upload_data_t *data)
{
    int n = 0;
    for (int i = 0; i < data->file_count; i++) {
        if (status_is_done(file_target_status(&data->files[i], data->target))) n++;
    }
    return n;
}

static int visible_file_count(wardrive_upload_data_t *data)
{
    return target_pending_count(data);
}

static int visible_file_index(wardrive_upload_data_t *data, int visible_idx)
{
    int n = 0;
    for (int i = 0; i < data->file_count; i++) {
        if (file_needs_target_upload(&data->files[i], data->target)) {
            if (n == visible_idx) return i;
            n++;
        }
    }
    return -1;
}

static bool add_file_entry(wardrive_upload_data_t *data, const char *dir, const char *token)
{
    if (!data || !dir || !token || !token[0]) return false;
    if (data->file_count >= MAX_UPLOAD_FILES) return false;
    if (!has_upload_ext(token)) return false;

    char full_path[160];
    if (token[0] == '/') {
        snprintf(full_path, sizeof(full_path), "%s", token);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", dir, token);
    }

    const char *base = file_basename(full_path);
    if ((strcmp(dir, "/sdcard/lab") == 0 || strcmp(dir, "/sdcard") == 0) &&
        !contains_nocase(base, "wardrive") && !contains_nocase(base, "wigle")) {
        return false;
    }

    for (int i = 0; i < data->file_count; i++) {
        if (strcasecmp(data->files[i].path, full_path) == 0) {
            return false;
        }
    }

    upload_file_t *dst = &data->files[data->file_count++];
    snprintf(dst->path, sizeof(dst->path), "%s", full_path);
    snprintf(dst->name, sizeof(dst->name), "%s", base);
    snprintf(dst->wigle_status, sizeof(dst->wigle_status), "%s", "pending");
    snprintf(dst->wdgwars_status, sizeof(dst->wdgwars_status), "%s", "pending");
    dst->selected = false;
    dst->checkbox_ptr = (void *)dst;
    return true;
}

static bool upsert_ward_file(wardrive_upload_data_t *data, const char *line)
{
    if (!data || !line) return false;

    char filename[96] = {0};
    if (!get_token_value(line, "filename", filename, sizeof(filename)) &&
        !get_token_value(line, "file", filename, sizeof(filename))) {
        return false;
    }

    upload_file_t *dst = NULL;
    for (int i = 0; i < data->file_count; i++) {
        if (strcasecmp(data->files[i].name, filename) == 0) {
            dst = &data->files[i];
            break;
        }
    }
    if (!dst) {
        if (data->file_count >= MAX_UPLOAD_FILES) return false;
        dst = &data->files[data->file_count++];
        memset(dst, 0, sizeof(*dst));
    }

    snprintf(dst->name, sizeof(dst->name), "%s", filename);
    snprintf(dst->path, sizeof(dst->path), "%s", filename);
    get_token_value(line, "hash", dst->hash, sizeof(dst->hash));
    if (!get_token_value(line, "wigle", dst->wigle_status, sizeof(dst->wigle_status))) {
        snprintf(dst->wigle_status, sizeof(dst->wigle_status), "%s", "pending");
    }
    if (!get_token_value(line, "wdgwars", dst->wdgwars_status, sizeof(dst->wdgwars_status))) {
        snprintf(dst->wdgwars_status, sizeof(dst->wdgwars_status), "%s", "pending");
    }
    dst->size_bytes = get_token_long(line, "size", dst->size_bytes);
    dst->wifi_rows = get_token_int(line, "wifi", dst->wifi_rows);
    dst->ble_rows = get_token_int(line, "ble", dst->ble_rows);
    dst->bt_rows = get_token_int(line, "bt", dst->bt_rows);
    dst->bad_rows = get_token_int(line, "bad", dst->bad_rows);
    dst->checkbox_ptr = (void *)dst;
    return true;
}

static bool add_upload_state_entry(wardrive_upload_data_t *data, const char *line)
{
    if (!data || !line || data->history_count >= MAX_UPLOAD_HISTORY) return false;

    upload_history_t entry = {0};
    get_token_value(line, "service", entry.service, sizeof(entry.service));
    get_token_value(line, "filename", entry.filename, sizeof(entry.filename));
    if (entry.service[0] == '\0' || entry.filename[0] == '\0') return false;

    get_token_value(line, "hash", entry.hash, sizeof(entry.hash));
    if (!get_token_value(line, "status", entry.status, sizeof(entry.status))) {
        snprintf(entry.status, sizeof(entry.status), "%s", "unknown");
    }
    entry.size_bytes = get_token_long(line, "size", 0);
    entry.wifi_rows = get_token_int(line, "wifi", 0);
    entry.ble_rows = get_token_int(line, "ble", 0);
    entry.bt_rows = get_token_int(line, "bt", 0);
    entry.bad_rows = get_token_int(line, "bad", 0);
    data->history[data->history_count++] = entry;
    return true;
}

static void parse_list_dir_line(const char *line, wardrive_upload_data_t *data)
{
    if (!line || !data || !data->file_scan_running) return;
    if (is_skip_line(line)) return;

    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", line);
    char *save = NULL;
    char *tok = strtok_r(tmp, " \t", &save);
    char candidate[160] = {0};

    while (tok) {
        char cleaned[160];
        snprintf(cleaned, sizeof(cleaned), "%s", tok);
        trim_token(cleaned);
        if (has_upload_ext(cleaned)) {
            snprintf(candidate, sizeof(candidate), "%s", cleaned);
        }
        tok = strtok_r(NULL, " \t", &save);
    }

    if (candidate[0]) {
        if (add_file_entry(data, data->active_dir, candidate)) {
            data->needs_redraw = true;
        }
    }
}

static bool parse_upload_summary(const char *line, int *up, int *fail, int *skip, int *rate)
{
    int a = 0, b = 0, c = 0;
    int r = 0;
    if (sscanf(line, "Done: %d uploaded, %d skipped, %d failed, %d rate_limited", &a, &b, &c, &r) == 4) {
        *up = a; *skip = b; *fail = c; if (rate) *rate = r; return true;
    }
    if (sscanf(line, "up=%d fail=%d skip=%d", &a, &b, &c) == 3) {
        *up = a; *fail = b; *skip = c; return true;
    }
    if (sscanf(line, "uploaded=%d failed=%d skipped=%d", &a, &b, &c) == 3) {
        *up = a; *fail = b; *skip = c; return true;
    }
    if (sscanf(line, "Done: %d uploaded, %d duplicate, %d failed", &a, &b, &c) == 3) {
        *up = a; *skip = b; *fail = c; return true;
    }
    if (sscanf(line, "Done: %d uploaded, %d skipped, %d failed", &a, &b, &c) == 3) {
        *up = a; *skip = b; *fail = c; return true;
    }
    return false;
}

static bool parse_upload_batch_start(const char *line, int *total)
{
    if (!line || !total) return false;
    const char *p = strstr(line, "Uploading ");
    int n = 0;
    if (p && sscanf(p, "Uploading %d Wardrive file(s)", &n) == 1 && n > 0) {
        *total = n;
        return true;
    }
    return false;
}

static bool parse_upload_progress(const char *line, int *index, int *total)
{
    if (!line || !index || !total) return false;
    const char *p = strchr(line, '[');
    int a = 0;
    int b = 0;
    if (p && sscanf(p, "[%d/%d]", &a, &b) == 2 && a > 0 && b > 0) {
        *index = a;
        *total = b;
        return true;
    }
    return false;
}

static void parse_cleanup_line(wardrive_upload_data_t *data, const char *line)
{
    if (!data || !line) return;

    if (strstr(line, "[WARD_CLEANUP] filename=") != NULL) {
        char filename[96] = {0};
        char action[24] = {0};
        get_token_value(line, "filename", filename, sizeof(filename));
        get_token_value(line, "action", action, sizeof(action));
        snprintf(data->cleanup_last, sizeof(data->cleanup_last), "%.15s %.32s",
                 action[0] ? action : "matched",
                 filename[0] ? filename : "(file)");
        data->needs_redraw = true;
        return;
    }

    if (strstr(line, "[WARD_CLEANUP] SUMMARY") != NULL) {
        data->cleanup_scanned = get_token_int(line, "scanned", 0);
        data->cleanup_matched = get_token_int(line, "matched", 0);
        data->cleanup_moved = get_token_int(line, "moved", 0);
        data->cleanup_failed = get_token_int(line, "failed", 0);
        data->needs_redraw = true;
        return;
    }

    if (strstr(line, "[WARD_CLEANUP] END") != NULL) {
        data->cleanup_end_seen = true;
        data->cleanup_running = false;
        data->needs_redraw = true;
        return;
    }

    if (strstr(line, "Usage: wardrive_cleanup") != NULL ||
        strstr(line, "Unrecognized command") != NULL) {
        data->cleanup_unsupported = true;
        data->cleanup_running = false;
        data->needs_redraw = true;
    }
}

static void uart_line_callback(const char *line, void *user_data)
{
    wardrive_upload_data_t *data = (wardrive_upload_data_t *)user_data;
    if (!data || data->closing) return;

    if (data->cleanup_running) {
        parse_cleanup_line(data, line);
        if (strstr(line, "[WARD_CLEANUP]") != NULL ||
            strstr(line, "Usage: wardrive_cleanup") != NULL ||
            strstr(line, "Unrecognized command") != NULL) {
            return;
        }
    }

    if (data->loading_wardrive_files) {
        if (strstr(line, "[WARD_FILE]")) {
            if (strstr(line, "filename=") || strstr(line, "file=")) {
                if (upsert_ward_file(data, line)) {
                    data->needs_redraw = true;
                }
            }
            if (strstr(line, "END")) {
                data->wardrive_files_end_seen = true;
                data->loading_wardrive_files = false;
            }
            return;
        }
        if (strstr(line, "Unrecognized command")) {
            data->loading_wardrive_files = false;
            return;
        }
    }

    if (data->loading_upload_state) {
        if (strstr(line, "[UPLOAD_STATE]")) {
            if (strstr(line, "service=") && strstr(line, "filename=")) {
                if (add_upload_state_entry(data, line)) {
                    data->needs_redraw = true;
                }
            }
            if (strstr(line, "END")) {
                data->upload_state_end_seen = true;
                data->loading_upload_state = false;
            }
            return;
        }
        if (strstr(line, "Unrecognized command")) {
            data->loading_upload_state = false;
            return;
        }
    }

    if (data->file_scan_running) {
        parse_list_dir_line(line, data);
    }

    if (data->waiting_key_check) {
        if (contains_nocase(line, "not set") ||
            contains_nocase(line, "missing") ||
            contains_nocase(line, "NO WIGLE CREDENTIALS") ||
            contains_nocase(line, "NO WDGWARS CREDENTIALS")) {
            data->key_missing = true;
            data->key_checked = true;
            data->waiting_key_check = false;
            data->needs_redraw = true;
            return;
        }
        if (contains_nocase(line, "key") || contains_nocase(line, "credential")) {
            data->key_checked = true;
            data->waiting_key_check = false;
        }
    }

    if (!data->task_running) return;
    if (is_skip_line(line)) return;

    int progress_index = 0;
    int progress_total = 0;
    if (parse_upload_batch_start(line, &progress_total)) {
        data->upload_total = progress_total;
        data->upload_index = 0;
        data->needs_redraw = true;
    } else if (parse_upload_progress(line, &progress_index, &progress_total)) {
        data->upload_index = progress_index;
        data->upload_total = progress_total;
        data->needs_redraw = true;
    }

    if (contains_nocase(line, "NO WIGLE CREDENTIALS") ||
        contains_nocase(line, "NO WDGWARS CREDENTIALS") ||
        contains_nocase(line, "WIFI NOT CONNECTED") ||
        contains_nocase(line, "WDGWARS AUTH FAILED") ||
        contains_nocase(line, "AUTH FAILED") ||
        contains_nocase(line, "RATE LIMITED") ||
        contains_nocase(line, "Unrecognized command")) {
        snprintf(data->fatal_reason, sizeof(data->fatal_reason), "%.63s", line);
        if (contains_nocase(line, "RATE LIMITED")) data->upload_rate_limited++;
        data->upload_fatal = true;
        data->waiting_file_done = false;
        data->needs_redraw = true;
        return;
    }

    int up = 0, fail = 0, skip = 0, rate = 0;
    if (!data->cur_accounted && parse_upload_summary(line, &up, &fail, &skip, &rate)) {
        data->upload_uploaded += up;
        data->upload_failed += fail;
        data->upload_skipped += skip;
        data->upload_rate_limited += rate;
        data->cur_accounted = true;
        data->waiting_file_done = false;
        if (rate > 0) {
            snprintf(data->fatal_reason, sizeof(data->fatal_reason), "RATE LIMITED");
            data->upload_fatal = true;
        }
        data->needs_redraw = true;
        return;
    }

    if (contains_nocase(line, "-> OK")) data->cur_ok++;
    if (contains_nocase(line, "-> skipped") || contains_nocase(line, "duplicate")) data->cur_skip++;
    if (contains_nocase(line, "-> FAILED") || contains_nocase(line, "FAILED")) data->cur_fail++;

    if (contains_nocase(line, "Done:")) {
        if (!data->cur_accounted) {
            data->upload_uploaded += data->cur_ok;
            data->upload_skipped += data->cur_skip;
            data->upload_failed += data->cur_fail;
            data->cur_accounted = true;
        }
        data->waiting_file_done = false;
        data->needs_redraw = true;
    }
}

static void reset_upload_listing(wardrive_upload_data_t *data)
{
    data->file_count = 0;
    data->history_count = 0;
    data->selected_index = 0;
    data->history_index = 0;
    data->scroll_offset = 0;
    data->history_scroll = 0;
    data->selected_count = 0;
    memset(data->files, 0, sizeof(data->files));
    memset(data->history, 0, sizeof(data->history));
}

static void refresh_upload_status(wardrive_upload_data_t *data)
{
    reset_upload_listing(data);
    data->loading_wardrive_files = true;
    data->wardrive_files_end_seen = false;
    snprintf(data->status_line, sizeof(data->status_line), "Loading wardrive_files...");
    data->needs_redraw = true;
    uart_flush_rx();
    uart_send_command("wardrive_files");
    for (int t = 0; t < 1200 && !data->file_scan_stop && data->loading_wardrive_files; t++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    data->loading_wardrive_files = false;
    if (!data->wardrive_files_end_seen && data->file_count > 0) {
        snprintf(data->status_line, sizeof(data->status_line), "wardrive_files timeout");
        data->needs_redraw = true;
        return;
    }

    data->loading_upload_state = true;
    data->upload_state_end_seen = false;
    snprintf(data->status_line, sizeof(data->status_line), "Loading upload_state...");
    data->needs_redraw = true;
    uart_flush_rx();
    uart_send_command("upload_state");
    for (int t = 0; t < 100 && !data->file_scan_stop && data->loading_upload_state; t++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    data->loading_upload_state = false;
}

static void fallback_scan_dirs(wardrive_upload_data_t *data)
{
    for (int i = 0; i < MAX_SCAN_DIRS && !data->file_scan_stop; i++) {
        snprintf(data->active_dir, sizeof(data->active_dir), "%s", k_scan_dirs[i]);
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "list_dir %s", data->active_dir);
        uart_flush_rx();
        uart_send_command(cmd);
        for (int t = 0; t < 12 && !data->file_scan_stop; t++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

static void file_scan_task(void *arg)
{
    wardrive_upload_data_t *data = (wardrive_upload_data_t *)arg;
    if (!data) {
        vTaskDelete(NULL);
        return;
    }

    data->file_scan_running = true;
    data->state = STATE_LOADING_FILES;
    snprintf(data->status_line, sizeof(data->status_line), "Loading files...");
    data->needs_redraw = true;

    refresh_upload_status(data);
    if (data->file_count == 0 && data->history_count == 0 && !data->file_scan_stop) {
        snprintf(data->status_line, sizeof(data->status_line), "Fallback list_dir...");
        data->needs_redraw = true;
        fallback_scan_dirs(data);
    }

    data->file_scan_running = false;
    data->state = STATE_PICK_FILES;
    snprintf(data->status_line, sizeof(data->status_line), "Found %d pending, %d history",
             visible_file_count(data), data->history_count);
    data->needs_redraw = true;
    data->file_scan_task = NULL;
    vTaskDelete(NULL);
}

static void reset_upload_counters(wardrive_upload_data_t *data)
{
    data->upload_uploaded = 0;
    data->upload_failed = 0;
    data->upload_skipped = 0;
    data->upload_rate_limited = 0;
}

static void upload_one_command(wardrive_upload_data_t *data, const char *cmd)
{
    data->cur_ok = 0;
    data->cur_skip = 0;
    data->cur_fail = 0;
    data->cur_accounted = false;
    data->waiting_file_done = true;
    data->upload_fatal = false;
    data->fatal_reason[0] = '\0';

    uart_flush_rx();
    uart_send_command(cmd);

    int timeout_ticks = (data->upload_mode == UPLOAD_MODE_SELECTED) ? 600 : 3600;
    for (int t = 0; t < timeout_ticks && data->waiting_file_done && !data->cancel_requested && !data->upload_fatal; t++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (data->waiting_file_done && !data->cur_accounted && !data->upload_fatal) {
        data->upload_failed++;
        data->waiting_file_done = false;
    }
}

static void upload_task(void *arg)
{
    wardrive_upload_data_t *data = (wardrive_upload_data_t *)arg;
    if (!data) {
        vTaskDelete(NULL);
        return;
    }

    if (uart_is_wardrive_active()) {
        data->state = STATE_ERROR;
        snprintf(data->status_line, sizeof(data->status_line), "Stop wardrive first");
        data->task_running = false;
        data->upload_task = NULL;
        data->needs_redraw = true;
        vTaskDelete(NULL);
        return;
    }

    const char *key_cmd = (data->target == WARDRIVE_UPLOAD_TARGET_WIGLE) ? "wigle_key read" : "wdgwars_key read";
    const char *upload_prefix = (data->target == WARDRIVE_UPLOAD_TARGET_WIGLE) ? "wigle_upload" : "wdgwars_upload";

    data->waiting_key_check = true;
    data->key_checked = false;
    data->key_missing = false;
    uart_flush_rx();
    uart_send_command(key_cmd);
    for (int i = 0; i < 40 && !data->key_checked && !data->cancel_requested; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    data->waiting_key_check = false;
    if (data->key_missing) {
        data->state = STATE_ERROR;
        snprintf(data->status_line, sizeof(data->status_line), "Credentials missing");
        data->task_running = false;
        data->upload_task = NULL;
        data->needs_redraw = true;
        vTaskDelete(NULL);
        return;
    }

    reset_upload_counters(data);

    if (data->upload_mode == UPLOAD_MODE_PENDING || data->upload_mode == UPLOAD_MODE_ALL) {
        data->upload_total = 1;
        data->upload_index = 0;
        data->needs_redraw = true;

        char cmd[240];
        if (data->upload_mode == UPLOAD_MODE_ALL) {
            snprintf(cmd, sizeof(cmd), "%s all", upload_prefix);
        } else {
            snprintf(cmd, sizeof(cmd), "%s", upload_prefix);
        }
        upload_one_command(data, cmd);
    } else {
        int processed = 0;
        for (int v = 0; v < visible_file_count(data) && !data->cancel_requested; v++) {
            int i = visible_file_index(data, v);
            if (i < 0 || !data->files[i].selected) continue;
            processed++;
            data->upload_index = processed;
            data->needs_redraw = true;

            char cmd[240];
            snprintf(cmd, sizeof(cmd), "%s \"%s\"", upload_prefix, data->files[i].name);
            upload_one_command(data, cmd);

            if (data->upload_fatal) break;
        }
    }

    if (data->upload_fatal) {
        data->state = STATE_ERROR;
        snprintf(data->status_line, sizeof(data->status_line), "%.63s", data->fatal_reason);
        data->task_running = false;
        data->upload_task = NULL;
        data->needs_redraw = true;
        vTaskDelete(NULL);
        return;
    }

    data->file_scan_stop = false;
    refresh_upload_status(data);
    if (data->cancel_requested) {
        data->state = STATE_ERROR;
        snprintf(data->status_line, sizeof(data->status_line), "Upload cancelled");
    } else {
        data->state = STATE_DONE;
        snprintf(data->status_line, sizeof(data->status_line), "Upload complete");
    }
    data->task_running = false;
    data->upload_task = NULL;
    data->needs_redraw = true;
    vTaskDelete(NULL);
}

static void reset_cleanup_state(wardrive_upload_data_t *data, bool move)
{
    data->cleanup_move = move;
    data->cleanup_end_seen = false;
    data->cleanup_unsupported = false;
    data->cleanup_scanned = 0;
    data->cleanup_matched = 0;
    data->cleanup_moved = 0;
    data->cleanup_failed = 0;
    data->cleanup_last[0] = '\0';
}

static void cleanup_task(void *arg)
{
    wardrive_upload_data_t *data = (wardrive_upload_data_t *)arg;
    if (!data) {
        vTaskDelete(NULL);
        return;
    }

    char cmd[96];
    if (data->cleanup_move) {
        snprintf(cmd, sizeof(cmd), "wardrive_cleanup all done move");
        snprintf(data->status_line, sizeof(data->status_line), "Moving archived files...");
    } else {
        snprintf(cmd, sizeof(cmd), "wardrive_cleanup all done");
        snprintf(data->status_line, sizeof(data->status_line), "Archive dry-run...");
    }

    data->state = STATE_ARCHIVING;
    data->cleanup_running = true;
    data->needs_redraw = true;
    uart_flush_rx();
    uart_send_command(cmd);

    for (int t = 0; t < 1200 && !data->closing && data->cleanup_running; t++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    data->cleanup_running = false;

    if (data->cleanup_unsupported) {
        data->state = STATE_ERROR;
        snprintf(data->status_line, sizeof(data->status_line), "cleanup unsupported");
    } else if (!data->cleanup_end_seen) {
        data->state = STATE_ERROR;
        snprintf(data->status_line, sizeof(data->status_line), "cleanup timeout");
    } else if (data->cleanup_move) {
        snprintf(data->status_line, sizeof(data->status_line), "Moved:%d Fail:%d",
                 data->cleanup_moved, data->cleanup_failed);
        data->cleanup_move_ready = false;
        data->file_scan_stop = false;
        refresh_upload_status(data);
        data->state = STATE_DONE;
        snprintf(data->status_line, sizeof(data->status_line), "Moved:%d Fail:%d",
                 data->cleanup_moved, data->cleanup_failed);
    } else {
        data->cleanup_move_ready = data->cleanup_matched > 0 && data->cleanup_failed == 0;
        data->state = STATE_DONE;
        if (data->cleanup_matched > 0) {
            snprintf(data->status_line, sizeof(data->status_line), "Matched:%d M:Move",
                     data->cleanup_matched);
        } else {
            snprintf(data->status_line, sizeof(data->status_line), "Nothing to archive");
        }
    }

    data->cleanup_task = NULL;
    data->needs_redraw = true;
    vTaskDelete(NULL);
}

static void start_cleanup(wardrive_upload_data_t *data, bool move)
{
    if (!data || data->task_running || data->cleanup_task || data->file_scan_task) return;
    if (move && !data->cleanup_move_ready) return;

    reset_cleanup_state(data, move);
    if (!move) {
        data->cleanup_move_ready = false;
    }
    if (xTaskCreate(cleanup_task, "wd_cleanup", 6144, data, 7, &data->cleanup_task) != pdPASS) {
        data->state = STATE_ERROR;
        snprintf(data->status_line, sizeof(data->status_line), "cleanup task failed");
        data->cleanup_task = NULL;
        data->needs_redraw = true;
    }
}

static void draw_files_list(wardrive_upload_data_t *data)
{
    int total = visible_file_count(data);
    if (data->selected_index >= total) data->selected_index = total > 0 ? total - 1 : 0;
    if (data->selected_index < data->scroll_offset) data->scroll_offset = data->selected_index;
    if (data->selected_index >= data->scroll_offset + VISIBLE_ITEMS) {
        data->scroll_offset = data->selected_index - VISIBLE_ITEMS + 1;
    }

    for (int i = 0; i < VISIBLE_ITEMS; i++) {
        int visible_idx = data->scroll_offset + i;
        int idx = visible_file_index(data, visible_idx);
        if (idx >= 0) {
            char label[32];
            const char *provider_status = (data->target == WARDRIVE_UPLOAD_TARGET_WIGLE) ?
                data->files[idx].wigle_status : data->files[idx].wdgwars_status;
            snprintf(label, sizeof(label), "%.14s %.8s", data->files[idx].name, provider_status);
            ui_draw_menu_item(2 + i, label, visible_idx == data->selected_index, true, data->files[idx].selected);
        } else {
            display_fill_rect(0, (2 + i) * 16, DISPLAY_WIDTH, 16, UI_COLOR_BG);
        }
    }

    if (data->scroll_offset > 0) ui_print(UI_COLS - 2, 2, "^", UI_COLOR_DIMMED);
    if (data->scroll_offset + VISIBLE_ITEMS < total) ui_print(UI_COLS - 2, 5, "v", UI_COLOR_DIMMED);
}

static void draw_history_list(wardrive_upload_data_t *data)
{
    if (data->history_index >= data->history_count) {
        data->history_index = data->history_count > 0 ? data->history_count - 1 : 0;
    }
    if (data->history_index < data->history_scroll) data->history_scroll = data->history_index;
    if (data->history_index >= data->history_scroll + VISIBLE_ITEMS) {
        data->history_scroll = data->history_index - VISIBLE_ITEMS + 1;
    }

    for (int i = 0; i < VISIBLE_ITEMS; i++) {
        int idx = data->history_scroll + i;
        int row = 2 + i;
        if (idx < data->history_count) {
            upload_history_t *h = &data->history[idx];
            char label[32];
            const char *svc = strcasecmp(h->service, "wdgwars") == 0 ? "WDG" : "WIG";
            snprintf(label, sizeof(label), "%s %.12s %.7s", svc, h->filename, h->status);
            ui_draw_menu_item(row, label, idx == data->history_index, false, false);
        } else {
            display_fill_rect(0, row * 16, DISPLAY_WIDTH, 16, UI_COLOR_BG);
        }
    }

    if (data->history_scroll > 0) ui_print(UI_COLS - 2, 2, "^", UI_COLOR_DIMMED);
    if (data->history_scroll + VISIBLE_ITEMS < data->history_count) ui_print(UI_COLS - 2, 5, "v", UI_COLOR_DIMMED);
}

static void draw_top_line(wardrive_upload_data_t *data)
{
    char top[40];
    snprintf(top, sizeof(top), "%s %s P:%d D:%d Sel:%d",
             data->show_history ? "Hist" : "Up",
             target_short_name(data->target),
             target_pending_count(data),
             target_done_count(data),
             data->selected_count);
    display_fill_rect(0, 1 * 16, DISPLAY_WIDTH, 16, UI_COLOR_BG);
    ui_print(0, 1, top, UI_COLOR_TEXT);
}

static void draw_send_line(wardrive_upload_data_t *data)
{
    bool send_enabled = (data->selected_count > 0 && !data->task_running);
    char line[40];
    snprintf(line, sizeof(line), "S:%s P:AllPend A:All C:Arc",
             send_enabled ? "Sel" : "--");
    display_fill_rect(0, 6 * 16, DISPLAY_WIDTH, 16, UI_COLOR_BG);
    ui_print(0, 6, line, UI_COLOR_TEXT);
}

static void draw_pick_files(wardrive_upload_data_t *data)
{
    draw_top_line(data);
    if (data->show_history) {
        draw_history_list(data);
    } else {
        draw_files_list(data);
    }
    draw_send_line(data);
    ui_draw_status(data->show_history ? "UP/DN H C:Arc ESC" : "UP/DN SPC S/P/A C H L/R");
}

static void draw_screen(screen_t *self)
{
    wardrive_upload_data_t *data = (wardrive_upload_data_t *)self->user_data;

    ui_clear();
    ui_draw_title("Wardrive Upload");
    draw_top_line(data);

    if (data->state == STATE_LOADING_FILES) {
        ui_print_center(3, "Loading files...", UI_COLOR_DIMMED);
        ui_print_center(4, data->status_line, UI_COLOR_DIMMED);
        ui_draw_status("ESC:Back");
        return;
    }

    if (data->state == STATE_WAIT_WIFI) {
        ui_print_center(3, "Connect to WiFi...", UI_COLOR_TEXT);
        ui_print_center(4, "Returning to upload", UI_COLOR_DIMMED);
        ui_draw_status("ESC:Cancel");
        return;
    }

    if (data->state == STATE_UPLOADING) {
        char p1[40];
        const char *mode = data->upload_mode == UPLOAD_MODE_PENDING ? "Pending" :
                           (data->upload_mode == UPLOAD_MODE_ALL ? "All" : "Selected");
        snprintf(p1, sizeof(p1), "%s %d/%d", mode, data->upload_index, data->upload_total);
        ui_print_center(3, p1, UI_COLOR_TEXT);
        char p2[40];
        snprintf(p2, sizeof(p2), "up:%d skip:%d fail:%d", data->upload_uploaded,
                 data->upload_skipped, data->upload_failed);
        ui_print_center(4, p2, UI_COLOR_DIMMED);
        ui_draw_status("ESC:Cancel");
        return;
    }

    if (data->state == STATE_ARCHIVING) {
        ui_print_center(2, data->status_line, UI_COLOR_TEXT);
        char p2[40];
        snprintf(p2, sizeof(p2), "scan:%d match:%d move:%d fail:%d",
                 data->cleanup_scanned, data->cleanup_matched,
                 data->cleanup_moved, data->cleanup_failed);
        ui_print_center(4, p2, UI_COLOR_DIMMED);
        if (data->cleanup_last[0]) {
            ui_print_center(5, data->cleanup_last, UI_COLOR_DIMMED);
        }
        ui_draw_status("Working...");
        return;
    }

    if (data->state == STATE_DONE || data->state == STATE_ERROR) {
        ui_print_center(2, data->status_line, data->state == STATE_DONE ? UI_COLOR_HIGHLIGHT : UI_COLOR_TEXT);
        char p2[40];
        if (data->cleanup_move_ready || data->cleanup_matched > 0 || data->cleanup_moved > 0) {
            snprintf(p2, sizeof(p2), "scan:%d match:%d moved:%d",
                     data->cleanup_scanned, data->cleanup_matched, data->cleanup_moved);
        } else {
            snprintf(p2, sizeof(p2), "up:%d skip:%d fail:%d", data->upload_uploaded,
                     data->upload_skipped, data->upload_failed);
        }
        ui_print_center(4, p2, UI_COLOR_DIMMED);
        ui_draw_status(data->cleanup_move_ready ? "M:Move ESC:Back" : "ESC:Back");
        return;
    }

    draw_pick_files(data);
}

static void recalc_selected(wardrive_upload_data_t *data)
{
    int count = 0;
    for (int i = 0; i < data->file_count; i++) {
        if (data->files[i].selected && file_needs_target_upload(&data->files[i], data->target)) count++;
    }
    data->selected_count = count;
}

static void start_upload(wardrive_upload_data_t *data, upload_mode_t mode)
{
    if (data->task_running) return;
    if (data->cleanup_task) return;
    if (mode == UPLOAD_MODE_SELECTED && data->selected_count <= 0) return;
    data->upload_mode = mode;
    if (uart_is_wardrive_active()) {
        data->state = STATE_ERROR;
        snprintf(data->status_line, sizeof(data->status_line), "Wardrive is active");
        data->needs_redraw = true;
        return;
    }
    if (!uart_is_wifi_connected()) {
        data->state = STATE_WAIT_WIFI;
        data->needs_redraw = true;
        return;
    }

    data->upload_total = (mode == UPLOAD_MODE_SELECTED) ? data->selected_count : 1;
    data->upload_index = (mode == UPLOAD_MODE_SELECTED) ? 0 : 1;
    data->cleanup_move_ready = false;
    data->cleanup_matched = 0;
    data->cleanup_moved = 0;
    data->cleanup_failed = 0;
    data->cleanup_last[0] = '\0';
    data->cancel_requested = false;
    data->task_running = true;
    data->state = STATE_UPLOADING;
    data->needs_redraw = true;
    xTaskCreate(upload_task, "wardrive_upload", 6144, data, 8, &data->upload_task);
}

static void on_tick(screen_t *self)
{
    wardrive_upload_data_t *data = (wardrive_upload_data_t *)self->user_data;
    if (!data) return;

    if (data->state == STATE_WAIT_WIFI && uart_is_wifi_connected()) {
        start_upload(data, data->upload_mode);
    }

    if (data->needs_redraw) {
        data->needs_redraw = false;
        if (data->state == STATE_PICK_FILES) {
            draw_pick_files(data);
        } else {
            draw_screen(self);
        }
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    wardrive_upload_data_t *data = (wardrive_upload_data_t *)self->user_data;
    if (!data) return;

    if (data->state == STATE_WAIT_WIFI) {
        if (key == KEY_ESC || key == KEY_BACKSPACE) {
            data->state = STATE_PICK_FILES;
            data->needs_redraw = true;
        }
        return;
    }

    if (data->state == STATE_UPLOADING) {
        if (key == KEY_ESC || key == KEY_BACKSPACE) {
            data->cancel_requested = true;
            snprintf(data->status_line, sizeof(data->status_line), "Cancelling...");
            data->needs_redraw = true;
        }
        return;
    }

    if (data->state == STATE_ARCHIVING) {
        return;
    }

    if (data->state == STATE_DONE || data->state == STATE_ERROR) {
        if (data->state == STATE_DONE && key == KEY_M && data->cleanup_move_ready) {
            start_cleanup(data, true);
            return;
        }
        if (key == KEY_ESC || key == KEY_BACKSPACE || key == KEY_ENTER || key == KEY_SPACE) {
            screen_manager_pop();
        }
        return;
    }

    switch (key) {
        case KEY_UP:
            if (data->show_history) {
                if (data->history_count > 0) {
                    data->history_index = data->history_index > 0 ? data->history_index - 1 : data->history_count - 1;
                    data->needs_redraw = true;
                }
            } else if (visible_file_count(data) > 0) {
                int total = visible_file_count(data);
                if (data->selected_index > 0) {
                    data->selected_index--;
                } else {
                    data->selected_index = total - 1;
                }
                if (data->selected_index < data->scroll_offset) {
                    data->scroll_offset = data->selected_index;
                }
                if (data->selected_index >= data->scroll_offset + VISIBLE_ITEMS) {
                    data->scroll_offset = data->selected_index - VISIBLE_ITEMS + 1;
                }
                data->needs_redraw = true;
            }
            break;
        case KEY_DOWN:
            if (data->show_history) {
                if (data->history_count > 0) {
                    data->history_index = data->history_index < data->history_count - 1 ? data->history_index + 1 : 0;
                    data->needs_redraw = true;
                }
            } else if (visible_file_count(data) > 0) {
                int total = visible_file_count(data);
                if (data->selected_index < total - 1) {
                    data->selected_index++;
                } else {
                    data->selected_index = 0;
                }
                if (data->selected_index < data->scroll_offset) {
                    data->scroll_offset = data->selected_index;
                }
                if (data->selected_index >= data->scroll_offset + VISIBLE_ITEMS) {
                    data->scroll_offset = data->selected_index - VISIBLE_ITEMS + 1;
                }
                data->needs_redraw = true;
            }
            break;
        case KEY_LEFT:
        case KEY_RIGHT:
            data->target = (data->target == WARDRIVE_UPLOAD_TARGET_WIGLE) ?
                WARDRIVE_UPLOAD_TARGET_WDGWARS : WARDRIVE_UPLOAD_TARGET_WIGLE;
            recalc_selected(data);
            data->selected_index = 0;
            data->scroll_offset = 0;
            data->needs_redraw = true;
            break;
        case KEY_H:
            data->show_history = !data->show_history;
            data->needs_redraw = true;
            break;
        case KEY_C:
            if (!data->task_running && !data->cleanup_task && !data->file_scan_task) {
                start_cleanup(data, false);
            }
            break;
        case KEY_SPACE:
        case KEY_ENTER:
            if (!data->show_history && visible_file_count(data) > 0) {
                int idx = visible_file_index(data, data->selected_index);
                if (idx < 0) break;
                upload_file_t *f = &data->files[idx];
                f->selected = !f->selected;
                recalc_selected(data);
                data->needs_redraw = true;
            }
            break;
        case KEY_S:
            if (!data->show_history && data->selected_count > 0 && !data->task_running) {
                start_upload(data, UPLOAD_MODE_SELECTED);
                if (data->state == STATE_WAIT_WIFI) {
                    wifi_connect_params_t *w = malloc(sizeof(wifi_connect_params_t));
                    if (w) {
                        w->auto_close_on_success = true;
                        screen_manager_push(wifi_connect_screen_create, w);
                    } else {
                        screen_manager_push(wifi_connect_screen_create, NULL);
                    }
                }
            }
            break;
        case KEY_P:
            if (!data->show_history && !data->task_running) {
                start_upload(data, UPLOAD_MODE_PENDING);
                if (data->state == STATE_WAIT_WIFI) {
                    wifi_connect_params_t *w = malloc(sizeof(wifi_connect_params_t));
                    if (w) {
                        w->auto_close_on_success = true;
                        screen_manager_push(wifi_connect_screen_create, w);
                    } else {
                        screen_manager_push(wifi_connect_screen_create, NULL);
                    }
                }
            }
            break;
        case KEY_A:
            if (!data->show_history && !data->task_running) {
                start_upload(data, UPLOAD_MODE_ALL);
                if (data->state == STATE_WAIT_WIFI) {
                    wifi_connect_params_t *w = malloc(sizeof(wifi_connect_params_t));
                    if (w) {
                        w->auto_close_on_success = true;
                        screen_manager_push(wifi_connect_screen_create, w);
                    } else {
                        screen_manager_push(wifi_connect_screen_create, NULL);
                    }
                }
            }
            break;
        case KEY_ESC:
        case KEY_BACKSPACE:
            screen_manager_pop();
            break;
        default:
            break;
    }
}

static void on_resume(screen_t *self)
{
    wardrive_upload_data_t *data = (wardrive_upload_data_t *)self->user_data;
    if (!data) return;
    uart_register_line_callback(uart_line_callback, data);
    if (data->state == STATE_WAIT_WIFI) {
        if (uart_is_wifi_connected()) {
            start_upload(data, data->upload_mode);
        } else {
            data->state = STATE_PICK_FILES;
        }
    }
    data->needs_redraw = true;
}

static void on_destroy(screen_t *self)
{
    wardrive_upload_data_t *data = (wardrive_upload_data_t *)self->user_data;
    uart_clear_line_callback();

    if (data) {
        data->closing = true;
        data->file_scan_stop = true;
        data->cancel_requested = true;
        data->cleanup_running = false;
        for (int i = 0; i < 25 && (data->upload_task || data->file_scan_task || data->cleanup_task); i++) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (data->upload_task) {
            vTaskDelete(data->upload_task);
            data->upload_task = NULL;
        }
        if (data->file_scan_task) {
            vTaskDelete(data->file_scan_task);
            data->file_scan_task = NULL;
        }
        if (data->cleanup_task) {
            vTaskDelete(data->cleanup_task);
            data->cleanup_task = NULL;
        }
        free(data);
    }
}

screen_t* wardrive_upload_screen_create(void *params)
{
    (void)params;

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    wardrive_upload_data_t *data = calloc(1, sizeof(wardrive_upload_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }

    data->state = STATE_LOADING_FILES;
    data->target = WARDRIVE_UPLOAD_TARGET_WIGLE;
    wardrive_upload_params_t *in = (wardrive_upload_params_t *)params;
    if (in) {
        data->target = in->target;
        free(in);
    }
    snprintf(data->status_line, sizeof(data->status_line), "Loading files...");
    data->needs_redraw = true;

    screen->user_data = data;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;
    screen->on_key = on_key;
    screen->on_resume = on_resume;
    screen->on_destroy = on_destroy;

    uart_register_line_callback(uart_line_callback, data);
    xTaskCreate(file_scan_task, "wardrive_files", 4096, data, 6, &data->file_scan_task);

    draw_screen(screen);
    ESP_LOGI(TAG, "Wardrive upload screen created");
    return screen;
}
