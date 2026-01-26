/**
 * @file sniffer_probes_screen.c
 * @brief Sniffer probes screen showing probe requests
 */

#include "sniffer_probes_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

static const char *TAG = "SNIFF_PROBES";

// Maximum entries
#define MAX_PROBES      32
#define MAX_PROBE_LEN   32

// Screen user data
typedef struct {
    char probes[MAX_PROBES][MAX_PROBE_LEN];
    int probe_count;
    int total_probes;  // From header "Probe requests: N"
    int scroll_offset;
    bool loading;
    bool needs_redraw;
    screen_t *self;
} sniffer_probes_data_t;

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
    if (strncmp(line, "show_probes", 11) == 0) return true;  // Echo of command
    
    return false;
}

/**
 * @brief Check if line is a probe entry (format: "SSID (MAC)")
 */
static bool is_probe_line(const char *line)
{
    // Must contain "(" and end with ")"
    const char *paren = strchr(line, '(');
    if (!paren) return false;
    
    size_t len = strlen(line);
    if (len == 0) return false;
    
    // Check if ends with ")" (possibly with trailing whitespace)
    for (int i = len - 1; i >= 0; i--) {
        if (line[i] == ')') return true;
        if (line[i] != ' ' && line[i] != '\n' && line[i] != '\r') return false;
    }
    return false;
}

/**
 * @brief UART line callback for parsing probes
 */
static void uart_line_callback(const char *line, void *user_data)
{
    sniffer_probes_data_t *data = (sniffer_probes_data_t *)user_data;
    if (!data) return;
    
    // Skip empty lines
    if (strlen(line) == 0) return;
    
    // Skip ESP log lines
    if (is_esp_log_line(line)) return;
    
    // Check for header: "Probe requests: N"
    const char *header = "Probe requests: ";
    if (strstr(line, header)) {
        const char *num_start = strstr(line, header);
        if (num_start) {
            data->total_probes = atoi(num_start + strlen(header));
        }
        data->loading = false;
        data->needs_redraw = true;
        return;
    }
    
    // Check for "No probe requests" or similar
    if (strstr(line, "No probe") != NULL || strstr(line, "no probe") != NULL) {
        data->loading = false;
        data->needs_redraw = true;
        return;
    }
    
    // Only store lines matching probe format: "SSID (MAC)"
    if (is_probe_line(line) && data->probe_count < MAX_PROBES) {
        strncpy(data->probes[data->probe_count], line, MAX_PROBE_LEN - 1);
        data->probes[data->probe_count][MAX_PROBE_LEN - 1] = '\0';
        data->probe_count++;
        data->needs_redraw = true;
    }
}

static void draw_screen(screen_t *self)
{
    sniffer_probes_data_t *data = (sniffer_probes_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title with count
    char title[32];
    snprintf(title, sizeof(title), "Probes (%d)", data->total_probes);
    ui_draw_title(title);
    
    if (data->loading) {
        ui_print_center(3, "Loading...", UI_COLOR_DIMMED);
    } else if (data->probe_count == 0) {
        ui_print_center(3, "No probes found", UI_COLOR_DIMMED);
    } else {
        // Draw visible probes
        int visible_rows = 6;
        int start_row = 1;
        
        for (int i = 0; i < visible_rows; i++) {
            int probe_idx = data->scroll_offset + i;
            
            if (probe_idx < data->probe_count) {
                // Truncate for display
                char display[31];
                strncpy(display, data->probes[probe_idx], 30);
                display[30] = '\0';
                
                ui_print(0, start_row + i, display, UI_COLOR_TEXT);
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
    ui_draw_status("UP/DOWN:Scroll ESC:Back");
}

static void on_tick(screen_t *self)
{
    sniffer_probes_data_t *data = (sniffer_probes_data_t *)self->user_data;
    
    // Check if redraw needed from UART callback (thread-safe)
    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    sniffer_probes_data_t *data = (sniffer_probes_data_t *)self->user_data;
    int visible_rows = 6;
    
    switch (key) {
        case KEY_UP:
            if (data->scroll_offset > 0) {
                // Page jump up
                data->scroll_offset -= visible_rows;
                if (data->scroll_offset < 0) data->scroll_offset = 0;
                draw_screen(self);
            }
            break;
            
        case KEY_DOWN:
            if (data->scroll_offset + visible_rows < data->probe_count) {
                // Page jump down - don't adjust back for partial pages
                data->scroll_offset += visible_rows;
                draw_screen(self);
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
    sniffer_probes_data_t *data = (sniffer_probes_data_t *)self->user_data;
    
    // Clear UART callback
    uart_clear_line_callback();
    
    if (data) {
        free(data);
    }
}

screen_t* sniffer_probes_screen_create(void *params)
{
    (void)params;  // Not used
    
    ESP_LOGI(TAG, "Creating sniffer probes screen...");
    
    screen_t *screen = screen_alloc();
    if (!screen) {
        return NULL;
    }
    
    // Allocate user data
    sniffer_probes_data_t *data = calloc(1, sizeof(sniffer_probes_data_t));
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
    uart_send_command("show_probes");
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Sniffer probes screen created");
    return screen;
}

