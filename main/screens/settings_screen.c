/**
 * @file settings_screen.c
 * @brief Settings menu screen implementation
 */

#include "settings_screen.h"
#include "uart_pins_screen.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "SETTINGS_SCREEN";

// Menu items
static const char *menu_items[] = {
    "UART Pins",
};

#define MENU_ITEM_COUNT (sizeof(menu_items) / sizeof(menu_items[0]))

// Screen user data
typedef struct {
    int selected_index;
} settings_screen_data_t;

static void draw_screen(screen_t *self)
{
    settings_screen_data_t *data = (settings_screen_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title
    ui_draw_title("Settings");
    
    // Draw menu items
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        ui_draw_menu_item(i + 1, menu_items[i], i == data->selected_index, false, false);
    }
    
    // Draw status bar
    ui_draw_status("UP/DOWN:Nav ENTER:Select ESC:Back");
}

// Optimized: redraw only two changed rows
static void redraw_selection(settings_screen_data_t *data, int old_index, int new_index)
{
    ui_draw_menu_item(old_index + 1, menu_items[old_index], false, false, false);
    ui_draw_menu_item(new_index + 1, menu_items[new_index], true, false, false);
}

static void on_key(screen_t *self, key_code_t key)
{
    settings_screen_data_t *data = (settings_screen_data_t *)self->user_data;
    
    switch (key) {
        case KEY_UP:
            if (data->selected_index > 0) {
                int old = data->selected_index;
                data->selected_index--;
                redraw_selection(data, old, data->selected_index);
            }
            break;
            
        case KEY_DOWN:
            if (data->selected_index < (int)MENU_ITEM_COUNT - 1) {
                int old = data->selected_index;
                data->selected_index++;
                redraw_selection(data, old, data->selected_index);
            }
            break;
            
        case KEY_ENTER:
        case KEY_SPACE:
            {
                switch (data->selected_index) {
                    case 0:  // UART Pins
                        screen_manager_push(uart_pins_screen_create, NULL);
                        break;
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

screen_t* settings_screen_create(void *params)
{
    (void)params;
    
    ESP_LOGI(TAG, "Creating settings screen...");
    
    screen_t *screen = screen_alloc();
    if (!screen) return NULL;
    
    // Allocate user data
    settings_screen_data_t *data = calloc(1, sizeof(settings_screen_data_t));
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
    
    ESP_LOGI(TAG, "Settings screen created");
    return screen;
}

