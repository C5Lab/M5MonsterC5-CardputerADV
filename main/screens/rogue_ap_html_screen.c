/**
 * @file rogue_ap_html_screen.c
 * @brief Rogue AP HTML selection screen
 * 
 * Sends list_sd, shows HTML files, then starts Rogue AP attack.
 */

#include "rogue_ap_html_screen.h"
#include "rogue_ap_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "buzzer.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "ROGUE_HTML";

#define MAX_HTML_FILES  24
#define VISIBLE_ITEMS   6
#define MAX_FILENAME_LEN 32

typedef struct {
    char ssid[33];
    char password[65];
    char html_files[MAX_HTML_FILES][MAX_FILENAME_LEN];
    int file_ids[MAX_HTML_FILES];
    int file_count;
    int selected_index;
    int scroll_offset;
    bool loading;
    bool needs_redraw;
    int network_id;
    screen_t *self;
} rogue_ap_html_data_t;

static void draw_screen(screen_t *self);

/**
 * @brief UART line callback for parsing list_sd output
 */
static void uart_line_callback(const char *line, void *user_data)
{
    rogue_ap_html_data_t *data = (rogue_ap_html_data_t *)user_data;
    if (!data || data->file_count >= MAX_HTML_FILES) return;
    
    // Skip header line
    if (strstr(line, "HTML files found") != NULL) {
        return;
    }
    
    // Parse lines like "1 PLAY.html"
    const char *ptr = line;
    
    while (*ptr && isspace((unsigned char)*ptr)) ptr++;
    
    if (!isdigit((unsigned char)*ptr)) return;
    
    int file_id = 0;
    while (*ptr && isdigit((unsigned char)*ptr)) {
        file_id = file_id * 10 + (*ptr - '0');
        ptr++;
    }
    
    while (*ptr && isspace((unsigned char)*ptr)) ptr++;
    
    if (strlen(ptr) < 6) return;
    if (strstr(ptr, ".html") == NULL) return;
    
    strncpy(data->html_files[data->file_count], ptr, MAX_FILENAME_LEN - 1);
    data->html_files[data->file_count][MAX_FILENAME_LEN - 1] = '\0';
    
    // Remove trailing whitespace
    char *end = data->html_files[data->file_count] + strlen(data->html_files[data->file_count]) - 1;
    while (end > data->html_files[data->file_count] && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    
    data->file_ids[data->file_count] = file_id;
    data->file_count++;
    
    ESP_LOGI(TAG, "Found HTML file [%d]: %s", file_id, data->html_files[data->file_count - 1]);
    
    if (data->loading && data->file_count > 0) {
        data->loading = false;
        data->needs_redraw = true;
    }
}

static void on_tick(screen_t *self)
{
    rogue_ap_html_data_t *data = (rogue_ap_html_data_t *)self->user_data;
    if (data && data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

// Helper to draw a single file row
static void draw_file_row(rogue_ap_html_data_t *data, int file_idx)
{
    int row_on_screen = file_idx - data->scroll_offset;
    if (row_on_screen < 0 || row_on_screen >= VISIBLE_ITEMS) return;
    
    int start_row = 1;
    
    char label[28];
    strncpy(label, data->html_files[file_idx], sizeof(label) - 1);
    label[sizeof(label) - 1] = '\0';
    char *ext = strstr(label, ".html");
    if (ext) *ext = '\0';
    
    bool is_selected = (file_idx == data->selected_index);
    ui_draw_menu_item(start_row + row_on_screen, label, is_selected, false, false);
}

// Optimized: redraw only two changed rows
static void redraw_two_rows(rogue_ap_html_data_t *data, int old_idx, int new_idx)
{
    draw_file_row(data, old_idx);
    draw_file_row(data, new_idx);
}

static void draw_screen(screen_t *self)
{
    rogue_ap_html_data_t *data = (rogue_ap_html_data_t *)self->user_data;
    
    ui_clear();
    ui_draw_title("Select HTML Portal");
    
    if (data->loading) {
        ui_print_center(3, "Loading...", UI_COLOR_DIMMED);
    } else if (data->file_count == 0) {
        ui_print_center(3, "No HTML files found", UI_COLOR_DIMMED);
    } else {
        int start_row = 1;
        int visible_end = data->scroll_offset + VISIBLE_ITEMS;
        if (visible_end > data->file_count) {
            visible_end = data->file_count;
        }
        
        for (int i = data->scroll_offset; i < visible_end; i++) {
            int row = (i - data->scroll_offset) + start_row;
            bool is_selected = (i == data->selected_index);
            
            char display_name[28];
            strncpy(display_name, data->html_files[i], sizeof(display_name) - 1);
            display_name[sizeof(display_name) - 1] = '\0';
            
            // Remove .html extension for display
            char *ext = strstr(display_name, ".html");
            if (ext) *ext = '\0';
            
            ui_draw_menu_item(row, display_name, is_selected, false, false);
        }
        
        // Scroll indicators
        if (data->scroll_offset > 0) {
            ui_print(UI_COLS - 2, 1, "^", UI_COLOR_DIMMED);
        }
        if (visible_end < data->file_count) {
            ui_print(UI_COLS - 2, VISIBLE_ITEMS, "v", UI_COLOR_DIMMED);
        }
    }
    
    if (data->loading) {
        ui_draw_status("Loading HTML files...");
    } else {
        ui_draw_status("ENTER:Select ESC:Back");
    }
}

static void launch_rogue_ap(rogue_ap_html_data_t *data)
{
    if (data->file_count == 0 || data->selected_index >= data->file_count) {
        return;
    }

    // Ensure the selected network is set before starting
    if (data->network_id > 0) {
        char select_cmd[32];
        snprintf(select_cmd, sizeof(select_cmd), "select_networks %d", data->network_id);
        uart_send_command(select_cmd);
    }
    
    // Send select_html command
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "select_html %d", data->file_ids[data->selected_index]);
    uart_send_command(cmd);
    
    ESP_LOGI(TAG, "Selected HTML: %s (ID: %d)", 
             data->html_files[data->selected_index], 
             data->file_ids[data->selected_index]);
    
    // Send start_rogueap command
    char rogueap_cmd[128];
    snprintf(rogueap_cmd, sizeof(rogueap_cmd), "start_rogueap %s %s", 
             data->ssid, data->password);
    uart_send_command(rogueap_cmd);
    buzzer_beep_attack();
    
    // Create Rogue AP running screen
    rogue_ap_params_t *params = malloc(sizeof(rogue_ap_params_t));
    if (params) {
        strncpy(params->ssid, data->ssid, sizeof(params->ssid) - 1);
        params->ssid[sizeof(params->ssid) - 1] = '\0';
        screen_manager_push(rogue_ap_screen_create, params);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    rogue_ap_html_data_t *data = (rogue_ap_html_data_t *)self->user_data;
    
    switch (key) {
        case KEY_UP:
            if (!data->loading && data->selected_index > 0) {
                int old_idx = data->selected_index;
                // Check if at first visible item on page - do page jump
                if (data->selected_index == data->scroll_offset && data->scroll_offset > 0) {
                    data->scroll_offset -= VISIBLE_ITEMS;
                    if (data->scroll_offset < 0) data->scroll_offset = 0;
                    data->selected_index = data->scroll_offset + VISIBLE_ITEMS - 1;
                    if (data->selected_index >= data->file_count) {
                        data->selected_index = data->file_count - 1;
                    }
                    draw_screen(self);  // Full redraw on page jump
                } else {
                    data->selected_index--;
                    redraw_two_rows(data, old_idx, data->selected_index);
                }
            } else if (!data->loading && data->file_count > 0) {
                data->selected_index = data->file_count - 1;
                data->scroll_offset = (data->selected_index / VISIBLE_ITEMS) * VISIBLE_ITEMS;
                draw_screen(self);
            }
            break;
            
        case KEY_DOWN:
            if (!data->loading && data->selected_index < data->file_count - 1) {
                int old_idx = data->selected_index;
                // Check if at last visible item on page - do page jump
                if (data->selected_index == data->scroll_offset + VISIBLE_ITEMS - 1) {
                    data->scroll_offset += VISIBLE_ITEMS;
                    data->selected_index = data->scroll_offset;
                    draw_screen(self);  // Full redraw on page jump
                } else {
                    data->selected_index++;
                    redraw_two_rows(data, old_idx, data->selected_index);
                }
            } else if (!data->loading && data->file_count > 0) {
                data->selected_index = 0;
                data->scroll_offset = 0;
                draw_screen(self);
            }
            break;
            
        case KEY_ENTER:
        case KEY_SPACE:
            launch_rogue_ap(data);
            break;
            
        case KEY_ESC:
        case KEY_Q:
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

screen_t* rogue_ap_html_screen_create(void *params)
{
    rogue_ap_html_params_t *html_params = (rogue_ap_html_params_t *)params;
    
    if (!html_params) {
        ESP_LOGE(TAG, "Invalid parameters");
        return NULL;
    }
    
    ESP_LOGI(TAG, "Creating Rogue AP HTML screen for SSID: %s", html_params->ssid);
    
    screen_t *screen = screen_alloc();
    if (!screen) {
        free(html_params);
        return NULL;
    }
    
    rogue_ap_html_data_t *data = calloc(1, sizeof(rogue_ap_html_data_t));
    if (!data) {
        free(screen);
        free(html_params);
        return NULL;
    }
    
    strncpy(data->ssid, html_params->ssid, sizeof(data->ssid) - 1);
    strncpy(data->password, html_params->password, sizeof(data->password) - 1);
    data->network_id = html_params->network_id;
    data->loading = true;
    data->self = screen;
    free(html_params);
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;
    
    // Register UART callback and send list_sd
    uart_register_line_callback(uart_line_callback, data);
    uart_send_command("list_sd");
    
    // Request initial screen draw via on_tick (avoids double draw)
    data->needs_redraw = true;
    
    ESP_LOGI(TAG, "Rogue AP HTML screen created");
    return screen;
}
