/**
 * @file channel_time_settings_screen.c
 * @brief Channel time settings screen implementation
 */

#include "channel_time_settings_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "keyboard.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

static const char *TAG = "CHANNEL_TIME_SETTINGS";

#define MIN_TIME_MS     100
#define MAX_TIME_MS     1500
#define STEP_LARGE      10      // Increment for arrow keys
#define STEP_SMALL      1       // Increment for fine adjustments

// Field indices
#define FIELD_MIN       0
#define FIELD_MAX       1
#define FIELD_COUNT     2

// Screen user data
typedef struct {
    int min_value;              // Current/loaded min value
    int max_value;              // Current/loaded max value
    int edited_min;             // Edited min value (being modified)
    int edited_max;             // Edited max value (being modified)
    int selected_field;         // Currently selected field (FIELD_MIN or FIELD_MAX)
    bool loading;               // Waiting for UART responses
    int loading_count;          // How many responses we still expect
    bool saved;                 // Shows "Saved!" message
    bool needs_redraw;          // Flag for deferred redraw from UART callback
    char status_msg[64];        // Error/status message
} channel_time_data_t;

/**
 * @brief Parse integer from response, handling newlines and whitespace
 * Looks for pattern like "min: 100" or just "100"
 */
static int parse_integer_response(const char *line)
{
    if (!line) return -1;
    
    // Skip leading whitespace
    while (*line && isspace((unsigned char)*line)) {
        line++;
    }
    
    // Find start of number
    const char *start = line;
    while (*start && !isdigit((unsigned char)*start)) {
        start++;
    }
    
    if (!*start) return -1;
    
    char *endptr;
    int value = strtol(start, &endptr, 10);
    return value;
}

/**
 * @brief Validate the current edited values
 * @return true if valid, false if invalid (and sets status_msg)
 */
static bool validate_values(channel_time_data_t *data)
{
    if (data->edited_min < MIN_TIME_MS || data->edited_min > MAX_TIME_MS) {
        snprintf(data->status_msg, sizeof(data->status_msg), 
                 "Min must be 100-1500 ms");
        return false;
    }
    
    if (data->edited_max < MIN_TIME_MS || data->edited_max > MAX_TIME_MS) {
        snprintf(data->status_msg, sizeof(data->status_msg), 
                 "Max must be 100-1500 ms");
        return false;
    }
    
    if (data->edited_min >= data->edited_max) {
        snprintf(data->status_msg, sizeof(data->status_msg), 
                 "Min must be < Max");
        return false;
    }
    
    return true;
}

/**
 * @brief UART response callback - runs in UART RX task context
 * CRITICAL: DO NOT call display functions here!
 */
static void on_uart_response(const char *line, void *user_data)
{
    screen_t *self = (screen_t *)user_data;
    channel_time_data_t *data = (channel_time_data_t *)self->user_data;
    
    if (!line || data->loading_count <= 0) {
        return;
    }
    
    ESP_LOGI(TAG, "Response: %s", line);
    
    // Parse response - we expect either "get min" or "get max" responses
    if (strstr(line, "min") != NULL) {
        int value = parse_integer_response(line);
        if (value >= 0) {
            data->min_value = value;
            data->edited_min = value;
            data->loading_count--;
            ESP_LOGI(TAG, "Parsed min: %d", value);
        }
    } else if (strstr(line, "max") != NULL) {
        int value = parse_integer_response(line);
        if (value >= 0) {
            data->max_value = value;
            data->edited_max = value;
            data->loading_count--;
            ESP_LOGI(TAG, "Parsed max: %d", value);
        }
    } else {
        // Try to parse as plain number if neither min/max in response
        int value = parse_integer_response(line);
        if (value >= 0 && data->loading_count > 0) {
            // Assume first response is min, second is max
            if (data->loading_count == 2) {
                data->min_value = value;
                data->edited_min = value;
                data->loading_count--;
                ESP_LOGI(TAG, "Parsed value as min: %d", value);
            } else if (data->loading_count == 1) {
                data->max_value = value;
                data->edited_max = value;
                data->loading_count--;
                ESP_LOGI(TAG, "Parsed value as max: %d", value);
            }
        }
    }
    
    // If we got all responses, signal redraw
    if (data->loading_count == 0) {
        data->loading = false;
        data->status_msg[0] = '\0';
        uart_clear_line_callback();
        data->needs_redraw = true;
    }
}

