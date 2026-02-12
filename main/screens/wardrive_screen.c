/**
 * @file wardrive_screen.c
 * @brief Wardrive screen implementation
 */

#include "wardrive_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "buzzer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "WARDRIVE";

// Maximum log line length
#define MAX_LOG_LINE 128

// Refresh timer interval (200ms)
#define REFRESH_INTERVAL_US 200000

// Wardrive states
typedef enum {
    STATE_WAITING_GPS,
    STATE_RUNNING
} wardrive_state_t;

// Screen user data
typedef struct {
    wardrive_state_t state;
    char last_log_line[MAX_LOG_LINE];
    char last_ssid[64];
    char lat[16];  
    char lon[16];  
    bool needs_redraw;
    esp_timer_handle_t refresh_timer;
    screen_t *self;
} wardrive_data_t;

// Forward declaration
static void draw_screen(screen_t *self);

/**
 * @brief Timer callback - checks if redraw is needed
 */
static void refresh_timer_callback(void *arg)
{
    wardrive_data_t *data = (wardrive_data_t *)arg;
    if (data && data->needs_redraw && data->self) {
        data->needs_redraw = false;
        draw_screen(data->self);
    }
}

/**
 * @brief UART line callback for parsing wardrive output
 * Patterns:
 * - "GPS fix obtained" -> transition to STATE_RUNNING
 * - "Logged N networks to [path]" -> store as last_log_line
 * - "GPS: Lat=... Lon=..." -> store coordinates
 */
static void uart_line_callback(const char *line, void *user_data)
{
    wardrive_data_t *data = (wardrive_data_t *)user_data;
    if (!data) return;
    
    // Check for network CSV lines (MAC,SSID,[AUTH],...) to extract last SSID
    // MAC format: XX:XX:XX:XX:XX:XX (17 chars) followed by comma
    if (strlen(line) > 18 && line[2] == ':' && line[5] == ':' && 
        line[8] == ':' && line[11] == ':' && line[14] == ':' && line[17] == ',') {
        // Extract SSID between first and second comma
        const char *ssid_start = line + 18;  // right after "MAC,"
        const char *ssid_end = strchr(ssid_start, ',');
        if (ssid_end && ssid_end > ssid_start) {
            size_t ssid_len = ssid_end - ssid_start;
            if (ssid_len < sizeof(data->last_ssid)) {
                strncpy(data->last_ssid, ssid_start, ssid_len);
                data->last_ssid[ssid_len] = '\0';
                data->needs_redraw = true;
            }
        }
        // Don't return - still process the line for other patterns
    }
    
    // Check for GPS fix
    if (strstr(line, "GPS fix obtained") != NULL) {
        ESP_LOGI(TAG, "GPS fix obtained!");
        data->state = STATE_RUNNING;
        data->needs_redraw = true;
        return;
    }
    
    // Check for GPS coordinates
    const char *gps_marker = "GPS: Lat=";
    const char *gps_found = strstr(line, gps_marker);
    if (gps_found) {
        const char *lat_start = gps_found + strlen(gps_marker);
        const char *lat_end = strchr(lat_start, ' ');
        if (lat_end) {
            size_t lat_len = lat_end - lat_start;
            if (lat_len < sizeof(data->lat)) {
                strncpy(data->lat, lat_start, lat_len);
                data->lat[lat_len] = '\0';
            }
            
            // Find Lon=
            const char *lon_marker = "Lon=";
            const char *lon_start = strstr(lat_end, lon_marker);
            if (lon_start) {
                lon_start += strlen(lon_marker);
                const char *lon_end = strchr(lon_start, ' ');
                if (lon_end) {
                    size_t lon_len = lon_end - lon_start;
                    if (lon_len < sizeof(data->lon)) {
                        strncpy(data->lon, lon_start, lon_len);
                        data->lon[lon_len] = '\0';
                    }
                }
            }
            
            ESP_LOGI(TAG, "GPS update: %s, %s", data->lat, data->lon);
            data->needs_redraw = true;
        }
        return;
    }
    
    // Check for "Logged " at start (after any whitespace)
    const char *logged_marker = "Logged ";
    const char *found = strstr(line, logged_marker);
    
    if (found) {
        // Make sure it contains "networks to" to confirm it's the right line
        if (strstr(found, "networks to") != NULL) {
            // Store the entire log line
            strncpy(data->last_log_line, found, MAX_LOG_LINE - 1);
            data->last_log_line[MAX_LOG_LINE - 1] = '\0';
            
            // Remove trailing newline/whitespace
            char *end = data->last_log_line + strlen(data->last_log_line) - 1;
            while (end > data->last_log_line && (*end == '\n' || *end == '\r' || *end == ' ')) {
                *end = '\0';
                end--;
            }
            
            ESP_LOGI(TAG, "Log update: %s", data->last_log_line);
            data->needs_redraw = true;
        }
    }
}

