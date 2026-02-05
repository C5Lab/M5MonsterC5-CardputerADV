/**
 * @file data_detail_screen.c
 * @brief Detail view screen for displaying full text content with scrolling
 * 
 * Displays a title (SSID) and full content with automatic line wrapping
 * and vertical scrolling support. Optionally supports WiFi auto-connect.
 */

#include "data_detail_screen.h"
#include "arp_hosts_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "DATA_DETAIL";

// Display constants
#define CHARS_PER_LINE  28  // Leave some margin
#define CONTENT_ROWS    5   // Rows available for content (rows 1-5, row 0=title, row 7=status)
#define MAX_LINES       32  // Maximum wrapped lines to support

// Screen states
typedef enum {
    STATE_VIEW,
    STATE_CONNECTING,
    STATE_RESULT
} detail_state_t;

// Screen user data
typedef struct {
    char title[DETAIL_MAX_TITLE_LEN];
    char *lines[MAX_LINES];  // Pointers to wrapped lines
    int line_count;
    int scroll_offset;
    // Connect feature
    char connect_ssid[33];
    char connect_password[65];
    bool has_connect;
    detail_state_t state;
    bool connect_success;
    char result_msg[64];
    bool needs_redraw;
    screen_t *self;
} data_detail_data_t;

// Forward declarations
static void draw_screen(screen_t *self);

/**
 * @brief Wrap content into lines
 * Splits content by comma/space and wraps long segments
 */
static void wrap_content(data_detail_data_t *data, const char *content)
{
    data->line_count = 0;
    
    if (!content || strlen(content) == 0) {
        return;
    }
    
    // Make a working copy
    char *work = strdup(content);
    if (!work) return;
    
    const char *p = work;
    
    while (*p && data->line_count < MAX_LINES) {
        // Skip leading whitespace
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        
        // Find segment end (comma or end of string)
        const char *seg_start = p;
        const char *seg_end = p;
        
        // Look for comma separator or end
        while (*seg_end && *seg_end != ',') seg_end++;
        
        size_t seg_len = seg_end - seg_start;
        
        // Trim trailing whitespace from segment
        while (seg_len > 0 && (seg_start[seg_len-1] == ' ' || seg_start[seg_len-1] == '\t')) {
            seg_len--;
        }
        
        if (seg_len > 0) {
            // If segment fits on one line
            if (seg_len <= CHARS_PER_LINE) {
                char *line = malloc(seg_len + 1);
                if (line) {
                    strncpy(line, seg_start, seg_len);
                    line[seg_len] = '\0';
                    data->lines[data->line_count++] = line;
                }
            } else {
                // Need to wrap this segment
                size_t offset = 0;
                while (offset < seg_len && data->line_count < MAX_LINES) {
                    size_t chunk = seg_len - offset;
                    if (chunk > CHARS_PER_LINE) {
                        chunk = CHARS_PER_LINE;
                        // Try to break at space
                        size_t break_at = chunk;
                        for (size_t i = chunk; i > chunk/2; i--) {
                            if (seg_start[offset + i] == ' ') {
                                break_at = i;
                                break;
                            }
                        }
                        chunk = break_at;
                    }
                    
                    char *line = malloc(chunk + 1);
                    if (line) {
                        strncpy(line, seg_start + offset, chunk);
                        line[chunk] = '\0';
                        // Trim leading space on continuation
                        char *trimmed = line;
                        while (*trimmed == ' ') trimmed++;
                        if (trimmed != line) {
                            memmove(line, trimmed, strlen(trimmed) + 1);
                        }
                        if (strlen(line) > 0) {
                            data->lines[data->line_count++] = line;
                        } else {
                            free(line);
                        }
                    }
                    offset += chunk;
                }
            }
        }
        
        // Move past segment
        p = seg_end;
        if (*p == ',') {
            p++;  // Skip comma
            // Skip space after comma
            while (*p == ' ') p++;
        }
    }
    
    free(work);
}

