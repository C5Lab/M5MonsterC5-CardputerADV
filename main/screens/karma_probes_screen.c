/**
 * @file karma_probes_screen.c
 * @brief Karma probes selection screen implementation
 */

#include "karma_probes_screen.h"
#include "karma_html_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "KARMA_PROBES";

// Maximum probes
#define MAX_PROBES      32
#define MAX_SSID_LEN    33

// Screen user data
typedef struct {
    char ssids[MAX_PROBES][MAX_SSID_LEN];
    int probe_count;
    int selected_index;
    int scroll_offset;
    bool loading;
    bool needs_redraw;
    screen_t *self;
} karma_probes_data_t;

// Forward declaration
static void draw_screen(screen_t *self);

/**
 * @brief Check if line is an ESP log line
 */
static bool is_esp_log_line(const char *line)
{
    if (strlen(line) < 3) return false;
    
    // Check for ESP log format: "I (", "W (", "E (", "D ("
    if ((line[0] == 'I' || line[0] == 'W' || line[0] == 'E' || line[0] == 'D') 
        && line[1] == ' ' && line[2] == '(') {
        return true;
    }
    
    // Check for other common log patterns
    if (strstr(line, "[MEM]") != NULL) return true;
    if (strncmp(line, "list_probes", 11) == 0) return true;  // Echo of command
    
    return false;
}

/**
 * @brief UART line callback for parsing list_probes output
 * Format: "1 SSID_name", "2 SSID_name2", etc.
 */
static void uart_line_callback(const char *line, void *user_data)
{
    karma_probes_data_t *data = (karma_probes_data_t *)user_data;
    if (!data || data->probe_count >= MAX_PROBES) return;
    
    // Skip empty lines
    if (strlen(line) == 0) return;
    
    // Skip ESP log lines
    if (is_esp_log_line(line)) return;
    
    // Skip lines starting with ">" (command prompt)
    const char *p = line;
    while (*p == ' ') p++;
    if (*p == '>') return;
    
    // Check for "No probes" message
    if (strstr(line, "No probe") != NULL || strstr(line, "no probe") != NULL) {
        data->loading = false;
        data->needs_redraw = true;
        return;
    }
    
    // Parse numbered lines: "1 SSID_name" or " 1 SSID_name"
    p = line;
    while (*p == ' ') p++;
    
    // First character should be a digit
    if (!isdigit((unsigned char)*p)) return;
    
    // Skip the number
    while (isdigit((unsigned char)*p)) p++;
    
    // Skip space after number
    if (*p != ' ') return;
    p++;
    
    // Rest is the SSID
    if (strlen(p) > 0) {
        strncpy(data->ssids[data->probe_count], p, MAX_SSID_LEN - 1);
        data->ssids[data->probe_count][MAX_SSID_LEN - 1] = '\0';
        
        // Remove trailing whitespace
        char *end = data->ssids[data->probe_count] + strlen(data->ssids[data->probe_count]) - 1;
        while (end > data->ssids[data->probe_count] && (*end == '\n' || *end == '\r' || *end == ' ')) {
            *end = '\0';
            end--;
        }
        
        ESP_LOGW(TAG, "PARSED: array[%d] = '%s' (from line: '%s')", 
                 data->probe_count, data->ssids[data->probe_count], line);
        data->probe_count++;
        data->loading = false;
        data->needs_redraw = true;
    }
}

static void draw_screen(screen_t *self)
{
    karma_probes_data_t *data = (karma_probes_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title
    ui_draw_title("Select Probe for Karma");
    
    if (data->loading) {
        ui_print_center(3, "Loading probes...", UI_COLOR_DIMMED);
    } else if (data->probe_count == 0) {
        ui_print_center(3, "No probes found", UI_COLOR_DIMMED);
    } else {
        // Draw visible probes as menu items
        int visible_rows = 5;
        int start_row = 1;
        
        for (int i = 0; i < visible_rows; i++) {
            int probe_idx = data->scroll_offset + i;
            
            if (probe_idx < data->probe_count) {
                bool selected = (probe_idx == data->selected_index);
                ui_draw_menu_item(start_row + i, data->ssids[probe_idx], selected, false, false);
            }
        }
        
        // Scroll indicators
        if (data->scroll_offset > 0) {
            ui_print(UI_COLS - 2, 1, "^", UI_COLOR_DIMMED);
        }
        if (data->scroll_offset + visible_rows < data->probe_count) {
            ui_print(UI_COLS - 2, visible_rows, "v", UI_COLOR_DIMMED);
        }
    }
    
    // Draw status bar
    ui_draw_status("UP/DOWN:Nav ENTER:Select ESC:Back");
}

static void on_tick(screen_t *self)
{
    karma_probes_data_t *data = (karma_probes_data_t *)self->user_data;
    
    // Check if redraw needed from UART callback (thread-safe)
    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    karma_probes_data_t *data = (karma_probes_data_t *)self->user_data;
    int visible_rows = 5;
    
    // Also check on key press for faster response
    on_tick(self);
    
    switch (key) {
        case KEY_UP:
            if (data->selected_index > 0) {
                data->selected_index--;
                // Adjust scroll if needed
                if (data->selected_index < data->scroll_offset) {
                    data->scroll_offset = data->selected_index;
                }
                draw_screen(self);
            }
            break;
            
        case KEY_DOWN:
            if (data->selected_index < data->probe_count - 1) {
                data->selected_index++;
                // Adjust scroll if needed
                if (data->selected_index >= data->scroll_offset + visible_rows) {
                    data->scroll_offset = data->selected_index - visible_rows + 1;
                }
                draw_screen(self);
            }
            break;
            
        case KEY_ENTER:
        case KEY_SPACE:
            if (data->probe_count > 0 && data->selected_index < data->probe_count) {
                // Create params for karma HTML screen
                karma_html_params_t *params = malloc(sizeof(karma_html_params_t));
                if (params) {
                    params->probe_index = data->selected_index + 1;  // 1-based index
                    strncpy(params->ssid, data->ssids[data->selected_index], sizeof(params->ssid) - 1);
                    params->ssid[sizeof(params->ssid) - 1] = '\0';
                    
                    ESP_LOGW(TAG, "=== KARMA PROBE SELECTION ===");
                    ESP_LOGW(TAG, "probe_count=%d, selected_index=%d", data->probe_count, data->selected_index);
                    ESP_LOGW(TAG, "probe_index (1-based)=%d, ssid='%s'", params->probe_index, params->ssid);
                    ESP_LOGW(TAG, "All probes in array:");
                    for (int i = 0; i < data->probe_count && i < 5; i++) {
                        ESP_LOGW(TAG, "  [%d] = '%s'%s", i, data->ssids[i], 
                                 (i == data->selected_index) ? " <-- SELECTED" : "");
                    }
                    
                    screen_manager_push(karma_html_screen_create, params);
                }
            }
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
    karma_probes_data_t *data = (karma_probes_data_t *)self->user_data;
    
    // Clear UART callback
    uart_clear_line_callback();
    
    if (data) {
        free(data);
    }
}

screen_t* karma_probes_screen_create(void *params)
{
    (void)params;
    
    ESP_LOGI(TAG, "Creating karma probes screen...");
    
    screen_t *screen = screen_alloc();
    if (!screen) return NULL;
    
    // Allocate user data
    karma_probes_data_t *data = calloc(1, sizeof(karma_probes_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }
    
    data->loading = true;
    data->self = screen;
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;
    
    // Register UART callback
    uart_register_line_callback(uart_line_callback, data);
    
    // Send command to get probes
    uart_send_command("list_probes");
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Karma probes screen created");
    return screen;
}

