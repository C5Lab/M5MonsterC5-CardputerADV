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
#define MAX_SCAN_DIRS 4
#define VISIBLE_ITEMS 4

typedef enum {
    STATE_LOADING_FILES = 0,
    STATE_PICK_FILES,
    STATE_WAIT_WIFI,
    STATE_UPLOADING,
    STATE_DONE,
    STATE_ERROR
} upload_state_t;

typedef struct {
    char path[160];
    char name[96];
    bool selected;
    void *checkbox_ptr;
} upload_file_t;

typedef struct {
    upload_state_t state;
    wardrive_upload_target_t target;
    upload_file_t files[MAX_UPLOAD_FILES];
    int file_count;
    int selected_index;
    int scroll_offset;
    int selected_count;

    volatile bool needs_redraw;
    volatile bool closing;

    volatile bool file_scan_running;
    volatile bool file_scan_stop;
    TaskHandle_t file_scan_task;
    char active_dir[48];
    char status_line[64];

    volatile bool task_running;
    volatile bool cancel_requested;
    TaskHandle_t upload_task;
    int upload_uploaded;
    int upload_failed;
    int upload_skipped;
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
    dst->selected = false;
    dst->checkbox_ptr = (void *)dst;
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

static bool parse_upload_summary(const char *line, int *up, int *fail, int *skip)
{
    int a = 0, b = 0, c = 0;
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

static void uart_line_callback(const char *line, void *user_data)
{
    wardrive_upload_data_t *data = (wardrive_upload_data_t *)user_data;
    if (!data || data->closing) return;

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

    if (contains_nocase(line, "NO WIGLE CREDENTIALS") ||
        contains_nocase(line, "NO WDGWARS CREDENTIALS") ||
        contains_nocase(line, "WIFI NOT CONNECTED") ||
        contains_nocase(line, "WDGWARS AUTH FAILED") ||
        contains_nocase(line, "Unrecognized command")) {
        snprintf(data->fatal_reason, sizeof(data->fatal_reason), "%.63s", line);
        data->upload_fatal = true;
        data->waiting_file_done = false;
        data->needs_redraw = true;
        return;
    }

    int up = 0, fail = 0, skip = 0;
    if (!data->cur_accounted && parse_upload_summary(line, &up, &fail, &skip)) {
        data->upload_uploaded += up;
        data->upload_failed += fail;
        data->upload_skipped += skip;
        data->cur_accounted = true;
        data->waiting_file_done = false;
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

    data->file_scan_running = false;
    data->state = STATE_PICK_FILES;
    snprintf(data->status_line, sizeof(data->status_line), "Found %d files", data->file_count);
    data->needs_redraw = true;
    data->file_scan_task = NULL;
    vTaskDelete(NULL);
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

    data->upload_uploaded = 0;
    data->upload_failed = 0;
    data->upload_skipped = 0;

    int processed = 0;
    for (int i = 0; i < data->file_count && !data->cancel_requested; i++) {
        if (!data->files[i].selected) continue;
        processed++;
        data->upload_index = processed;
        data->needs_redraw = true;

        data->cur_ok = 0;
        data->cur_skip = 0;
        data->cur_fail = 0;
        data->cur_accounted = false;
        data->waiting_file_done = true;
        data->upload_fatal = false;
        data->fatal_reason[0] = '\0';

        char cmd[240];
        snprintf(cmd, sizeof(cmd), "%s \"%s\"", upload_prefix, data->files[i].name);
        uart_flush_rx();
        uart_send_command(cmd);

        for (int t = 0; t < 250 && data->waiting_file_done && !data->cancel_requested && !data->upload_fatal; t++) {
            vTaskDelay(pdMS_TO_TICKS(100));
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

        if (data->waiting_file_done && !data->cur_accounted) {
            data->upload_failed++;
            data->waiting_file_done = false;
        }
    }

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

static void draw_files_list(wardrive_upload_data_t *data)
{
    for (int i = 0; i < VISIBLE_ITEMS; i++) {
        int idx = data->scroll_offset + i;
        if (idx < data->file_count) {
            char label[32];
            snprintf(label, sizeof(label), "%.23s", data->files[idx].name);
            ui_draw_menu_item(2 + i, label, idx == data->selected_index, true, data->files[idx].selected);
        } else {
            display_fill_rect(0, (2 + i) * 16, DISPLAY_WIDTH, 16, UI_COLOR_BG);
        }
    }
}

static void draw_screen(screen_t *self)
{
    wardrive_upload_data_t *data = (wardrive_upload_data_t *)self->user_data;

    ui_clear();
    ui_draw_title("Wardrive Upload");

    const char *target = (data->target == WARDRIVE_UPLOAD_TARGET_WIGLE) ? "WiGLE" : "WDGWars";
    char top[40];
    snprintf(top, sizeof(top), "Target:%s Sel:%d/%d", target, data->selected_count, data->file_count);
    ui_print(0, 1, top, UI_COLOR_TEXT);

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
        snprintf(p1, sizeof(p1), "Uploading %d/%d", data->upload_index, data->upload_total);
        ui_print_center(3, p1, UI_COLOR_TEXT);
        char p2[40];
        snprintf(p2, sizeof(p2), "up:%d skip:%d fail:%d", data->upload_uploaded, data->upload_skipped, data->upload_failed);
        ui_print_center(4, p2, UI_COLOR_DIMMED);
        ui_draw_status("ESC:Cancel");
        return;
    }

    if (data->state == STATE_DONE || data->state == STATE_ERROR) {
        ui_print_center(2, data->status_line, data->state == STATE_DONE ? UI_COLOR_HIGHLIGHT : UI_COLOR_TEXT);
        char p2[40];
        snprintf(p2, sizeof(p2), "up:%d skip:%d fail:%d", data->upload_uploaded, data->upload_skipped, data->upload_failed);
        ui_print_center(4, p2, UI_COLOR_DIMMED);
        ui_draw_status("ESC:Back");
        return;
    }

    draw_files_list(data);
    bool send_enabled = (data->selected_count > 0 && !data->task_running);
    ui_print(0, 6, send_enabled ? "Send: READY" : "Send: disabled", send_enabled ? UI_COLOR_HIGHLIGHT : UI_COLOR_DIMMED);
    ui_draw_status("UP/DOWN SPACE S:Send L/R:Target ESC:Back");
}

static void recalc_selected(wardrive_upload_data_t *data)
{
    int count = 0;
    for (int i = 0; i < data->file_count; i++) {
        if (data->files[i].selected) count++;
    }
    data->selected_count = count;
}

static void start_upload(wardrive_upload_data_t *data)
{
    if (data->task_running || data->selected_count <= 0) return;
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

    data->upload_total = data->selected_count;
    data->upload_index = 0;
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
        start_upload(data);
    }

    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
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

    if (data->state == STATE_DONE || data->state == STATE_ERROR) {
        if (key == KEY_ESC || key == KEY_BACKSPACE || key == KEY_ENTER || key == KEY_SPACE) {
            screen_manager_pop();
        }
        return;
    }

    switch (key) {
        case KEY_UP:
            if (data->file_count > 0) {
                if (data->selected_index > 0) {
                    data->selected_index--;
                } else {
                    data->selected_index = data->file_count - 1;
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
            if (data->file_count > 0) {
                if (data->selected_index < data->file_count - 1) {
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
            data->needs_redraw = true;
            break;
        case KEY_SPACE:
        case KEY_ENTER:
            if (data->file_count > 0 && data->selected_index < data->file_count) {
                upload_file_t *f = &data->files[data->selected_index];
                f->selected = !f->selected;
                recalc_selected(data);
                data->needs_redraw = true;
            }
            break;
        case KEY_S:
            if (data->selected_count > 0 && !data->task_running) {
                start_upload(data);
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
    if (data->state == STATE_WAIT_WIFI && !uart_is_wifi_connected()) {
        data->state = STATE_PICK_FILES;
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
        for (int i = 0; i < 25 && (data->upload_task || data->file_scan_task); i++) {
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
