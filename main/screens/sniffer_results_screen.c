/**
 * @file sniffer_results_screen.c
 * @brief Sniffer results screen showing SSIDs and clients with selection
 */

#include "sniffer_results_screen.h"
#include "station_deauth_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "SNIFF_RES";

// Maximum entries
#define MAX_LINES       32
#define MAX_LINE_LEN    32
#define MAX_SSID_LEN    33

// Screen user data
typedef struct {
    char lines[MAX_LINES][MAX_LINE_LEN];
    char parent_ssid[MAX_LINES][MAX_SSID_LEN];  // Parent SSID for each MAC line
    char current_ssid[MAX_SSID_LEN];            // Current SSID during parsing
    int line_count;
    int selected_index;     // Currently selected line
    int scroll_offset;
    bool loading;
    bool needs_redraw;
    screen_t *self;
} sniffer_results_data_t;

// Forward declaration
static void draw_screen(screen_t *self);

/**
 * @brief Check if line is an ESP log line (starts with I/W/E/D followed by space and parenthesis)
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
    if (strncmp(line, "show_sniffer", 12) == 0) return true;  // Echo of command
    
    return false;
}

/**
 * @brief Check if line looks like a MAC address (may have leading space)
 * Format: " XX:XX:XX:XX:XX:XX" or "XX:XX:XX:XX:XX:XX"
 */
static bool is_mac_line(const char *line)
{
    const char *p = line;
    
    // Skip leading whitespace
    while (*p == ' ') p++;
    
    // Need at least 17 chars for MAC
    if (strlen(p) < 17) return false;
    
    // Check pattern: hex:hex:hex:hex:hex:hex
    for (int i = 0; i < 17; i++) {
        if (i == 2 || i == 5 || i == 8 || i == 11 || i == 14) {
            if (p[i] != ':') return false;
        } else {
            if (!isxdigit((unsigned char)p[i])) return false;
        }
    }
    return true;
}

/**
 * @brief Check if line is an SSID line (contains ", CH")
 */
static bool is_ssid_line(const char *line)
{
    return strstr(line, ", CH") != NULL;
}

/**
 * @brief Check if line indicates no results
 */
static bool is_no_results_line(const char *line)
{
    return strstr(line, "No APs") != NULL || strstr(line, "no clients") != NULL;
}

/**
 * @brief Extract SSID from SSID line (everything before ", CH")
 */
static void extract_ssid(const char *line, char *ssid, size_t ssid_len)
{
    const char *ch_marker = strstr(line, ", CH");
    if (ch_marker && ssid_len > 0) {
        size_t len = ch_marker - line;
        if (len >= ssid_len) len = ssid_len - 1;
        strncpy(ssid, line, len);
        ssid[len] = '\0';
    } else if (ssid_len > 0) {
        strncpy(ssid, line, ssid_len - 1);
        ssid[ssid_len - 1] = '\0';
    }
}

/**
 * @brief Extract MAC address from line (skip leading whitespace)
 */
static void extract_mac(const char *line, char *mac, size_t mac_len)
{
    const char *p = line;
    
    // Skip leading whitespace
    while (*p == ' ') p++;
    
    // Copy MAC (17 chars)
    if (mac_len > 17) {
        strncpy(mac, p, 17);
        mac[17] = '\0';
    } else if (mac_len > 0) {
        strncpy(mac, p, mac_len - 1);
        mac[mac_len - 1] = '\0';
    }
}

/**
 * @brief UART line callback for parsing results
 */
static void uart_line_callback(const char *line, void *user_data)
{
    sniffer_results_data_t *data = (sniffer_results_data_t *)user_data;
    if (!data || data->line_count >= MAX_LINES) return;
    
    // Skip empty lines
    if (strlen(line) == 0) return;
    
    // Skip ESP log lines
    if (is_esp_log_line(line)) return;
    
    // Check for "no results" message
    if (is_no_results_line(line)) {
        data->loading = false;
        data->needs_redraw = true;
        return;
    }
    
    // Check if it's an SSID line
    if (is_ssid_line(line)) {
        // Extract and store current SSID for following MAC lines
        extract_ssid(line, data->current_ssid, MAX_SSID_LEN);
        
        // Store line
        strncpy(data->lines[data->line_count], line, MAX_LINE_LEN - 1);
        data->lines[data->line_count][MAX_LINE_LEN - 1] = '\0';
        // SSID lines don't have a parent SSID
        data->parent_ssid[data->line_count][0] = '\0';
        data->line_count++;
        data->loading = false;
        data->needs_redraw = true;
    }
    // Check if it's a MAC line
    else if (is_mac_line(line)) {
        // Store line
        strncpy(data->lines[data->line_count], line, MAX_LINE_LEN - 1);
        data->lines[data->line_count][MAX_LINE_LEN - 1] = '\0';
        // Store parent SSID for this MAC
        strncpy(data->parent_ssid[data->line_count], data->current_ssid, MAX_SSID_LEN - 1);
        data->parent_ssid[data->line_count][MAX_SSID_LEN - 1] = '\0';
        data->line_count++;
        data->loading = false;
        data->needs_redraw = true;
    }
    // Ignore all other lines
}

