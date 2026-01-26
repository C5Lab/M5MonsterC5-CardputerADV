/**
 * @file evil_twin_screen.c
 * @brief Evil Twin attack running screen implementation
 */

#include "evil_twin_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "buzzer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "EVIL_TWIN";

// Refresh timer interval (200ms)
#define REFRESH_INTERVAL_US 200000

// Screen states
typedef enum {
    STATE_RUNNING,   // Attack active
    STATE_SUCCESS,   // Password verified
    STATE_STOPPED    // Attack ended
} evil_twin_state_t;

// Screen user data
typedef struct {
    wifi_network_t *networks;
    int count;
    evil_twin_state_t state;
    char captured_ssid[MAX_SSID_LEN];
    char captured_password[64];
    bool needs_redraw;  // Flag for thread-safe redraw
    esp_timer_handle_t refresh_timer;
    screen_t *self;  // Reference to self for callback
} evil_twin_screen_data_t;

// Forward declaration
static void draw_screen(screen_t *self);

/**
 * @brief Timer callback - checks if redraw is needed
 */
static void refresh_timer_callback(void *arg)
{
    evil_twin_screen_data_t *data = (evil_twin_screen_data_t *)arg;
    if (data && data->needs_redraw && data->self) {
        data->needs_redraw = false;
        draw_screen(data->self);
    }
}

/**
 * @brief Extract value between single quotes from a string
 * @param line Input line
 * @param prefix Prefix before the quoted value (e.g., "SSID='")
 * @param out Output buffer
 * @param out_size Size of output buffer
 * @return true if found and extracted
 */
static bool extract_quoted_value(const char *line, const char *prefix, char *out, size_t out_size)
{
    const char *start = strstr(line, prefix);
    if (!start) return false;
    
    start += strlen(prefix);
    const char *end = strchr(start, '\'');
    if (!end) return false;
    
    size_t len = end - start;
    if (len >= out_size) len = out_size - 1;
    
    strncpy(out, start, len);
    out[len] = '\0';
    return true;
}

/**
 * @brief UART line callback for parsing Evil Twin output
 */
static void uart_line_callback(const char *line, void *user_data)
{
    evil_twin_screen_data_t *data = (evil_twin_screen_data_t *)user_data;
    if (!data) return;
    
    // Check for password capture: "Wi-Fi: connected to SSID='...' with password='...'"
    if (strstr(line, "Wi-Fi: connected to SSID='") && strstr(line, "with password='")) {
        char ssid[MAX_SSID_LEN] = {0};
        char password[64] = {0};
        
        if (extract_quoted_value(line, "SSID='", ssid, sizeof(ssid)) &&
            extract_quoted_value(line, "password='", password, sizeof(password))) {
            
            strncpy(data->captured_ssid, ssid, sizeof(data->captured_ssid) - 1);
            strncpy(data->captured_password, password, sizeof(data->captured_password) - 1);
            ESP_LOGI(TAG, "Captured credentials - SSID: %s, Password: %s", ssid, password);
        }
    }
    
    // Check for password verified
    if (strstr(line, "Password verified!")) {
        if (data->captured_ssid[0] && data->captured_password[0]) {
            data->state = STATE_SUCCESS;
            ESP_LOGI(TAG, "Password verified! Attack successful.");
            data->needs_redraw = true;
            buzzer_beep_success();
        }
    }
    
    // Check for attack ended
    if (strstr(line, "Evil Twin portal shut down")) {
        if (data->state != STATE_SUCCESS) {
            data->state = STATE_STOPPED;
        }
        ESP_LOGI(TAG, "Evil Twin portal shut down.");
        data->needs_redraw = true;
    }
}