/**
 * @brief UART callback for WiFi connection result
 */
static void uart_line_callback(const char *line, void *user_data)
{
    data_detail_data_t *data = (data_detail_data_t *)user_data;
    if (!data || data->state != STATE_CONNECTING) return;
    
    // Check for success
    if (strstr(line, "SUCCESS:") != NULL && strstr(line, "Connected") != NULL) {
        data->connect_success = true;
        snprintf(data->result_msg, sizeof(data->result_msg), "Connected!");
        data->state = STATE_RESULT;
        uart_set_wifi_connected(true);
        data->needs_redraw = true;
        ESP_LOGI(TAG, "WiFi connected successfully");
    }
    // Check for failure
    else if (strstr(line, "FAILED:") != NULL) {
        data->connect_success = false;
        snprintf(data->result_msg, sizeof(data->result_msg), "Connection failed");
        data->state = STATE_RESULT;
        uart_set_wifi_connected(false);
        data->needs_redraw = true;
        ESP_LOGW(TAG, "WiFi connection failed");
    }
}

static void draw_screen(screen_t *self)
{
    data_detail_data_t *data = (data_detail_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title (truncated if needed)
    char title_display[32];
    snprintf(title_display, sizeof(title_display), "%.28s", data->title);
    ui_draw_title(title_display);
    
    if (data->state == STATE_CONNECTING) {
        // Show connecting status
        ui_print_center(2, data->connect_ssid, UI_COLOR_HIGHLIGHT);
        ui_print_center(4, "Connecting...", UI_COLOR_DIMMED);
        ui_draw_status("Please wait...");
        return;
    }
    
    if (data->state == STATE_RESULT) {
        // Show result
        ui_print_center(2, data->connect_ssid, UI_COLOR_HIGHLIGHT);
        if (data->connect_success) {
            ui_print_center(4, data->result_msg, UI_COLOR_HIGHLIGHT);
            ui_print_center(5, "ENTER: ARP Menu", UI_COLOR_TEXT);
        } else {
            ui_print_center(4, data->result_msg, UI_COLOR_TEXT);
        }
        ui_draw_status("ENTER:Continue ESC:Back");
        return;
    }
    
    // STATE_VIEW - normal content display
    if (data->line_count == 0) {
        ui_print_center(3, "No data", UI_COLOR_DIMMED);
    } else {
        // Draw visible content lines
        for (int i = 0; i < CONTENT_ROWS; i++) {
            int line_idx = data->scroll_offset + i;
            int row = i + 1;  // Start from row 1 (after title)
            
            if (line_idx < data->line_count) {
                ui_print(0, row, data->lines[line_idx], UI_COLOR_TEXT);
            }
        }
        
        // Scroll indicators
        if (data->scroll_offset > 0) {
            ui_print(UI_COLS - 2, 1, "^", UI_COLOR_DIMMED);
        }
        if (data->scroll_offset + CONTENT_ROWS < data->line_count) {
            ui_print(UI_COLS - 2, CONTENT_ROWS, "v", UI_COLOR_DIMMED);
        }
    }
    
    // Draw status bar based on features
    if (data->has_connect) {
        ui_draw_status("ENTER:Connect ESC:Back");
    } else if (data->line_count > CONTENT_ROWS) {
        ui_draw_status("UP/DOWN:Scroll ESC:Back");
    } else {
        ui_draw_status("ESC:Back");
    }
}

static void on_tick(screen_t *self)
{
    data_detail_data_t *data = (data_detail_data_t *)self->user_data;
    
    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    data_detail_data_t *data = (data_detail_data_t *)self->user_data;
    
    // Handle result state
    if (data->state == STATE_RESULT) {
        if (key == KEY_ENTER || key == KEY_SPACE) {
            if (data->connect_success) {
                // Push ARP hosts screen
                uart_clear_line_callback();
                screen_manager_push(arp_hosts_screen_create, NULL);
            } else {
                // Go back to view state
                data->state = STATE_VIEW;
                draw_screen(self);
            }
        } else if (key == KEY_ESC || key == KEY_BACKSPACE) {
            screen_manager_pop();
        }
        return;
    }
    
    // Handle connecting state - ignore most keys
    if (data->state == STATE_CONNECTING) {
        if (key == KEY_ESC) {
            // Cancel and go back
            uart_clear_line_callback();
            screen_manager_pop();
        }
        return;
    }
    
    // Handle view state
    switch (key) {
        case KEY_UP:
            if (data->scroll_offset > 0) {
                data->scroll_offset--;
                draw_screen(self);
            } else if (data->line_count > CONTENT_ROWS) {
                data->scroll_offset = data->line_count - CONTENT_ROWS;
                draw_screen(self);
            }
            break;
            
        case KEY_DOWN:
            if (data->scroll_offset + CONTENT_ROWS < data->line_count) {
                data->scroll_offset++;
                draw_screen(self);
            } else if (data->line_count > CONTENT_ROWS) {
                data->scroll_offset = 0;
                draw_screen(self);
            }
            break;
            
        case KEY_ENTER:
        case KEY_SPACE:
            if (data->has_connect) {
                // Start WiFi connection
                ESP_LOGI(TAG, "Connecting to %s...", data->connect_ssid);
                data->state = STATE_CONNECTING;
                draw_screen(self);
                
                // Register UART callback
                uart_register_line_callback(uart_line_callback, data);
                
                // Send connect command
                char cmd[128];
                snprintf(cmd, sizeof(cmd), "wifi_connect %s %s", 
                         data->connect_ssid, data->connect_password);
                uart_send_command(cmd);
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
    data_detail_data_t *data = (data_detail_data_t *)self->user_data;
    
    // Clear UART callback if we registered one
    uart_clear_line_callback();
    
    if (data) {
        // Free all wrapped lines
        for (int i = 0; i < data->line_count; i++) {
            if (data->lines[i]) {
                free(data->lines[i]);
            }
        }
        free(data);
    }
}

static void on_resume(screen_t *self)
{
    // Redraw when returning from ARP screen
    draw_screen(self);
}

screen_t* data_detail_screen_create(void *params)
{
    data_detail_params_t *detail_params = (data_detail_params_t *)params;
    
    if (!detail_params) {
        ESP_LOGE(TAG, "No parameters provided");
        return NULL;
    }
    
    ESP_LOGI(TAG, "Creating data detail screen for '%s'...", detail_params->title);
    
    screen_t *screen = screen_alloc();
    if (!screen) {
        free(detail_params);
        return NULL;
    }
    
    data_detail_data_t *data = calloc(1, sizeof(data_detail_data_t));
    if (!data) {
        free(screen);
        free(detail_params);
        return NULL;
    }
    
    data->self = screen;
    data->state = STATE_VIEW;
    
    // Copy title
    strncpy(data->title, detail_params->title, DETAIL_MAX_TITLE_LEN - 1);
    data->title[DETAIL_MAX_TITLE_LEN - 1] = '\0';
    
    // Wrap content into lines
    wrap_content(data, detail_params->content);
    
    // Copy connect credentials if provided
    if (detail_params->connect_ssid[0] != '\0') {
        strncpy(data->connect_ssid, detail_params->connect_ssid, sizeof(data->connect_ssid) - 1);
        data->connect_ssid[sizeof(data->connect_ssid) - 1] = '\0';
        strncpy(data->connect_password, detail_params->connect_password, sizeof(data->connect_password) - 1);
        data->connect_password[sizeof(data->connect_password) - 1] = '\0';
        data->has_connect = true;
        ESP_LOGI(TAG, "Connect feature enabled for SSID: %s", data->connect_ssid);
    }
    
    // Free params (we've copied what we need)
    free(detail_params);
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;
    screen->on_resume = on_resume;
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Data detail screen created with %d lines", data->line_count);
    return screen;
}