/**
 * @brief Periodic tick handler - runs in main task context
 * Safe place to call display functions!
 */
static void on_tick(screen_t *self)
{
    channel_time_data_t *data = (channel_time_data_t *)self->user_data;
    
    if (data->needs_redraw) {
        data->needs_redraw = false;
        // Draw will be called by screen manager
    }
}

static void draw_screen(screen_t *self)
{
    channel_time_data_t *data = (channel_time_data_t *)self->user_data;
    
    ui_clear();
    ui_draw_title("Channel Time");
    
    if (data->loading) {
        ui_print_center(3, "Loading...", UI_COLOR_DIMMED);
    } else {
        // Draw min field
        const char *min_label = "Min (ms):";
        ui_print(0, 2, min_label, UI_COLOR_TEXT);
        
        // Highlight selected field with indicator
        uint16_t min_color = (data->selected_field == FIELD_MIN) ? UI_COLOR_TITLE : UI_COLOR_TEXT;
        char min_str[32];
        const char *min_indicator = (data->selected_field == FIELD_MIN) ? "> " : "  ";
        snprintf(min_str, sizeof(min_str), "%s%4d", min_indicator, data->edited_min);
        ui_print(12, 2, min_str, min_color);
        
        // Draw max field
        const char *max_label = "Max (ms):";
        ui_print(0, 3, max_label, UI_COLOR_TEXT);
        
        uint16_t max_color = (data->selected_field == FIELD_MAX) ? UI_COLOR_TITLE : UI_COLOR_TEXT;
        char max_str[32];
        const char *max_indicator = (data->selected_field == FIELD_MAX) ? "> " : "  ";
        snprintf(max_str, sizeof(max_str), "%s%4d", max_indicator, data->edited_max);
        ui_print(12, 3, max_str, max_color);
        
        // Draw help text
        ui_print(0, 5, "UP/DOWN: Select field", UI_COLOR_DIMMED);
        ui_print(0, 6, "</>: Adjust value", UI_COLOR_DIMMED);
        
        // Draw status or saved message
        if (data->status_msg[0]) {
            ui_print(0, 8, data->status_msg, UI_COLOR_BORDER);
        } else if (data->saved) {
            ui_print(0, 8, "Saved!", UI_COLOR_TITLE);
        }
    }
    
    ui_draw_status("UP/DOWN:Field </>:Adj ENTER:Save ESC:Back");
}

