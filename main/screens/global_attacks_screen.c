/**
 * @file global_attacks_screen.c
 * @brief Global WiFi Attacks sub-menu screen implementation
 */

#include "global_attacks_screen.h"
#include "blackout_screen.h"
#include "global_handshaker_screen.h"
#include "text_input_screen.h"
#include "global_portal_html_screen.h"
#include "sniffer_dog_screen.h"
#include "wardrive_screen.h"
#include "placeholder_screen.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "GLOBAL_ATK";

/**
 * @brief Callback when portal SSID is entered
 */
static void on_portal_ssid_entered(const char *ssid, void *user_data)
{
    (void)user_data;
    
    ESP_LOGI(TAG, "Portal SSID entered: %s", ssid);
    
    // Create params for HTML selection screen
    global_portal_html_params_t *params = malloc(sizeof(global_portal_html_params_t));
    if (params) {
        strncpy(params->ssid, ssid, sizeof(params->ssid) - 1);
        params->ssid[sizeof(params->ssid) - 1] = '\0';
        
        // Pop the text input screen and push HTML selection
        screen_manager_pop();
        screen_manager_push(global_portal_html_screen_create, params);
    }
}

// Menu items
static const char *menu_items[] = {
    "Blackout",
    "Handshaker",
    "Portal",
    "Sniffer Dog",
    "Wardrive",
};

#define MENU_ITEM_COUNT (sizeof(menu_items) / sizeof(menu_items[0]))

// Screen user data
typedef struct {
    int selected_index;
} global_attacks_data_t;

static void draw_screen(screen_t *self)
{
    global_attacks_data_t *data = (global_attacks_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title
    ui_draw_title("Global WiFi Attacks");
    
    // Draw menu items
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        ui_draw_menu_item(i + 1, menu_items[i], i == data->selected_index, false, false);
    }
    
    // Draw status bar
    ui_draw_status("UP/DOWN:Nav ENTER:Select ESC:Back");
}

// Optimized: redraw only two changed rows
static void redraw_selection(global_attacks_data_t *data, int old_index, int new_index)
{
    ui_draw_menu_item(old_index + 1, menu_items[old_index], false, false, false);
    ui_draw_menu_item(new_index + 1, menu_items[new_index], true, false, false);
}

static void on_key(screen_t *self, key_code_t key)
{
    global_attacks_data_t *data = (global_attacks_data_t *)self->user_data;
    
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
                    case 0:  // Blackout
                        screen_manager_push(blackout_screen_create, NULL);
                        break;
                    case 1:  // Handshaker
                        screen_manager_push(global_handshaker_screen_create, NULL);
                        break;
                    case 2:  // Portal
                        {
                            text_input_params_t *params = malloc(sizeof(text_input_params_t));
                            if (params) {
                                params->title = "Enter Portal SSID";
                                params->hint = "Use keyboard, ENTER to confirm";
                                params->on_submit = on_portal_ssid_entered;
                                params->user_data = NULL;
                                screen_manager_push(text_input_screen_create, params);
                            }
                        }
                        break;
                    case 3:  // Sniffer Dog
                        screen_manager_push(sniffer_dog_screen_create, NULL);
                        break;
                    case 4:  // Wardrive
                        screen_manager_push(wardrive_screen_create, NULL);
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

screen_t* global_attacks_screen_create(void *params)
{
    (void)params;
    
    ESP_LOGI(TAG, "Creating global attacks screen...");
    
    screen_t *screen = screen_alloc();
    if (!screen) return NULL;
    
    // Allocate user data
    global_attacks_data_t *data = calloc(1, sizeof(global_attacks_data_t));
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
    
    ESP_LOGI(TAG, "Global attacks screen created");
    return screen;
}


