/**
 * @file sniffer_results_screen.c
 * @brief Sniffer results screen showing SSIDs and clients with selection
 */

#include "sniffer_results_screen.h"
#include "station_deauth_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "buzzer.h"
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
    // Deauth pending state - waiting for scan results to get network index
    bool pending_deauth;                        // Whether we're waiting for scan results
    char deauth_mac[18];                        // MAC address to deauth
    char deauth_ssid[MAX_SSID_LEN];             // SSID to find in scan results
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
 * @brief Check if line is a scan result CSV line
 * Format: "1","SSID","","BSSID","CH","Security","RSSI","Band"
 */
static bool is_scan_result_line(const char *line)
{
    // Must start with a quote and digit
    if (line[0] != '"') return false;
    if (!isdigit((unsigned char)line[1])) return false;
    return true;
}

/**
 * @brief Parse scan result CSV line and extract index and SSID
 * Format: "1","SSID","","BSSID","CH","Security","RSSI","Band"
 * @return network index if successful, -1 on failure
 */
static int parse_scan_result_line(const char *line, char *ssid_out, size_t ssid_len)
{
    if (!is_scan_result_line(line)) return -1;
    
    // Parse index (first quoted field)
    const char *p = line + 1;  // Skip opening quote
    int index = atoi(p);
    if (index <= 0) return -1;
    
    // Find second quoted field (SSID)
    // Skip to first comma after index
    p = strchr(p, ',');
    if (!p) return -1;
    p++;  // Skip comma
    
    // Skip opening quote of SSID field
    if (*p != '"') return -1;
    p++;
    
    // Find closing quote of SSID
    const char *ssid_end = strchr(p, '"');
    if (!ssid_end) return -1;
    
    // Copy SSID
    size_t len = ssid_end - p;
    if (len >= ssid_len) len = ssid_len - 1;
    strncpy(ssid_out, p, len);
    ssid_out[len] = '\0';
    
    return index;
}

/**
 * @brief Execute deauth sequence after finding network index
 */
static void execute_deauth_sequence(sniffer_results_data_t *data, int network_index)
{
    ESP_LOGI(TAG, "Executing deauth: network=%d, station=%s", network_index, data->deauth_mac);
    
    // Send stop first
    uart_send_command("stop");
    
    // Select the network by index
    char select_net_cmd[32];
    snprintf(select_net_cmd, sizeof(select_net_cmd), "select_networks %d", network_index);
    uart_send_command(select_net_cmd);
    
    // Select the station
    char select_sta_cmd[64];
    snprintf(select_sta_cmd, sizeof(select_sta_cmd), "select_stations %s", data->deauth_mac);
    uart_send_command(select_sta_cmd);
    
    // Start deauth
    uart_send_command("start_deauth");
    buzzer_beep_attack();
    
    // Create params for station deauth screen
    station_deauth_params_t *params = calloc(1, sizeof(station_deauth_params_t));
    if (params) {
        strncpy(params->mac, data->deauth_mac, sizeof(params->mac) - 1);
        strncpy(params->ssid, data->deauth_ssid, sizeof(params->ssid) - 1);
        
        // Clear pending state before pushing new screen
        data->pending_deauth = false;
        
        // Push deauth screen
        screen_manager_push(station_deauth_screen_create, params);
    } else {
        data->pending_deauth = false;
    }
}

/**
 * @brief UART line callback for parsing results
 */
