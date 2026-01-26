/**
 * @file global_handshaker_screen.c
 * @brief Global Handshaker attack screen implementation
 */

#include "global_handshaker_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "buzzer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "GLOBAL_HS";

// Maximum SSID length
#define MAX_SSID_LEN 33

// Refresh timer interval (200ms)
#define REFRESH_INTERVAL_US  200000

// Screen user data
typedef struct {
    char last_ssid[MAX_SSID_LEN];
    int total_count;
    bool needs_redraw;
    esp_timer_handle_t refresh_timer;
    screen_t *self;
} global_handshaker_data_t;

// Forward declaration
static void draw_screen(screen_t *self);

/**
 * @brief Timer callback - checks if redraw is needed
 */
static void refresh_timer_callback(void *arg)
{
    global_handshaker_data_t *data = (global_handshaker_data_t *)arg;
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
    global_handshaker_data_t *data = (global_handshaker_data_t *)user_data;
    if (!data) return;
    
    // Look for: "Complete 4-way handshake saved for SSID: [SSID]"
    const char *marker = "Complete 4-way handshake saved for SSID: ";
    const char *found = strstr(line, marker);
    
    if (found) {
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
        
        // Store last SSID
        strncpy(data->last_ssid, ssid_start, ssid_len);
        data->last_ssid[ssid_len] = '\0';
        
        // Increment total count
        data->total_count++;
        
        ESP_LOGI(TAG, "Handshake #%d captured for SSID: %s", 
                 data->total_count, data->last_ssid);
        
        // Signal redraw needed - timer will handle it
        data->needs_redraw = true;
    }
}

static void draw_screen(screen_t *self)
{
    global_handshaker_data_t *data = (global_handshaker_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title
    ui_draw_title("Global Handshaker");
    
    int row = 2;
    
    // Last handshake
    if (data->last_ssid[0] != '\0') {
        char line[48];
        snprintf(line, sizeof(line), "Last handshake: %.20s", data->last_ssid);
        ui_print(0, row, line, UI_COLOR_HIGHLIGHT);
    } else {
        ui_print(0, row, "Last handshake: -", UI_COLOR_DIMMED);
    }
    row += 2;
    
    // Total count
    char total_line[32];
    snprintf(total_line, sizeof(total_line), "Total: %d", data->total_count);
    ui_print(0, row, total_line, UI_COLOR_TEXT);
    
    // Draw status bar
    ui_draw_status("ESC: Stop");
}

static void on_key(screen_t *self, key_code_t key)
{
    switch (key) {
        case KEY_ESC:
        case KEY_Q:
        case KEY_BACKSPACE:
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
    global_handshaker_data_t *data = (global_handshaker_data_t *)self->user_data;
    
    // Stop and delete timer
    if (data && data->refresh_timer) {
        esp_timer_stop(data->refresh_timer);
        esp_timer_delete(data->refresh_timer);
    }
    
    // Clear UART callback
    uart_clear_line_callback();
    
    if (data) {
        free(data);
    }
}

screen_t* global_handshaker_screen_create(void *params)
{
    (void)params;
    
    ESP_LOGI(TAG, "Creating global handshaker screen...");
    
    screen_t *screen = screen_alloc();
    if (!screen) return NULL;
    
    // Allocate user data
    global_handshaker_data_t *data = calloc(1, sizeof(global_handshaker_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }
    
    data->self = screen;
    data->last_ssid[0] = '\0';
    data->total_count = 0;
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    
    // Create periodic refresh timer
    esp_timer_create_args_t timer_args = {
        .callback = refresh_timer_callback,
        .arg = data,
        .name = "ghs_refresh"
    };
    
    if (esp_timer_create(&timer_args, &data->refresh_timer) == ESP_OK) {
        esp_timer_start_periodic(data->refresh_timer, REFRESH_INTERVAL_US);
    } else {
        ESP_LOGW(TAG, "Failed to create refresh timer");
    }
    
    // Register UART callback for parsing handshake output
    uart_register_line_callback(uart_line_callback, data);
    
    // Send start_handshake command (no select_networks needed)
    uart_send_command("start_handshake");
    buzzer_beep_attack();
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Global handshaker screen created");
    return screen;
}










