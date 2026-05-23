/**
 * @file subghz_scanner_settings_screen.c
 * @brief Quick Scan ("scanner") tunables — RSSI floor, dwell, min edges, fast.
 *
 * Persisted via subghz_rf_settings (NVS namespace "subghz_rf"). On every
 * change we write through to NVS so the Scanner screen picks it up next
 * time it builds the scanner command via subghz_rf_build_scanner_cmd().
 *
 * Rows:
 *   RSSI floor     -85 .. -60 dBm   (sc_rssi)
 *   Dwell           40 / 80 / 120 / 200 ms (sc_dwell)
 *   Min edges       2 / 4 / 8 / 12  (sc_edges)
 *   Fast scan       ON/OFF          (sc_fast)
 *
 * Keys:
 *   UP/DOWN     - move selection
 *   LEFT/RIGHT  - cycle the value on the selected row
 *   ENTER       - same as RIGHT (one-handed)
 *   ESC         - back to Quick Scan (which restarts the scanner)
 */

#include "subghz_scanner_settings_screen.h"
#include "subghz_rf_settings.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "SUBGHZ_SCAN_SET";

#define RSSI_OPT_COUNT  6   /* { -85, -80, -75, -70, -65, -60 } */
#define DWELL_OPT_COUNT 4   /* { 40, 80, 120, 200 } */
#define EDGES_OPT_COUNT 4   /* { 2, 4, 8, 12 } */

typedef enum {
    ROW_RSSI = 0,
    ROW_DWELL,
    ROW_EDGES,
    ROW_FAST,
    ROW_COUNT,
} scanner_set_row_t;

typedef struct {
    subghz_rf_settings_t cfg;
    int selected;
} scanner_set_data_t;

/* Row layout: 0=title, 1=blank, 2..5 = rows, 7=status. */
#define UI_ROW_OFFSET 2

static void format_setting_line(char *buf, size_t buf_size,
                                const char *label, const char *value)
{
    int label_len = (int)strlen(label);
    int value_len = (int)strlen(value);
    int total_cols = 29;
    int padding = total_cols - label_len - value_len;
    if (padding < 1) padding = 1;

    int pos = 0;
    if (buf_size > 0) {
        snprintf(buf, buf_size, "%s", label);
        pos = label_len;
    }
    for (int i = 0; i < padding && pos < (int)buf_size - 1; i++) {
        buf[pos++] = ' ';
    }
    snprintf(buf + pos, buf_size - pos, "%s", value);
}

static void row_label_value(const scanner_set_data_t *data, scanner_set_row_t row,
                            const char **label_out, char *value_buf, size_t value_buf_size)
{
    switch (row) {
        case ROW_RSSI:
            *label_out = "RSSI floor";
            snprintf(value_buf, value_buf_size, "%d dBm",
                     (int)data->cfg.scanner_rssi_dbm);
            break;
        case ROW_DWELL:
            *label_out = "Dwell";
            snprintf(value_buf, value_buf_size, "%u ms",
                     (unsigned)data->cfg.scanner_dwell_ms);
            break;
        case ROW_EDGES:
            *label_out = "Min edges";
            snprintf(value_buf, value_buf_size, "%u",
                     (unsigned)data->cfg.scanner_edges);
            break;
        case ROW_FAST:
            *label_out = "Fast scan";
            snprintf(value_buf, value_buf_size, "%s",
                     data->cfg.scanner_fast ? "ON" : "OFF");
            break;
        default:
            *label_out = "";
            value_buf[0] = '\0';
            break;
    }
}

static void draw_row(scanner_set_data_t *data, scanner_set_row_t row)
{
    int ui_row = UI_ROW_OFFSET + (int)row;
    int y = ui_row * 16;
    display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);

    const char *label;
    char value[16];
    row_label_value(data, row, &label, value, sizeof(value));

    char line[36];
    format_setting_line(line, sizeof(line), label, value);

    bool selected = (data->selected == (int)row);
    ui_draw_menu_item(ui_row, line, selected, false, false);
}