static void draw_screen(screen_t *self)
{
    evil_twin_screen_data_t *data = (evil_twin_screen_data_t *)self->user_data;
    
    ui_clear();
    
    if (data->state == STATE_SUCCESS) {
        // Draw success screen
        ui_draw_title("SUCCESS!");
        
        // Draw captured credentials
        int row = 2;
        ui_print_center(row, "Password Captured!", UI_COLOR_HIGHLIGHT);
        
        row = 3;
        char ssid_line[32];
        snprintf(ssid_line, sizeof(ssid_line), "SSID: %.20s", data->captured_ssid);
        ui_print_center(row, ssid_line, UI_COLOR_TEXT);
        
        row = 4;
        char pass_line[32];
        snprintf(pass_line, sizeof(pass_line), "Pass: %.20s", data->captured_password);
        ui_print_center(row, pass_line, UI_COLOR_HIGHLIGHT);
        
        // Draw status bar
        ui_draw_status("ESC: Back");
    } else {
        // Draw running/stopped screen
        if (data->state == STATE_STOPPED) {
            ui_draw_title("Evil Twin Stopped");
        } else {
            ui_draw_title("Evil Twin Running");
        }
        
        // Draw first network (Evil Network)
        int row = 1;
        if (data->count > 0) {
            wifi_network_t *first_net = &data->networks[0];
            char label[32];
            if (first_net->ssid[0]) {
                snprintf(label, sizeof(label), "Evil Network: %.14s", first_net->ssid);
            } else {
                snprintf(label, sizeof(label), "Evil Network: [%.10s]", first_net->bssid);
            }
            int y = row * 16;
            display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
            ui_draw_text(0, y, label, UI_COLOR_HIGHLIGHT, UI_COLOR_BG);
        }
        
        // Draw "Other attacked networks:" label if there are more networks
        if (data->count > 1) {
            row = 2;
            int y = row * 16;
            display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
            ui_draw_text(0, y, "Other attacked networks:", UI_COLOR_DIMMED, UI_COLOR_BG);
            
            // Build comma-separated list of other networks
            row = 3;
            y = row * 16;
            char others[128] = {0};
            int others_len = 0;
            
            for (int i = 1; i < data->count && others_len < 100; i++) {
                wifi_network_t *net = &data->networks[i];
                const char *name = net->ssid[0] ? net->ssid : net->bssid;
                
                if (others_len > 0) {
                    others_len += snprintf(others + others_len, sizeof(others) - others_len, ", ");
                }
                others_len += snprintf(others + others_len, sizeof(others) - others_len, "%s", name);
            }
            
            // Draw the list (may wrap, but we'll truncate for simplicity)
            display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
            ui_draw_text(0, y, others, RGB565(255, 68, 68), UI_COLOR_BG);
            
            // If text is too long, show continuation on next row
            if (strlen(others) > 30) {
                row = 4;
                y = row * 16;
                display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
                if (strlen(others) > 30) {
                    ui_draw_text(0, y, others + 30, RGB565(255, 68, 68), UI_COLOR_BG);
                }
            }
        }
        
        // Fill gap to status bar
        int gap_y = 5 * 16;
        int status_y = DISPLAY_HEIGHT - 16 - 2;
        if (gap_y < status_y) {
            display_fill_rect(0, gap_y, DISPLAY_WIDTH, status_y - gap_y, UI_COLOR_BG);
        }
        
        // Draw status bar
        ui_draw_status("ESC: Stop");
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    evil_twin_screen_data_t *data = (evil_twin_screen_data_t *)self->user_data;
    
    switch (key) {
        case KEY_ESC:
        case KEY_Q:
            // Send stop command and go back
            if (data->state == STATE_RUNNING) {
                uart_send_command("stop");
            }
            screen_manager_pop();
            break;
            
        default:
            break;
    }
}

static void on_destroy(screen_t *self)
{
    evil_twin_screen_data_t *data = (evil_twin_screen_data_t *)self->user_data;
    
    // Stop and delete timer
    if (data && data->refresh_timer) {
        esp_timer_stop(data->refresh_timer);
        esp_timer_delete(data->refresh_timer);
    }
    
    // Clear UART callback
    uart_clear_line_callback();
    
    if (data) {
        if (data->networks) {
            free(data->networks);
        }
        free(data);
    }
}

screen_t* evil_twin_screen_create(void *params)
{
    evil_twin_screen_params_t *et_params = (evil_twin_screen_params_t *)params;
    
    if (!et_params) {
        ESP_LOGE(TAG, "Invalid parameters");
        return NULL;
    }
    
    ESP_LOGI(TAG, "Creating evil twin screen for %d networks...", et_params->count);
    
    screen_t *screen = screen_alloc();
    if (!screen) {
        if (et_params->networks) free(et_params->networks);
        free(et_params);
        return NULL;
    }
    
    // Allocate user data
    evil_twin_screen_data_t *data = calloc(1, sizeof(evil_twin_screen_data_t));
    if (!data) {
        free(screen);
        if (et_params->networks) free(et_params->networks);
        free(et_params);
        return NULL;
    }
    
    // Take ownership
    data->networks = et_params->networks;
    data->count = et_params->count;
    data->state = STATE_RUNNING;
    data->self = screen;
    free(et_params);
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    
    // Create periodic refresh timer
    esp_timer_create_args_t timer_args = {
        .callback = refresh_timer_callback,
        .arg = data,
        .name = "evil_twin_refresh"
    };
    
    if (esp_timer_create(&timer_args, &data->refresh_timer) == ESP_OK) {
        esp_timer_start_periodic(data->refresh_timer, REFRESH_INTERVAL_US);
    } else {
        ESP_LOGW(TAG, "Failed to create refresh timer");
    }
    
    // Register UART callback for parsing Evil Twin output
    uart_register_line_callback(uart_line_callback, data);
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Evil twin screen created");
    return screen;
}

