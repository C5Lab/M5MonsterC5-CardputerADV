/**
 * @file attack_select_screen.c
 * @brief Attack selection screen with list menu
 */

#include "attack_select_screen.h"
#include "deauth_screen.h"
#include "evil_twin_name_screen.h"
#include "sae_overflow_screen.h"
#include "handshaker_screen.h"
#include "sniffer_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ATTACK_SEL";

// Attack menu items
static const char *attack_names[] = {
    "Deauth",
    "Evil Twin",
    "SAE Overflow",
    "Handshaker",
    "Sniffer",
};

#define ATTACK_COUNT (sizeof(attack_names) / sizeof(attack_names[0]))

// Screen user data
typedef struct {
    wifi_network_t *networks;
    int network_count;
    int selected_index;
} attack_select_data_t;

static void draw_screen(screen_t *self)
{
    attack_select_data_t *data = (attack_select_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title
    char title[32];
    snprintf(title, sizeof(title), "Attack (%d nets)", data->network_count);
    ui_draw_title(title);
    
    // Draw menu items (same style as home screen)
    for (int i = 0; i < ATTACK_COUNT; i++) {
        ui_draw_menu_item(i + 1, attack_names[i], i == data->selected_index, false, false);
    }
    
    // Draw status bar
    ui_draw_status("UP/DOWN:Nav ENTER:Run ESC:Back");
}

// Optimized: redraw only two changed rows
static void redraw_selection(attack_select_data_t *data, int old_index, int new_index)
{
    // Redraw old selection (now unselected)
    ui_draw_menu_item(old_index + 1, attack_names[old_index], false, false, false);
    // Redraw new selection (now selected)
    ui_draw_menu_item(new_index + 1, attack_names[new_index], true, false, false);
}

static void on_key(screen_t *self, key_code_t key)
{
    attack_select_data_t *data = (attack_select_data_t *)self->user_data;
    
    switch (key) {
        case KEY_UP:
            if (data->selected_index > 0) {
                int old = data->selected_index;
                data->selected_index--;
                redraw_selection(data, old, data->selected_index);
            }
            break;
            
        case KEY_DOWN:
            if (data->selected_index < ATTACK_COUNT - 1) {
                int old = data->selected_index;
                data->selected_index++;
                redraw_selection(data, old, data->selected_index);
            }
            break;
            
        case KEY_ENTER:
        case KEY_SPACE:
            if (data->selected_index == 0) {
                // Deauth attack selected
                uart_send_command("start_deauth");
                
                // Create deauth screen params
                deauth_screen_params_t *params = malloc(sizeof(deauth_screen_params_t));
                if (params) {
                    // Copy networks to deauth screen
                    params->networks = malloc(data->network_count * sizeof(wifi_network_t));
                    params->count = data->network_count;
                    
                    if (params->networks) {
                        memcpy(params->networks, data->networks, 
                               data->network_count * sizeof(wifi_network_t));
                        screen_manager_push(deauth_screen_create, params);
                    } else {
                        free(params);
                    }
                }
            } else if (data->selected_index == 1) {
                // Evil Twin attack selected - go to Evil Twin Name selection first
                evil_twin_name_params_t *params = malloc(sizeof(evil_twin_name_params_t));
                if (params) {
                    // Copy networks to Evil Twin Name screen
                    params->networks = malloc(data->network_count * sizeof(wifi_network_t));
                    params->count = data->network_count;
                    
                    if (params->networks) {
                        memcpy(params->networks, data->networks, 
                               data->network_count * sizeof(wifi_network_t));
                        screen_manager_push(evil_twin_name_screen_create, params);
                    } else {
                        free(params);
                    }
                }
            } else if (data->selected_index == 2) {
                // SAE Overflow - requires exactly 1 network
                if (data->network_count != 1) {
                    ui_show_message("Error", "Select exactly 1 network");
                    draw_screen(self);  // Redraw after message
                    break;
                }
                
                // Send start command
                uart_send_command("start_sae_overflow");
                
                // Create SAE overflow screen params
                sae_overflow_screen_params_t *params = malloc(sizeof(sae_overflow_screen_params_t));
                if (params) {
                    // Copy single network
                    params->network = data->networks[0];
                    screen_manager_push(sae_overflow_screen_create, params);
                }
            } else if (data->selected_index == 3) {
                // Handshaker attack selected
                uart_send_command("start_handshake");
                
                // Create handshaker screen params
                handshaker_screen_params_t *params = malloc(sizeof(handshaker_screen_params_t));
                if (params) {
                    // Copy networks to handshaker screen
                    params->networks = malloc(data->network_count * sizeof(wifi_network_t));
                    params->count = data->network_count;
                    
                    if (params->networks) {
                        memcpy(params->networks, data->networks, 
                               data->network_count * sizeof(wifi_network_t));
                        screen_manager_push(handshaker_screen_create, params);
                    } else {
                        free(params);
                    }
                }
            } else if (data->selected_index == 4) {
                // Sniffer attack selected
                uart_send_command("start_sniffer");
                
                // Create sniffer screen params
                sniffer_screen_params_t *params = malloc(sizeof(sniffer_screen_params_t));
                if (params) {
                    // Copy networks to sniffer screen
                    params->networks = malloc(data->network_count * sizeof(wifi_network_t));
                    params->count = data->network_count;
                    
                    if (params->networks) {
                        memcpy(params->networks, data->networks, 
                               data->network_count * sizeof(wifi_network_t));
                        screen_manager_push(sniffer_screen_create, params);
                    } else {
                        free(params);
                    }
                }
            }
            break;
            
        case KEY_ESC:
        case KEY_Q:
            screen_manager_pop();
            break;
            
        default:
            break;
    }
}

static void on_destroy(screen_t *self)
{
    attack_select_data_t *data = (attack_select_data_t *)self->user_data;
    
    if (data) {
        if (data->networks) {
            free(data->networks);
        }
        free(data);
    }
}

screen_t* attack_select_screen_create(void *params)
{
    attack_select_params_t *attack_params = (attack_select_params_t *)params;
    
    if (!attack_params) {
        ESP_LOGE(TAG, "Invalid parameters");
        return NULL;
    }
    
    ESP_LOGI(TAG, "Creating attack select screen for %d networks...", attack_params->count);
    
    screen_t *screen = screen_alloc();
    if (!screen) {
        if (attack_params->networks) free(attack_params->networks);
        free(attack_params);
        return NULL;
    }
    
    // Allocate user data
    attack_select_data_t *data = calloc(1, sizeof(attack_select_data_t));
    if (!data) {
        free(screen);
        if (attack_params->networks) free(attack_params->networks);
        free(attack_params);
        return NULL;
    }
    
    // Take ownership
    data->networks = attack_params->networks;
    data->network_count = attack_params->count;
    free(attack_params);
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Attack select screen created");
    return screen;
}
