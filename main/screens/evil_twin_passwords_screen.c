/**
 * @file evil_twin_passwords_screen.c
 * @brief Evil Twin captured passwords display screen implementation
 * 
 * Sends "show_pass evil" command and parses output format:
 * "SSID", "password"
 */

#include "evil_twin_passwords_screen.h"
#include "data_detail_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "EVIL_TWIN_PASS";

// Maximum entries
#define MAX_ENTRIES     32
#define MAX_SSID_LEN    33
#define MAX_PASS_LEN    64
#define VISIBLE_ITEMS   5

// Password entry
typedef struct {
    char ssid[MAX_SSID_LEN];
    char password[MAX_PASS_LEN];
} password_entry_t;

// Screen user data
typedef struct {
    password_entry_t entries[MAX_ENTRIES];
    int entry_count;
    int selected_index;
    int scroll_offset;
    bool loading;
    bool needs_redraw;
} evil_twin_passwords_data_t;

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
    if (strncmp(line, "show_pass", 9) == 0) return true;  // Echo of command
    
    // Skip lines starting with ">" (command prompt)
    const char *p = line;
    while (*p == ' ') p++;
    if (*p == '>') return true;
    
    return false;
}

/**
 * @brief Parse a quoted field from the line
 * @param src Source string pointer (updated to after the parsed field)
 * @param dest Destination buffer
 * @param max_len Maximum length of destination
 * @return true if field was parsed successfully
 */
static bool parse_quoted_field(const char **src, char *dest, size_t max_len)
{
    const char *p = *src;
    
    // Skip whitespace and commas
    while (*p == ' ' || *p == ',' || *p == '\t') p++;
    
    // Expect opening quote
    if (*p != '"') return false;
    p++;
    
    // Copy until closing quote
    size_t i = 0;
    while (*p && *p != '"' && i < max_len - 1) {
        dest[i++] = *p++;
    }
    dest[i] = '\0';
    
    // Skip closing quote
    if (*p == '"') p++;
    
    *src = p;
    return true;
}

/**
 * @brief UART line callback for parsing show_pass evil output
 * Format: "SSID", "password"
 */
static void uart_line_callback(const char *line, void *user_data)
{
    evil_twin_passwords_data_t *data = (evil_twin_passwords_data_t *)user_data;
    if (!data || data->entry_count >= MAX_ENTRIES) return;
    
    // Skip empty lines
    if (strlen(line) == 0) return;
    
    // Skip log/system lines
    if (is_skip_line(line)) return;
    
    // Check for "No" or "no" messages (no data)
    if (strstr(line, "No ") != NULL || strstr(line, "no ") != NULL || 
        strstr(line, "empty") != NULL || strstr(line, "Empty") != NULL) {
        data->loading = false;
        data->needs_redraw = true;
        return;
    }
    
    // Try to parse quoted CSV: "SSID", "password"
    const char *p = line;
    password_entry_t *entry = &data->entries[data->entry_count];
    
    if (!parse_quoted_field(&p, entry->ssid, MAX_SSID_LEN)) return;
    if (!parse_quoted_field(&p, entry->password, MAX_PASS_LEN)) return;
    
    ESP_LOGI(TAG, "Parsed: SSID='%s', pass='%s'", entry->ssid, entry->password);
    
    data->entry_count++;
    data->loading = false;
    data->needs_redraw = true;
}

static void draw_screen(screen_t *self)
{
    evil_twin_passwords_data_t *data = (evil_twin_passwords_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title
    char title[32];
    snprintf(title, sizeof(title), "Evil Twin Pass (%d)", data->entry_count);
    ui_draw_title(title);
    
    if (data->loading) {
        ui_print_center(3, "Loading...", UI_COLOR_DIMMED);
    } else if (data->entry_count == 0) {
        ui_print_center(3, "No passwords found", UI_COLOR_DIMMED);
    } else {
        // Draw visible entries
        int start_row = 1;
        
        for (int i = 0; i < VISIBLE_ITEMS; i++) {
            int entry_idx = data->scroll_offset + i;
            
            if (entry_idx < data->entry_count) {
                password_entry_t *entry = &data->entries[entry_idx];
                
                // Format: truncated SSID: password
                char label[32];
                snprintf(label, sizeof(label), "%.12s: %.14s", entry->ssid, entry->password);
                
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
    ui_draw_status("ENTER:Details UP/DN:Scroll ESC:Back");
}

static void on_tick(screen_t *self)
{
    evil_twin_passwords_data_t *data = (evil_twin_passwords_data_t *)self->user_data;
    
    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    evil_twin_passwords_data_t *data = (evil_twin_passwords_data_t *)self->user_data;
    
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
                            password_entry_t *entry = &data->entries[idx];
                            char label[32];
                            snprintf(label, sizeof(label), "%.12s: %.14s", entry->ssid, entry->password);
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
                    data->scroll_offset += VISIBLE_ITEMS;
                    int max_scroll = data->entry_count - VISIBLE_ITEMS;
                    if (max_scroll < 0) max_scroll = 0;
                    if (data->scroll_offset > max_scroll) data->scroll_offset = max_scroll;
                    data->selected_index = data->scroll_offset;
                    draw_screen(self);  // Full redraw on page jump
                } else {
                    data->selected_index++;
                    // Redraw only 2 rows
                    int start_row = 1;
                    for (int idx = old_idx; idx <= data->selected_index; idx++) {
                        int i = idx - data->scroll_offset;
                        if (i >= 0 && i < VISIBLE_ITEMS && idx < data->entry_count) {
                            password_entry_t *entry = &data->entries[idx];
                            char label[32];
                            snprintf(label, sizeof(label), "%.12s: %.14s", entry->ssid, entry->password);
                            bool selected = (idx == data->selected_index);
                            ui_draw_menu_item(start_row + i, label, selected, false, false);
                        }
                    }
                }
            }
            break;
            
        case KEY_ENTER:
        case KEY_SPACE:
            if (data->entry_count > 0 && data->selected_index < data->entry_count) {
                password_entry_t *entry = &data->entries[data->selected_index];
                
                // Create detail screen params
                data_detail_params_t *params = malloc(sizeof(data_detail_params_t));
                if (params) {
                    snprintf(params->title, DETAIL_MAX_TITLE_LEN, "SSID: %s", entry->ssid);
                    snprintf(params->content, DETAIL_MAX_CONTENT_LEN, "Password: %s", entry->password);
                    screen_manager_push(data_detail_screen_create, params);
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

screen_t* evil_twin_passwords_screen_create(void *params)
{
    (void)params;
    
    ESP_LOGI(TAG, "Creating evil twin passwords screen...");
    
    screen_t *screen = screen_alloc();
    if (!screen) return NULL;
    
    evil_twin_passwords_data_t *data = calloc(1, sizeof(evil_twin_passwords_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }
    
    data->loading = true;
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;
    
    // Register UART callback
    uart_register_line_callback(uart_line_callback, data);
    
    // Send command
    uart_send_command("show_pass evil");
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Evil twin passwords screen created");
    return screen;
}

