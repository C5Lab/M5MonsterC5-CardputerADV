/**
 * @file bt_locator_screen.c
 * @brief BT Locator device selection screen - selectable list of BLE devices
 */

#include "bt_locator_screen.h"
#include "bt_locator_track_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "BT_LOCATOR";

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
    int selected_index;
    int scroll_offset;
    bool loading;
    bool needs_redraw;
    screen_t *self;
} bt_locator_data_t;

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
 */
static void uart_line_callback(const char *line, void *user_data)
{
    bt_locator_data_t *data = (bt_locator_data_t *)user_data;
    if (!data || data->device_count >= MAX_DEVICES) return;
    
    if (strlen(line) == 0) return;
    if (is_esp_log_line(line)) return;
    
    // Check for "Found N devices:"
    if (strstr(line, "Found") != NULL && strstr(line, "devices") != NULL) {
        data->loading = false;
        return;
    }
    
    // Check for Summary line
    if (strstr(line, "Summary:") != NULL) {
        data->loading = false;
        data->needs_redraw = true;
        return;
    }
    
    // Parse device line
    const char *p = line;
    while (*p == ' ') p++;
    
    if (!isdigit((unsigned char)*p)) return;
    while (isdigit((unsigned char)*p)) p++;
    if (*p != '.') return;
    p++;
    while (*p == ' ') p++;
    
    if (strlen(p) < 17) return;
    
    bt_device_t *dev = &data->devices[data->device_count];
    strncpy(dev->mac, p, 17);
    dev->mac[17] = '\0';
    
    const char *rssi_marker = "RSSI: ";
    const char *rssi_pos = strstr(p, rssi_marker);
    if (rssi_pos) {
        dev->rssi = atoi(rssi_pos + strlen(rssi_marker));
    } else {
        dev->rssi = 0;
    }
    
    const char *name_marker = "Name: ";
    const char *name_pos = strstr(p, name_marker);
    if (name_pos) {
        strncpy(dev->name, name_pos + strlen(name_marker), MAX_NAME_LEN - 1);
        dev->name[MAX_NAME_LEN - 1] = '\0';
        char *end = dev->name + strlen(dev->name) - 1;
        while (end > dev->name && (*end == '\n' || *end == '\r' || *end == ' ')) {
            *end = '\0';
            end--;
        }
    } else {
        dev->name[0] = '\0';
    }
    
    data->device_count++;
    data->needs_redraw = true;
}

static void draw_screen(screen_t *self)
{
    bt_locator_data_t *data = (bt_locator_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title
    char title[32];
    snprintf(title, sizeof(title), "BT Locator (%d)", data->device_count);
    ui_draw_title(title);
    
    if (data->loading && data->device_count == 0) {
        ui_print_center(3, "Scanning...", UI_COLOR_DIMMED);
    } else if (data->device_count == 0) {
        ui_print_center(3, "No devices found", UI_COLOR_DIMMED);
    } else {
        int visible_rows = 5;
        int start_row = 1;
        
        for (int i = 0; i < visible_rows; i++) {
            int dev_idx = data->scroll_offset + i;
            
            if (dev_idx < data->device_count) {
                bt_device_t *dev = &data->devices[dev_idx];
                char line[32];
                
                if (dev->name[0] != '\0') {
                    snprintf(line, sizeof(line), "%.20s", dev->name);
                } else {
                    snprintf(line, sizeof(line), "%s", dev->mac);
                }
                
                bool selected = (dev_idx == data->selected_index);
                ui_draw_menu_item(start_row + i, line, selected, false, false);
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
    ui_draw_status("UP/DOWN:Nav ENTER:Track ESC:Back");
}

static void on_tick(screen_t *self)
{
    bt_locator_data_t *data = (bt_locator_data_t *)self->user_data;
    
    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    bt_locator_data_t *data = (bt_locator_data_t *)self->user_data;
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
            if (data->selected_index < data->device_count - 1) {
                data->selected_index++;
                if (data->selected_index >= data->scroll_offset + visible_rows) {
                    data->scroll_offset = data->selected_index - visible_rows + 1;
                }
                draw_screen(self);
            }
            break;
            
        case KEY_ENTER:
        case KEY_SPACE:
            if (data->device_count > 0 && data->selected_index < data->device_count) {
                bt_device_t *dev = &data->devices[data->selected_index];
                
                // Create params for tracking screen
                bt_locator_track_params_t *params = malloc(sizeof(bt_locator_track_params_t));
                if (params) {
                    strncpy(params->mac, dev->mac, sizeof(params->mac) - 1);
                    params->mac[sizeof(params->mac) - 1] = '\0';
                    strncpy(params->name, dev->name, sizeof(params->name) - 1);
                    params->name[sizeof(params->name) - 1] = '\0';
                    
                    ESP_LOGI(TAG, "Selected device: %s (%s)", dev->mac, dev->name);
                    screen_manager_push(bt_locator_track_screen_create, params);
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
    bt_locator_data_t *data = (bt_locator_data_t *)self->user_data;
    
    uart_clear_line_callback();
    
    if (data) {
        free(data);
    }
}

screen_t* bt_locator_screen_create(void *params)
{
    (void)params;
    
    ESP_LOGI(TAG, "Creating BT Locator screen...");
    
    screen_t *screen = screen_alloc();
    if (!screen) return NULL;
    
    bt_locator_data_t *data = calloc(1, sizeof(bt_locator_data_t));
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
    
    uart_register_line_callback(uart_line_callback, data);
    uart_send_command("scan_bt");
    draw_screen(screen);
    
    ESP_LOGI(TAG, "BT Locator screen created");
    return screen;
}


