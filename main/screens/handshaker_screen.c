/**
 * @file handshaker_screen.c
 * @brief Handshaker attack running screen implementation
 */

#include "handshaker_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "HANDSHAKER";

// Maximum captured handshakes to display
#define MAX_CAPTURED  8

// Refresh timer interval (200ms)
#define REFRESH_INTERVAL_US  200000

// Screen user data
typedef struct {
    wifi_network_t *networks;
    int count;
    char captured_ssids[MAX_CAPTURED][MAX_SSID_LEN];
    int captured_count;
    bool needs_redraw;
    esp_timer_handle_t refresh_timer;
    screen_t *self;
} handshaker_screen_data_t;

// Forward declaration
static void draw_screen(screen_t *self);

/**
 * @brief Timer callback - checks if redraw is needed
 */
static void refresh_timer_callback(void *arg)
{
    handshaker_screen_data_t *data = (handshaker_screen_data_t *)arg;
    if (data && data->needs_redraw && data->self) {
        data->needs_redraw = false;
        draw_screen(data->self);
    }
}

/**
 * @brief UART line callback for parsing handshake capture output
 */
static void uart_line_callback(const char *line, void *user_data)
{
    handshaker_screen_data_t *data = (handshaker_screen_data_t *)user_data;
    if (!data) return;
    
    // Look for: "Complete 4-way handshake saved for SSID: [SSID]"
    const char *marker = "Complete 4-way handshake saved for SSID: ";
    const char *found = strstr(line, marker);
    
    if (found && data->captured_count < MAX_CAPTURED) {
        // Extract SSID - it's after the marker, until end of line or ' ('
        const char *ssid_start = found + strlen(marker);
        const char *ssid_end = strchr(ssid_start, ' ');
        if (!ssid_end) {
            ssid_end = ssid_start + strlen(ssid_start);
        }
        
        size_t ssid_len = ssid_end - ssid_start;
        if (ssid_len >= MAX_SSID_LEN) {
            ssid_len = MAX_SSID_LEN - 1;
        }
        
        // Check if we already captured this SSID
        bool already_captured = false;
        for (int i = 0; i < data->captured_count; i++) {
            if (strncmp(data->captured_ssids[i], ssid_start, ssid_len) == 0 &&
                data->captured_ssids[i][ssid_len] == '\0') {
                already_captured = true;
                break;
            }
        }
        
        if (!already_captured) {
            strncpy(data->captured_ssids[data->captured_count], ssid_start, ssid_len);
            data->captured_ssids[data->captured_count][ssid_len] = '\0';
            data->captured_count++;
            
            ESP_LOGI(TAG, "Handshake captured for SSID: %s", 
                     data->captured_ssids[data->captured_count - 1]);
            
            // Signal redraw needed - timer will handle it
            data->needs_redraw = true;
        }
    }
}

static void draw_screen(screen_t *self)
{
    handshaker_screen_data_t *data = (handshaker_screen_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title
    ui_draw_title("Handshaker Running");
    
    // Row 1: Attacking networks label
    int row = 1;
    ui_print(0, row, "Attacking:", UI_COLOR_DIMMED);
    row++;
    
    // Row 2-3: Network SSIDs
    char networks_line[64] = "";
    int len = 0;
    for (int i = 0; i < data->count && len < 50; i++) {
        const char *ssid = data->networks[i].ssid[0] ? 
                           data->networks[i].ssid : "[Hidden]";
        if (i > 0) {
            len += snprintf(networks_line + len, sizeof(networks_line) - len, ", ");
        }
        len += snprintf(networks_line + len, sizeof(networks_line) - len, "%s", ssid);
    }
    
    // Truncate if needed
    if (strlen(networks_line) > 29) {
        networks_line[26] = '.';
        networks_line[27] = '.';
        networks_line[28] = '.';
        networks_line[29] = '\0';
    }
    ui_print(0, row, networks_line, UI_COLOR_TEXT);
    row++;
    
    // Empty row
    row++;
    
    // Captured handshakes section
    if (data->captured_count > 0) {
        ui_print(0, row, "Captured:", UI_COLOR_HIGHLIGHT);
        row++;
        
        for (int i = 0; i < data->captured_count && row < 7; i++) {
            char line[48];
            snprintf(line, sizeof(line), " %.18s - Complete!", data->captured_ssids[i]);
            ui_print(0, row, line, RGB565(0, 255, 0));  // Bright green
            row++;
        }
    } else {
        ui_print(0, row, "Waiting for handshake...", UI_COLOR_DIMMED);
    }
    
    // Draw status bar
    ui_draw_status("ESC: Stop");
}

static void on_key(screen_t *self, key_code_t key)
{
    handshaker_screen_data_t *data = (handshaker_screen_data_t *)self->user_data;
    (void)data;  // May be unused now
    
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
    handshaker_screen_data_t *data = (handshaker_screen_data_t *)self->user_data;
    
    // Stop and delete timer
    if (data && data->refresh_timer) {
        esp_timer_stop(data->refresh_timer);
        esp_timer_delete(data->refresh_timer);
    }
    
    // Clear UART callback
    uart_clear_line_callback();
    
    if (data) {
        if (data->networks) {
            free(data->networks);
        }
        free(data);
    }
}

screen_t* handshaker_screen_create(void *params)
{
    handshaker_screen_params_t *hs_params = (handshaker_screen_params_t *)params;
    
    if (!hs_params) {
        ESP_LOGE(TAG, "Invalid parameters");
        return NULL;
    }
    
    ESP_LOGI(TAG, "Creating handshaker screen for %d networks...", hs_params->count);
    
    screen_t *screen = screen_alloc();
    if (!screen) {
        if (hs_params->networks) free(hs_params->networks);
        free(hs_params);
        return NULL;
    }
    
    // Allocate user data
    handshaker_screen_data_t *data = calloc(1, sizeof(handshaker_screen_data_t));
    if (!data) {
        free(screen);
        if (hs_params->networks) free(hs_params->networks);
        free(hs_params);
        return NULL;
    }
    
    // Take ownership
    data->networks = hs_params->networks;
    data->count = hs_params->count;
    data->self = screen;
    free(hs_params);
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    
    // Create periodic refresh timer
    esp_timer_create_args_t timer_args = {
        .callback = refresh_timer_callback,
        .arg = data,
        .name = "hs_refresh"
    };
    
    if (esp_timer_create(&timer_args, &data->refresh_timer) == ESP_OK) {
        esp_timer_start_periodic(data->refresh_timer, REFRESH_INTERVAL_US);
    } else {
        ESP_LOGW(TAG, "Failed to create refresh timer");
    }
    
    // Register UART callback for parsing handshake output
    uart_register_line_callback(uart_line_callback, data);
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Handshaker screen created");
    return screen;
}
