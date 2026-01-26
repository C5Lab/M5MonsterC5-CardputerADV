/**
 * @file wifi_connect_screen.c
 * @brief WiFi connection screen implementation
 * 
 * Flow: SSID input -> Password input -> send wifi_connect -> show result
 */

#include "wifi_connect_screen.h"
#include "text_input_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "WIFI_CONNECT";

// Connection state
typedef enum {
    STATE_ENTER_SSID,
    STATE_ENTER_PASSWORD,
    STATE_CONNECTING,
    STATE_RESULT
} connect_state_t;

// Screen user data
typedef struct {
    connect_state_t state;
    char ssid[33];
    char password[65];
    bool success;
    char result_msg[64];
    bool needs_redraw;
    bool needs_push_ssid_input;  // Flag to push SSID input on first tick
    screen_t *self;
} wifi_connect_data_t;

// Forward declarations
static void draw_screen(screen_t *self);
static void on_ssid_submitted(const char *text, void *user_data);
static void on_password_submitted(const char *text, void *user_data);

/**
 * @brief UART callback for connection result
 */
static void uart_line_callback(const char *line, void *user_data)
{
    wifi_connect_data_t *data = (wifi_connect_data_t *)user_data;
    if (!data || data->state != STATE_CONNECTING) return;
    
    // Check for success
    if (strstr(line, "SUCCESS:") != NULL && strstr(line, "Connected") != NULL) {
        data->success = true;
        snprintf(data->result_msg, sizeof(data->result_msg), "Connected to %s", data->ssid);
        data->state = STATE_RESULT;
        uart_set_wifi_connected(true);
        data->needs_redraw = true;
        ESP_LOGI(TAG, "WiFi connected successfully");
    }
    // Check for failure
    else if (strstr(line, "FAILED:") != NULL) {
        data->success = false;
        snprintf(data->result_msg, sizeof(data->result_msg), "Failed to connect");
        data->state = STATE_RESULT;
        uart_set_wifi_connected(false);
        data->needs_redraw = true;
        ESP_LOGW(TAG, "WiFi connection failed");
    }
}

static void draw_screen(screen_t *self)
{
    wifi_connect_data_t *data = (wifi_connect_data_t *)self->user_data;
    
    ui_clear();
    ui_draw_title("WiFi Connect");
    
    switch (data->state) {
        case STATE_ENTER_SSID:
            ui_print_center(3, "Enter network SSID", UI_COLOR_TEXT);
            break;
            
        case STATE_ENTER_PASSWORD:
            ui_print_center(2, data->ssid, UI_COLOR_HIGHLIGHT);
            ui_print_center(4, "Enter password", UI_COLOR_TEXT);
            break;
            
        case STATE_CONNECTING:
            ui_print_center(2, data->ssid, UI_COLOR_HIGHLIGHT);
            ui_print_center(4, "Connecting...", UI_COLOR_DIMMED);
            break;
            
        case STATE_RESULT:
            if (data->success) {
                ui_print_center(3, data->result_msg, UI_COLOR_HIGHLIGHT);
            } else {
                ui_print_center(3, data->result_msg, UI_COLOR_TEXT);
            }
            break;
    }
    
    ui_draw_status("ESC:Back");
}

static void on_tick(screen_t *self)
{
    wifi_connect_data_t *data = (wifi_connect_data_t *)self->user_data;
    
    // Push SSID input screen on first tick (after we're on the stack)
    if (data->needs_push_ssid_input) {
        data->needs_push_ssid_input = false;
        
        text_input_params_t *input_params = malloc(sizeof(text_input_params_t));
        if (input_params) {
            input_params->title = "Enter SSID";
            input_params->hint = "Network name";
            input_params->on_submit = on_ssid_submitted;
            input_params->user_data = data;
            screen_manager_push(text_input_screen_create, input_params);
        }
        return;
    }
    
    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

/**
 * @brief Called when SSID is submitted
 */
static void on_ssid_submitted(const char *text, void *user_data)
{
    wifi_connect_data_t *data = (wifi_connect_data_t *)user_data;
    
    // Store SSID
    strncpy(data->ssid, text, sizeof(data->ssid) - 1);
    data->ssid[sizeof(data->ssid) - 1] = '\0';
    
    ESP_LOGI(TAG, "SSID entered: %s", data->ssid);
    
    // Pop the text input screen
    screen_manager_pop();
    
    // Update state
    data->state = STATE_ENTER_PASSWORD;
    
    // Push password input screen
    text_input_params_t *params = malloc(sizeof(text_input_params_t));
    if (params) {
        params->title = "Enter Password";
        params->hint = "WiFi password";
        params->on_submit = on_password_submitted;
        params->user_data = data;
        screen_manager_push(text_input_screen_create, params);
    }
}

/**
 * @brief Called when password is submitted
 */
static void on_password_submitted(const char *text, void *user_data)
{
    wifi_connect_data_t *data = (wifi_connect_data_t *)user_data;
    
    // Store password
    strncpy(data->password, text, sizeof(data->password) - 1);
    data->password[sizeof(data->password) - 1] = '\0';
    
    ESP_LOGI(TAG, "Password entered, connecting to %s", data->ssid);
    
    // Pop the text input screen
    screen_manager_pop();
    
    // Update state
    data->state = STATE_CONNECTING;
    draw_screen(data->self);
    
    // Register UART callback
    uart_register_line_callback(uart_line_callback, data);
    
    // Send connect command
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "wifi_connect %s %s", data->ssid, data->password);
    uart_send_command(cmd);
}

static void on_key(screen_t *self, key_code_t key)
{
    wifi_connect_data_t *data = (wifi_connect_data_t *)self->user_data;
    
    switch (key) {
        case KEY_ESC:
        case KEY_BACKSPACE:
            screen_manager_pop();
            break;
            
        case KEY_ENTER:
        case KEY_SPACE:
            // If showing result, go back
            if (data->state == STATE_RESULT) {
                screen_manager_pop();
            }
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

static void on_resume(screen_t *self)
{
    wifi_connect_data_t *data = (wifi_connect_data_t *)self->user_data;
    
    // When returning from text input, check state and act
    if (data->state == STATE_ENTER_SSID) {
        // Returned from SSID input without submitting, just redraw
        draw_screen(self);
    } else if (data->state == STATE_ENTER_PASSWORD) {
        // State was updated by callback, just redraw
        draw_screen(self);
    } else if (data->state == STATE_CONNECTING) {
        draw_screen(self);
    } else {
        draw_screen(self);
    }
}

screen_t* wifi_connect_screen_create(void *params)
{
    (void)params;
    
    ESP_LOGI(TAG, "Creating WiFi connect screen...");
    
    screen_t *screen = screen_alloc();
    if (!screen) return NULL;
    
    // Allocate user data
    wifi_connect_data_t *data = calloc(1, sizeof(wifi_connect_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }
    
    data->state = STATE_ENTER_SSID;
    data->self = screen;
    data->needs_push_ssid_input = true;  // Push on first tick, after we're on stack
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "WiFi connect screen created");
    return screen;
}
