/**
 * @file handshakes_screen.c
 * @brief Captured handshakes display screen implementation
 * 
 * Sends "list_dir /sdcard/lab/handshakes" command and parses output.
 * Filters for .pcap files only (ignores .hccapx) and displays
 * filenames without extension.
 * 
 * Example output:
 * Files in /sdcard/lab/handshakes:
 * 1 VMA84A66C-2.4_83C73F_91148.hccapx
 * 2 VMA84A66C-2.4_83C73F_91148.pcap
 * ...
 * Found 6 file(s) in /sdcard/lab/handshakes
 */

#include "handshakes_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "HANDSHAKES";

// Maximum entries
#define MAX_ENTRIES     32
#define MAX_NAME_LEN    48
#define VISIBLE_ITEMS   6

// Screen user data
typedef struct {
    char names[MAX_ENTRIES][MAX_NAME_LEN];
    int entry_count;
    int selected_index;
    int scroll_offset;
    bool loading;
    bool needs_redraw;
    bool first_draw_done;
    int ticks_since_first_draw;  // Block redraws shortly after first render
} handshakes_data_t;

// Forward declaration
static void draw_screen(screen_t *self);

/**
 * @brief Check if line is an ESP log line or command echo
 */
static bool is_skip_line(const char *line)
{
    if (strlen(line) < 3) return true;
    
    // Check for ESP log format: "I (", "W (", "E (", "D ("
    if ((line[0] == 'I' || line[0] == 'W' || line[0] == 'E' || line[0] == 'D') 
        && line[1] == ' ' && line[2] == '(') {
        return true;
    }
    
    // Skip other common patterns
    if (strstr(line, "[MEM]") != NULL) return true;
    if (strncmp(line, "list_dir", 8) == 0) return true;
    
    // Skip lines starting with ">" (command prompt)
    const char *p = line;
    while (*p == ' ') p++;
    if (*p == '>') return true;
    
    // Skip header and footer lines
    if (strncmp(line, "Files in", 8) == 0) return true;
    if (strncmp(line, "Found", 5) == 0) return true;
    
    return false;
}

/**
 * @brief UART line callback for parsing list_dir output
 * Format: "1 filename.pcap" or "2 filename.hccapx"
 * We only want .pcap files, stripped of extension
 */
static void uart_line_callback(const char *line, void *user_data)
{
    handshakes_data_t *data = (handshakes_data_t *)user_data;
    if (!data || data->entry_count >= MAX_ENTRIES) return;
    
    // Skip empty lines
    if (strlen(line) == 0) return;
    
    // Skip log/system lines
    if (is_skip_line(line)) return;
    
    // Check for "No" or "no" messages (no data)
    if (strstr(line, "No ") != NULL || strstr(line, "no ") != NULL || 
        strstr(line, "empty") != NULL || strstr(line, "Empty") != NULL ||
        strstr(line, "not found") != NULL) {
        data->loading = false;
        if (data->first_draw_done) {
            data->needs_redraw = true;
        }
        return;
    }
    
    // Parse numbered lines: "1 filename.pcap"
    const char *p = line;
    while (*p == ' ') p++;
    
    // First character should be a digit
    if (!isdigit((unsigned char)*p)) return;
    
    // Skip the number
    while (isdigit((unsigned char)*p)) p++;
    
    // Skip space after number
    if (*p != ' ') return;
    p++;
    
    // Skip any additional whitespace
    while (*p == ' ') p++;
    
    // Check if this is a .pcap file (not .hccapx)
    const char *pcap_ext = strstr(p, ".pcap");
    if (pcap_ext == NULL) return;  // Not a .pcap file
    
    // Make sure it's actually .pcap and not .pcap-something
    if (pcap_ext[5] != '\0' && pcap_ext[5] != '\n' && pcap_ext[5] != '\r' && pcap_ext[5] != ' ') {
        return;  // Extension continues, skip
    }
    
    // Copy filename without extension
    size_t name_len = pcap_ext - p;
    if (name_len >= MAX_NAME_LEN) {
        name_len = MAX_NAME_LEN - 1;
    }
    
    strncpy(data->names[data->entry_count], p, name_len);
    data->names[data->entry_count][name_len] = '\0';
    
    // Trim trailing whitespace
    char *end = data->names[data->entry_count] + strlen(data->names[data->entry_count]) - 1;
    while (end > data->names[data->entry_count] && (*end == '\n' || *end == '\r' || *end == ' ')) {
        *end = '\0';
        end--;
    }
    
    ESP_LOGI(TAG, "Parsed handshake: '%s'", data->names[data->entry_count]);
    
    data->entry_count++;
    data->loading = false;
    if (data->first_draw_done) {
        data->needs_redraw = true;
    }
}

