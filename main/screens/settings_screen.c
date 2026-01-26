/**
 * @file settings_screen.c
 * @brief Settings menu screen implementation
 */

#include "settings_screen.h"
#include "uart_pins_screen.h"
#include "vendor_lookup_screen.h"
#include "settings.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "SETTINGS_SCREEN";

// Menu item indices
#define MENU_UART_PINS      0
#define MENU_VENDOR_LOOKUP  1
#define MENU_RED_TEAM       2
#define MENU_ITEM_COUNT     3

// Screen user data
typedef struct {
    int selected_index;
    bool awaiting_disclaimer_confirm;  // Waiting for user to confirm disclaimer
} settings_screen_data_t;

static void draw_menu_item_at(int index, bool selected)
{
    bool red_team = settings_get_red_team_enabled();
    
    switch (index) {
        case MENU_UART_PINS:
            ui_draw_menu_item(index + 1, "UART Pins", selected, false, false);
            break;
        case MENU_VENDOR_LOOKUP:
            ui_draw_menu_item(index + 1, "Vendor Lookup", selected, false, false);
            break;
        case MENU_RED_TEAM:
            ui_draw_menu_item(index + 1, "Enable Red Team", selected, true, red_team);
            break;
    }
}

static void draw_screen(screen_t *self)
{
    settings_screen_data_t *data = (settings_screen_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title
    ui_draw_title("Settings");
    
    // Draw menu items
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        draw_menu_item_at(i, i == data->selected_index);
    }
    
    // Draw status bar
    ui_draw_status("UP/DOWN:Nav ENTER:Select ESC:Back");
}

// Optimized: redraw only two changed rows
static void redraw_selection(settings_screen_data_t *data, int old_index, int new_index)
{
    draw_menu_item_at(old_index, false);
    draw_menu_item_at(new_index, true);
}

static void show_red_team_disclaimer(void)
{
    ui_show_message("DISCLAIMER", 
        "Test YOUR networks only!");
}

static void on_key(screen_t *self, key_code_t key)
{
    settings_screen_data_t *data = (settings_screen_data_t *)self->user_data;
    
    // If awaiting disclaimer confirmation
    if (data->awaiting_disclaimer_confirm) {
        if (key == KEY_ENTER || key == KEY_SPACE) {
            // User accepted disclaimer - enable Red Team
            settings_set_red_team_enabled(true);
            data->awaiting_disclaimer_confirm = false;
            draw_screen(self);
        } else if (key == KEY_ESC || key == KEY_Q || key == KEY_BACKSPACE) {
            // User declined - just redraw
            data->awaiting_disclaimer_confirm = false;
            draw_screen(self);
        }
        return;
    }
    
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
                switch (data->selected_index) {
                    case MENU_UART_PINS:
                        screen_manager_push(uart_pins_screen_create, NULL);
                        break;
                    case MENU_VENDOR_LOOKUP:
                        screen_manager_push(vendor_lookup_screen_create, NULL);
                        break;
                    case MENU_RED_TEAM:
                        if (settings_get_red_team_enabled()) {
                            // Already enabled - just disable it
                            settings_set_red_team_enabled(false);
                            draw_menu_item_at(MENU_RED_TEAM, true);
                        } else {
                            // Show disclaimer before enabling
                            show_red_team_disclaimer();
                            data->awaiting_disclaimer_confirm = true;
                        }
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