static void draw_screen(screen_t *self)
{
    scanner_set_data_t *data = (scanner_set_data_t *)self->user_data;

    ui_clear();
    ui_draw_title("Scanner Settings");

    for (int i = 0; i < ROW_COUNT; i++) {
        draw_row(data, (scanner_set_row_t)i);
    }

    ui_draw_status("L/R:Change ESC:Back");
}

static void cycle_rssi(scanner_set_data_t *data, int delta)
{
    int idx = subghz_rf_scanner_rssi_index(data->cfg.scanner_rssi_dbm);
    idx += delta;
    if (idx < 0) idx = RSSI_OPT_COUNT - 1;
    if (idx >= RSSI_OPT_COUNT) idx = 0;
    data->cfg.scanner_rssi_dbm = subghz_rf_scanner_rssi_from_index(idx);
}

static void cycle_dwell(scanner_set_data_t *data, int delta)
{
    int idx = subghz_rf_scanner_dwell_index(data->cfg.scanner_dwell_ms);
    idx += delta;
    if (idx < 0) idx = DWELL_OPT_COUNT - 1;
    if (idx >= DWELL_OPT_COUNT) idx = 0;
    data->cfg.scanner_dwell_ms = subghz_rf_scanner_dwell_from_index(idx);
}

static void cycle_edges(scanner_set_data_t *data, int delta)
{
    int idx = subghz_rf_scanner_edges_index(data->cfg.scanner_edges);
    idx += delta;
    if (idx < 0) idx = EDGES_OPT_COUNT - 1;
    if (idx >= EDGES_OPT_COUNT) idx = 0;
    data->cfg.scanner_edges = subghz_rf_scanner_edges_from_index(idx);
}

static void change_row(scanner_set_data_t *data, int delta)
{
    bool changed = true;
    switch ((scanner_set_row_t)data->selected) {
        case ROW_RSSI:  cycle_rssi(data, delta); break;
        case ROW_DWELL: cycle_dwell(data, delta); break;
        case ROW_EDGES: cycle_edges(data, delta); break;
        case ROW_FAST:  data->cfg.scanner_fast = !data->cfg.scanner_fast; break;
        default:        changed = false; break;
    }
    if (!changed) return;

    subghz_rf_settings_save(&data->cfg);
    draw_row(data, (scanner_set_row_t)data->selected);
}

static void move_selection(scanner_set_data_t *data, int delta)
{
    int old = data->selected;
    int next = old + delta;
    if (next < 0) next = ROW_COUNT - 1;
    if (next >= ROW_COUNT) next = 0;
    data->selected = next;
    draw_row(data, (scanner_set_row_t)old);
    draw_row(data, (scanner_set_row_t)next);
}

static void on_key(screen_t *self, key_code_t key)
{
    scanner_set_data_t *data = (scanner_set_data_t *)self->user_data;

    switch (key) {
        case KEY_UP:    move_selection(data, -1); break;
        case KEY_DOWN:  move_selection(data, +1); break;
        case KEY_LEFT:  change_row(data, -1); break;
        case KEY_RIGHT: change_row(data, +1); break;
        case KEY_ENTER:
        case KEY_SPACE:
            change_row(data, +1);
            break;
        case KEY_ESC:
        case KEY_Q:
        case KEY_BACKSPACE:
            screen_manager_pop();
            break;
        default:
            break;
    }
}

static void on_destroy(screen_t *self)
{
    if (self->user_data) {
        free(self->user_data);
        self->user_data = NULL;
    }
}

static void on_resume(screen_t *self)
{
    draw_screen(self);
}

screen_t* subghz_scanner_settings_screen_create(void *params)
{
    (void)params;

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    scanner_set_data_t *data = calloc(1, sizeof(scanner_set_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }

    subghz_rf_settings_load(&data->cfg);
    data->selected = ROW_RSSI;

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;

    draw_screen(screen);

    ESP_LOGI(TAG,
             "Scanner settings screen created (rssi=%d, dwell=%u, edges=%u, fast=%d)",
             (int)data->cfg.scanner_rssi_dbm,
             (unsigned)data->cfg.scanner_dwell_ms,
             (unsigned)data->cfg.scanner_edges,
             data->cfg.scanner_fast);
    return screen;
}
