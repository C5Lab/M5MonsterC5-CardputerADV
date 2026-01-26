/**
 * @file rogue_ap_screen.c
 * @brief Rogue AP attack running screen
 * 
 * Monitors UART for client connect/disconnect events.
 */

#include "rogue_ap_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "buzzer.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ROGUE_AP";

#define MAX_LOG_LINES 5
#define MAX_LINE_LEN 32

typedef struct {
    char ssid[33];
    int client_count;
    char log_lines[MAX_LOG_LINES][MAX_LINE_LEN];
    int log_count;
    bool needs_redraw;
    screen_t *self;
} rogue_ap_data_t;

static void draw_screen(screen_t *self);

/**
 * @brief Add a log line to the display buffer
 */
static void add_log_line(rogue_ap_data_t *data, const char *line)
{
    // Shift existing lines up
    if (data->log_count >= MAX_LOG_LINES) {
        for (int i = 0; i < MAX_LOG_LINES - 1; i++) {
            strncpy(data->log_lines[i], data->log_lines[i + 1], MAX_LINE_LEN - 1);
        }
        data->log_count = MAX_LOG_LINES - 1;
    }
    
    strncpy(data->log_lines[data->log_count], line, MAX_LINE_LEN - 1);
    data->log_lines[data->log_count][MAX_LINE_LEN - 1] = '\0';
    data->log_count++;
}

/**
 * @brief Extract MAC address from line
 */
static bool extract_mac(const char *line, char *mac_out, size_t max_len)
{
    const char *mac_start = strstr(line, "MAC:");
    if (!mac_start) return false;
    
    mac_start += 4;
    while (*mac_start == ' ') mac_start++;
    
    // Copy until comma, space, or end
    size_t i = 0;
    while (mac_start[i] && mac_start[i] != ',' && mac_start[i] != ' ' && 
           mac_start[i] != '\n' && i < max_len - 1) {
        mac_out[i] = mac_start[i];
        i++;
    }
    mac_out[i] = '\0';
    
    return i > 0;
}

/**
 * @brief UART callback for Rogue AP events
 */
static void uart_line_callback(const char *line, void *user_data)
{
    rogue_ap_data_t *data = (rogue_ap_data_t *)user_data;
    if (!data) return;
    
    // Check for client connected
    if (strstr(line, "AP: Client connected") != NULL) {
        char mac[24] = {0};
        extract_mac(line, mac, sizeof(mac));
        
        char log_line[MAX_LINE_LEN];
        snprintf(log_line, sizeof(log_line), "+ %.17s", mac);
        add_log_line(data, log_line);
        
        ESP_LOGI(TAG, "Client connected: %s", mac);
        buzzer_beep_success();
        data->needs_redraw = true;
    }
    
    // Check for client count
    if (strstr(line, "Portal: Client count") != NULL) {
        const char *eq = strchr(line, '=');
        if (eq) {
            data->client_count = atoi(eq + 1);
            ESP_LOGI(TAG, "Client count: %d", data->client_count);
            data->needs_redraw = true;
        }
    }
    
    // Check for client disconnected
    if (strstr(line, "AP: Client disconnected") != NULL) {
        char mac[24] = {0};
        extract_mac(line, mac, sizeof(mac));
        
        char log_line[MAX_LINE_LEN];
        snprintf(log_line, sizeof(log_line), "- %.17s", mac);
        add_log_line(data, log_line);
        
        ESP_LOGI(TAG, "Client disconnected: %s", mac);
        data->needs_redraw = true;
    }
}

static void draw_screen(screen_t *self)
{
    rogue_ap_data_t *data = (rogue_ap_data_t *)self->user_data;
    
    ui_clear();
    ui_draw_title("Rogue AP Running");
    
    // Show SSID
    char ssid_line[40];
    snprintf(ssid_line, sizeof(ssid_line), "SSID: %.24s", data->ssid);
    ui_print(1, 1, ssid_line, UI_COLOR_TEXT);
    
    // Show client count
    char count_line[24];
    snprintf(count_line, sizeof(count_line), "Clients: %d", data->client_count);
    ui_print(1, 2, count_line, UI_COLOR_TEXT);
    
    // Show log lines
    for (int i = 0; i < data->log_count && i < MAX_LOG_LINES; i++) {
        ui_print(1, 3 + i, data->log_lines[i], UI_COLOR_DIMMED);
    }
    
    ui_draw_status("ESC:Stop & Back");
}

static void on_tick(screen_t *self)
{
    rogue_ap_data_t *data = (rogue_ap_data_t *)self->user_data;
    
    if (data && data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    (void)self;
    
    switch (key) {
        case KEY_ESC:
        case KEY_Q:
            // Stop the attack
            uart_send_command("stop");
            ESP_LOGI(TAG, "Stopping Rogue AP attack");
            
            // Pop back to attack select (pop HTML, password, ssid screens too)
            screen_manager_pop();
            screen_manager_pop();
            screen_manager_pop();
            screen_manager_pop();
            break;
            
        default:
            break;
    }
}

static void on_destroy(screen_t *self)
{
    uart_clear_line_callback();
    
    if (self->user_data) {
        free(self->user_data);
    }
}

screen_t* rogue_ap_screen_create(void *params)
{
    rogue_ap_params_t *ap_params = (rogue_ap_params_t *)params;
    
    if (!ap_params) {
        ESP_LOGE(TAG, "Invalid parameters");
        return NULL;
    }
    
    ESP_LOGI(TAG, "Creating Rogue AP screen for SSID: %s", ap_params->ssid);
    
    screen_t *screen = screen_alloc();
    if (!screen) {
        free(ap_params);
        return NULL;
    }
    
    rogue_ap_data_t *data = calloc(1, sizeof(rogue_ap_data_t));
    if (!data) {
        free(screen);
        free(ap_params);
        return NULL;
    }
    
    strncpy(data->ssid, ap_params->ssid, sizeof(data->ssid) - 1);
    data->self = screen;
    free(ap_params);
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;
    
    // Register UART callback
    uart_register_line_callback(uart_line_callback, data);
    
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Rogue AP screen created");
    return screen;
}
