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
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "SUBGHZ_MENU";

typedef struct {
    const char *label;
    screen_create_fn create_fn;
} subghz_menu_entry_t;

static const subghz_menu_entry_t s_entries[] = {
    {"Quick Scan", subghz_scanner_screen_create},
    {"Hunter",     subghz_hunter_screen_create},
    {"Listen",     subghz_listen_screen_create},
    {"SD Signals", subghz_manage_screen_create},
    {"Weather",    subghz_weather_screen_create},
    {"Jammer",     subghz_jammer_screen_create},
    {"Tesla",      subghz_tesla_screen_create},
};

#define MENU_COUNT     ((int)(sizeof(s_entries) / sizeof(s_entries[0])))
#define VISIBLE_ITEMS  6

typedef struct {
    int selected_index;
    int scroll_offset;
} subghz_menu_data_t;

static void draw_screen(screen_t *self)
{
    subghz_menu_data_t *data = (subghz_menu_data_t *)self->user_data;

    ui_clear();
    ui_draw_title("Sub-GHz");

    int visible_end = data->scroll_offset + VISIBLE_ITEMS;
    if (visible_end > MENU_COUNT) visible_end = MENU_COUNT;

    for (int i = data->scroll_offset; i < visible_end; i++) {
        int row = (i - data->scroll_offset) + 1;
        ui_draw_menu_item(row, s_entries[i].label,
                          data->selected_index == i, false, false);
    }

    if (data->scroll_offset > 0) {
        ui_print(UI_COLS - 2, 1, "^", UI_COLOR_DIMMED);
    }
    if (data->scroll_offset + VISIBLE_ITEMS < MENU_COUNT) {
        ui_print(UI_COLS - 2, VISIBLE_ITEMS, "v", UI_COLOR_DIMMED);
    }

    ui_draw_status("UP/DOWN ENTER:Open ESC:Back");
}

static void redraw_two(subghz_menu_data_t *data, int old_idx, int new_idx)
{
    if (old_idx >= data->scroll_offset &&
        old_idx <  data->scroll_offset + VISIBLE_ITEMS) {
        int row = (old_idx - data->scroll_offset) + 1;
        ui_draw_menu_item(row, s_entries[old_idx].label, false, false, false);
    }
    if (new_idx >= data->scroll_offset &&
        new_idx <  data->scroll_offset + VISIBLE_ITEMS) {
        int row = (new_idx - data->scroll_offset) + 1;
        ui_draw_menu_item(row, s_entries[new_idx].label, true, false, false);
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
                data->selected_index = MENU_COUNT - 1;
                data->scroll_offset = data->selected_index - VISIBLE_ITEMS + 1;
                if (data->scroll_offset < 0) data->scroll_offset = 0;
                draw_screen(self);
            }
            break;

        case KEY_DOWN:
            if (data->selected_index < MENU_COUNT - 1) {
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
            const subghz_menu_entry_t *e = &s_entries[data->selected_index];
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

    draw_screen(screen);
    ESP_LOGI(TAG, "Sub-GHz menu created");
    return screen;
}
