/**
 * @file global_portal_screen.c
 * @brief Global Portal running screen - monitors form submissions
 */

#include "global_portal_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "buzzer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "GPORTAL";

// Maximum length for captured data
#define MAX_DATA_LEN 64

// Refresh timer interval (200ms)
#define REFRESH_INTERVAL_US 200000

// Screen user data
typedef struct {
    char ssid[33];
    char last_data[MAX_DATA_LEN];
    int submission_count;
    bool needs_redraw;
    esp_timer_handle_t refresh_timer;
    screen_t *self;
} global_portal_data_t;

// Forward declaration
static void draw_screen(screen_t *self);

/**
 * @brief Timer callback - checks if redraw is needed
 */
static void refresh_timer_callback(void *arg)
{
    global_portal_data_t *data = (global_portal_data_t *)arg;
    if (data && data->needs_redraw && data->self) {
        data->needs_redraw = false;
        draw_screen(data->self);
    }
}

/**
 * @brief UART line callback for parsing form submissions
 */
static void uart_line_callback(const char *line, void *user_data)
{
    global_portal_data_t *data = (global_portal_data_t *)user_data;
    if (!data) return;
    
    // Look for various patterns of captured data
    // Pattern 1: "Password: [value]"
    const char *pwd_marker = "Password: ";
    const char *found = strstr(line, pwd_marker);
    if (found) {
        const char *value = found + strlen(pwd_marker);
        strncpy(data->last_data, value, MAX_DATA_LEN - 1);
        data->last_data[MAX_DATA_LEN - 1] = '\0';
        
        // Remove trailing whitespace
        char *end = data->last_data + strlen(data->last_data) - 1;
        while (end > data->last_data && (*end == '\n' || *end == '\r' || *end == ' ')) {
            *end = '\0';
            end--;
        }
        
        data->submission_count++;
        data->needs_redraw = true;
        buzzer_beep_capture();
        ESP_LOGI(TAG, "Password captured #%d: %s", data->submission_count, data->last_data);
        return;
    }
    
    // Pattern 2: "password=[value]" (URL encoded form data)
    const char *form_marker = "password=";
    found = strstr(line, form_marker);
    if (found) {
        const char *value = found + strlen(form_marker);
        strncpy(data->last_data, value, MAX_DATA_LEN - 1);
        data->last_data[MAX_DATA_LEN - 1] = '\0';
        
        // Remove trailing & or whitespace
        char *end = data->last_data;
        while (*end && *end != '&' && *end != ' ' && *end != '\n' && *end != '\r') end++;
        *end = '\0';
        
        data->submission_count++;
        data->needs_redraw = true;
        buzzer_beep_capture();
        ESP_LOGI(TAG, "Form data captured #%d: %s", data->submission_count, data->last_data);
        return;
    }
    
    // Pattern 3: "Portal data saved" - increment count even if we didn't catch the data
    if (strstr(line, "Portal data saved") != NULL) {
        if (data->last_data[0] == '\0') {
            strncpy(data->last_data, "(data saved)", MAX_DATA_LEN - 1);
        }
        data->submission_count++;
        data->needs_redraw = true;
        buzzer_beep_capture();
        ESP_LOGI(TAG, "Portal data saved, submission #%d", data->submission_count);
    }
    
    // Pattern 4: "Received POST data:" - capture the whole line
    const char *post_marker = "Received POST data: ";
    found = strstr(line, post_marker);
    if (found) {
        const char *value = found + strlen(post_marker);
        strncpy(data->last_data, value, MAX_DATA_LEN - 1);
        data->last_data[MAX_DATA_LEN - 1] = '\0';
        data->needs_redraw = true;
        ESP_LOGI(TAG, "POST data: %s", data->last_data);
    }
}

static void draw_screen(screen_t *self)
{
    global_portal_data_t *data = (global_portal_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title
    ui_draw_title("Portal Running");
    
    int row = 1;
    
    // Portal SSID
    char ssid_line[40];
    snprintf(ssid_line, sizeof(ssid_line), "AP: %.25s", data->ssid);
    ui_print(0, row, ssid_line, UI_COLOR_TEXT);
    row += 2;
    
    // Submission count
    char count_line[32];
    snprintf(count_line, sizeof(count_line), "Submitted forms: %d", data->submission_count);
    ui_print(0, row, count_line, UI_COLOR_HIGHLIGHT);
    row += 2;
    
    // Last submitted data
    ui_print(0, row, "Last submitted data:", UI_COLOR_DIMMED);
    row++;
    
    if (data->last_data[0]) {
        // Truncate for display if needed
        char display_data[30];
        strncpy(display_data, data->last_data, sizeof(display_data) - 1);
        display_data[sizeof(display_data) - 1] = '\0';
        ui_print(0, row, display_data, RGB565(0, 255, 0));  // Green
    } else {
        ui_print(0, row, "-", UI_COLOR_DIMMED);
    }
    
    // Draw status bar
    ui_draw_status("ESC: Stop");
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
    global_portal_data_t *data = (global_portal_data_t *)self->user_data;
    
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

screen_t* global_portal_screen_create(void *params)
{
    global_portal_params_t *portal_params = (global_portal_params_t *)params;
    
    if (!portal_params) {
        ESP_LOGE(TAG, "Invalid parameters");
        return NULL;
    }
    
    ESP_LOGI(TAG, "Creating global portal screen for SSID: %s", portal_params->ssid);
    
    screen_t *screen = screen_alloc();
    if (!screen) {
        free(portal_params);
        return NULL;
    }
    
    // Allocate user data
    global_portal_data_t *data = calloc(1, sizeof(global_portal_data_t));
    if (!data) {
        free(screen);
        free(portal_params);
        return NULL;
    }
    
    // Copy SSID
    strncpy(data->ssid, portal_params->ssid, sizeof(data->ssid) - 1);
    data->self = screen;
    free(portal_params);
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    
    // Create periodic refresh timer
    esp_timer_create_args_t timer_args = {
        .callback = refresh_timer_callback,
        .arg = data,
        .name = "portal_refresh"
    };
    
    if (esp_timer_create(&timer_args, &data->refresh_timer) == ESP_OK) {
        esp_timer_start_periodic(data->refresh_timer, REFRESH_INTERVAL_US);
    } else {
        ESP_LOGW(TAG, "Failed to create refresh timer");
    }
    
    // Register UART callback for parsing form submissions
    uart_register_line_callback(uart_line_callback, data);
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Global portal screen created");
    return screen;
}