static void draw_screen(screen_t *self)
{
    sniffer_results_data_t *data = (sniffer_results_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title
    ui_draw_title("Sniffer Results");
    
    if (data->loading) {
        ui_print_center(3, "Loading...", UI_COLOR_DIMMED);
    } else if (data->line_count == 0) {
        ui_print_center(3, "No results", UI_COLOR_DIMMED);
    } else {
        // Draw visible lines
        int visible_rows = 5;
        int start_row = 1;
        
        for (int i = 0; i < visible_rows; i++) {
            int line_idx = data->scroll_offset + i;
            
            if (line_idx < data->line_count) {
                const char *line = data->lines[line_idx];
                bool is_selected = (line_idx == data->selected_index);
                bool is_mac = (line[0] == ' ');
                uint16_t color;
                
                // Determine color based on selection and type
                if (is_selected) {
                    color = UI_COLOR_SELECTED;
                } else if (is_mac) {
                    color = UI_COLOR_DIMMED;
                } else {
                    color = UI_COLOR_HIGHLIGHT;
                }
                
                // Build display string with selection indicator
                char display[31];
                if (is_selected) {
                    display[0] = '>';
                    strncpy(display + 1, line, 29);
                    display[30] = '\0';
                } else {
                    display[0] = ' ';
                    strncpy(display + 1, line, 29);
                    display[30] = '\0';
                }
                
                ui_print(0, start_row + i, display, color);
            }
        }
        
        // Scroll indicators
        if (data->scroll_offset > 0) {
            ui_print(UI_COLS - 2, 1, "^", UI_COLOR_DIMMED);
        }
        if (data->scroll_offset + visible_rows < data->line_count) {
            ui_print(UI_COLS - 2, visible_rows, "v", UI_COLOR_DIMMED);
        }
    }
    
    // Draw status bar
    ui_draw_status("UP/DN:Sel D:Deauth ESC:Back");
}

static void on_tick(screen_t *self)
{
    sniffer_results_data_t *data = (sniffer_results_data_t *)self->user_data;
    
    // Check if redraw needed from UART callback (thread-safe)
    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    sniffer_results_data_t *data = (sniffer_results_data_t *)self->user_data;
    int visible_rows = 5;
    
    // Also check on key press for faster response
    on_tick(self);
    
    switch (key) {
        case KEY_UP:
            if (data->selected_index > 0) {
                data->selected_index--;
                // Adjust scroll if selection goes above visible area
                if (data->selected_index < data->scroll_offset) {
                    data->scroll_offset = data->selected_index;
                }
                draw_screen(self);
            }
            break;
            
        case KEY_DOWN:
            if (data->selected_index < data->line_count - 1) {
                data->selected_index++;
                // Adjust scroll if selection goes below visible area
                if (data->selected_index >= data->scroll_offset + visible_rows) {
                    data->scroll_offset = data->selected_index - visible_rows + 1;
                }
                draw_screen(self);
            }
            break;
            
        case KEY_D:
            // Check if selected line is a MAC (client) line
            if (data->line_count > 0 && data->selected_index < data->line_count) {
                const char *line = data->lines[data->selected_index];
                
                // MAC lines start with space (indented)
                if (line[0] == ' ' && is_mac_line(line)) {
                    // Extract MAC address
                    char mac[18];
                    extract_mac(line, mac, sizeof(mac));
                    
                    // Get parent SSID
                    const char *ssid = data->parent_ssid[data->selected_index];
                    
                    ESP_LOGI(TAG, "Starting deauth on station: %s from network: %s", mac, ssid);
                    
                    // Send commands: stop, select_stations MAC, start_deauth
                    uart_send_command("stop");
                    
                    char select_cmd[64];
                    snprintf(select_cmd, sizeof(select_cmd), "select_stations %s", mac);
                    uart_send_command(select_cmd);
                    
                    uart_send_command("start_deauth");
                    
                    // Create params for station deauth screen
                    station_deauth_params_t *params = calloc(1, sizeof(station_deauth_params_t));
                    if (params) {
                        strncpy(params->mac, mac, sizeof(params->mac) - 1);
                        strncpy(params->ssid, ssid, sizeof(params->ssid) - 1);
                        
                        // Push deauth screen
                        screen_manager_push(station_deauth_screen_create, params);
                    }
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
    sniffer_results_data_t *data = (sniffer_results_data_t *)self->user_data;
    
    // Clear UART callback
    uart_clear_line_callback();
    
    if (data) {
        free(data);
    }
}

screen_t* sniffer_results_screen_create(void *params)
{
    (void)params;  // Not used
    
    ESP_LOGI(TAG, "Creating sniffer results screen...");
    
    screen_t *screen = screen_alloc();
    if (!screen) {
        return NULL;
    }
    
    // Allocate user data
    sniffer_results_data_t *data = calloc(1, sizeof(sniffer_results_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }
    
    data->loading = true;
    data->selected_index = 0;
    data->self = screen;
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;
    
    // Register UART callback
    uart_register_line_callback(uart_line_callback, data);
    
    // Send command to get results
    uart_send_command("show_sniffer_results");
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Sniffer results screen created");
    return screen;
}