static void uart_line_callback(const char *line, void *user_data)
{
    sniffer_results_data_t *data = (sniffer_results_data_t *)user_data;
    if (!data) return;
    
    // Skip empty lines
    if (strlen(line) == 0) return;
    
    // Skip ESP log lines
    if (is_esp_log_line(line)) return;
    
    // If we're waiting for scan results to find network index for deauth
    if (data->pending_deauth) {
        // Try to parse as scan result line
        char parsed_ssid[MAX_SSID_LEN];
        int network_index = parse_scan_result_line(line, parsed_ssid, sizeof(parsed_ssid));
        
        if (network_index > 0) {
            // Check if this SSID matches the one we're looking for
            if (strcmp(parsed_ssid, data->deauth_ssid) == 0) {
                ESP_LOGI(TAG, "Found network '%s' at index %d", parsed_ssid, network_index);
                execute_deauth_sequence(data, network_index);
                return;
            }
        }
        
        // Check for end of scan results
        if (strstr(line, "Scan results printed") != NULL) {
            ESP_LOGW(TAG, "Network '%s' not found in scan results", data->deauth_ssid);
            data->pending_deauth = false;
            data->needs_redraw = true;
        }
        return;
    }
    
    // Normal sniffer results parsing
    if (data->line_count >= MAX_LINES) return;
    
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
        int visible_rows = 6;
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
    int visible_rows = 6;
    
    switch (key) {
        case KEY_UP:
            if (data->selected_index > 0) {
                int old_idx = data->selected_index;
                // Check if at first visible item on page - do page jump
                if (data->selected_index == data->scroll_offset && data->scroll_offset > 0) {
                    data->scroll_offset -= visible_rows;
                    if (data->scroll_offset < 0) data->scroll_offset = 0;
                    data->selected_index = data->scroll_offset + visible_rows - 1;
                    if (data->selected_index >= data->line_count) {
                        data->selected_index = data->line_count - 1;
                    }
                    draw_screen(self);  // Full redraw on page jump
                } else {
                    data->selected_index--;
                    // Redraw only 2 rows
                    int start_row = 1;
                    for (int idx = old_idx; idx >= data->selected_index; idx--) {
                        int i = idx - data->scroll_offset;
                        if (i >= 0 && i < visible_rows && idx < data->line_count) {
                            const char *line = data->lines[idx];
                            bool is_selected = (idx == data->selected_index);
                            bool is_mac = (line[0] == ' ');
                            uint16_t color = is_selected ? UI_COLOR_SELECTED : (is_mac ? UI_COLOR_DIMMED : UI_COLOR_HIGHLIGHT);
                            char display[31];
                            display[0] = is_selected ? '>' : ' ';
                            strncpy(display + 1, line, 29);
                            display[30] = '\0';
                            ui_print(0, start_row + i, display, color);
                        }
                    }
                }
            }
            break;
            
        case KEY_DOWN:
            if (data->selected_index < data->line_count - 1) {
                int old_idx = data->selected_index;
                // Check if at last visible item on page - do page jump
                if (data->selected_index == data->scroll_offset + visible_rows - 1) {
                    // Jump to next page - don't adjust back for partial pages
                    data->scroll_offset += visible_rows;
                    data->selected_index = data->scroll_offset;
                    draw_screen(self);  // Full redraw on page jump
                } else {
                    data->selected_index++;
                    // Redraw only 2 rows
                    int start_row = 1;
                    for (int idx = old_idx; idx <= data->selected_index; idx++) {
                        int i = idx - data->scroll_offset;
                        if (i >= 0 && i < visible_rows && idx < data->line_count) {
                            const char *line = data->lines[idx];
                            bool is_selected = (idx == data->selected_index);
                            bool is_mac = (line[0] == ' ');
                            uint16_t color = is_selected ? UI_COLOR_SELECTED : (is_mac ? UI_COLOR_DIMMED : UI_COLOR_HIGHLIGHT);
                            char display[31];
                            display[0] = is_selected ? '>' : ' ';
                            strncpy(display + 1, line, 29);
                            display[30] = '\0';
                            ui_print(0, start_row + i, display, color);
                        }
                    }
                }
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
                    
                    // Check if we have a valid SSID
                    if (ssid[0] == '\0') {
                        ESP_LOGW(TAG, "No parent SSID for MAC %s", mac);
                        break;
                    }
                    
                    ESP_LOGI(TAG, "Initiating deauth on station: %s from network: %s", mac, ssid);
                    
                    // Store deauth target info
                    strncpy(data->deauth_mac, mac, sizeof(data->deauth_mac) - 1);
                    data->deauth_mac[sizeof(data->deauth_mac) - 1] = '\0';
                    strncpy(data->deauth_ssid, ssid, sizeof(data->deauth_ssid) - 1);
                    data->deauth_ssid[sizeof(data->deauth_ssid) - 1] = '\0';
                    
                    // Set pending state and request scan results to find network index
                    data->pending_deauth = true;
                    uart_send_command("show_scan_results");
                    
                    ESP_LOGI(TAG, "Waiting for scan results to find network index...");
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
