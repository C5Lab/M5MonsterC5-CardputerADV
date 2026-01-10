/**
 * @file network_info_screen.c
 * @brief Network information detail screen implementation
 * 
 * Displays detailed information about a WiFi network:
 * SSID, BSSID, security, signal strength, channel
 */

#include "network_info_screen.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "NET_INFO";

// Screen user data
typedef struct {
    wifi_network_t network;  // Copy of network data
} network_info_data_t;

static void draw_screen(screen_t *self)
{
    network_info_data_t *data = (network_info_data_t *)self->user_data;
    wifi_network_t *net = &data->network;
    
    ui_clear();
    
    // Draw title
    ui_draw_title("Network Info");
    
    // Row 1: SSID
    char line[32];
    if (net->ssid[0]) {
        snprintf(line, sizeof(line), "SSID: %.21s", net->ssid);
    } else {
        snprintf(line, sizeof(line), "SSID: [Hidden]");
    }
    ui_print(0, 1, line, UI_COLOR_TEXT);
    
    // Row 2: BSSID
    snprintf(line, sizeof(line), "BSSID: %s", net->bssid);
    ui_print(0, 2, line, UI_COLOR_TEXT);
    
    // Row 3: Security
    snprintf(line, sizeof(line), "Security: %.18s", net->security);
    ui_print(0, 3, line, UI_COLOR_TEXT);
    
    // Row 4: Signal strength
    snprintf(line, sizeof(line), "Signal: %d dBm", net->rssi);
    ui_print(0, 4, line, UI_COLOR_TEXT);
    
    // Row 5: Channel
    snprintf(line, sizeof(line), "Channel: %d", net->channel);
    ui_print(0, 5, line, UI_COLOR_TEXT);
    
    // Draw status bar
    ui_draw_status("ESC:Back");
}

static void on_key(screen_t *self, key_code_t key)
{
    switch (key) {
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
    network_info_data_t *data = (network_info_data_t *)self->user_data;
    
    if (data) {
        free(data);
    }
}

screen_t* network_info_screen_create(void *params)
{
    network_info_params_t *info_params = (network_info_params_t *)params;
    
    if (!info_params || !info_params->network) {
        ESP_LOGE(TAG, "Invalid parameters");
        if (info_params) free(info_params);
        return NULL;
    }
    
    ESP_LOGI(TAG, "Creating network info screen for '%s'...", 
             info_params->network->ssid[0] ? info_params->network->ssid : "[Hidden]");
    
    screen_t *screen = screen_alloc();
    if (!screen) {
        free(info_params);
        return NULL;
    }
    
    network_info_data_t *data = calloc(1, sizeof(network_info_data_t));
    if (!data) {
        free(screen);
        free(info_params);
        return NULL;
    }
    
    // Copy network data (don't take ownership)
    data->network = *info_params->network;
    
    // Free params struct
    free(info_params);
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Network info screen created");
    return screen;
}


