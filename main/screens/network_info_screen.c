/**
 * @file network_info_screen.c
 * @brief Network information detail screen implementation
 * 
 * Displays detailed information about a WiFi network:
 * SSID, BSSID, security, signal strength, channel
 * With option to connect to the network.
 */

#include "network_info_screen.h"
#include "text_input_screen.h"
#include "arp_hosts_screen.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "NET_INFO";

// Screen states
typedef enum {
    STATE_VIEW,
    STATE_CONNECTING,
    STATE_RESULT
} info_state_t;

// Screen user data
typedef struct {
    wifi_network_t network;  // Copy of network data
    char password[65];
    info_state_t state;
    bool success;
    char result_msg[64];
    bool needs_redraw;
    bool needs_push_password;
    screen_t *self;
} network_info_data_t;

// Forward declarations
static void draw_screen(screen_t *self);
static void on_password_submitted(const char *text, void *user_data);

/**
 * @brief UART callback for WiFi connection result
 */
static void uart_line_callback(const char *line, void *user_data)
{
    network_info_data_t *data = (network_info_data_t *)user_data;
    if (!data || data->state != STATE_CONNECTING) return;
    
    // Check for success
    if (strstr(line, "SUCCESS:") != NULL && strstr(line, "Connected") != NULL) {
        data->success = true;
        snprintf(data->result_msg, sizeof(data->result_msg), "Connected!");
        data->state = STATE_RESULT;
        uart_set_wifi_connected(true);
        data->needs_redraw = true;
        ESP_LOGI(TAG, "WiFi connected successfully");
    }
    // Check for failure
    else if (strstr(line, "FAILED:") != NULL) {
        data->success = false;
        snprintf(data->result_msg, sizeof(data->result_msg), "Connection failed");
        data->state = STATE_RESULT;
        uart_set_wifi_connected(false);
        data->needs_redraw = true;
        ESP_LOGW(TAG, "WiFi connection failed");
    }
}

static void draw_screen(screen_t *self)
{
    network_info_data_t *data = (network_info_data_t *)self->user_data;
    wifi_network_t *net = &data->network;
    
    ui_clear();
    
    // Draw title
    ui_draw_title("Network Info");
    
    if (data->state == STATE_CONNECTING) {
        // Show connecting status
        char ssid_line[32];
        snprintf(ssid_line, sizeof(ssid_line), "%.28s", net->ssid[0] ? net->ssid : "[Hidden]");
        ui_print_center(2, ssid_line, UI_COLOR_HIGHLIGHT);
        ui_print_center(4, "Connecting...", UI_COLOR_DIMMED);
        ui_draw_status("Please wait...");
        return;
    }
    
    if (data->state == STATE_RESULT) {
        // Show result
        char ssid_line[32];
        snprintf(ssid_line, sizeof(ssid_line), "%.28s", net->ssid[0] ? net->ssid : "[Hidden]");
        ui_print_center(2, ssid_line, UI_COLOR_HIGHLIGHT);
        if (data->success) {
            ui_print_center(4, data->result_msg, UI_COLOR_HIGHLIGHT);
            ui_print_center(5, "ENTER: ARP Menu", UI_COLOR_TEXT);
        } else {
            ui_print_center(4, data->result_msg, UI_COLOR_TEXT);
            ui_print_center(5, "ENTER: Try again", UI_COLOR_DIMMED);
        }
        ui_draw_status("ENTER:Continue ESC:Back");
        return;
    }
    
    // STATE_VIEW - normal info display
    char line[32];
    
    // Row 1: SSID
    if (net->ssid[0]) {
        snprintf(line, sizeof(line), "SSID: %.21s", net->ssid);
    } else {
        snprintf(line, sizeof(line), "SSID: [Hidden]");
    }
    ui_print(0, 1, line, UI_COLOR_TEXT);
    
    // Row 2: BSSID
    snprintf(line, sizeof(line), "BSSID: %s", net->bssid);
    ui_print(0, 2, line, UI_COLOR_TEXT);
    
    // Row 3: Security
    snprintf(line, sizeof(line), "Security: %.18s", net->security);
    ui_print(0, 3, line, UI_COLOR_TEXT);
    
    // Row 4: Signal strength
    snprintf(line, sizeof(line), "Signal: %d dBm", net->rssi);
    ui_print(0, 4, line, UI_COLOR_TEXT);
    
    // Row 5: Channel
    snprintf(line, sizeof(line), "Channel: %d", net->channel);
    ui_print(0, 5, line, UI_COLOR_TEXT);
    
    // Row 6: Connect hint
    ui_print_center(6, "[ENTER to Connect]", UI_COLOR_HIGHLIGHT);
    
    // Draw status bar
    ui_draw_status("ENTER:Connect ESC:Back");
}

