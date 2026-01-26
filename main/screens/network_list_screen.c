/**
 * @file network_list_screen.c
 * @brief Network list screen with checkboxes implementation
 */

#include "network_list_screen.h"
#include "attack_select_screen.h"
#include "network_info_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "NET_LIST";

// Maximum visible items
#define VISIBLE_ITEMS   5

// Screen user data
typedef struct {
    wifi_network_t *networks;
    int count;
    int selected_index;
    int scroll_offset;
    bool focus_on_next;
} network_list_data_t;

static int count_selected(network_list_data_t *data)
{
    int count = 0;
    for (int i = 0; i < data->count; i++) {
        if (data->networks[i].selected) count++;
    }
    return count;
}

static void send_select_networks(network_list_data_t *data)
{
    char cmd[256] = "select_networks";
    for (int i = 0; i < data->count; i++) {
        if (data->networks[i].selected) {
            char idx[8];
            snprintf(idx, sizeof(idx), " %d", data->networks[i].id);
            strlcat(cmd, idx, sizeof(cmd));
        }
    }
    uart_send_command(cmd);
}

static void navigate_to_attack(network_list_data_t *data)
{
    int sel_count = count_selected(data);
    if (sel_count > 0) {
        // Send select_networks command first
        send_select_networks(data);
        
        // Create attack params
        attack_select_params_t *params = malloc(sizeof(attack_select_params_t));
        if (params) {
            // Copy selected networks
            params->networks = malloc(sel_count * sizeof(wifi_network_t));
            params->count = 0;
            
            if (params->networks) {
                for (int i = 0; i < data->count; i++) {
                    if (data->networks[i].selected) {
                        params->networks[params->count++] = data->networks[i];
                    }
                }
                screen_manager_push(attack_select_screen_create, params);
            } else {
                free(params);
            }
        }
    }
}

// Helper to draw a single network row
static void draw_network_row(network_list_data_t *data, int net_idx)
{
    int row_on_screen = net_idx - data->scroll_offset;
    if (row_on_screen < 0 || row_on_screen >= VISIBLE_ITEMS) return;
    
    int start_row = 1;
    wifi_network_t *net = &data->networks[net_idx];
    
    char label[32];
    if (net->ssid[0]) {
        char ssid_short[20];
        strncpy(ssid_short, net->ssid, sizeof(ssid_short) - 1);
        ssid_short[sizeof(ssid_short) - 1] = '\0';
        snprintf(label, sizeof(label), "%.18s %ddB", ssid_short, net->rssi);
    } else {
        snprintf(label, sizeof(label), "[%.17s]", net->bssid);
    }
    
    bool is_selected = (!data->focus_on_next) && (net_idx == data->selected_index);
    ui_draw_menu_item(start_row + row_on_screen, label, is_selected, true, net->selected);
}

// Optimized: redraw only two changed rows (for navigation without scroll)
static void redraw_two_rows(network_list_data_t *data, int old_idx, int new_idx)
{
    draw_network_row(data, old_idx);
    draw_network_row(data, new_idx);
}

// Update title only (for selection count change)
static void redraw_title(network_list_data_t *data)
{
    char title[32];
    int sel = count_selected(data);
    snprintf(title, sizeof(title), "Networks (%d sel)", sel);
    ui_draw_title(title);
}

// Full list redraw (when scrolling)
static void redraw_list(network_list_data_t *data)
{
    redraw_title(data);
    
    int start_row = 1;
    for (int i = 0; i < VISIBLE_ITEMS; i++) {
        int net_idx = data->scroll_offset + i;
        
        if (net_idx < data->count) {
            draw_network_row(data, net_idx);
        } else {
            int y = (start_row + i) * 16;
            display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
        }
    }
    
    // Redraw scroll indicators
    display_fill_rect(DISPLAY_WIDTH - 16, 1 * 16, 16, 16, UI_COLOR_BG);
    display_fill_rect(DISPLAY_WIDTH - 16, VISIBLE_ITEMS * 16, 16, 16, UI_COLOR_BG);
    
    if (data->scroll_offset > 0) {
        ui_print(UI_COLS - 2, 1, "^", UI_COLOR_DIMMED);
    }
    if (data->scroll_offset + VISIBLE_ITEMS < data->count) {
        ui_print(UI_COLS - 2, VISIBLE_ITEMS, "v", UI_COLOR_DIMMED);
    }
}

