/**
 * @file home_screen.c
 * @brief Home screen implementation with main menu
 */

#include "home_screen.h"
#include "wifi_scan_screen.h"
#include "global_attacks_screen.h"
#include "sniff_karma_menu_screen.h"
#include "bt_menu_screen.h"
#include "deauth_detector_screen.h"
#include "placeholder_screen.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "HOME_SCREEN";

// Menu items
typedef struct {
    const char *title;
    screen_create_fn create_fn;
    const char *placeholder_title;
} menu_item_t;

static const menu_item_t menu_items[] = {
    {"WiFi Scan & Attack", wifi_scan_screen_create, NULL},
    {"Global WiFi Attacks", global_attacks_screen_create, NULL},
    {"WiFi Sniff&Karma", sniff_karma_menu_screen_create, NULL},
    {"Deauth Detector", deauth_detector_screen_create, NULL},
    {"Bluetooth", bt_menu_screen_create, NULL},
};

#define MENU_ITEM_COUNT (sizeof(menu_items) / sizeof(menu_items[0]))

// Screen user data
typedef struct {
    int selected_index;
} home_screen_data_t;

static void draw_screen(screen_t *self)
{
    home_screen_data_t *data = (home_screen_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title
    ui_draw_title("LABORATORIUM");
    
    // Draw menu items
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        ui_draw_menu_item(i + 1, menu_items[i].title, i == data->selected_index, false, false);
    }
    
    // Draw status bar
    ui_draw_status("UP/DOWN:Navigate ENTER:Select");
}

// Optimized: redraw only two changed rows
static void redraw_selection(home_screen_data_t *data, int old_index, int new_index)
{
    // Redraw old selection (now unselected)
    ui_draw_menu_item(old_index + 1, menu_items[old_index].title, false, false, false);
    // Redraw new selection (now selected)
    ui_draw_menu_item(new_index + 1, menu_items[new_index].title, true, false, false);
}

static void on_key(screen_t *self, key_code_t key)
{
    home_screen_data_t *data = (home_screen_data_t *)self->user_data;
    
    switch (key) {
        case KEY_UP:
            if (data->selected_index > 0) {
                int old = data->selected_index;
                data->selected_index--;
                redraw_selection(data, old, data->selected_index);
            }
            break;
            
        case KEY_DOWN:
            if (data->selected_index < MENU_ITEM_COUNT - 1) {
                int old = data->selected_index;
                data->selected_index++;
                redraw_selection(data, old, data->selected_index);
            }
            break;
            
        case KEY_ENTER:
        case KEY_SPACE:
            {
                const menu_item_t *item = &menu_items[data->selected_index];
                if (item->create_fn) {
                    screen_manager_push(item->create_fn, NULL);
                } else {
                    // Push placeholder screen
                    screen_manager_push(placeholder_screen_create, (void*)item->placeholder_title);
                }
            }
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

screen_t* home_screen_create(void *params)
{
    (void)params;
    
    ESP_LOGI(TAG, "Creating home screen...");
    
    screen_t *screen = screen_alloc();
    if (!screen) return NULL;
    
    // Allocate user data
    home_screen_data_t *data = calloc(1, sizeof(home_screen_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Home screen created");
    return screen;
}
