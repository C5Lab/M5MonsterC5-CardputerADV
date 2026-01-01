/**
 * @file karma_html_screen.c
 * @brief Karma HTML portal selection screen implementation
 */

#include "karma_html_screen.h"
#include "karma_attack_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "KARMA_HTML";

// Maximum HTML files
#define MAX_FILES       32
#define MAX_FILENAME    32

// Screen user data
typedef struct {
    int probe_index;                    // 1-based probe index
    char ssid[33];                      // SSID for display and attack
    char files[MAX_FILES][MAX_FILENAME];
    int file_count;
    int selected_index;
    int scroll_offset;
    bool loading;
    bool needs_redraw;
    screen_t *self;
} karma_html_data_t;

// Forward declaration
static void draw_screen(screen_t *self);

/**
 * @brief Check if line is an ESP log line
 */
static bool is_esp_log_line(const char *line)
{
    if (strlen(line) < 3) return false;
    
    if ((line[0] == 'I' || line[0] == 'W' || line[0] == 'E' || line[0] == 'D') 
        && line[1] == ' ' && line[2] == '(') {
        return true;
    }
    
    if (strstr(line, "[MEM]") != NULL) return true;
    if (strncmp(line, "list_sd", 7) == 0) return true;
    
    return false;
}

/**
 * @brief UART line callback for parsing list_sd output
 * Format: "1 PLAY.html", "2 SocialMedium.html", etc.
 */
static void uart_line_callback(const char *line, void *user_data)
{
    karma_html_data_t *data = (karma_html_data_t *)user_data;
    if (!data || data->file_count >= MAX_FILES) return;
    
    // Skip empty lines
    if (strlen(line) == 0) return;
    
    // Skip ESP log lines
    if (is_esp_log_line(line)) return;
    
    // Skip "HTML files found" header
    if (strstr(line, "HTML files") != NULL) {
        data->loading = false;
        data->needs_redraw = true;
        return;
    }
    
    // Skip command prompt
    const char *p = line;
    while (*p == ' ') p++;
    if (*p == '>') return;
    
    // Parse numbered lines: "1 filename.html"
    p = line;
    while (*p == ' ') p++;
    
    if (!isdigit((unsigned char)*p)) return;
    
    // Skip the number
    while (isdigit((unsigned char)*p)) p++;
    
    // Skip space after number
    if (*p != ' ') return;
    p++;
    
    // Rest is the filename
    if (strlen(p) > 0 && strstr(p, ".html") != NULL) {
        strncpy(data->files[data->file_count], p, MAX_FILENAME - 1);
        data->files[data->file_count][MAX_FILENAME - 1] = '\0';
        
        // Remove trailing whitespace
        char *end = data->files[data->file_count] + strlen(data->files[data->file_count]) - 1;
        while (end > data->files[data->file_count] && (*end == '\n' || *end == '\r' || *end == ' ')) {
            *end = '\0';
            end--;
        }
        
        ESP_LOGI(TAG, "HTML file %d: %s", data->file_count + 1, data->files[data->file_count]);
        data->file_count++;
        data->needs_redraw = true;
    }
}

