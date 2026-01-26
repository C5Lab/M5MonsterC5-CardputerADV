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
#include "settings.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "GLOBAL_ATK";

// Global attack type identifiers
typedef enum {
    GLOBAL_ATK_BLACKOUT,
    GLOBAL_ATK_HANDSHAKER,
    GLOBAL_ATK_PORTAL,
    GLOBAL_ATK_SNIFFER_DOG,
    GLOBAL_ATK_WARDRIVE,
    GLOBAL_ATK_COUNT
} global_attack_type_t;

// Attack menu item definition
typedef struct {
    const char *name;
    global_attack_type_t type;
    bool requires_red_team;
} global_attack_def_t;

// All available global attacks with red team requirements
static const global_attack_def_t all_global_attacks[] = {
    {"Blackout",    GLOBAL_ATK_BLACKOUT,     true},
    {"Handshaker",  GLOBAL_ATK_HANDSHAKER,   true},
    {"Portal",      GLOBAL_ATK_PORTAL,       false},
    {"Sniffer Dog", GLOBAL_ATK_SNIFFER_DOG,  true},
    {"Wardrive",    GLOBAL_ATK_WARDRIVE,     false},
};

#define ALL_GLOBAL_ATTACKS_COUNT (sizeof(all_global_attacks) / sizeof(all_global_attacks[0]))
#define MAX_VISIBLE_GLOBAL_ATTACKS 5

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

// Screen user data
typedef struct {
    int selected_index;
    // Dynamic filtered menu
    global_attack_type_t visible_attacks[MAX_VISIBLE_GLOBAL_ATTACKS];
    const char *visible_names[MAX_VISIBLE_GLOBAL_ATTACKS];
    int visible_count;
} global_attacks_data_t;

static void draw_screen(screen_t *self)
{
    global_attacks_data_t *data = (global_attacks_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title - use "Tests" when red team disabled
    ui_draw_title(settings_get_red_team_enabled() ? "Global WiFi Attacks" : "Global WiFi Tests");
    
    // Draw visible menu items
    for (int i = 0; i < data->visible_count; i++) {
        ui_draw_menu_item(i + 1, data->visible_names[i], i == data->selected_index, false, false);
    }
    
    // Draw status bar
    ui_draw_status("UP/DOWN:Nav ENTER:Select ESC:Back");
}

// Optimized: redraw only two changed rows
static void redraw_selection(global_attacks_data_t *data, int old_index, int new_index)
{
    ui_draw_menu_item(old_index + 1, data->visible_names[old_index], false, false, false);
    ui_draw_menu_item(new_index + 1, data->visible_names[new_index], true, false, false);
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
                global_attack_type_t attack = data->visible_attacks[data->selected_index];
                
                switch (attack) {
                    case GLOBAL_ATK_BLACKOUT:
                        screen_manager_push(blackout_screen_create, NULL);
                        break;
                        
                    case GLOBAL_ATK_HANDSHAKER:
                        screen_manager_push(global_handshaker_screen_create, NULL);
                        break;
                        
                    case GLOBAL_ATK_PORTAL:
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
                        
                    case GLOBAL_ATK_SNIFFER_DOG:
                        screen_manager_push(sniffer_dog_screen_create, NULL);
                        break;
                        
                    case GLOBAL_ATK_WARDRIVE:
                        screen_manager_push(wardrive_screen_create, NULL);
                        break;
                        
                    default:
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

static void build_visible_global_attacks(global_attacks_data_t *data)
{
    bool red_team = settings_get_red_team_enabled();
    data->visible_count = 0;
    
    for (int i = 0; i < (int)ALL_GLOBAL_ATTACKS_COUNT && data->visible_count < MAX_VISIBLE_GLOBAL_ATTACKS; i++) {
        // Skip red team attacks if not enabled
        if (all_global_attacks[i].requires_red_team && !red_team) {
            continue;
        }
        
        data->visible_attacks[data->visible_count] = all_global_attacks[i].type;
        data->visible_names[data->visible_count] = all_global_attacks[i].name;
        data->visible_count++;
    }
    
    ESP_LOGI(TAG, "Built global attack menu with %d visible items (red_team=%d)", 
             data->visible_count, red_team);
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
    
    // Build filtered attack list based on red team setting
    build_visible_global_attacks(data);
    
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


