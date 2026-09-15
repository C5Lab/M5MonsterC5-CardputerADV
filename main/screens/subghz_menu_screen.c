/**
 * @file subghz_menu_screen.c
 * @brief Sub-GHz top-level menu (7 entries, scrollable)
 */

#include "subghz_menu_screen.h"
#include "subghz_scanner_screen.h"
#include "subghz_hunter_screen.h"
#include "subghz_listen_screen.h"
#include "subghz_manage_screen.h"
#include "subghz_weather_screen.h"
#include "subghz_jammer_screen.h"
#include "subghz_tesla_screen.h"
#include "subghz_settings_screen.h"
#include "text_ui.h"
#include "settings.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "SUBGHZ_MENU";

typedef struct {
    const char *label;
    screen_create_fn create_fn;
    bool hide_in_cap;
} subghz_menu_entry_t;

static const subghz_menu_entry_t s_all_entries[] = {
    {"Quick Scan", subghz_scanner_screen_create, true},
    {"Hunter",     subghz_hunter_screen_create, true},
    {"Listen",     subghz_listen_screen_create, false},
    {"SD Signals", subghz_manage_screen_create, false},
    {"Weather",    subghz_weather_screen_create, true},
    {"Jammer",     subghz_jammer_screen_create, false},
    {"Tesla",      subghz_tesla_screen_create, false},
    {"Settings",   subghz_settings_screen_create, false},
};

#define ALL_MENU_COUNT ((int)(sizeof(s_all_entries) / sizeof(s_all_entries[0])))
#define VISIBLE_ITEMS  6

typedef struct {
    int selected_index;
    int scroll_offset;
    int visible_count;
    int visible[ALL_MENU_COUNT];
} subghz_menu_data_t;

static void rebuild_visible(subghz_menu_data_t *data)
{
    bool cap = settings_get_use_cc1101_cap();
    data->visible_count = 0;
    for (int i = 0; i < ALL_MENU_COUNT; i++) {
        if (cap && s_all_entries[i].hide_in_cap) {
            continue;
        }
        data->visible[data->visible_count++] = i;
    }
    if (data->selected_index >= data->visible_count) {
        data->selected_index = data->visible_count > 0 ? data->visible_count - 1 : 0;
    }
    if (data->scroll_offset > data->visible_count - VISIBLE_ITEMS) {
        data->scroll_offset = data->visible_count - VISIBLE_ITEMS;
    }
    if (data->scroll_offset < 0) {
        data->scroll_offset = 0;
    }
}

static const subghz_menu_entry_t *visible_entry(const subghz_menu_data_t *data, int vis_idx)
{
    return &s_all_entries[data->visible[vis_idx]];
}

static void draw_screen(screen_t *self)
{
    subghz_menu_data_t *data = (subghz_menu_data_t *)self->user_data;
    rebuild_visible(data);

    ui_clear();
    ui_draw_title("Sub-GHz");

    int visible_end = data->scroll_offset + VISIBLE_ITEMS;
    if (visible_end > data->visible_count) visible_end = data->visible_count;

    for (int i = data->scroll_offset; i < visible_end; i++) {
        int row = (i - data->scroll_offset) + 1;
        ui_draw_menu_item(row, visible_entry(data, i)->label,
                          data->selected_index == i, false, false);
    }

    if (data->scroll_offset > 0) {
        ui_print(UI_COLS - 2, 1, "^", UI_COLOR_DIMMED);
    }
    if (data->scroll_offset + VISIBLE_ITEMS < data->visible_count) {
        ui_print(UI_COLS - 2, VISIBLE_ITEMS, "v", UI_COLOR_DIMMED);
    }

    ui_draw_status("UP/DOWN ENTER:Open ESC:Back");
}

static void redraw_two(subghz_menu_data_t *data, int old_idx, int new_idx)
{
    if (old_idx >= data->scroll_offset &&
        old_idx <  data->scroll_offset + VISIBLE_ITEMS) {
        int row = (old_idx - data->scroll_offset) + 1;
        ui_draw_menu_item(row, visible_entry(data, old_idx)->label, false, false, false);
    }
    if (new_idx >= data->scroll_offset &&
        new_idx <  data->scroll_offset + VISIBLE_ITEMS) {
        int row = (new_idx - data->scroll_offset) + 1;
        ui_draw_menu_item(row, visible_entry(data, new_idx)->label, true, false, false);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    subghz_menu_data_t *data = (subghz_menu_data_t *)self->user_data;

    switch (key) {
        case KEY_UP:
            if (data->selected_index > 0) {
                int old = data->selected_index;
                int new_idx = old - 1;
                if (new_idx < data->scroll_offset) {
                    data->scroll_offset = new_idx;
                    data->selected_index = new_idx;
                    draw_screen(self);
                } else {
                    data->selected_index = new_idx;
                    redraw_two(data, old, new_idx);
                }
            } else {
                data->selected_index = data->visible_count - 1;
                data->scroll_offset = data->selected_index - VISIBLE_ITEMS + 1;
                if (data->scroll_offset < 0) data->scroll_offset = 0;
                draw_screen(self);
            }
            break;

        case KEY_DOWN:
            if (data->selected_index < data->visible_count - 1) {
                int old = data->selected_index;
                int new_idx = old + 1;
                if (new_idx >= data->scroll_offset + VISIBLE_ITEMS) {
                    data->scroll_offset = new_idx - VISIBLE_ITEMS + 1;
                    data->selected_index = new_idx;
                    draw_screen(self);
                } else {
                    data->selected_index = new_idx;
                    redraw_two(data, old, new_idx);
                }
            } else {
                data->selected_index = 0;
                data->scroll_offset = 0;
                draw_screen(self);
            }
            break;

        case KEY_ENTER:
        case KEY_SPACE: {
            const subghz_menu_entry_t *e = visible_entry(data, data->selected_index);
            ESP_LOGI(TAG, "Opening: %s", e->label);
            if (e->create_fn) {
                screen_manager_push(e->create_fn, NULL);
            }
            break;
        }

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

screen_t* subghz_menu_screen_create(void *params)
{
    (void)params;

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    subghz_menu_data_t *data = calloc(1, sizeof(subghz_menu_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;

    rebuild_visible(data);
    draw_screen(screen);
    ESP_LOGI(TAG, "Sub-GHz menu created");
    return screen;
}
