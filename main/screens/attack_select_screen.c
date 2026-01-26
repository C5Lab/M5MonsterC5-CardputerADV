/**
 * @file attack_select_screen.c
 * @brief Attack selection screen with list menu
 */

#include "attack_select_screen.h"
#include "deauth_screen.h"
#include "evil_twin_name_screen.h"
#include "rogue_ap_ssid_screen.h"
#include "rogue_ap_password_screen.h"
#include "sae_overflow_screen.h"
#include "handshaker_screen.h"
#include "sniffer_screen.h"
#include "settings.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "buzzer.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ATTACK_SEL";

// Attack type identifiers
typedef enum {
    ATTACK_DEAUTH,
    ATTACK_EVIL_TWIN,
    ATTACK_ROGUE_AP,
    ATTACK_SAE_OVERFLOW,
    ATTACK_HANDSHAKER,
    ATTACK_SNIFFER,
    ATTACK_COUNT
} attack_type_t;

// Attack menu item definition
typedef struct {
    const char *name;
    attack_type_t type;
    bool requires_red_team;
} attack_menu_def_t;

// All available attacks with red team requirements
static const attack_menu_def_t all_attacks[] = {
    {"Deauth",       ATTACK_DEAUTH,       true},
    {"Evil Twin",    ATTACK_EVIL_TWIN,    true},
    {"Rogue AP",     ATTACK_ROGUE_AP,     false},
    {"SAE Overflow", ATTACK_SAE_OVERFLOW, true},
    {"Handshaker",   ATTACK_HANDSHAKER,   true},
    {"Sniffer",      ATTACK_SNIFFER,      false},
};

#define ALL_ATTACKS_COUNT (sizeof(all_attacks) / sizeof(all_attacks[0]))
#define MAX_VISIBLE_ATTACKS 6

// Screen user data
typedef struct {
    wifi_network_t *networks;
    int network_count;
    int selected_index;
    // Dynamic filtered menu
    attack_type_t visible_attacks[MAX_VISIBLE_ATTACKS];
    const char *visible_names[MAX_VISIBLE_ATTACKS];
    int visible_count;
} attack_select_data_t;

static void draw_screen(screen_t *self)
{
    attack_select_data_t *data = (attack_select_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title - use "Test" when red team disabled, "Attack" when enabled
    char title[32];
    snprintf(title, sizeof(title), "%s (%d nets)", 
             settings_get_red_team_enabled() ? "Attack" : "Test",
             data->network_count);
    ui_draw_title(title);
    
    // Draw visible menu items
    for (int i = 0; i < data->visible_count; i++) {
        ui_draw_menu_item(i + 1, data->visible_names[i], i == data->selected_index, false, false);
    }
    
    // Draw status bar
    ui_draw_status("UP/DOWN:Nav ENTER:Run ESC:Back");
}

// Optimized: redraw only two changed rows
static void redraw_selection(attack_select_data_t *data, int old_index, int new_index)
{
    // Redraw old selection (now unselected)
    ui_draw_menu_item(old_index + 1, data->visible_names[old_index], false, false, false);
    // Redraw new selection (now selected)
    ui_draw_menu_item(new_index + 1, data->visible_names[new_index], true, false, false);
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
            if (data->selected_index < data->visible_count - 1) {
                int old = data->selected_index;
                data->selected_index++;
                redraw_selection(data, old, data->selected_index);
            }
            break;
            
        case KEY_ENTER:
        case KEY_SPACE:
            {
                // Get the actual attack type from the filtered list
                attack_type_t attack = data->visible_attacks[data->selected_index];
                
                switch (attack) {
                    case ATTACK_DEAUTH:
                        {
                            uart_send_command("start_deauth");
                            buzzer_beep_attack();
                            
                            deauth_screen_params_t *params = malloc(sizeof(deauth_screen_params_t));
                            if (params) {
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
                        }
                        break;
                        
                    case ATTACK_EVIL_TWIN:
                        {
                            evil_twin_name_params_t *params = malloc(sizeof(evil_twin_name_params_t));
                            if (params) {
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
                        }
                        break;
                        
                    case ATTACK_ROGUE_AP:
                        if (data->network_count == 1) {
                            rogue_ap_password_params_t *params = malloc(sizeof(rogue_ap_password_params_t));
                            if (params) {
                                strncpy(params->ssid, data->networks[0].ssid, sizeof(params->ssid) - 1);
                                params->ssid[sizeof(params->ssid) - 1] = '\0';
                                screen_manager_push(rogue_ap_password_screen_create, params);
                            }
                        } else {
                            rogue_ap_ssid_params_t *params = malloc(sizeof(rogue_ap_ssid_params_t));
                            if (params) {
                                params->networks = malloc(data->network_count * sizeof(wifi_network_t));
                                params->count = data->network_count;
                                
                                if (params->networks) {
                                    memcpy(params->networks, data->networks, 
                                           data->network_count * sizeof(wifi_network_t));
                                    screen_manager_push(rogue_ap_ssid_screen_create, params);
                                } else {
                                    free(params);
                                }
                            }
                        }
                        break;
                        
                    case ATTACK_SAE_OVERFLOW:
                        if (data->network_count != 1) {
                            ui_show_message("Error", "Select exactly 1 network");
                            draw_screen(self);
                            break;
                        }
                        
                        uart_send_command("start_sae_overflow");
                        buzzer_beep_attack();
                        
                        {
                            sae_overflow_screen_params_t *params = malloc(sizeof(sae_overflow_screen_params_t));
                            if (params) {
                                params->network = data->networks[0];
                                screen_manager_push(sae_overflow_screen_create, params);
                            }
                        }
                        break;
                        
                    case ATTACK_HANDSHAKER:
                        {
                            uart_send_command("start_handshake");
                            buzzer_beep_attack();
                            
                            handshaker_screen_params_t *params = malloc(sizeof(handshaker_screen_params_t));
                            if (params) {
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
                        }
                        break;
                        
                    case ATTACK_SNIFFER:
                        {
                            uart_send_command("start_sniffer");
                            buzzer_beep_attack();
                            
                            sniffer_screen_params_t *params = malloc(sizeof(sniffer_screen_params_t));
                            if (params) {
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
                        
                    default:
                        break;
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

static void build_visible_attacks(attack_select_data_t *data)
{
    bool red_team = settings_get_red_team_enabled();
    data->visible_count = 0;
    
    for (int i = 0; i < (int)ALL_ATTACKS_COUNT && data->visible_count < MAX_VISIBLE_ATTACKS; i++) {
        // Skip red team attacks if not enabled
        if (all_attacks[i].requires_red_team && !red_team) {
            continue;
        }
        
        // Skip Rogue AP with 1 network when red team disabled
        // (Rogue AP with single network is an offensive attack)
        if (all_attacks[i].type == ATTACK_ROGUE_AP && !red_team && data->network_count == 1) {
            continue;
        }
        
        data->visible_attacks[data->visible_count] = all_attacks[i].type;
        data->visible_names[data->visible_count] = all_attacks[i].name;
        data->visible_count++;
    }
    
    ESP_LOGI(TAG, "Built attack menu with %d visible items (red_team=%d, networks=%d)", 
             data->visible_count, red_team, data->network_count);
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
    
    // Build filtered attack list based on red team setting
    build_visible_attacks(data);
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Attack select screen created");
    return screen;
}
