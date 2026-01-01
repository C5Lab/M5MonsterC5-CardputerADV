/**
 * @file bt_scan_screen.c
 * @brief BT scan screen implementation - shows scrollable list of BLE devices
 */

#include "bt_scan_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "BT_SCAN";

// Maximum devices
#define MAX_DEVICES     64
#define MAX_NAME_LEN    24
#define MAX_MAC_LEN     18

// Device entry
typedef struct {
    char mac[MAX_MAC_LEN];
    char name[MAX_NAME_LEN];
    int rssi;
} bt_device_t;

// Screen user data
typedef struct {
    bt_device_t devices[MAX_DEVICES];
    int device_count;
    int total_count;
    int scroll_offset;
    bool loading;
    bool needs_redraw;
    screen_t *self;
} bt_scan_data_t;

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
    if (strncmp(line, "scan_bt", 7) == 0) return true;
    
    return false;
}

/**
 * @brief UART line callback for parsing scan_bt output
 * Format: "  1. XX:XX:XX:XX:XX:XX  RSSI: -82 dBm  Name: Device Name"
 */
static void uart_line_callback(const char *line, void *user_data)
{
    bt_scan_data_t *data = (bt_scan_data_t *)user_data;
    if (!data || data->device_count >= MAX_DEVICES) return;
    
    // Skip empty lines
    if (strlen(line) == 0) return;
    
    // Skip ESP log lines
    if (is_esp_log_line(line)) return;
    
    // Check for "Found N devices:"
    if (strstr(line, "Found") != NULL && strstr(line, "devices") != NULL) {
        int count = 0;
        if (sscanf(line, "Found %d devices", &count) == 1) {
            data->total_count = count;
            data->loading = false;
            ESP_LOGI(TAG, "Found %d devices", count);
        }
        return;
    }
    
    // Check for Summary line (end of scan)
    if (strstr(line, "Summary:") != NULL) {
        data->loading = false;
        data->needs_redraw = true;
        return;
    }
    
    // Parse device line: "  1. XX:XX:XX:XX:XX:XX  RSSI: -82 dBm  Name: ..."
    const char *p = line;
    
    // Skip leading whitespace
    while (*p == ' ') p++;
    
    // Check for number followed by dot
    if (!isdigit((unsigned char)*p)) return;
    while (isdigit((unsigned char)*p)) p++;
    if (*p != '.') return;
    p++; // Skip dot
    while (*p == ' ') p++; // Skip space after dot
    
    // Now p should point to MAC address
    if (strlen(p) < 17) return;
    
    // Copy MAC (17 chars)
    bt_device_t *dev = &data->devices[data->device_count];
    strncpy(dev->mac, p, 17);
    dev->mac[17] = '\0';
    
    // Find RSSI
    const char *rssi_marker = "RSSI: ";
    const char *rssi_pos = strstr(p, rssi_marker);
    if (rssi_pos) {
        dev->rssi = atoi(rssi_pos + strlen(rssi_marker));
    } else {
        dev->rssi = 0;
    }
    
    // Find Name (optional)
    const char *name_marker = "Name: ";
    const char *name_pos = strstr(p, name_marker);
    if (name_pos) {
        strncpy(dev->name, name_pos + strlen(name_marker), MAX_NAME_LEN - 1);
        dev->name[MAX_NAME_LEN - 1] = '\0';
        // Remove trailing whitespace
        char *end = dev->name + strlen(dev->name) - 1;
        while (end > dev->name && (*end == '\n' || *end == '\r' || *end == ' ')) {
            *end = '\0';
            end--;
        }
    } else {
        dev->name[0] = '\0';
    }
    
    ESP_LOGI(TAG, "Device %d: %s RSSI:%d Name:'%s'", 
             data->device_count, dev->mac, dev->rssi, dev->name);
    
    data->device_count++;
    data->needs_redraw = true;
}

static void draw_screen(screen_t *self)
{
    bt_scan_data_t *data = (bt_scan_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title with count
    char title[32];
    snprintf(title, sizeof(title), "BT Scan (%d)", data->device_count);
    ui_draw_title(title);
    
    if (data->loading && data->device_count == 0) {
        ui_print_center(3, "Scanning...", UI_COLOR_DIMMED);
    } else if (data->device_count == 0) {
        ui_print_center(3, "No devices found", UI_COLOR_DIMMED);
    } else {
        // Draw visible devices
        int visible_rows = 5;
        int start_row = 1;
        
        for (int i = 0; i < visible_rows; i++) {
            int dev_idx = data->scroll_offset + i;
            
            if (dev_idx < data->device_count) {
                bt_device_t *dev = &data->devices[dev_idx];
                char line[40];
                
                if (dev->name[0] != '\0') {
                    // Show name and RSSI
                    snprintf(line, sizeof(line), "%.18s %ddB", dev->name, dev->rssi);
                } else {
                    // Show MAC and RSSI
                    snprintf(line, sizeof(line), "%s %ddB", dev->mac, dev->rssi);
                }
                
                ui_print(0, start_row + i, line, UI_COLOR_TEXT);
            }
        }
        
        // Scroll indicators
        if (data->scroll_offset > 0) {
            ui_print(UI_COLS - 2, 1, "^", UI_COLOR_DIMMED);
        }
        if (data->scroll_offset + visible_rows < data->device_count) {
            ui_print(UI_COLS - 2, visible_rows, "v", UI_COLOR_DIMMED);
        }
    }
    
    // Draw status bar
    ui_draw_status("UP/DOWN:Scroll ESC:Back");
}

static void on_tick(screen_t *self)
{
    bt_scan_data_t *data = (bt_scan_data_t *)self->user_data;
    
    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    bt_scan_data_t *data = (bt_scan_data_t *)self->user_data;
    int visible_rows = 5;
    
    on_tick(self);
    
    switch (key) {
        case KEY_UP:
            if (data->scroll_offset > 0) {
                data->scroll_offset--;
                draw_screen(self);
            }
            break;
            
        case KEY_DOWN:
            if (data->scroll_offset + visible_rows < data->device_count) {
                data->scroll_offset++;
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
    bt_scan_data_t *data = (bt_scan_data_t *)self->user_data;
    
    uart_clear_line_callback();
    
    if (data) {
        free(data);
    }
}

screen_t* bt_scan_screen_create(void *params)
{
    (void)params;
    
    ESP_LOGI(TAG, "Creating BT scan screen...");
    
    screen_t *screen = screen_alloc();
    if (!screen) return NULL;
    
    // Allocate user data
    bt_scan_data_t *data = calloc(1, sizeof(bt_scan_data_t));
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
    
    // Send scan_bt command
    uart_send_command("scan_bt");
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "BT scan screen created");
    return screen;
}


