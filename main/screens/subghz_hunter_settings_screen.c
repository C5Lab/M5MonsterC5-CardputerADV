/**
 * @file subghz_hunter_settings_screen.c
 * @brief Hunter Settings — Frequency Analyzer hunt-mode tunables.
 *
 * Persisted via subghz_rf_settings (NVS namespace "subghz_rf"). On every
 * change we write through to NVS so the Hunter screen picks it up next
 * time it builds the analyzer command via subghz_rf_build_hunter_cmd().
 *
 * Rows:
 *   Trigger        -85 .. -60 dBm                (h_trig)
 *   Mode           Decode | Raw                  (h_raw)
 *   Single burst   ON/OFF, only shown if Decode  (h_single)
 *   Fast scan      ON/OFF                        (h_fast)
 *   Capture        1 / 2 / 3 / 5 s               (h_tmo)
 *
 * Keys:
 *   UP/DOWN     - move selection
 *   LEFT/RIGHT  - cycle the value on the selected row
 *   ENTER       - same as RIGHT (one-handed)
 *   ESC         - back to Hunter (which restarts the analyzer)
 */

#include "subghz_hunter_settings_screen.h"
#include "subghz_rf_settings.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "SUBGHZ_HUNT_SET";

#define TRIGGER_OPT_COUNT 6   /* { -85, -80, -75, -70, -65, -60 } */
#define TIMEOUT_OPT_COUNT 4   /* { 1, 2, 3, 5 } seconds */

typedef enum {
    ROW_TRIGGER = 0,
    ROW_MODE,
    ROW_SINGLE,
    ROW_FAST,
    ROW_CAPTURE,
    ROW_COUNT,
} hunter_set_row_t;

typedef struct {
    subghz_rf_settings_t cfg;
    int selected;
} hunter_set_data_t;

/* Row layout: 0=title, 1=blank, 2..6 = rows, 7=status. */
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

static void row_label_value(const hunter_set_data_t *data, hunter_set_row_t row,
                            const char **label_out, char *value_buf, size_t value_buf_size)
{
    switch (row) {
        case ROW_TRIGGER:
            *label_out = "Trigger";
            snprintf(value_buf, value_buf_size, "%d dBm",
                     (int)data->cfg.hunter_trigger_dbm);
            break;
        case ROW_MODE:
            *label_out = "Mode";
            snprintf(value_buf, value_buf_size, "%s",
                     data->cfg.hunter_raw ? "Raw" : "Decode");
            break;
        case ROW_SINGLE:
            *label_out = "Single burst";
            if (data->cfg.hunter_raw) {
                snprintf(value_buf, value_buf_size, "n/a");
            } else {
                snprintf(value_buf, value_buf_size, "%s",
                         data->cfg.hunter_single ? "ON" : "OFF");
            }
            break;
        case ROW_FAST:
            *label_out = "Fast scan";
            snprintf(value_buf, value_buf_size, "%s",
                     data->cfg.hunter_fast ? "ON" : "OFF");
            break;
        case ROW_CAPTURE:
            *label_out = "Capture";
            snprintf(value_buf, value_buf_size, "%u ms",
                     (unsigned)data->cfg.hunter_timeout_ms);
            break;
        default:
            *label_out = "";
            value_buf[0] = '\0';
            break;
    }
}

static void draw_row(hunter_set_data_t *data, hunter_set_row_t row)
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
    hunter_set_data_t *data = (hunter_set_data_t *)self->user_data;

    ui_clear();
    ui_draw_title("Hunter Settings");

    for (int i = 0; i < ROW_COUNT; i++) {
        draw_row(data, (hunter_set_row_t)i);
    }

    ui_draw_status("L/R:Change ESC:Back");
}

static void cycle_trigger(hunter_set_data_t *data, int delta)
{
    int idx = subghz_rf_hunter_trigger_index(data->cfg.hunter_trigger_dbm);
    idx += delta;
    if (idx < 0) idx = TRIGGER_OPT_COUNT - 1;
    if (idx >= TRIGGER_OPT_COUNT) idx = 0;
    data->cfg.hunter_trigger_dbm = subghz_rf_hunter_trigger_from_index(idx);
}

static void cycle_timeout(hunter_set_data_t *data, int delta)
{
    int idx = subghz_rf_hunter_timeout_index(data->cfg.hunter_timeout_ms);
    idx += delta;
    if (idx < 0) idx = TIMEOUT_OPT_COUNT - 1;
    if (idx >= TIMEOUT_OPT_COUNT) idx = 0;
    data->cfg.hunter_timeout_ms = subghz_rf_hunter_timeout_from_index(idx);
}

static void change_row(hunter_set_data_t *data, int delta)
{
    bool changed = true;
    switch ((hunter_set_row_t)data->selected) {
        case ROW_TRIGGER:
            cycle_trigger(data, delta);
            break;
        case ROW_MODE:
            data->cfg.hunter_raw = !data->cfg.hunter_raw;
            /* Single is meaningless in raw mode; keep flag but UI shows n/a. */
            break;
        case ROW_SINGLE:
            if (data->cfg.hunter_raw) {
                changed = false;
            } else {
                data->cfg.hunter_single = !data->cfg.hunter_single;
            }
            break;
        case ROW_FAST:
            data->cfg.hunter_fast = !data->cfg.hunter_fast;
            break;
        case ROW_CAPTURE:
            cycle_timeout(data, delta);
            break;
        default:
            changed = false;
            break;
    }
    if (!changed) return;

    subghz_rf_settings_save(&data->cfg);

    /* Redraw the changed row, plus the dependent "Single burst" row when
     * Mode flips because its display switches between ON/OFF and n/a. */
    draw_row(data, (hunter_set_row_t)data->selected);
    if ((hunter_set_row_t)data->selected == ROW_MODE) {
        draw_row(data, ROW_SINGLE);
    }
}

static void move_selection(hunter_set_data_t *data, int delta)
{
    int old = data->selected;
    int next = old + delta;
    if (next < 0) next = ROW_COUNT - 1;
    if (next >= ROW_COUNT) next = 0;
    data->selected = next;
    draw_row(data, (hunter_set_row_t)old);
    draw_row(data, (hunter_set_row_t)next);
}

static void on_key(screen_t *self, key_code_t key)
{
    hunter_set_data_t *data = (hunter_set_data_t *)self->user_data;

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

screen_t* subghz_hunter_settings_screen_create(void *params)
{
    (void)params;

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    hunter_set_data_t *data = calloc(1, sizeof(hunter_set_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }

    subghz_rf_settings_load(&data->cfg);
    data->selected = ROW_TRIGGER;

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;

    draw_screen(screen);

    ESP_LOGI(TAG,
             "Hunter settings screen created (trig=%d, tmo=%u, raw=%d, single=%d, fast=%d)",
             (int)data->cfg.hunter_trigger_dbm,
             (unsigned)data->cfg.hunter_timeout_ms,
             data->cfg.hunter_raw, data->cfg.hunter_single, data->cfg.hunter_fast);
    return screen;
}
