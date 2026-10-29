/**
 * @file deauth_detector_screen.c
 * @brief Deauth attack detector screen implementation
 * 
 * Displays detected deauth attacks in real-time.
 * Parses UART output: [DEAUTH] CH: <ch> | AP: <name> (<bssid>) | RSSI: <rssi>
 */

#include "deauth_detector_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>

static const char *TAG = "DEAUTH_DETECTOR";

// Screen user data
typedef struct {
    int channel;
    char ap_name[33];
    char bssid[18];
    int rssi;
    int detection_count;
    bool has_detection;
    bool needs_redraw;
    screen_t *self;
} deauth_detector_data_t;

// Forward declaration
static void draw_screen(screen_t *self);

/* Require decimal digits and detect overflow before converting to int. */
static bool parse_deauth_integer(const char *start, bool allow_negative,
                                 int *value, const char **end)
{
    const char *digits = start;
    if (allow_negative && *digits == '-') digits++;
    if (*digits < '0' || *digits > '9') return false;

    errno = 0;
    char *parsed_end;
    long parsed = strtol(start, &parsed_end, 10);
    if (errno == ERANGE || parsed < INT_MIN || parsed > INT_MAX) return false;
    *value = (int)parsed;
    *end = parsed_end;
    return true;
}

/**
 * @brief Parse a complete detection record, never the startup format example.
 * Format: [DEAUTH] CH: <ch> | AP: <name> (<bssid>) | RSSI: <rssi>
 */
static bool parse_deauth_line(const char *line, deauth_detector_data_t *data)
{
    const char *marker = "[DEAUTH] CH: ";
    if (!line || !data || strncmp(line, marker, strlen(marker)) != 0) return false;

    int channel, rssi;
    const char *end;
    if (!parse_deauth_integer(line + strlen(marker), false, &channel, &end) ||
        channel <= 0) return false;

    const char *ap_marker = " | AP: ";
    if (strncmp(end, ap_marker, strlen(ap_marker)) != 0) return false;
    const char *ap_start = end + strlen(ap_marker);

    // Use the last suffix so parentheses and delimiter-like text in SSIDs work.
    const char *rssi_marker = ") | RSSI: ";
    const char *bssid_end = NULL;
    const char *candidate = ap_start;
    while ((candidate = strstr(candidate, rssi_marker)) != NULL) {
        bssid_end = candidate;
        candidate++;
    }
    if (!bssid_end || bssid_end - ap_start < 19) return false;
    const char *bssid_start = bssid_end - 17;
    if (bssid_start[-2] != ' ' || bssid_start[-1] != '(') return false;

    size_t ap_len = (size_t)(bssid_start - 2 - ap_start);
    if (ap_len == 0 || ap_len > 32) return false;
    for (size_t i = 0; i < 17; i++) {
        if (i % 3 == 2) {
            if (bssid_start[i] != ':') return false;
        } else if (!isxdigit((unsigned char)bssid_start[i])) {
            return false;
        }
    }

    if (!parse_deauth_integer(bssid_end + strlen(rssi_marker), true, &rssi, &end) ||
        *end != '\0') return false;

    // Commit only after every field has passed validation.
    char ap_name[33];
    char bssid[18];
    memcpy(ap_name, ap_start, ap_len);
    ap_name[ap_len] = '\0';
    memcpy(bssid, bssid_start, 17);
    bssid[17] = '\0';
    memcpy(data->ap_name, ap_name, ap_len + 1);
    memcpy(data->bssid, bssid, sizeof(bssid));
    data->channel = channel;
    data->rssi = rssi;
    data->has_detection = true;
    if (data->detection_count < INT_MAX) data->detection_count++;

    ESP_LOGI(TAG, "Parsed: CH=%d AP=%s BSSID=%s RSSI=%d (total: %d)",
             data->channel, data->ap_name, data->bssid, data->rssi, data->detection_count);
    return true;
}

/**
 * @brief UART line callback for parsing deauth detector output
 */
static void uart_line_callback(const char *line, void *user_data)
{
    deauth_detector_data_t *data = (deauth_detector_data_t *)user_data;
    if (!data) return;
    
    if (parse_deauth_line(line, data)) {
        data->needs_redraw = true;
    }
}

static void draw_screen(screen_t *self)
{
    deauth_detector_data_t *data = (deauth_detector_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title
    ui_draw_title("DEAUTH DETECTOR");
    
    if (data->has_detection) {
        // Show last detection
        ui_print(1, 2, "Last Detection:", UI_COLOR_TEXT);
        
        // Channel and RSSI on same line
        char info_str[32];
        snprintf(info_str, sizeof(info_str), "CH: %d  RSSI: %d dBm", data->channel, data->rssi);
        ui_print(1, 3, info_str, UI_COLOR_HIGHLIGHT);
        
        // AP name
        char ap_str[32];
        snprintf(ap_str, sizeof(ap_str), "AP: %.24s", data->ap_name);
        ui_print(1, 4, ap_str, UI_COLOR_TEXT);
        
        // BSSID
        char bssid_str[32];
        snprintf(bssid_str, sizeof(bssid_str), "BSSID: %s", data->bssid);
        ui_print(1, 5, bssid_str, UI_COLOR_DIMMED);
        
        // Total count (row 6 to avoid overlap with status bar)
        char count_str[32];
        snprintf(count_str, sizeof(count_str), "Total: %d detections", data->detection_count);
        ui_print_center(6, count_str, UI_COLOR_TEXT);
    } else {
        // Waiting for detections
        ui_print_center(3, "Scanning for deauth", UI_COLOR_TEXT);
        ui_print_center(4, "attacks...", UI_COLOR_TEXT);
        ui_print_center(6, "Waiting for data", UI_COLOR_DIMMED);
    }
    
    // Draw status bar
    ui_draw_status("ESC: Stop");
}

static void on_tick(screen_t *self)
{
    deauth_detector_data_t *data = (deauth_detector_data_t *)self->user_data;
    
    // Check if redraw needed from UART callback
    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    switch (key) {
        case KEY_ESC:
        case KEY_Q:
            // Stop detector and go back
            ESP_LOGI(TAG, "Stopping deauth detector");
            uart_send_command("stop");
            screen_manager_pop();
            break;
            
        default:
            break;
    }
}

static void on_destroy(screen_t *self)
{
    // Clear UART callback
    uart_clear_line_callback();
    
    if (self->user_data) {
        free(self->user_data);
    }
}

screen_t* deauth_detector_screen_create(void *params)
{
    (void)params;
    
    ESP_LOGI(TAG, "Creating deauth detector screen...");
    
    screen_t *screen = screen_alloc();
    if (!screen) return NULL;
    
    // Allocate user data
    deauth_detector_data_t *data = calloc(1, sizeof(deauth_detector_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }
    
    data->self = screen;
    data->has_detection = false;
    data->detection_count = 0;
    data->needs_redraw = false;
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;
    
    // Register UART callback
    uart_register_line_callback(uart_line_callback, data);
    
    // Send command to start deauth detector
    uart_send_command("deauth_detector");
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Deauth detector screen created");
    return screen;
}