static void draw_screen(screen_t *self)
{
    karma_html_data_t *data = (karma_html_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title
    ui_draw_title("Select HTML Portal");
    
    if (data->loading) {
        ui_print_center(3, "Loading files...", UI_COLOR_DIMMED);
    } else if (data->file_count == 0) {
        ui_print_center(3, "No HTML files found", UI_COLOR_DIMMED);
    } else {
        // Draw visible files as menu items
        int visible_rows = 5;
        int start_row = 1;
        
        for (int i = 0; i < visible_rows; i++) {
            int file_idx = data->scroll_offset + i;
            
            if (file_idx < data->file_count) {
                bool selected = (file_idx == data->selected_index);
                ui_draw_menu_item(start_row + i, data->files[file_idx], selected, false, false);
            }
        }
        
        // Scroll indicators
        if (data->scroll_offset > 0) {
            ui_print(UI_COLS - 2, 1, "^", UI_COLOR_DIMMED);
        }
        if (data->scroll_offset + visible_rows < data->file_count) {
            ui_print(UI_COLS - 2, visible_rows, "v", UI_COLOR_DIMMED);
        }
    }
    
    // Draw status bar
    ui_draw_status("UP/DOWN:Nav ENTER:Select ESC:Back");
}

static void on_tick(screen_t *self)
{
    karma_html_data_t *data = (karma_html_data_t *)self->user_data;
    
    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    karma_html_data_t *data = (karma_html_data_t *)self->user_data;
    int visible_rows = 5;
    
    on_tick(self);
    
    switch (key) {
        case KEY_UP:
            if (data->selected_index > 0) {
                data->selected_index--;
                if (data->selected_index < data->scroll_offset) {
                    data->scroll_offset = data->selected_index;
                }
                draw_screen(self);
            }
            break;
            
        case KEY_DOWN:
            if (data->selected_index < data->file_count - 1) {
                data->selected_index++;
                if (data->selected_index >= data->scroll_offset + visible_rows) {
                    data->scroll_offset = data->selected_index - visible_rows + 1;
                }
                draw_screen(self);
            }
            break;
            
        case KEY_ENTER:
        case KEY_SPACE:
            if (data->file_count > 0 && data->selected_index < data->file_count) {
                ESP_LOGW(TAG, "=== KARMA HTML SELECTION ===");
                ESP_LOGW(TAG, "Stored probe_index=%d, stored ssid='%s'", data->probe_index, data->ssid);
                
                // Send select_html command (1-based index)
                char cmd[32];
                snprintf(cmd, sizeof(cmd), "select_html %d", data->selected_index + 1);
                ESP_LOGW(TAG, "UART TX: '%s'", cmd);
                uart_send_command(cmd);
                
                // Send start_karma command with probe index
                char karma_cmd[32];
                snprintf(karma_cmd, sizeof(karma_cmd), "start_karma %d", data->probe_index);
                ESP_LOGW(TAG, "UART TX: '%s' (for SSID: '%s')", karma_cmd, data->ssid);
                uart_send_command(karma_cmd);
                
                // Create params for attack screen
                karma_attack_params_t *params = malloc(sizeof(karma_attack_params_t));
                if (params) {
                    strncpy(params->ssid, data->ssid, sizeof(params->ssid) - 1);
                    params->ssid[sizeof(params->ssid) - 1] = '\0';
                    
                    ESP_LOGW(TAG, "Pushing attack screen with ssid='%s'", params->ssid);
                    screen_manager_push(karma_attack_screen_create, params);
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
    karma_html_data_t *data = (karma_html_data_t *)self->user_data;
    
    uart_clear_line_callback();
    
    if (data) {
        free(data);
    }
}

screen_t* karma_html_screen_create(void *params)
{
    karma_html_params_t *html_params = (karma_html_params_t *)params;
    
    if (!html_params) {
        ESP_LOGE(TAG, "No parameters provided");
        return NULL;
    }
    
    ESP_LOGI(TAG, "Creating karma HTML screen for probe %d: %s", 
             html_params->probe_index, html_params->ssid);
    
    screen_t *screen = screen_alloc();
    if (!screen) {
        free(html_params);
        return NULL;
    }
    
    // Allocate user data
    karma_html_data_t *data = calloc(1, sizeof(karma_html_data_t));
    if (!data) {
        free(screen);
        free(html_params);
        return NULL;
    }
    
    data->probe_index = html_params->probe_index;
    strncpy(data->ssid, html_params->ssid, sizeof(data->ssid) - 1);
    data->ssid[sizeof(data->ssid) - 1] = '\0';
    data->loading = true;
    data->self = screen;
    
    free(html_params);
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;
    
    // Register UART callback
    uart_register_line_callback(uart_line_callback, data);
    
    // Send command to list HTML files
    uart_send_command("list_sd");
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Karma HTML screen created");
    return screen;
}

