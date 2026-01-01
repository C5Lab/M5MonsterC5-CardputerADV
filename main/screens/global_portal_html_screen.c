/**
 * @file global_portal_html_screen.c
 * @brief HTML selection screen for Global Portal attack
 */

#include "global_portal_html_screen.h"
#include "global_portal_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "GPORTAL_HTML";

// Maximum HTML files and visible items
#define MAX_HTML_FILES  24
#define VISIBLE_ITEMS   5
#define MAX_FILENAME_LEN 32

// Screen user data
typedef struct {
    char ssid[33];
    char html_files[MAX_HTML_FILES][MAX_FILENAME_LEN];
    int file_ids[MAX_HTML_FILES];
    int file_count;
    int selected_index;
    int scroll_offset;
    bool loading;
    bool needs_redraw;
    screen_t *self;
} global_portal_html_data_t;

// Forward declaration
static void draw_screen(screen_t *self);

/**
 * @brief UART line callback for parsing list_sd output
 */
static void uart_line_callback(const char *line, void *user_data)
{
    global_portal_html_data_t *data = (global_portal_html_data_t *)user_data;
    if (!data || data->file_count >= MAX_HTML_FILES) return;
    
    // Skip header line
    if (strstr(line, "HTML files found") != NULL) {
        return;
    }
    
    // Parse lines like "1 PLAY.html"
    const char *ptr = line;
    
    // Skip leading whitespace
    while (*ptr && isspace((unsigned char)*ptr)) ptr++;
    
    // Check if starts with digit
    if (!isdigit((unsigned char)*ptr)) return;
    
    // Parse the number
    int file_id = 0;
    while (*ptr && isdigit((unsigned char)*ptr)) {
        file_id = file_id * 10 + (*ptr - '0');
        ptr++;
    }
    
    // Skip whitespace after number
    while (*ptr && isspace((unsigned char)*ptr)) ptr++;
    
    // Rest is filename - check if it ends with .html
    if (strlen(ptr) < 6) return;
    if (strstr(ptr, ".html") == NULL) return;
    
    // Store the file
    strncpy(data->html_files[data->file_count], ptr, MAX_FILENAME_LEN - 1);
    data->html_files[data->file_count][MAX_FILENAME_LEN - 1] = '\0';
    
    // Remove trailing whitespace/newline
    char *end = data->html_files[data->file_count] + strlen(data->html_files[data->file_count]) - 1;
    while (end > data->html_files[data->file_count] && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    
    data->file_ids[data->file_count] = file_id;
    data->file_count++;
    
    ESP_LOGI(TAG, "Found HTML file [%d]: %s", file_id, data->html_files[data->file_count - 1]);
    
    // Update loading state
    if (data->loading && data->file_count > 0) {
        data->loading = false;
        data->needs_redraw = true;
    }
}

static void on_tick(screen_t *self)
{
    global_portal_html_data_t *data = (global_portal_html_data_t *)self->user_data;
    if (data && data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

static void draw_screen(screen_t *self)
{
    global_portal_html_data_t *data = (global_portal_html_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title
    ui_draw_title("Select HTML Portal");
    
    if (data->loading) {
        ui_print_center(3, "Loading...", UI_COLOR_DIMMED);
    } else if (data->file_count == 0) {
        ui_print_center(3, "No HTML files found", UI_COLOR_DIMMED);
    } else {
        // Draw visible file items
        int start_row = 1;
        for (int i = 0; i < VISIBLE_ITEMS; i++) {
            int file_idx = data->scroll_offset + i;
            
            if (file_idx < data->file_count) {
                // Truncate filename for display
                char label[28];
                strncpy(label, data->html_files[file_idx], sizeof(label) - 1);
                label[sizeof(label) - 1] = '\0';
                
                // Remove .html extension for cleaner display
                char *ext = strstr(label, ".html");
                if (ext) *ext = '\0';
                
                bool is_selected = (file_idx == data->selected_index);
                ui_draw_menu_item(start_row + i, label, is_selected, false, false);
            } else {
                // Clear empty row
                int y = (start_row + i) * 16;
                display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
            }
        }
        
        // Draw scroll indicators if needed
        if (data->scroll_offset > 0) {
            ui_print(UI_COLS - 2, 1, "^", UI_COLOR_DIMMED);
        }
        if (data->scroll_offset + VISIBLE_ITEMS < data->file_count) {
            ui_print(UI_COLS - 2, VISIBLE_ITEMS, "v", UI_COLOR_DIMMED);
        }
    }
    
    // Fill gap to status bar
    int gap_y = 6 * 16;
    int status_y = DISPLAY_HEIGHT - 16 - 2;
    if (gap_y < status_y) {
        display_fill_rect(0, gap_y, DISPLAY_WIDTH, status_y - gap_y, UI_COLOR_BG);
    }
    
    // Draw status bar
    if (data->loading) {
        ui_draw_status("Loading HTML files...");
    } else {
        ui_draw_status("ENTER:Select ESC:Back");
    }
}

static void launch_portal(global_portal_html_data_t *data)
{
    if (data->file_count == 0 || data->selected_index >= data->file_count) {
        return;
    }
    
    // Send select_html command with the 1-based ID
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "select_html %d", data->file_ids[data->selected_index]);
    uart_send_command(cmd);
    
    ESP_LOGI(TAG, "Selected HTML portal: %s (ID: %d)", 
             data->html_files[data->selected_index], 
             data->file_ids[data->selected_index]);
    
    // Send start_portal command with SSID
    char portal_cmd[64];
    snprintf(portal_cmd, sizeof(portal_cmd), "start_portal %s", data->ssid);
    uart_send_command(portal_cmd);
    
    // Create portal running screen params
    global_portal_params_t *params = malloc(sizeof(global_portal_params_t));
    if (params) {
        strncpy(params->ssid, data->ssid, sizeof(params->ssid) - 1);
        params->ssid[sizeof(params->ssid) - 1] = '\0';
        screen_manager_push(global_portal_screen_create, params);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    global_portal_html_data_t *data = (global_portal_html_data_t *)self->user_data;
    
    // Check if redraw needed from UART callback
    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
    
    switch (key) {
        case KEY_UP:
            if (!data->loading && data->selected_index > 0) {
                data->selected_index--;
                
                // Scroll up if needed
                if (data->selected_index < data->scroll_offset) {
                    data->scroll_offset = data->selected_index;
                }
                draw_screen(self);
            }
            break;
            
        case KEY_DOWN:
            if (!data->loading && data->selected_index < data->file_count - 1) {
                data->selected_index++;
                
                // Scroll down if needed
                if (data->selected_index >= data->scroll_offset + VISIBLE_ITEMS) {
                    data->scroll_offset = data->selected_index - VISIBLE_ITEMS + 1;
                }
                draw_screen(self);
            }
            break;
            
        case KEY_ENTER:
        case KEY_SPACE:
            if (!data->loading && data->file_count > 0) {
                launch_portal(data);
            }
            break;
            
        case KEY_ESC:
        case KEY_BACKSPACE:
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

screen_t* global_portal_html_screen_create(void *params)
{
    global_portal_html_params_t *html_params = (global_portal_html_params_t *)params;
    
    if (!html_params) {
        ESP_LOGE(TAG, "Invalid parameters");
        return NULL;
    }
    
    ESP_LOGI(TAG, "Creating global portal HTML screen for SSID: %s", html_params->ssid);
    
    screen_t *screen = screen_alloc();
    if (!screen) {
        free(html_params);
        return NULL;
    }
    
    // Allocate user data
    global_portal_html_data_t *data = calloc(1, sizeof(global_portal_html_data_t));
    if (!data) {
        free(screen);
        free(html_params);
        return NULL;
    }
    
    // Copy SSID
    strncpy(data->ssid, html_params->ssid, sizeof(data->ssid) - 1);
    data->loading = true;
    data->self = screen;
    free(html_params);
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;
    
    // Register UART callback for parsing list_sd output
    uart_register_line_callback(uart_line_callback, data);
    
    // Send list_sd command
    uart_send_command("list_sd");
    
    // Draw initial screen (loading state)
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Global portal HTML screen created");
    return screen;
}