static void on_key(screen_t *self, key_code_t key)
{
    channel_time_data_t *data = (channel_time_data_t *)self->user_data;
    
    if (data->loading) {
        if (key == KEY_ESC || key == KEY_Q || key == KEY_BACKSPACE) {
            uart_clear_line_callback();
            screen_manager_pop();
        }
        return;
    }
    
    switch (key) {
        case KEY_UP:
            if (data->selected_field > 0) {
                data->selected_field--;
                data->saved = false;
                data->status_msg[0] = '\0';
                draw_screen(self);
            } else {
                data->selected_field = FIELD_COUNT - 1;
                data->saved = false;
                data->status_msg[0] = '\0';
                draw_screen(self);
            }
            break;
            
        case KEY_DOWN:
            if (data->selected_field < FIELD_COUNT - 1) {
                data->selected_field++;
                data->saved = false;
                data->status_msg[0] = '\0';
                draw_screen(self);
            } else {
                data->selected_field = 0;
                data->saved = false;
                data->status_msg[0] = '\0';
                draw_screen(self);
            }
            break;
            
        case KEY_LEFT:
            {
                data->saved = false;
                data->status_msg[0] = '\0';
                
                if (data->selected_field == FIELD_MIN) {
                    data->edited_min -= STEP_LARGE;
                    if (data->edited_min < MIN_TIME_MS) {
                        data->edited_min = MIN_TIME_MS;
                    }
                } else {
                    data->edited_max -= STEP_LARGE;
                    if (data->edited_max < MIN_TIME_MS) {
                        data->edited_max = MIN_TIME_MS;
                    }
                }
                draw_screen(self);
            }
            break;
            
        case KEY_RIGHT:
            {
                data->saved = false;
                data->status_msg[0] = '\0';
                
                if (data->selected_field == FIELD_MIN) {
                    data->edited_min += STEP_LARGE;
                    if (data->edited_min > MAX_TIME_MS) {
                        data->edited_min = MAX_TIME_MS;
                    }
                } else {
                    data->edited_max += STEP_LARGE;
                    if (data->edited_max > MAX_TIME_MS) {
                        data->edited_max = MAX_TIME_MS;
                    }
                }
                draw_screen(self);
            }
            break;
            
        case KEY_ENTER:
        case KEY_SPACE:
            {
                // Validate before sending
                if (!validate_values(data)) {
                    draw_screen(self);
                    break;
                }
                
                // Send commands to set both values
                char cmd_min[64], cmd_max[64];
                snprintf(cmd_min, sizeof(cmd_min), "channel_time set min %d", data->edited_min);
                snprintf(cmd_max, sizeof(cmd_max), "channel_time set max %d", data->edited_max);
                
                esp_err_t ret1 = uart_send_command(cmd_min);
                esp_err_t ret2 = uart_send_command(cmd_max);
                
                if (ret1 == ESP_OK && ret2 == ESP_OK) {
                    data->min_value = data->edited_min;
                    data->max_value = data->edited_max;
                    data->saved = true;
                    data->status_msg[0] = '\0';
                    ESP_LOGI(TAG, "Saved: min=%d, max=%d", data->edited_min, data->edited_max);
                } else {
                    snprintf(data->status_msg, sizeof(data->status_msg), "Send failed!");
                    ESP_LOGE(TAG, "Failed to send commands");
                }
                draw_screen(self);
            }
            break;
            
        case KEY_ESC:
        case KEY_Q:
        case KEY_BACKSPACE:
            uart_clear_line_callback();
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

static void on_resume(screen_t *self)
{
    channel_time_data_t *data = (channel_time_data_t *)self->user_data;
    
    // Reset state and request current values
    data->loading = true;
    data->loading_count = 2;  // We expect 2 responses
    data->saved = false;
    data->needs_redraw = false;
    data->status_msg[0] = '\0';
    data->selected_field = FIELD_MIN;
    
    // Register callback and send commands
    uart_register_line_callback(on_uart_response, self);
    uart_send_command("channel_time read min");
    uart_send_command("channel_time read max");
    
    draw_screen(self);
}

screen_t* channel_time_settings_screen_create(void *params)
{
    (void)params;
    
    ESP_LOGI(TAG, "Creating channel time settings screen...");
    
    screen_t *screen = screen_alloc();
    if (!screen) {
        return NULL;
    }
    
    channel_time_data_t *data = calloc(1, sizeof(channel_time_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }
    
    // Initialize data
    data->min_value = 100;
    data->max_value = 300;
    data->edited_min = 100;
    data->edited_max = 300;
    data->selected_field = FIELD_MIN;
    data->loading = true;
    data->loading_count = 2;
    data->saved = false;
    data->needs_redraw = false;
    data->status_msg[0] = '\0';
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;
    
    // Load initial values
    uart_register_line_callback(on_uart_response, screen);
    uart_send_command("channel_time read min");
    uart_send_command("channel_time read max");
    
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Channel time settings screen created");
    return screen;
}
