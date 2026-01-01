/**
 * @file karma_attack_screen.c
 * @brief Karma attack running screen implementation
 */

#include "karma_attack_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "KARMA_ATK";

// Maximum lengths
#define MAX_SSID_LEN    33
#define MAX_MAC_LEN     18
#define MAX_PWD_LEN     64

// Refresh timer interval (200ms)
#define REFRESH_INTERVAL_US 200000

// Screen user data
typedef struct {
    char ssid[MAX_SSID_LEN];
    char last_mac[MAX_MAC_LEN];
    char password[MAX_PWD_LEN];
    bool portal_started;
    bool needs_redraw;
    esp_timer_handle_t refresh_timer;
    screen_t *self;
} karma_attack_data_t;

// Forward declaration
static void draw_screen(screen_t *self);

/**
 * @brief Timer callback - checks if redraw is needed
 */
static void refresh_timer_callback(void *arg)
{
    karma_attack_data_t *data = (karma_attack_data_t *)arg;
    if (data && data->needs_redraw && data->self) {
        data->needs_redraw = false;
        draw_screen(data->self);
    }
}

/**
 * @brief UART line callback for parsing karma attack output
 * Patterns:
 * - "Captive portal started" or "AP Name:" -> portal started
 * - "AP: Client connected - MAC: XX:XX:XX:XX:XX:XX" -> store MAC
 * - "Password: xyz" -> store password
 */
static void uart_line_callback(const char *line, void *user_data)
{
    karma_attack_data_t *data = (karma_attack_data_t *)user_data;
    if (!data) return;
    
    // Check for portal started
    if (strstr(line, "Captive portal started") != NULL || 
        strstr(line, "AP Name:") != NULL) {
        data->portal_started = true;
        data->needs_redraw = true;
        ESP_LOGI(TAG, "Portal started detected");
        return;
    }
    
    // Check for client connected: "AP: Client connected - MAC: XX:XX:XX:XX:XX:XX"
    const char *mac_marker = "Client connected - MAC: ";
    const char *mac_found = strstr(line, mac_marker);
    if (mac_found) {
        const char *mac_start = mac_found + strlen(mac_marker);
        // Copy MAC address (17 chars)
        strncpy(data->last_mac, mac_start, MAX_MAC_LEN - 1);
        data->last_mac[MAX_MAC_LEN - 1] = '\0';
        
        // Truncate at space or newline
        char *end = data->last_mac;
        while (*end && *end != ' ' && *end != '\n' && *end != '\r') end++;
        *end = '\0';
        
        ESP_LOGI(TAG, "Client connected: %s", data->last_mac);
        data->needs_redraw = true;
        return;
    }
    
    // Check for password: "Portal password received: xyz" or "Password: xyz"
    const char *pwd_marker = "Portal password received: ";
    const char *pwd_found = strstr(line, pwd_marker);
    if (!pwd_found) {
        // Fallback to simpler pattern
        pwd_marker = "Password: ";
        pwd_found = strstr(line, pwd_marker);
    }
    if (pwd_found) {
        const char *pwd_start = pwd_found + strlen(pwd_marker);
        strncpy(data->password, pwd_start, MAX_PWD_LEN - 1);
        data->password[MAX_PWD_LEN - 1] = '\0';
        
        // Remove trailing whitespace
        char *end = data->password + strlen(data->password) - 1;
        while (end > data->password && (*end == '\n' || *end == '\r' || *end == ' ')) {
            *end = '\0';
            end--;
        }
        
        ESP_LOGI(TAG, "Password obtained: %s", data->password);
        data->needs_redraw = true;
        return;
    }
}

static void draw_screen(screen_t *self)
{
    karma_attack_data_t *data = (karma_attack_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title
    ui_draw_title("Karma Attack");
    
    int row = 2;
    
    // Portal status
    if (data->portal_started) {
        char portal_line[48];
        snprintf(portal_line, sizeof(portal_line), "Portal started: %.20s", data->ssid);
        ui_print(0, row, portal_line, UI_COLOR_HIGHLIGHT);
    } else {
        ui_print(0, row, "Starting portal...", UI_COLOR_DIMMED);
    }
    row += 2;
    
    // Last connected MAC
    if (data->last_mac[0] != '\0') {
        char mac_line[48];
        snprintf(mac_line, sizeof(mac_line), "Last MAC: %s", data->last_mac);
        ui_print(0, row, mac_line, UI_COLOR_TEXT);
    } else {
        ui_print(0, row, "Last MAC connected: -", UI_COLOR_DIMMED);
    }
    row += 2;
    
    // Password
    if (data->password[0] != '\0') {
        char pwd_line[48];
        snprintf(pwd_line, sizeof(pwd_line), "Password: %.25s", data->password);
        ui_print(0, row, pwd_line, UI_COLOR_HIGHLIGHT);
    } else {
        ui_print(0, row, "Password obtained: -", UI_COLOR_DIMMED);
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
    karma_attack_data_t *data = (karma_attack_data_t *)self->user_data;
    
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

screen_t* karma_attack_screen_create(void *params)
{
    karma_attack_params_t *atk_params = (karma_attack_params_t *)params;
    
    if (!atk_params) {
        ESP_LOGE(TAG, "No parameters provided");
        return NULL;
    }
    
    ESP_LOGI(TAG, "Creating karma attack screen for SSID: %s", atk_params->ssid);
    
    screen_t *screen = screen_alloc();
    if (!screen) {
        free(atk_params);
        return NULL;
    }
    
    // Allocate user data
    karma_attack_data_t *data = calloc(1, sizeof(karma_attack_data_t));
    if (!data) {
        free(screen);
        free(atk_params);
        return NULL;
    }
    
    strncpy(data->ssid, atk_params->ssid, sizeof(data->ssid) - 1);
    data->ssid[sizeof(data->ssid) - 1] = '\0';
    data->self = screen;
    
    free(atk_params);
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    
    // Create periodic refresh timer
    esp_timer_create_args_t timer_args = {
        .callback = refresh_timer_callback,
        .arg = data,
        .name = "karma_refresh"
    };
    
    if (esp_timer_create(&timer_args, &data->refresh_timer) == ESP_OK) {
        esp_timer_start_periodic(data->refresh_timer, REFRESH_INTERVAL_US);
    } else {
        ESP_LOGW(TAG, "Failed to create refresh timer");
    }
    
    // Register UART callback for parsing attack output
    uart_register_line_callback(uart_line_callback, data);
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Karma attack screen created");
    return screen;
}

