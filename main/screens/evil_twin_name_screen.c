/**
 * @file evil_twin_name_screen.c
 * @brief Evil Twin name selection screen implementation
 */

#include "evil_twin_name_screen.h"
#include "html_select_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ET_NAME";

// Maximum visible items
#define VISIBLE_ITEMS   6

// Screen user data
typedef struct {
    wifi_network_t *networks;
    int count;
    int selected_index;
    int scroll_offset;
} evil_twin_name_data_t;

static void draw_screen(screen_t *self)
{
    evil_twin_name_data_t *data = (evil_twin_name_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title
    ui_draw_title("Select Evil Twin Name");
    
    // Draw visible network items
    int start_row = 1;
    for (int i = 0; i < VISIBLE_ITEMS; i++) {
        int net_idx = data->scroll_offset + i;
        
        if (net_idx < data->count) {
            wifi_network_t *net = &data->networks[net_idx];
            
            // Build label - show SSID or BSSID
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
            // Clear empty row
            int y = (start_row + i) * 16;
            display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
        }
    }
    
    // Draw scroll indicators if needed
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
    
    // Draw status bar
    ui_draw_status("ENTER:Select ESC:Back");
}

/**
 * @brief Send select_networks command with chosen network first
 */
static void send_reordered_select_networks(evil_twin_name_data_t *data, int chosen_idx)
{
    char cmd[256] = "select_networks";
    
    // First: add the chosen network ID
    char idx_str[8];
    snprintf(idx_str, sizeof(idx_str), " %d", data->networks[chosen_idx].id);
    strlcat(cmd, idx_str, sizeof(cmd));
    
    // Then: add all other network IDs
    for (int i = 0; i < data->count; i++) {
        if (i != chosen_idx) {
            snprintf(idx_str, sizeof(idx_str), " %d", data->networks[i].id);
            strlcat(cmd, idx_str, sizeof(cmd));
        }
    }
    
    ESP_LOGI(TAG, "Sending reordered: %s", cmd);
    uart_send_command(cmd);
}

/**
 * @brief Create reordered networks array with chosen one first
 */
static wifi_network_t* create_reordered_networks(evil_twin_name_data_t *data, int chosen_idx)
{
    wifi_network_t *reordered = malloc(data->count * sizeof(wifi_network_t));
    if (!reordered) return NULL;
    
    // First: the chosen network
    reordered[0] = data->networks[chosen_idx];
    
    // Then: all others
    int out_idx = 1;
    for (int i = 0; i < data->count; i++) {
        if (i != chosen_idx) {
            reordered[out_idx++] = data->networks[i];
        }
    }
    
    return reordered;
}

static void on_select(evil_twin_name_data_t *data)
{
    if (data->count == 0) return;
    
    int chosen_idx = data->selected_index;
    
    // Send reordered select_networks command
    send_reordered_select_networks(data, chosen_idx);
    
    // Create params for HTML select screen with reordered networks
    html_select_screen_params_t *params = malloc(sizeof(html_select_screen_params_t));
    if (params) {
        params->networks = create_reordered_networks(data, chosen_idx);
        params->network_count = data->count;
        
        if (params->networks) {
            ESP_LOGI(TAG, "Selected Evil Twin Name: %s (ID: %d)", 
                     data->networks[chosen_idx].ssid, 
                     data->networks[chosen_idx].id);
            screen_manager_push(html_select_screen_create, params);
        } else {
            free(params);
        }
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    evil_twin_name_data_t *data = (evil_twin_name_data_t *)self->user_data;
    
    switch (key) {
        case KEY_UP:
            if (data->selected_index > 0) {
                int old_idx = data->selected_index;
                data->selected_index--;
                
                // Scroll up if needed
                if (data->selected_index < data->scroll_offset) {
                    data->scroll_offset = data->selected_index;
                    draw_screen(self);
                } else {
                    // Redraw just two rows
                    int start_row = 1;
                    int old_row = old_idx - data->scroll_offset;
                    int new_row = data->selected_index - data->scroll_offset;
                    
                    wifi_network_t *old_net = &data->networks[old_idx];
                    wifi_network_t *new_net = &data->networks[data->selected_index];
                    
                    char old_label[28], new_label[28];
                    if (old_net->ssid[0]) {
                        strncpy(old_label, old_net->ssid, sizeof(old_label) - 1);
                        old_label[sizeof(old_label) - 1] = '\0';
                    } else {
                        snprintf(old_label, sizeof(old_label), "[%s]", old_net->bssid);
                    }
                    if (new_net->ssid[0]) {
                        strncpy(new_label, new_net->ssid, sizeof(new_label) - 1);
                        new_label[sizeof(new_label) - 1] = '\0';
                    } else {
                        snprintf(new_label, sizeof(new_label), "[%s]", new_net->bssid);
                    }
                    
                    ui_draw_menu_item(start_row + old_row, old_label, false, false, false);
                    ui_draw_menu_item(start_row + new_row, new_label, true, false, false);
                }
            }
            break;
            
        case KEY_DOWN:
            if (data->selected_index < data->count - 1) {
                int old_idx = data->selected_index;
                data->selected_index++;
                
                // Scroll down if needed
                if (data->selected_index >= data->scroll_offset + VISIBLE_ITEMS) {
                    data->scroll_offset = data->selected_index - VISIBLE_ITEMS + 1;
                    draw_screen(self);
                } else {
                    // Redraw just two rows
                    int start_row = 1;
                    int old_row = old_idx - data->scroll_offset;
                    int new_row = data->selected_index - data->scroll_offset;
                    
                    wifi_network_t *old_net = &data->networks[old_idx];
                    wifi_network_t *new_net = &data->networks[data->selected_index];
                    
                    char old_label[28], new_label[28];
                    if (old_net->ssid[0]) {
                        strncpy(old_label, old_net->ssid, sizeof(old_label) - 1);
                        old_label[sizeof(old_label) - 1] = '\0';
                    } else {
                        snprintf(old_label, sizeof(old_label), "[%s]", old_net->bssid);
                    }
                    if (new_net->ssid[0]) {
                        strncpy(new_label, new_net->ssid, sizeof(new_label) - 1);
                        new_label[sizeof(new_label) - 1] = '\0';
                    } else {
                        snprintf(new_label, sizeof(new_label), "[%s]", new_net->bssid);
                    }
                    
                    ui_draw_menu_item(start_row + old_row, old_label, false, false, false);
                    ui_draw_menu_item(start_row + new_row, new_label, true, false, false);
                }
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
    evil_twin_name_data_t *data = (evil_twin_name_data_t *)self->user_data;
    
    if (data) {
        if (data->networks) {
            free(data->networks);
        }
        free(data);
    }
}

screen_t* evil_twin_name_screen_create(void *params)
{
    evil_twin_name_params_t *name_params = (evil_twin_name_params_t *)params;
    
    if (!name_params) {
        ESP_LOGE(TAG, "Invalid parameters");
        return NULL;
    }
    
    ESP_LOGI(TAG, "Creating evil twin name screen for %d networks...", name_params->count);
    
    screen_t *screen = screen_alloc();
    if (!screen) {
        if (name_params->networks) free(name_params->networks);
        free(name_params);
        return NULL;
    }
    
    // Allocate user data
    evil_twin_name_data_t *data = calloc(1, sizeof(evil_twin_name_data_t));
    if (!data) {
        free(screen);
        if (name_params->networks) free(name_params->networks);
        free(name_params);
        return NULL;
    }
    
    // Take ownership
    data->networks = name_params->networks;
    data->count = name_params->count;
    free(name_params);
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Evil twin name screen created");
    return screen;
}