static void on_tick(screen_t *self)
{
    network_info_data_t *data = (network_info_data_t *)self->user_data;
    
    // Push password input on first tick after flag is set
    if (data->needs_push_password) {
        data->needs_push_password = false;
        
        text_input_params_t *params = malloc(sizeof(text_input_params_t));
        if (params) {
            params->title = "Enter Password";
            params->hint = "WiFi password";
            params->on_submit = on_password_submitted;
            params->user_data = data;
            screen_manager_push(text_input_screen_create, params);
        }
        return;
    }
    
    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

/**
 * @brief Called when password is submitted
 */
static void on_password_submitted(const char *text, void *user_data)
{
    network_info_data_t *data = (network_info_data_t *)user_data;
    
    // Store password
    strncpy(data->password, text, sizeof(data->password) - 1);
    data->password[sizeof(data->password) - 1] = '\0';
    
    ESP_LOGI(TAG, "Password entered, connecting to %s", data->network.ssid);
    
    // Pop the text input screen
    screen_manager_pop();
    
    // Update state
    data->state = STATE_CONNECTING;
    draw_screen(data->self);
    
    // Register UART callback
    uart_register_line_callback(uart_line_callback, data);
    
    // Send connect command
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "wifi_connect %s %s", data->network.ssid, data->password);
    uart_send_command(cmd);
}

static void on_key(screen_t *self, key_code_t key)
{
    network_info_data_t *data = (network_info_data_t *)self->user_data;
    
    // Handle result state
    if (data->state == STATE_RESULT) {
        if (key == KEY_ENTER || key == KEY_SPACE) {
            if (data->success) {
                // Push ARP hosts screen
                uart_clear_line_callback();
                screen_manager_push(arp_hosts_screen_create, NULL);
            } else {
                // Try again - go back to view state
                data->state = STATE_VIEW;
                draw_screen(self);
            }
        } else if (key == KEY_ESC || key == KEY_BACKSPACE) {
            uart_clear_line_callback();
            screen_manager_pop();
        }
        return;
    }
    
    // Handle connecting state - ignore most keys
    if (data->state == STATE_CONNECTING) {
        if (key == KEY_ESC) {
            // Cancel and go back
            uart_clear_line_callback();
            screen_manager_pop();
        }
        return;
    }
    
    // Handle view state
    switch (key) {
        case KEY_ENTER:
        case KEY_SPACE:
            // Request password input - will be pushed on next tick
            data->needs_push_password = true;
            break;
            
        case KEY_ESC:
        case KEY_Q:
        case KEY_BACKSPACE:
            screen_manager_pop();
            break;
            
        default:
            break;
    }
}

static void on_destroy(screen_t *self)
{
    network_info_data_t *data = (network_info_data_t *)self->user_data;
    
    // Clear UART callback if we registered one
    uart_clear_line_callback();
    
    if (data) {
        free(data);
    }
}

static void on_resume(screen_t *self)
{
    // Redraw when returning from password input or ARP screen
    draw_screen(self);
}

screen_t* network_info_screen_create(void *params)
{
    network_info_params_t *info_params = (network_info_params_t *)params;
    
    if (!info_params || !info_params->network) {
        ESP_LOGE(TAG, "Invalid parameters");
        if (info_params) free(info_params);
        return NULL;
    }
    
    ESP_LOGI(TAG, "Creating network info screen for '%s'...", 
             info_params->network->ssid[0] ? info_params->network->ssid : "[Hidden]");
    
    screen_t *screen = screen_alloc();
    if (!screen) {
        free(info_params);
        return NULL;
    }
    
    network_info_data_t *data = calloc(1, sizeof(network_info_data_t));
    if (!data) {
        free(screen);
        free(info_params);
        return NULL;
    }
    
    // Copy network data (don't take ownership)
    data->network = *info_params->network;
    data->self = screen;
    data->state = STATE_VIEW;
    
    // Free params struct
    free(info_params);
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;
    screen->on_resume = on_resume;
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Network info screen created");
    return screen;
}