static void draw_screen(screen_t *self)
{
    wardrive_data_t *data = (wardrive_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title
    ui_draw_title("Wardrive");
    
    int row = 2;
    
    if (data->state == STATE_WAITING_GPS) {
        // Waiting for GPS fix
        ui_print(0, row, "Acquiring GPS Fix,", UI_COLOR_HIGHLIGHT);
        row++;
        ui_print(0, row, "need clear view of the sky.", UI_COLOR_HIGHLIGHT);
    } else {
        // Running - show last log line or scanning message
        if (data->last_log_line[0] != '\0') {
            ui_print(0, row, data->last_log_line, UI_COLOR_TEXT);
        } else {
            ui_print(0, row, "Scanning networks...", UI_COLOR_DIMMED);
        }
        row += 2;
        
        // Show last SSID
        if (data->last_ssid[0] != '\0') {
            char ssid_line[80];
            snprintf(ssid_line, sizeof(ssid_line), "Last SSID: %s", data->last_ssid);
            ui_print(0, row, ssid_line, UI_COLOR_TEXT);
        } else {
            ui_print(0, row, "Last SSID: -", UI_COLOR_DIMMED);
        }
        row += 2;
        
        // Show GPS coordinates (truncated to 5 decimal places to fit screen)
        if (data->lat[0] != '\0' && data->lon[0] != '\0') {
            char gps_line[48];
            double lat_val = strtod(data->lat, NULL);
            double lon_val = strtod(data->lon, NULL);
            snprintf(gps_line, sizeof(gps_line), "Last GPS: %.5f, %.5f", lat_val, lon_val);
            ui_print(0, row, gps_line, UI_COLOR_DIMMED);
        } else {
            ui_print(0, row, "Last GPS: Waiting...", UI_COLOR_DIMMED);
        }
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
    wardrive_data_t *data = (wardrive_data_t *)self->user_data;
    
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

screen_t* wardrive_screen_create(void *params)
{
    (void)params;
    
    ESP_LOGI(TAG, "Creating wardrive screen...");
    
    screen_t *screen = screen_alloc();
    if (!screen) return NULL;
    
    // Allocate user data
    wardrive_data_t *data = calloc(1, sizeof(wardrive_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }
    
    data->self = screen;
    data->state = STATE_WAITING_GPS;
    data->last_log_line[0] = '\0';
    data->last_ssid[0] = '\0';
    data->lat[0] = '\0';
    data->lon[0] = '\0';
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    
    // Create periodic refresh timer
    esp_timer_create_args_t timer_args = {
        .callback = refresh_timer_callback,
        .arg = data,
        .name = "wardrive_refresh"
    };
    
    if (esp_timer_create(&timer_args, &data->refresh_timer) == ESP_OK) {
        esp_timer_start_periodic(data->refresh_timer, REFRESH_INTERVAL_US);
    } else {
        ESP_LOGW(TAG, "Failed to create refresh timer");
    }
    
    // Register UART callback for parsing wardrive output
    uart_register_line_callback(uart_line_callback, data);
    
    // Draw initial screen first (shows "Acquiring GPS Fix...")
    draw_screen(screen);
    
    // Send gps_set m5 command first
    uart_send_command("gps_set m5");
    ESP_LOGI(TAG, "Sent gps_set m5, waiting 3 seconds...");
    
    // Wait 3 seconds for GPS to initialize
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    // Now send start_wardrive command
    uart_send_command("start_wardrive");
    buzzer_beep_attack();
    
    ESP_LOGI(TAG, "Wardrive screen created");
    return screen;
}

