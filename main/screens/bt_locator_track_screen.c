/**
 * @file bt_locator_track_screen.c
 * @brief BT Locator tracking screen - tracks single device RSSI
 */

#include "bt_locator_track_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "BT_TRACK";

// Refresh timer interval (200ms)
#define REFRESH_INTERVAL_US 200000

// Screen user data
typedef struct {
    char mac[18];
    char name[24];
    int rssi;
    bool device_found;
    bool needs_redraw;
    esp_timer_handle_t refresh_timer;
    screen_t *self;
} bt_track_data_t;

// Forward declaration
static void draw_screen(screen_t *self);

/**
 * @brief Timer callback - checks if redraw is needed
 */
static void refresh_timer_callback(void *arg)
{
    bt_track_data_t *data = (bt_track_data_t *)arg;
    if (data && data->needs_redraw && data->self) {
        data->needs_redraw = false;
        draw_screen(data->self);
    }
}

/**
 * @brief UART line callback for parsing tracking output
 * Format: "XX:XX:XX:XX:XX:XX  RSSI: -93 dBm  Name: ..."
 */
static void uart_line_callback(const char *line, void *user_data)
{
    bt_track_data_t *data = (bt_track_data_t *)user_data;
    if (!data) return;
    
    // Check if this line contains our target MAC
    if (strstr(line, data->mac) == NULL) return;
    
    // Parse RSSI
    const char *rssi_marker = "RSSI: ";
    const char *rssi_pos = strstr(line, rssi_marker);
    if (rssi_pos) {
        data->rssi = atoi(rssi_pos + strlen(rssi_marker));
        data->device_found = true;
        data->needs_redraw = true;
        
        ESP_LOGI(TAG, "Device %s RSSI: %d", data->mac, data->rssi);
    }
}

static void draw_screen(screen_t *self)
{
    bt_track_data_t *data = (bt_track_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title
    ui_draw_title("BT Locator");
    
    int row = 2;
    
    // Show device name or MAC
    if (data->name[0] != '\0') {
        ui_print_center(row, data->name, UI_COLOR_HIGHLIGHT);
    } else {
        ui_print_center(row, data->mac, UI_COLOR_HIGHLIGHT);
    }
    row += 2;
    
    // Show RSSI
    if (data->device_found) {
        char rssi_str[32];
        snprintf(rssi_str, sizeof(rssi_str), "RSSI: %d dBm", data->rssi);
        ui_print_center(row, rssi_str, UI_COLOR_TEXT);
        
        // Show signal strength indicator
        row += 2;
        const char *strength;
        if (data->rssi > -50) {
            strength = "Signal: EXCELLENT";
        } else if (data->rssi > -60) {
            strength = "Signal: GOOD";
        } else if (data->rssi > -70) {
            strength = "Signal: FAIR";
        } else if (data->rssi > -80) {
            strength = "Signal: WEAK";
        } else {
            strength = "Signal: VERY WEAK";
        }
        ui_print_center(row, strength, UI_COLOR_DIMMED);
    } else {
        ui_print_center(row, "Searching...", UI_COLOR_DIMMED);
    }
    
    // Draw status bar
    ui_draw_status("ESC: Stop & Exit");
}

static void on_key(screen_t *self, key_code_t key)
{
    switch (key) {
        case KEY_ESC:
        case KEY_Q:
        case KEY_BACKSPACE:
            uart_send_command("stop");
            screen_manager_pop();
            break;
            
        default:
            break;
    }
}

static void on_destroy(screen_t *self)
{
    bt_track_data_t *data = (bt_track_data_t *)self->user_data;
    
    if (data && data->refresh_timer) {
        esp_timer_stop(data->refresh_timer);
        esp_timer_delete(data->refresh_timer);
    }
    
    uart_clear_line_callback();
    
    if (data) {
        free(data);
    }
}

screen_t* bt_locator_track_screen_create(void *params)
{
    bt_locator_track_params_t *track_params = (bt_locator_track_params_t *)params;
    
    if (!track_params) {
        ESP_LOGE(TAG, "No parameters provided");
        return NULL;
    }
    
    ESP_LOGI(TAG, "Creating BT tracking screen for: %s (%s)", 
             track_params->mac, track_params->name);
    
    screen_t *screen = screen_alloc();
    if (!screen) {
        free(track_params);
        return NULL;
    }
    
    bt_track_data_t *data = calloc(1, sizeof(bt_track_data_t));
    if (!data) {
        free(screen);
        free(track_params);
        return NULL;
    }
    
    strncpy(data->mac, track_params->mac, sizeof(data->mac) - 1);
    data->mac[sizeof(data->mac) - 1] = '\0';
    strncpy(data->name, track_params->name, sizeof(data->name) - 1);
    data->name[sizeof(data->name) - 1] = '\0';
    data->self = screen;
    data->device_found = false;
    
    free(track_params);
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    
    // Create periodic refresh timer
    esp_timer_create_args_t timer_args = {
        .callback = refresh_timer_callback,
        .arg = data,
        .name = "bt_track_refresh"
    };
    
    if (esp_timer_create(&timer_args, &data->refresh_timer) == ESP_OK) {
        esp_timer_start_periodic(data->refresh_timer, REFRESH_INTERVAL_US);
    } else {
        ESP_LOGW(TAG, "Failed to create refresh timer");
    }
    
    uart_register_line_callback(uart_line_callback, data);
    
    // Send scan_bt with MAC address
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "scan_bt %s", data->mac);
    uart_send_command(cmd);
    
    draw_screen(screen);
    
    ESP_LOGI(TAG, "BT tracking screen created");
    return screen;
}


