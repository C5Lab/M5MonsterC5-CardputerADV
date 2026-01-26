/**
 * @file sniffer_dog_screen.c
 * @brief Sniffer Dog attack screen implementation
 */

#include "sniffer_dog_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "buzzer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "SNIFFER_DOG";

// MAC address length (XX:XX:XX:XX:XX:XX + null)
#define MAC_LEN 18

// Refresh timer interval (200ms)
#define REFRESH_INTERVAL_US 200000

// Screen user data
typedef struct {
    char last_ap[MAC_LEN];
    char last_sta[MAC_LEN];
    int kick_count;
    bool needs_redraw;
    esp_timer_handle_t refresh_timer;
    screen_t *self;
} sniffer_dog_data_t;

// Forward declaration
static void draw_screen(screen_t *self);

/**
 * @brief Timer callback - checks if redraw is needed
 */
static void refresh_timer_callback(void *arg)
{
    sniffer_dog_data_t *data = (sniffer_dog_data_t *)arg;
    if (data && data->needs_redraw && data->self) {
        data->needs_redraw = false;
        draw_screen(data->self);
    }
}

/**
 * @brief UART line callback for parsing sniffer dog output
 * Pattern: [SnifferDog #N] DEAUTH sent: AP=XX:XX:XX:XX:XX:XX -> STA=YY:YY:YY:YY:YY:YY
 */
static void uart_line_callback(const char *line, void *user_data)
{
    sniffer_dog_data_t *data = (sniffer_dog_data_t *)user_data;
    if (!data) return;
    
    // Look for "[SnifferDog #" pattern
    const char *marker = "[SnifferDog #";
    const char *found = strstr(line, marker);
    
    if (found) {
        // Extract the number after #
        const char *num_start = found + strlen(marker);
        int count = atoi(num_start);
        
        if (count > 0) {
            data->kick_count = count;
            
            // Extract AP MAC - look for "AP="
            const char *ap_marker = "AP=";
            const char *ap_start = strstr(line, ap_marker);
            if (ap_start) {
                ap_start += strlen(ap_marker);
                // Copy 17 characters (MAC address)
                strncpy(data->last_ap, ap_start, MAC_LEN - 1);
                data->last_ap[MAC_LEN - 1] = '\0';
                // Truncate at space or arrow if present
                char *end = strchr(data->last_ap, ' ');
                if (end) *end = '\0';
            }
            
            // Extract STA MAC - look for "STA="
            const char *sta_marker = "STA=";
            const char *sta_start = strstr(line, sta_marker);
            if (sta_start) {
                sta_start += strlen(sta_marker);
                // Copy 17 characters (MAC address)
                strncpy(data->last_sta, sta_start, MAC_LEN - 1);
                data->last_sta[MAC_LEN - 1] = '\0';
                // Truncate at space or parenthesis if present
                char *end = strchr(data->last_sta, ' ');
                if (end) *end = '\0';
                end = strchr(data->last_sta, '(');
                if (end) *end = '\0';
            }
            
            ESP_LOGI(TAG, "Kick #%d: STA=%s from AP=%s", 
                     data->kick_count, data->last_sta, data->last_ap);
            
            // Signal redraw needed
            data->needs_redraw = true;
        }
    }
}

static void draw_screen(screen_t *self)
{
    sniffer_dog_data_t *data = (sniffer_dog_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title
    ui_draw_title("Sniffer Dog");
    
    int row = 2;
    
    // Stations kicked out
    char count_line[32];
    snprintf(count_line, sizeof(count_line), "Stations kicked out: %d", data->kick_count);
    ui_print(0, row, count_line, UI_COLOR_HIGHLIGHT);
    row += 2;
    
    // Last station info
    if (data->last_sta[0] != '\0') {
        char sta_line[48];
        snprintf(sta_line, sizeof(sta_line), "Last station STA=%s", data->last_sta);
        ui_print(0, row, sta_line, UI_COLOR_TEXT);
        row++;
        
        char ap_line[48];
        snprintf(ap_line, sizeof(ap_line), "kicked out from AP=%s", data->last_ap);
        ui_print(0, row, ap_line, UI_COLOR_TEXT);
    } else {
        ui_print(0, row, "Waiting for deauth events...", UI_COLOR_DIMMED);
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
    sniffer_dog_data_t *data = (sniffer_dog_data_t *)self->user_data;
    
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

screen_t* sniffer_dog_screen_create(void *params)
{
    (void)params;
    
    ESP_LOGI(TAG, "Creating sniffer dog screen...");
    
    screen_t *screen = screen_alloc();
    if (!screen) return NULL;
    
    // Allocate user data
    sniffer_dog_data_t *data = calloc(1, sizeof(sniffer_dog_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }
    
    data->self = screen;
    data->last_ap[0] = '\0';
    data->last_sta[0] = '\0';
    data->kick_count = 0;
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    
    // Create periodic refresh timer
    esp_timer_create_args_t timer_args = {
        .callback = refresh_timer_callback,
        .arg = data,
        .name = "sniff_dog_refresh"
    };
    
    if (esp_timer_create(&timer_args, &data->refresh_timer) == ESP_OK) {
        esp_timer_start_periodic(data->refresh_timer, REFRESH_INTERVAL_US);
    } else {
        ESP_LOGW(TAG, "Failed to create refresh timer");
    }
    
    // Register UART callback for parsing sniffer dog output
    uart_register_line_callback(uart_line_callback, data);
    
    // Send start_sniffer_dog command
    uart_send_command("start_sniffer_dog");
    buzzer_beep_attack();
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Sniffer dog screen created");
    return screen;
}










