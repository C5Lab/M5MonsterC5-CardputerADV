/**
 * @file station_deauth_screen.c
 * @brief Station deauth attack running screen implementation
 */

#include "station_deauth_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "STA_DEAUTH";

// Screen user data
typedef struct {
    char mac[18];   // MAC address of the station being attacked
    char ssid[33];  // SSID of the network
} station_deauth_data_t;

static void draw_screen(screen_t *self)
{
    station_deauth_data_t *data = (station_deauth_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title
    ui_draw_title("Deauth Station");
    
    // Draw attacking info
    ui_print(0, 2, "Attacking:", UI_COLOR_TEXT);
    
    // Draw MAC address
    char mac_line[32];
    snprintf(mac_line, sizeof(mac_line), "MAC: %s", data->mac);
    ui_print(0, 3, mac_line, RGB565(255, 68, 68));  // Red color for attack
    
    // Draw network SSID
    char ssid_line[32];
    snprintf(ssid_line, sizeof(ssid_line), "Net: %.24s", data->ssid);
    ui_print(0, 4, ssid_line, UI_COLOR_HIGHLIGHT);
    
    // Draw status bar
    ui_draw_status("ESC: Stop");
}

static void on_key(screen_t *self, key_code_t key)
{
    (void)self;
    
    switch (key) {
        case KEY_ESC:
        case KEY_Q:
        case KEY_BACKSPACE:
            // Send stop command and go back to results
            uart_send_command("stop");
            screen_manager_pop();
            break;
            
        default:
            break;
    }
}

static void on_destroy(screen_t *self)
{
    station_deauth_data_t *data = (station_deauth_data_t *)self->user_data;
    
    if (data) {
        free(data);
    }
}

screen_t* station_deauth_screen_create(void *params)
{
    station_deauth_params_t *deauth_params = (station_deauth_params_t *)params;
    
    if (!deauth_params) {
        ESP_LOGE(TAG, "Invalid parameters");
        return NULL;
    }
    
    ESP_LOGI(TAG, "Creating station deauth screen for MAC: %s", deauth_params->mac);
    
    screen_t *screen = screen_alloc();
    if (!screen) {
        free(deauth_params);
        return NULL;
    }
    
    // Allocate user data
    station_deauth_data_t *data = calloc(1, sizeof(station_deauth_data_t));
    if (!data) {
        free(screen);
        free(deauth_params);
        return NULL;
    }
    
    // Copy data from params
    strncpy(data->mac, deauth_params->mac, sizeof(data->mac) - 1);
    strncpy(data->ssid, deauth_params->ssid, sizeof(data->ssid) - 1);
    
    // Free params (ownership transferred)
    free(deauth_params);
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Station deauth screen created");
    return screen;
}