static void draw_screen(screen_t *self)
{
    network_list_data_t *data = (network_list_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title with selected count
    char title[32];
    int sel = count_selected(data);
    snprintf(title, sizeof(title), "Networks (%d sel)", sel);
    ui_draw_title(title);
    
    // Draw visible network items
    int start_row = 1;
    for (int i = 0; i < VISIBLE_ITEMS; i++) {
        int net_idx = data->scroll_offset + i;
        
        if (net_idx < data->count) {
            wifi_network_t *net = &data->networks[net_idx];
            
            // Build label - truncate SSID if needed
            char label[32];
            if (net->ssid[0]) {
                char ssid_short[20];
                strncpy(ssid_short, net->ssid, sizeof(ssid_short) - 1);
                ssid_short[sizeof(ssid_short) - 1] = '\0';
                snprintf(label, sizeof(label), "%.18s %ddB", ssid_short, net->rssi);
            } else {
                snprintf(label, sizeof(label), "[%.17s]", net->bssid);
            }
            
            bool is_selected = (!data->focus_on_next) && (net_idx == data->selected_index);
            ui_draw_menu_item(start_row + i, label, is_selected, true, net->selected);
        }
    }
    
    // Draw scroll indicators if needed
    if (data->scroll_offset > 0) {
        ui_print(UI_COLS - 2, 1, "^", UI_COLOR_DIMMED);
    }
    if (data->scroll_offset + VISIBLE_ITEMS < data->count) {
        ui_print(UI_COLS - 2, VISIBLE_ITEMS, "v", UI_COLOR_DIMMED);
    }
    
    // Draw Next button
    int btn_row = 6;
    int btn_x = DISPLAY_WIDTH - 60;
    int btn_y = btn_row * 16;
    
    // Clear the entire row 6 first (left side of Next button)
    display_fill_rect(0, btn_y, btn_x, 16, UI_COLOR_BG);
    
    if (data->focus_on_next) {
        display_fill_rect(btn_x, btn_y, 56, 16, UI_COLOR_SELECTED);
        display_draw_rect(btn_x, btn_y, 56, 16, UI_COLOR_HIGHLIGHT);
    } else {
        display_fill_rect(btn_x, btn_y, 56, 16, RGB565(20, 40, 30));
    }
    ui_draw_text(btn_x + 6, btn_y, "Next>", UI_COLOR_HIGHLIGHT, 
                 data->focus_on_next ? UI_COLOR_SELECTED : RGB565(20, 40, 30));
    
    // Fill gap between Next button row and status bar
    int gap_y = btn_y + 16;  // After Next button row (112)
    int status_y = DISPLAY_HEIGHT - 16 - 2;  // Status bar y position (117)
    if (gap_y < status_y) {
        display_fill_rect(0, gap_y, DISPLAY_WIDTH, status_y - gap_y, UI_COLOR_BG);
    }
    
    // Draw status bar
    ui_draw_status("ENTER:Sel N:Nxt I:Info Esc:Bck");
}

static void on_key(screen_t *self, key_code_t key)
{
    network_list_data_t *data = (network_list_data_t *)self->user_data;
    
    switch (key) {
        case KEY_UP:
            if (data->focus_on_next) {
                data->focus_on_next = false;
                data->selected_index = data->count - 1;
                // Scroll to show selected
                if (data->selected_index >= data->scroll_offset + VISIBLE_ITEMS) {
                    data->scroll_offset = data->selected_index - VISIBLE_ITEMS + 1;
                }
                draw_screen(self);  // Full redraw when switching focus
            } else if (data->selected_index > 0) {
                int old_idx = data->selected_index;
                
                // Check if at first visible item on page - do page jump
                if (data->selected_index == data->scroll_offset && data->scroll_offset > 0) {
                    // Jump to previous page
                    data->scroll_offset -= VISIBLE_ITEMS;
                    if (data->scroll_offset < 0) {
                        data->scroll_offset = 0;
                    }
                    // Set cursor to bottom of previous page
                    data->selected_index = data->scroll_offset + VISIBLE_ITEMS - 1;
                    if (data->selected_index >= data->count) {
                        data->selected_index = data->count - 1;
                    }
                    redraw_list(data);  // Full page redraw
                } else {
                    // Normal single item navigation within page
                    data->selected_index--;
                    redraw_two_rows(data, old_idx, data->selected_index);
                }
            }
            break;
            
        case KEY_DOWN:
            if (!data->focus_on_next) {
                if (data->selected_index < data->count - 1) {
                    int old_idx = data->selected_index;
                    int old_scroll = data->scroll_offset;
                    
                    // Check if at last visible item on page - do page jump
                    if (data->selected_index == data->scroll_offset + VISIBLE_ITEMS - 1) {
                        // Jump to next page - don't adjust back for partial pages
                        data->scroll_offset += VISIBLE_ITEMS;
                        // Set cursor to top of new page
                        data->selected_index = data->scroll_offset;
                        redraw_list(data);  // Full page redraw
                    } else {
                        // Normal single item navigation within page
                        data->selected_index++;
                        redraw_two_rows(data, old_idx, data->selected_index);
                    }
                } else {
                    // Move focus to Next button
                    data->focus_on_next = true;
                    draw_screen(self);  // Full redraw when switching focus
                }
            }
            break;
            
        case KEY_SPACE:
            if (!data->focus_on_next && data->selected_index < data->count) {
                // Toggle selection
                data->networks[data->selected_index].selected = 
                    !data->networks[data->selected_index].selected;
                redraw_title(data);  // Update selection count
                draw_network_row(data, data->selected_index);  // Just this row
            }
            break;
            
        case KEY_N:
            // Quick shortcut to Next button (press N)
            data->focus_on_next = true;
            draw_screen(self);
            break;
            
        case KEY_I:
            // Show network info for currently selected network
            if (!data->focus_on_next && data->selected_index < data->count) {
                network_info_params_t *params = malloc(sizeof(network_info_params_t));
                if (params) {
                    params->network = &data->networks[data->selected_index];
                    screen_manager_push(network_info_screen_create, params);
                }
            }
            break;
            
        case KEY_RIGHT:
            // Navigate to attack selection (alternative to ENTER on Next)
            navigate_to_attack(data);
            break;
            
        case KEY_ENTER:
            if (data->focus_on_next) {
                // Navigate to attack selection
                navigate_to_attack(data);
            } else {
                // Toggle selection on enter too
                if (data->selected_index < data->count) {
                    data->networks[data->selected_index].selected = 
                        !data->networks[data->selected_index].selected;
                    redraw_title(data);  // Update selection count
                    draw_network_row(data, data->selected_index);  // Just this row
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

static void on_resume(screen_t *self)
{
    // Redraw screen when returning from info view
    draw_screen(self);
}

static void on_destroy(screen_t *self)
{
    network_list_data_t *data = (network_list_data_t *)self->user_data;
    
    if (data) {
        if (data->networks) {
            free(data->networks);
        }
        free(data);
    }
}

screen_t* network_list_screen_create(void *params)
{
    network_list_params_t *list_params = (network_list_params_t *)params;
    
    if (!list_params || !list_params->networks || list_params->count == 0) {
        ESP_LOGE(TAG, "Invalid parameters");
        return NULL;
    }
    
    ESP_LOGI(TAG, "Creating network list screen with %d networks...", list_params->count);
    
    screen_t *screen = screen_alloc();
    if (!screen) return NULL;
    
    // Allocate user data
    network_list_data_t *data = calloc(1, sizeof(network_list_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }
    
    // Take ownership of networks
    data->networks = list_params->networks;
    data->count = list_params->count;
    free(list_params);  // Free params struct (we took ownership of networks)
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    screen->on_resume = on_resume;
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Network list screen created");
    return screen;
}
