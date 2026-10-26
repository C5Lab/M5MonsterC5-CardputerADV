/**
 * @file wifi_scan_screen.c
 * @brief WiFi scanning screen implementation
 */

#include "wifi_scan_screen.h"
#include "network_list_screen.h"
#include "text_ui.h"
#include "uart_handler.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "WIFI_SCAN";

// Screen user data
typedef struct {
    esp_timer_handle_t update_timer;
    bool scan_complete;
    wifi_network_t *networks;
    int network_count;
    int animation_frame;
    screen_t *screen;
} wifi_scan_data_t;

// Forward declarations
static void on_scan_complete(wifi_network_t *networks, int count, void *user_data);
static void update_timer_callback(void *arg);
static void draw_screen(screen_t *self);
static void draw_screen_full(screen_t *self);
static void update_spinner(screen_t *self);

static void on_key(screen_t *self, key_code_t key)
{
    switch (key) {
        case KEY_ESC:
        case KEY_Q:
            // Only allow back if not scanning
            if (!uart_is_scanning()) {
                screen_manager_pop();
            }
            break;
            
        default:
            break;
    }
}

static void on_destroy(screen_t *self)
{
    wifi_scan_data_t *data = (wifi_scan_data_t *)self->user_data;
    
    if (data) {
        if (data->update_timer) {
            esp_timer_stop(data->update_timer);
            esp_timer_delete(data->update_timer);
        }
        
        if (data->networks) {
            free(data->networks);
        }
        
        free(data);
    }
}

static void on_scan_complete(wifi_network_t *networks, int count, void *user_data)
{
    wifi_scan_data_t *data = (wifi_scan_data_t *)user_data;
    
    ESP_LOGI(TAG, "Scan complete callback, %d networks", count);
    
    // Copy networks
    if (count > 0) {
        data->networks = malloc(count * sizeof(wifi_network_t));
        if (data->networks) {
            memcpy(data->networks, networks, count * sizeof(wifi_network_t));
            data->network_count = count;
        }
    }
    
    data->scan_complete = true;
}

static void update_timer_callback(void *arg)
{
    wifi_scan_data_t *data = (wifi_scan_data_t *)arg;
    
    if (data->scan_complete) {
        // Stop timer
        esp_timer_stop(data->update_timer);
        
        // Transition to network list screen
        if (data->network_count > 0) {
            // Create params structure
            network_list_params_t *params = malloc(sizeof(network_list_params_t));
            if (params) {
                params->networks = data->networks;
                params->count = data->network_count;
                data->networks = NULL;  // Transfer ownership
                
                screen_manager_replace(network_list_screen_create, params);
            }
        } else {
            // No networks found, redraw with message
            draw_screen_full(data->screen);
        }
    } else {
        // Update animation - only spinner, no full redraw
        data->animation_frame = (data->animation_frame + 1) % 4;
        update_spinner(data->screen);
    }
}

static void draw_screen_full(screen_t *self)
{
    wifi_scan_data_t *data = (wifi_scan_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title
    ui_draw_title("WiFi Scan");
    
    if (data->scan_complete && data->network_count == 0) {
        const char *status = uart_get_scan_status();
        ui_print_center(3, status && (strstr(status, "busy") || strstr(status, "unknown")) ? status :
                             "No networks found", UI_COLOR_TEXT);
    } else {
        // Scanning animation with spinner only
        const char *spinner[] = {"|", "/", "-", "\\"};
        char status[32];
        snprintf(status, sizeof(status), "Scanning... %s", spinner[data->animation_frame]);
        ui_print_center(3, status, UI_COLOR_TEXT);
    }
    
    // Draw status bar
    ui_draw_status("ESC:Cancel");
}

// Optimized update - only redraw spinner row
static void update_spinner(screen_t *self)
{
    wifi_scan_data_t *data = (wifi_scan_data_t *)self->user_data;
    
    // Clear only the spinner row
    int y3 = 3 * 16;
    display_fill_rect(0, y3, DISPLAY_WIDTH, 16, UI_COLOR_BG);
    
    // Scanning animation with spinner only
    const char *spinner[] = {"|", "/", "-", "\\"};
    char status[32];
    snprintf(status, sizeof(status), "Scanning... %s", spinner[data->animation_frame]);
    ui_print_center(3, status, UI_COLOR_TEXT);
}

static void draw_screen(screen_t *self)
{
    draw_screen_full(self);
}

screen_t* wifi_scan_screen_create(void *params)
{
    (void)params;
    
    ESP_LOGI(TAG, "Creating WiFi scan screen...");
    
    screen_t *screen = screen_alloc();
    if (!screen) return NULL;
    
    // Allocate user data
    wifi_scan_data_t *data = calloc(1, sizeof(wifi_scan_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }
    
    data->screen = screen;
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    
    // Draw initial screen
    draw_screen(screen);
    
    // Create update timer
    esp_timer_create_args_t timer_args = {
        .callback = update_timer_callback,
        .arg = data,
        .name = "scan_update"
    };
    esp_timer_create(&timer_args, &data->update_timer);
    esp_timer_start_periodic(data->update_timer, 200000);  // 200ms
    
    // Start WiFi scan
    esp_err_t ret = uart_start_wifi_scan(on_scan_complete, data);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start scan");
    }
    
    ESP_LOGI(TAG, "WiFi scan screen created");
    return screen;
}