static void draw_screen(screen_t *self)
{
    handshakes_data_t *data = (handshakes_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title
    char title[32];
    snprintf(title, sizeof(title), "Handshakes (%d)", data->entry_count);
    ui_draw_title(title);
    
    if (data->loading) {
        ui_print_center(3, "Loading...", UI_COLOR_DIMMED);
    } else if (data->entry_count == 0) {
        ui_print_center(3, "No handshakes found", UI_COLOR_DIMMED);
    } else {
        // Draw visible entries
        int start_row = 1;
        
        for (int i = 0; i < VISIBLE_ITEMS; i++) {
            int entry_idx = data->scroll_offset + i;
            
            if (entry_idx < data->entry_count) {
                // Truncate long names for display
                char label[32];
                snprintf(label, sizeof(label), "%.28s", data->names[entry_idx]);
                
                bool selected = (entry_idx == data->selected_index);
                ui_draw_menu_item(start_row + i, label, selected, false, false);
            }
        }
        
        // Scroll indicators
        if (data->scroll_offset > 0) {
            ui_print(UI_COLS - 2, 1, "^", UI_COLOR_DIMMED);
        }
        if (data->scroll_offset + VISIBLE_ITEMS < data->entry_count) {
            ui_print(UI_COLS - 2, VISIBLE_ITEMS, "v", UI_COLOR_DIMMED);
        }
    }
    
    // Draw status bar
    ui_draw_status("UP/DOWN:Scroll ESC:Back");
}

static void on_tick(screen_t *self)
{
    handshakes_data_t *data = (handshakes_data_t *)self->user_data;
    
    // First tick after creation - do the initial draw with whatever data we have
    if (!data->first_draw_done) {
        data->first_draw_done = true;
        data->ticks_since_first_draw = 0;
        data->needs_redraw = false;
        draw_screen(self);
        return;
    }
    
    data->ticks_since_first_draw++;
    
    // Block redraws for 2 ticks after first render to avoid double draw
    if (data->ticks_since_first_draw <= 2) {
        data->needs_redraw = false;
        return;
    }
    
    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    handshakes_data_t *data = (handshakes_data_t *)self->user_data;
    
    switch (key) {
        case KEY_UP:
            if (data->selected_index > 0) {
                int old_idx = data->selected_index;
                // Check if at first visible item on page - do page jump
                if (data->selected_index == data->scroll_offset && data->scroll_offset > 0) {
                    data->scroll_offset -= VISIBLE_ITEMS;
                    if (data->scroll_offset < 0) data->scroll_offset = 0;
                    data->selected_index = data->scroll_offset + VISIBLE_ITEMS - 1;
                    if (data->selected_index >= data->entry_count) {
                        data->selected_index = data->entry_count - 1;
                    }
                    draw_screen(self);  // Full redraw on page jump
                } else {
                    data->selected_index--;
                    // Redraw only 2 rows
                    int start_row = 1;
                    for (int idx = old_idx; idx >= data->selected_index; idx--) {
                        int i = idx - data->scroll_offset;
                        if (i >= 0 && i < VISIBLE_ITEMS && idx < data->entry_count) {
                            char label[32];
                            snprintf(label, sizeof(label), "%.28s", data->names[idx]);
                            bool selected = (idx == data->selected_index);
                            ui_draw_menu_item(start_row + i, label, selected, false, false);
                        }
                    }
                }
            }
            break;
            
        case KEY_DOWN:
            if (data->selected_index < data->entry_count - 1) {
                int old_idx = data->selected_index;
                // Check if at last visible item on page - do page jump
                if (data->selected_index == data->scroll_offset + VISIBLE_ITEMS - 1) {
                    // Jump to next page - don't adjust back for partial pages
                    data->scroll_offset += VISIBLE_ITEMS;
                    data->selected_index = data->scroll_offset;
                    draw_screen(self);  // Full redraw on page jump
                } else {
                    data->selected_index++;
                    // Redraw only 2 rows
                    int start_row = 1;
                    for (int idx = old_idx; idx <= data->selected_index; idx++) {
                        int i = idx - data->scroll_offset;
                        if (i >= 0 && i < VISIBLE_ITEMS && idx < data->entry_count) {
                            char label[32];
                            snprintf(label, sizeof(label), "%.28s", data->names[idx]);
                            bool selected = (idx == data->selected_index);
                            ui_draw_menu_item(start_row + i, label, selected, false, false);
                        }
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
    uart_clear_line_callback();
    
    if (self->user_data) {
        free(self->user_data);
    }
}

static void on_resume(screen_t *self)
{
    draw_screen(self);
}

screen_t* handshakes_screen_create(void *params)
{
    (void)params;
    
    ESP_LOGI(TAG, "Creating handshakes screen...");
    
    screen_t *screen = screen_alloc();
    if (!screen) return NULL;
    
    handshakes_data_t *data = calloc(1, sizeof(handshakes_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }
    
    data->loading = true;
    data->first_draw_done = false;
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;
    
    // Register UART callback and send command BEFORE any draw
    uart_register_line_callback(uart_line_callback, data);
    uart_send_command("list_dir /sdcard/lab/handshakes");
    
    ESP_LOGI(TAG, "Handshakes screen created");
    return screen;
}



