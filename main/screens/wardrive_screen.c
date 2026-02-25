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

// Refresh timer interval (200ms)
#define REFRESH_INTERVAL_US 200000

// Wardrive states
typedef enum {
    STATE_WAITING_GPS,
    STATE_RUNNING,
    STATE_GPS_LOST
} wardrive_state_t;

// Screen user data
typedef struct {
    wardrive_state_t state;
    char last_ssid[64];
    char lat[16];  
    char lon[16];  
    int gps_wait_elapsed;
    int gps_wait_timeout;
    int unique_networks;
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
 * @brief Helper to parse "Lat=VALUE Lon=VALUE" or "Lat=VALUE Lon=VALUE." from a string.
 *        Handles both space-separated and end-of-line/period-terminated Lon values.
 */
static void parse_lat_lon(const char *str, wardrive_data_t *data)
{
    const char *lat_marker = "Lat=";
    const char *lat_ptr = strstr(str, lat_marker);
    if (!lat_ptr) return;
    
    const char *lat_start = lat_ptr + strlen(lat_marker);
    const char *lat_end = strchr(lat_start, ' ');
    if (!lat_end) return;
    
    size_t lat_len = lat_end - lat_start;
    if (lat_len < sizeof(data->lat)) {
        strncpy(data->lat, lat_start, lat_len);
        data->lat[lat_len] = '\0';
    }
    
    const char *lon_marker = "Lon=";
    const char *lon_ptr = strstr(lat_end, lon_marker);
    if (!lon_ptr) return;
    
    const char *lon_start = lon_ptr + strlen(lon_marker);
    size_t lon_len = 0;
    const char *lon_end = lon_start;
    while (*lon_end && *lon_end != ' ' && *lon_end != '\n' && *lon_end != '\r') {
        if ((*lon_end >= '0' && *lon_end <= '9') || *lon_end == '-') {
            lon_end++;
        } else if (*lon_end == '.') {
            if (*(lon_end + 1) >= '0' && *(lon_end + 1) <= '9') {
                lon_end++;
            } else {
                break;
            }
        } else {
            break;
        }
    }
    lon_len = lon_end - lon_start;
    if (lon_len > 0 && lon_len < sizeof(data->lon)) {
        strncpy(data->lon, lon_start, lon_len);
        data->lon[lon_len] = '\0';
    }
    
    ESP_LOGI(TAG, "GPS update: %s, %s", data->lat, data->lon);
}

/**
 * @brief UART line callback for parsing wardrive promisc output
 */
static void uart_line_callback(const char *line, void *user_data)
{
    wardrive_data_t *data = (wardrive_data_t *)user_data;
    if (!data) return;
    
    // --- Network CSV lines: MAC,SSID,[AUTH],date,ch,rssi,lat,lon,alt,acc,WIFI ---
    if (strlen(line) > 18 && line[2] == ':' && line[5] == ':' && 
        line[8] == ':' && line[11] == ':' && line[14] == ':' && line[17] == ',') {
        const char *ssid_start = line + 18;
        const char *ssid_end = strchr(ssid_start, ',');
        if (ssid_end && ssid_end > ssid_start) {
            size_t ssid_len = ssid_end - ssid_start;
            if (ssid_len < sizeof(data->last_ssid)) {
                strncpy(data->last_ssid, ssid_start, ssid_len);
                data->last_ssid[ssid_len] = '\0';
            }
        }
        // Extract lat/lon from CSV columns 7 and 8 (0-indexed: 6 and 7)
        const char *p = line;
        int comma_count = 0;
        const char *field_starts[9] = {0};
        field_starts[0] = p;
        while (*p) {
            if (*p == ',') {
                comma_count++;
                if (comma_count < 9) {
                    field_starts[comma_count] = p + 1;
                }
            }
            p++;
        }
        // field 6 = lat, field 7 = lon
        if (comma_count >= 8 && field_starts[6] && field_starts[7]) {
            const char *lat_s = field_starts[6];
            const char *lat_e = strchr(lat_s, ',');
            const char *lon_s = field_starts[7];
            const char *lon_e = strchr(lon_s, ',');
            if (lat_e && lon_e) {
                size_t ll = lat_e - lat_s;
                size_t lo = lon_e - lon_s;
                if (ll > 0 && ll < sizeof(data->lat)) {
                    strncpy(data->lat, lat_s, ll);
                    data->lat[ll] = '\0';
                }
                if (lo > 0 && lo < sizeof(data->lon)) {
                    strncpy(data->lon, lon_s, lo);
                    data->lon[lo] = '\0';
                }
            }
        }
        data->unique_networks++;
        data->needs_redraw = true;
        return;
    }
    
    // --- GPS fix waiting countdown: "Still waiting for GPS fix... (N/M seconds)" ---
    const char *still_waiting = strstr(line, "Still waiting for GPS fix");
    if (still_waiting) {
        const char *paren = strchr(still_waiting, '(');
        if (paren) {
            int elapsed = 0, timeout = 0;
            if (sscanf(paren, "(%d/%d seconds)", &elapsed, &timeout) == 2) {
                data->gps_wait_elapsed = elapsed;
                data->gps_wait_timeout = timeout;
            }
        }
        data->needs_redraw = true;
        return;
    }
    
    // --- GPS fix obtained: "GPS fix obtained: Lat=... Lon=..." ---
    if (strstr(line, "GPS fix obtained") != NULL) {
        ESP_LOGI(TAG, "GPS fix obtained!");
        data->state = STATE_RUNNING;
        parse_lat_lon(line, data);
        data->needs_redraw = true;
        return;
    }
    
    // --- GPS fix lost: "GPS fix lost! Pausing wardrive..." ---
    if (strstr(line, "GPS fix lost") != NULL) {
        ESP_LOGW(TAG, "GPS fix lost!");
        data->state = STATE_GPS_LOST;
        data->needs_redraw = true;
        return;
    }
    
    // --- GPS fix recovered: "GPS fix recovered: Lat=... Lon=... Resuming wardrive." ---
    if (strstr(line, "GPS fix recovered") != NULL) {
        ESP_LOGI(TAG, "GPS fix recovered!");
        data->state = STATE_RUNNING;
        parse_lat_lon(line, data);
        data->needs_redraw = true;
        return;
    }
    
    // --- Wardrive promisc: N unique networks ---
    const char *promisc_stat = strstr(line, "Wardrive promisc:");
    if (promisc_stat) {
        int n = 0;
        if (sscanf(promisc_stat, "Wardrive promisc: %d unique networks", &n) == 1) {
            data->unique_networks = n;
            data->needs_redraw = true;
        }
        return;
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
        ui_print(0, row, "Acquiring GPS Fix...", UI_COLOR_HIGHLIGHT);
        row += 2;
        if (data->gps_wait_timeout > 0) {
            char wait_line[48];
            snprintf(wait_line, sizeof(wait_line), "Waiting: %d/%d seconds",
                     data->gps_wait_elapsed, data->gps_wait_timeout);
            ui_print(0, row, wait_line, UI_COLOR_DIMMED);
        } else {
            ui_print(0, row, "Need clear view of the sky.", UI_COLOR_DIMMED);
        }
    } else {
        // STATE_RUNNING or STATE_GPS_LOST
        char status_line[48];
        snprintf(status_line, sizeof(status_line), "Wardriving, %d networks found.", data->unique_networks);
        ui_print(0, row, status_line, UI_COLOR_TEXT);
        row += 2;
        
        if (data->last_ssid[0] != '\0') {
            char ssid_line[80];
            snprintf(ssid_line, sizeof(ssid_line), "Last SSID: %s", data->last_ssid);
            ui_print(0, row, ssid_line, UI_COLOR_TEXT);
        } else {
            ui_print(0, row, "Last SSID: -", UI_COLOR_DIMMED);
        }
        row += 2;
        
        if (data->state == STATE_GPS_LOST) {
            ui_print(0, row, "GPS fix lost! Pausing...", UI_COLOR_HIGHLIGHT);
        } else if (data->lat[0] != '\0' && data->lon[0] != '\0') {
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
    //uart_send_command("gps_set m5");
    //ESP_LOGI(TAG, "Sent gps_set m5, waiting 3 seconds...");
    
    // Wait 3 seconds for GPS to initialize
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    // Now send start_wardrive command
    uart_send_command("start_wardrive_promisc");
    buzzer_beep_attack();
    
    ESP_LOGI(TAG, "Wardrive screen created");
    return screen;
}

