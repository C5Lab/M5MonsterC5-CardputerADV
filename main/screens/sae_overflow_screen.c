/**
 * @file sae_overflow_screen.c
 * @brief SAE Overflow attack running screen implementation
 */

#include "sae_overflow_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "SAE_OVERFLOW";

// Screen user data
typedef struct {
    wifi_network_t network;
} sae_overflow_screen_data_t;

static void draw_screen(screen_t *self)
{
    sae_overflow_screen_data_t *data = (sae_overflow_screen_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title
    ui_draw_title("SAE Overflow Running");
    
    // Draw network details
    int row = 2;
    
    // Attacked Network: [SSID]
    char line[40];
    if (data->network.ssid[0]) {
        snprintf(line, sizeof(line), "Attacked Network:");
        ui_print(0, row, line, UI_COLOR_DIMMED);
        row++;
        snprintf(line, sizeof(line), " %.28s", data->network.ssid);
        ui_print(0, row, line, UI_COLOR_HIGHLIGHT);
    } else {
        snprintf(line, sizeof(line), "Attacked Network: [Hidden]");
        ui_print(0, row, line, UI_COLOR_HIGHLIGHT);
    }
    row++;
    
    // Channel: [nr]
    snprintf(line, sizeof(line), "Channel: %d", data->network.channel);
    ui_print(0, row, line, UI_COLOR_TEXT);
    row++;
    
    // BSSID: [bssid]
    snprintf(line, sizeof(line), "BSSID: %s", data->network.bssid);
    ui_print(0, row, line, UI_COLOR_TEXT);
    
    // Draw status bar
    ui_draw_status("ESC: Stop");
}

static void on_key(screen_t *self, key_code_t key)
{
    (void)self;  // Unused
    
    switch (key) {
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
    sae_overflow_screen_data_t *data = (sae_overflow_screen_data_t *)self->user_data;
    
    if (data) {
        free(data);
    }
}

screen_t* sae_overflow_screen_create(void *params)
{
    sae_overflow_screen_params_t *sae_params = (sae_overflow_screen_params_t *)params;
    
    if (!sae_params) {
        ESP_LOGE(TAG, "Invalid parameters");
        return NULL;
    }
    
    ESP_LOGI(TAG, "Creating SAE overflow screen for network: %s", sae_params->network.ssid);
    
    screen_t *screen = screen_alloc();
    if (!screen) {
        free(sae_params);
        return NULL;
    }
    
    // Allocate user data
    sae_overflow_screen_data_t *data = calloc(1, sizeof(sae_overflow_screen_data_t));
    if (!data) {
        free(screen);
        free(sae_params);
        return NULL;
    }
    
    // Copy network data
    data->network = sae_params->network;
    free(sae_params);
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "SAE overflow screen created");
    return screen;
}




