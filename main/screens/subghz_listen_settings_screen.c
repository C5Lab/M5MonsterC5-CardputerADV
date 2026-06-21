/**
 * @file subghz_listen_settings_screen.c
 * @brief Listen Settings — RSSI noise-gate floor for subghz_rx.
 *
 * Persisted via subghz_rf_settings (NVS namespace "subghz_rf", key ls_rssi).
 * The Listen screen reads this on every start_rx and appends "rssi=<dBm>"
 * to the subghz_rx command. Default -80 dBm matches the firmware default.
 *
 * Keys:
 *   UP/DOWN     - move selection (only one row today)
 *   LEFT/RIGHT  - cycle the RSSI floor across the preset table
 *   ESC         - back to Listen (which restarts RX with the new floor)
 */

#include "subghz_listen_settings_screen.h"
#include "subghz_rf_settings.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "SUBGHZ_LS_SET";

#define RSSI_OPT_COUNT 6   /* { -85, -80, -75, -70, -65, -60 } */

typedef struct {
    subghz_rf_settings_t cfg;
    int selected;          /* always 0 for now */
} listen_set_data_t;

#define ROW_LABEL  2
#define ROW_VALUE  3
#define ROW_DESC1  5
#define ROW_DESC2  6

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

static void draw_rssi_row(listen_set_data_t *data, bool selected)
{
    int y = ROW_VALUE * 16;
    display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);

    char val[12];
    snprintf(val, sizeof(val), "%d dBm", (int)data->cfg.listen_rssi_dbm);

    char line[36];
    format_setting_line(line, sizeof(line), "RSSI floor", val);
    ui_draw_menu_item(ROW_VALUE, line, selected, false, false);
}

static void draw_screen(screen_t *self)
{
    listen_set_data_t *data = (listen_set_data_t *)self->user_data;

    ui_clear();
    ui_draw_title("Listen Settings");

    ui_print(0, ROW_LABEL, "Noise gate for RX:", UI_COLOR_TEXT);
    draw_rssi_row(data, data->selected == 0);

    ui_print(0, ROW_DESC1, "Lower=more sensitive,",  UI_COLOR_DIMMED);
    ui_print(0, ROW_DESC2, "more noise. -80 default.", UI_COLOR_DIMMED);

    ui_draw_status("L/R:Change ESC:Back");
}

static void cycle_rssi(listen_set_data_t *data, int delta)
{
    int idx = subghz_rf_listen_rssi_index(data->cfg.listen_rssi_dbm);
    idx += delta;
    if (idx < 0) idx = RSSI_OPT_COUNT - 1;
    if (idx >= RSSI_OPT_COUNT) idx = 0;

    int8_t new_val = subghz_rf_listen_rssi_from_index(idx);
    if (new_val == data->cfg.listen_rssi_dbm) return;
    data->cfg.listen_rssi_dbm = new_val;
    subghz_rf_settings_save(&data->cfg);
    ESP_LOGI(TAG, "listen_rssi -> %d dBm", (int)data->cfg.listen_rssi_dbm);
    draw_rssi_row(data, true);
}

static void on_key(screen_t *self, key_code_t key)
{
    listen_set_data_t *data = (listen_set_data_t *)self->user_data;

    switch (key) {
        case KEY_LEFT:
            cycle_rssi(data, -1);
            break;
        case KEY_RIGHT:
            cycle_rssi(data, +1);
            break;
        case KEY_ENTER:
        case KEY_SPACE:
            /* Single setting today — ENTER cycles for one-handed use. */
            cycle_rssi(data, +1);
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

screen_t* subghz_listen_settings_screen_create(void *params)
{
    (void)params;

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    listen_set_data_t *data = calloc(1, sizeof(listen_set_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }

    subghz_rf_settings_load(&data->cfg);
    data->selected = 0;

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;

    draw_screen(screen);

    ESP_LOGI(TAG, "Listen settings screen created (rssi=%d dBm)",
             (int)data->cfg.listen_rssi_dbm);
    return screen;
}
