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
#include "compromised_menu_screen.h"
#include "network_attacks_screen.h"
#include "settings_screen.h"
#include "placeholder_screen.h"
#include "settings.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "HOME_SCREEN";

// Menu items with both attack and test versions of titles
typedef struct {
    const char *title_attack;   // Title when red team enabled
    const char *title_test;     // Title when red team disabled
    screen_create_fn create_fn;
    const char *placeholder_title;
} menu_item_t;

static const menu_item_t menu_items[] = {
    {"WiFi Scan & Attack", "WiFi Scan & Test", wifi_scan_screen_create, NULL},
    {"Global WiFi Attacks", "Global WiFi Tests", global_attacks_screen_create, NULL},
    {"WiFi Sniff&Karma", "WiFi Sniff&Karma", sniff_karma_menu_screen_create, NULL},
    {"Deauth Detector", "Deauth Detector", deauth_detector_screen_create, NULL},
    {"Bluetooth", "Bluetooth", bt_menu_screen_create, NULL},
    {"Compromised data", "Compromised data", compromised_menu_screen_create, NULL},
    {"Network Attacks", "Network Tests", network_attacks_screen_create, NULL},
    {"Settings", "Settings", settings_screen_create, NULL},
};

// Helper to get the correct title based on red team setting
static const char* get_menu_title(int index)
{
    return settings_get_red_team_enabled() 
        ? menu_items[index].title_attack 
        : menu_items[index].title_test;
}

#define MENU_ITEM_COUNT (sizeof(menu_items) / sizeof(menu_items[0]))
#define VISIBLE_ITEMS 6

// Screen user data
typedef struct {
    int selected_index;
    int scroll_offset;
} home_screen_data_t;

static void draw_screen(screen_t *self)
{
    home_screen_data_t *data = (home_screen_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title
    ui_draw_title("LABORATORIUM");
    
    // Draw only visible menu items
    int visible_end = data->scroll_offset + VISIBLE_ITEMS;
    if (visible_end > MENU_ITEM_COUNT) {
        visible_end = MENU_ITEM_COUNT;
    }
    
    for (int i = data->scroll_offset; i < visible_end; i++) {
        int row = (i - data->scroll_offset) + 1;
        ui_draw_menu_item(row, get_menu_title(i), i == data->selected_index, false, false);
    }
    
    // Scroll indicators
    if (data->scroll_offset > 0) {
        ui_print(UI_COLS - 2, 1, "^", UI_COLOR_DIMMED);
    }
    if (data->scroll_offset + VISIBLE_ITEMS < (int)MENU_ITEM_COUNT) {
        ui_print(UI_COLS - 2, VISIBLE_ITEMS, "v", UI_COLOR_DIMMED);
    }
    
    // Draw status bar
    ui_draw_status("UP/DOWN:Navigate ENTER:Select");
}

// Optimized: redraw only two changed rows
static void redraw_two_items(home_screen_data_t *data, int old_index, int new_index)
{
    // Only redraw if visible
    if (old_index >= data->scroll_offset && old_index < data->scroll_offset + VISIBLE_ITEMS) {
        int old_row = (old_index - data->scroll_offset) + 1;
        ui_draw_menu_item(old_row, get_menu_title(old_index), false, false, false);
    }
    if (new_index >= data->scroll_offset && new_index < data->scroll_offset + VISIBLE_ITEMS) {
        int new_row = (new_index - data->scroll_offset) + 1;
        ui_draw_menu_item(new_row, get_menu_title(new_index), true, false, false);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    home_screen_data_t *data = (home_screen_data_t *)self->user_data;
    
    switch (key) {
        case KEY_UP:
            if (data->selected_index > 0) {
                int old_index = data->selected_index;
                
                // Check if at first visible item on page - do page jump
                if (data->selected_index == data->scroll_offset && data->scroll_offset > 0) {
                    data->scroll_offset -= VISIBLE_ITEMS;
                    if (data->scroll_offset < 0) data->scroll_offset = 0;
                    // Land on last item of previous page
                    data->selected_index = data->scroll_offset + VISIBLE_ITEMS - 1;
                    if (data->selected_index >= (int)MENU_ITEM_COUNT) {
                        data->selected_index = (int)MENU_ITEM_COUNT - 1;
                    }
                    draw_screen(self);
                } else {
                    data->selected_index--;
                    redraw_two_items(data, old_index, data->selected_index);
                }
            }
            break;
            
        case KEY_DOWN:
            if (data->selected_index < (int)MENU_ITEM_COUNT - 1) {
                int old_index = data->selected_index;
                data->selected_index++;
                
                // Check if we need to scroll (page down)
                if (data->selected_index >= data->scroll_offset + VISIBLE_ITEMS) {
                    // Jump to next page - don't adjust back for partial pages
                    data->scroll_offset += VISIBLE_ITEMS;
                    data->selected_index = data->scroll_offset;
                    draw_screen(self);
                } else {
                    redraw_two_items(data, old_index, data->selected_index);
                }
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
