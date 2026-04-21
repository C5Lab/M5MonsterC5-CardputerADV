#include "beacon_spam_menu_screen.h"
#include "beacon_ssid_list_screen.h"
#include "beacon_spam_screen.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "BEACON_MENU";

typedef struct {
    const char *title;
    screen_create_fn create_fn;
} menu_item_t;

static const menu_item_t menu_items[] = {
    {"List SSIDs", beacon_ssid_list_screen_create},
    {"Start Spam", beacon_spam_screen_create},
};

#define MENU_ITEM_COUNT (sizeof(menu_items) / sizeof(menu_items[0]))

typedef struct {
    int selected_index;
} beacon_spam_menu_data_t;

static void draw_screen(screen_t *self)
{
    beacon_spam_menu_data_t *data = (beacon_spam_menu_data_t *)self->user_data;

    ui_clear();
    ui_draw_title("Beacon Spam");

    for (int i = 0; i < (int)MENU_ITEM_COUNT; i++) {
        ui_draw_menu_item(i + 1, menu_items[i].title, i == data->selected_index, false, false);
    }

    ui_draw_status("UP/DOWN:Nav ENTER:Select ESC:Back");
}

static void redraw_selection(beacon_spam_menu_data_t *data, int old_index, int new_index)
{
    ui_draw_menu_item(old_index + 1, menu_items[old_index].title, false, false, false);
    ui_draw_menu_item(new_index + 1, menu_items[new_index].title, true, false, false);
}

static void on_key(screen_t *self, key_code_t key)
{
    beacon_spam_menu_data_t *data = (beacon_spam_menu_data_t *)self->user_data;

    switch (key) {
        case KEY_UP:
            {
                int old = data->selected_index;
                data->selected_index = (data->selected_index > 0) ? data->selected_index - 1 : (int)MENU_ITEM_COUNT - 1;
                redraw_selection(data, old, data->selected_index);
            }
            break;

        case KEY_DOWN:
            {
                int old = data->selected_index;
                data->selected_index = (data->selected_index < (int)MENU_ITEM_COUNT - 1) ? data->selected_index + 1 : 0;
                redraw_selection(data, old, data->selected_index);
            }
            break;

        case KEY_ENTER:
        case KEY_SPACE:
            {
                const menu_item_t *item = &menu_items[data->selected_index];
                if (item->create_fn) {
                    screen_manager_push(item->create_fn, NULL);
                    screen_manager_tick();
                }
            }
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
    }
}

static void on_resume(screen_t *self)
{
    draw_screen(self);
}

screen_t* beacon_spam_menu_screen_create(void *params)
{
    (void)params;

    ESP_LOGI(TAG, "Creating beacon spam menu screen...");

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    beacon_spam_menu_data_t *data = calloc(1, sizeof(beacon_spam_menu_data_t));
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

    ESP_LOGI(TAG, "Beacon spam menu screen created");
    return screen;
}
