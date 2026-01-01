/**
 * @file airtag_scan_screen.c
 * @brief AirTag scan screen implementation
 */

#include "airtag_scan_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "AIRTAG_SCAN";

// Refresh timer interval (200ms)
#define REFRESH_INTERVAL_US 200000

// Screen user data
typedef struct {
    int airtag_count;
    int smarttag_count;
    bool needs_redraw;
    esp_timer_handle_t refresh_timer;
    screen_t *self;
} airtag_scan_data_t;

// Forward declaration
static void draw_screen(screen_t *self);

/**
 * @brief Timer callback - checks if redraw is needed
 */
static void refresh_timer_callback(void *arg)
{
    airtag_scan_data_t *data = (airtag_scan_data_t *)arg;
    if (data && data->needs_redraw && data->self) {
        data->needs_redraw = false;
        draw_screen(data->self);
    }
}

/**
 * @brief UART line callback for parsing airtag scan output
 * Format: "2,3" where 2=airtag_count, 3=smarttag_count
 */
static void uart_line_callback(const char *line, void *user_data)
{
    airtag_scan_data_t *data = (airtag_scan_data_t *)user_data;
    if (!data) return;
    
    int airtags = 0, smarttags = 0;
    
    // Try to parse "N,M" format
    if (sscanf(line, "%d,%d", &airtags, &smarttags) == 2) {
        data->airtag_count = airtags;
        data->smarttag_count = smarttags;
        data->needs_redraw = true;
        
        ESP_LOGI(TAG, "AirTags: %d, SmartTags: %d", airtags, smarttags);
    }
}

static void draw_screen(screen_t *self)
{
    airtag_scan_data_t *data = (airtag_scan_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title
    ui_draw_title("AirTag Scan");
    
    // Draw AirTag count
    char airtag_str[16];
    snprintf(airtag_str, sizeof(airtag_str), "%d", data->airtag_count);
    ui_print_center(2, airtag_str, UI_COLOR_HIGHLIGHT);
    ui_print_center(3, "AirTags", UI_COLOR_TEXT);
    
    // Draw SmartTag count
    char smarttag_str[16];
    snprintf(smarttag_str, sizeof(smarttag_str), "%d", data->smarttag_count);
    ui_print_center(5, smarttag_str, UI_COLOR_HIGHLIGHT);
    ui_print_center(6, "SmartTags", UI_COLOR_TEXT);
    
    // Draw status bar
    ui_draw_status("ESC: Stop & Exit");
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
    airtag_scan_data_t *data = (airtag_scan_data_t *)self->user_data;
    
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

screen_t* airtag_scan_screen_create(void *params)
{
    (void)params;
    
    ESP_LOGI(TAG, "Creating AirTag scan screen...");
    
    screen_t *screen = screen_alloc();
    if (!screen) return NULL;
    
    // Allocate user data
    airtag_scan_data_t *data = calloc(1, sizeof(airtag_scan_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }
    
    data->self = screen;
    data->airtag_count = 0;
    data->smarttag_count = 0;
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    
    // Create periodic refresh timer
    esp_timer_create_args_t timer_args = {
        .callback = refresh_timer_callback,
        .arg = data,
        .name = "airtag_refresh"
    };
    
    if (esp_timer_create(&timer_args, &data->refresh_timer) == ESP_OK) {
        esp_timer_start_periodic(data->refresh_timer, REFRESH_INTERVAL_US);
    } else {
        ESP_LOGW(TAG, "Failed to create refresh timer");
    }
    
    // Register UART callback
    uart_register_line_callback(uart_line_callback, data);
    
    // Send scan_airtag command
    uart_send_command("scan_airtag");
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "AirTag scan screen created");
    return screen;
}


