/**
 * @file deauth_screen.c
 * @brief Deauth attack running screen implementation
 */

#include "deauth_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "DEAUTH";

// Maximum visible items
#define VISIBLE_ITEMS   6

// Screen user data
typedef struct {
    wifi_network_t *networks;
    int count;
    int scroll_offset;
} deauth_screen_data_t;

static void draw_screen(screen_t *self)
{
    deauth_screen_data_t *data = (deauth_screen_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title
    ui_draw_title("Deauth Running");
    
    // Draw attacking networks
    int start_row = 1;
    for (int i = 0; i < VISIBLE_ITEMS; i++) {
        int net_idx = data->scroll_offset + i;
        int y = (start_row + i) * 16;  // FONT_HEIGHT = 16
        
        // Always clear the row background first
        display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
        
        if (net_idx < data->count) {
            wifi_network_t *net = &data->networks[net_idx];
            
            // Build label with SSID
            char label[32];
            if (net->ssid[0]) {
                snprintf(label, sizeof(label), "> %.26s", net->ssid);
            } else {
                snprintf(label, sizeof(label), "> [%.17s]", net->bssid);
            }
            
            // Draw in attack color (red)
            ui_draw_text(0, y, label, RGB565(255, 68, 68), UI_COLOR_BG);
        }
    }
    
    // Fill gap between list and status bar
    int gap_y = (start_row + VISIBLE_ITEMS) * 16;  // After last row
    int status_y = DISPLAY_HEIGHT - 16 - 2;  // Status bar y position
    if (gap_y < status_y) {
        display_fill_rect(0, gap_y, DISPLAY_WIDTH, status_y - gap_y, UI_COLOR_BG);
    }
    
    // Draw scroll indicators if needed
    if (data->scroll_offset > 0) {
        ui_print(UI_COLS - 2, 1, "^", UI_COLOR_DIMMED);
    }
    if (data->scroll_offset + VISIBLE_ITEMS < data->count) {
        ui_print(UI_COLS - 2, VISIBLE_ITEMS, "v", UI_COLOR_DIMMED);
    }
    
    // Draw status bar
    ui_draw_status("ESC: Stop");
}

static void on_key(screen_t *self, key_code_t key)
{
    deauth_screen_data_t *data = (deauth_screen_data_t *)self->user_data;
    
    switch (key) {
        case KEY_UP:
            if (data->scroll_offset > 0) {
                data->scroll_offset--;
                draw_screen(self);
            }
            break;
            
        case KEY_DOWN:
            if (data->scroll_offset + VISIBLE_ITEMS < data->count) {
                data->scroll_offset++;
                draw_screen(self);
            }
            break;
            
        case KEY_ESC:
        case KEY_Q:
            // Send stop command and go back
            uart_send_command("stop");
            screen_manager_pop();
            break;
            
        default:
            break;
    }
}

static void on_destroy(screen_t *self)
{
    deauth_screen_data_t *data = (deauth_screen_data_t *)self->user_data;
    
    if (data) {
        if (data->networks) {
            free(data->networks);
        }
        free(data);
    }
}

screen_t* deauth_screen_create(void *params)
{
    deauth_screen_params_t *deauth_params = (deauth_screen_params_t *)params;
    
    if (!deauth_params) {
        ESP_LOGE(TAG, "Invalid parameters");
        return NULL;
    }
    
    ESP_LOGI(TAG, "Creating deauth screen for %d networks...", deauth_params->count);
    
    screen_t *screen = screen_alloc();
    if (!screen) {
        if (deauth_params->networks) free(deauth_params->networks);
        free(deauth_params);
        return NULL;
    }
    
    // Allocate user data
    deauth_screen_data_t *data = calloc(1, sizeof(deauth_screen_data_t));
    if (!data) {
        free(screen);
        if (deauth_params->networks) free(deauth_params->networks);
        free(deauth_params);
        return NULL;
    }
    
    // Take ownership
    data->networks = deauth_params->networks;
    data->count = deauth_params->count;
    free(deauth_params);
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Deauth screen created");
    return screen;
}

