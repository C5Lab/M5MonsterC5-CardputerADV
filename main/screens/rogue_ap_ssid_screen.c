/**
 * @file rogue_ap_ssid_screen.c
 * @brief Rogue AP SSID selection screen implementation
 */

#include "rogue_ap_ssid_screen.h"
#include "rogue_ap_password_screen.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ROGUE_SSID";

// Maximum visible items
#define VISIBLE_ITEMS   6

// Screen user data
typedef struct {
    wifi_network_t *networks;
    int count;
    int selected_index;
    int scroll_offset;
} rogue_ap_ssid_data_t;

static void draw_screen(screen_t *self)
{
    rogue_ap_ssid_data_t *data = (rogue_ap_ssid_data_t *)self->user_data;
    
    ui_clear();
    ui_draw_title("Select Rogue AP SSID");
    
    // Draw visible network items
    int start_row = 1;
    for (int i = 0; i < VISIBLE_ITEMS; i++) {
        int net_idx = data->scroll_offset + i;
        
        if (net_idx < data->count) {
            wifi_network_t *net = &data->networks[net_idx];
            
            char label[28];
            if (net->ssid[0]) {
                strncpy(label, net->ssid, sizeof(label) - 1);
                label[sizeof(label) - 1] = '\0';
            } else {
                snprintf(label, sizeof(label), "[%s]", net->bssid);
            }
            
            bool is_selected = (net_idx == data->selected_index);
            ui_draw_menu_item(start_row + i, label, is_selected, false, false);
        } else {
            int y = (start_row + i) * 16;
            display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
        }
    }
    
    // Draw scroll indicators
    if (data->scroll_offset > 0) {
        ui_print(UI_COLS - 2, 1, "^", UI_COLOR_DIMMED);
    }
    if (data->scroll_offset + VISIBLE_ITEMS < data->count) {
        ui_print(UI_COLS - 2, VISIBLE_ITEMS, "v", UI_COLOR_DIMMED);
    }
    
    // Fill gap to status bar
    int gap_y = (start_row + VISIBLE_ITEMS) * 16;
    int status_y = DISPLAY_HEIGHT - 16 - 2;
    if (gap_y < status_y) {
        display_fill_rect(0, gap_y, DISPLAY_WIDTH, status_y - gap_y, UI_COLOR_BG);
    }
    
    ui_draw_status("ENTER:Select ESC:Back");
}

static void on_select(rogue_ap_ssid_data_t *data)
{
    if (data->count == 0) return;
    
    int chosen_idx = data->selected_index;
    wifi_network_t *chosen = &data->networks[chosen_idx];
    
    // Create params for password screen
    rogue_ap_password_params_t *params = malloc(sizeof(rogue_ap_password_params_t));
    if (params) {
        strncpy(params->ssid, chosen->ssid, sizeof(params->ssid) - 1);
        params->ssid[sizeof(params->ssid) - 1] = '\0';
        
        ESP_LOGI(TAG, "Selected SSID: %s", params->ssid);
        screen_manager_push(rogue_ap_password_screen_create, params);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    rogue_ap_ssid_data_t *data = (rogue_ap_ssid_data_t *)self->user_data;
    
    switch (key) {
        case KEY_UP:
            if (data->selected_index > 0) {
                data->selected_index--;
                if (data->selected_index < data->scroll_offset) {
                    data->scroll_offset = data->selected_index;
                }
                draw_screen(self);
            }
            break;
            
        case KEY_DOWN:
            if (data->selected_index < data->count - 1) {
                data->selected_index++;
                if (data->selected_index >= data->scroll_offset + VISIBLE_ITEMS) {
                    data->scroll_offset = data->selected_index - VISIBLE_ITEMS + 1;
                }
                draw_screen(self);
            }
            break;
            
        case KEY_ENTER:
        case KEY_SPACE:
            on_select(data);
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
    rogue_ap_ssid_data_t *data = (rogue_ap_ssid_data_t *)self->user_data;
    
    if (data) {
        if (data->networks) {
            free(data->networks);
        }
        free(data);
    }
}

screen_t* rogue_ap_ssid_screen_create(void *params)
{
    rogue_ap_ssid_params_t *ssid_params = (rogue_ap_ssid_params_t *)params;
    
    if (!ssid_params) {
        ESP_LOGE(TAG, "Invalid parameters");
        return NULL;
    }
    
    ESP_LOGI(TAG, "Creating Rogue AP SSID screen for %d networks...", ssid_params->count);
    
    screen_t *screen = screen_alloc();
    if (!screen) {
        if (ssid_params->networks) free(ssid_params->networks);
        free(ssid_params);
        return NULL;
    }
    
    rogue_ap_ssid_data_t *data = calloc(1, sizeof(rogue_ap_ssid_data_t));
    if (!data) {
        free(screen);
        if (ssid_params->networks) free(ssid_params->networks);
        free(ssid_params);
        return NULL;
    }
    
    data->networks = ssid_params->networks;
    data->count = ssid_params->count;
    free(ssid_params);
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Rogue AP SSID screen created");
    return screen;
}
