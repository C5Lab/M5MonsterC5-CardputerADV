/**
 * @file arp_hosts_screen.c
 * @brief ARP hosts list screen implementation
 * 
 * Displays hosts from list_hosts_vendor command with optimized scrolling
 */

#include "arp_hosts_screen.h"
#include "arp_attack_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "display.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ARP_HOSTS";

#define MAX_HOSTS 64
#define VISIBLE_ITEMS 6

// Host entry structure
typedef struct {
    char ip[16];
    char mac[18];
    char vendor[32];
} host_entry_t;

// Screen state
typedef struct {
    host_entry_t hosts[MAX_HOSTS];
    int host_count;
    int selected_index;
    int scroll_offset;
    bool scanning;
    bool needs_redraw;
    bool not_connected;  // True if WiFi not connected
    screen_t *self;
} arp_hosts_data_t;

static void draw_screen(screen_t *self);
static void draw_hosts_list(arp_hosts_data_t *data);

/**
 * @brief Format host label for display
 * Shows IP + Vendor, or IP + MAC if vendor is Unknown
 */
static void format_host_label(host_entry_t *host, char *buf, size_t len)
{
    if (strcmp(host->vendor, "Unknown") == 0) {
        snprintf(buf, len, "%-13s %s", host->ip, host->mac);
    } else {
        // Truncate vendor to fit
        snprintf(buf, len, "%-13s %.14s", host->ip, host->vendor);
    }
}

/**
 * @brief Parse a host line from list_hosts_vendor output
 * Format: "192.168.4.1  ->  C4:2B:44:12:29:15 [Huawei Device Co., Ltd.]"
 */
static bool parse_host_line(const char *line, host_entry_t *host)
{
    // Skip lines that don't contain "->"
    const char *arrow = strstr(line, "->");
    if (!arrow) return false;
    
    // Find IP (skip leading spaces)
    const char *ip_start = line;
    while (*ip_start == ' ') ip_start++;
    
    // Copy IP (up to spaces before arrow)
    const char *ip_end = ip_start;
    while (*ip_end && *ip_end != ' ') ip_end++;
    
    size_t ip_len = ip_end - ip_start;
    if (ip_len == 0 || ip_len >= sizeof(host->ip)) return false;
    
    strncpy(host->ip, ip_start, ip_len);
    host->ip[ip_len] = '\0';
    
    // Find MAC (after "->")
    const char *mac_start = arrow + 2;
    while (*mac_start == ' ') mac_start++;
    
    const char *mac_end = mac_start;
    while (*mac_end && *mac_end != ' ' && *mac_end != '[') mac_end++;
    
    size_t mac_len = mac_end - mac_start;
    if (mac_len == 0 || mac_len >= sizeof(host->mac)) return false;
    
    strncpy(host->mac, mac_start, mac_len);
    host->mac[mac_len] = '\0';
    
    // Find vendor (inside brackets)
    const char *vendor_start = strchr(mac_end, '[');
    if (vendor_start) {
        vendor_start++;
        const char *vendor_end = strchr(vendor_start, ']');
        if (vendor_end) {
            size_t vendor_len = vendor_end - vendor_start;
            if (vendor_len >= sizeof(host->vendor)) {
                vendor_len = sizeof(host->vendor) - 1;
            }
            strncpy(host->vendor, vendor_start, vendor_len);
            host->vendor[vendor_len] = '\0';
        }
    } else {
        strcpy(host->vendor, "Unknown");
    }
    
    return true;
}

/**
 * @brief UART callback for host discovery
 */
static void uart_line_callback(const char *line, void *user_data)
{
    arp_hosts_data_t *data = (arp_hosts_data_t *)user_data;
    if (!data) return;
    
    // Check for scan start
    if (strstr(line, "=== Discovered Hosts ===") != NULL) {
        data->scanning = true;
        data->host_count = 0;
        return;
    }
    
    // Check for scan complete
    if (strstr(line, "Found") != NULL && strstr(line, "hosts") != NULL) {
        data->scanning = false;
        data->needs_redraw = true;
        ESP_LOGI(TAG, "Host scan complete, found %d hosts", data->host_count);
        return;
    }
    
    // Try to parse as host entry
    if (data->scanning && data->host_count < MAX_HOSTS) {
        host_entry_t host = {0};
        if (parse_host_line(line, &host)) {
            data->hosts[data->host_count++] = host;
            ESP_LOGD(TAG, "Added host: %s -> %s [%s]", host.ip, host.mac, host.vendor);
        }
    }
}

static void draw_screen(screen_t *self)
{
    arp_hosts_data_t *data = (arp_hosts_data_t *)self->user_data;
    
    ui_clear();
    ui_draw_title("ARP Hosts");
    
    if (data->not_connected) {
        ui_print_center(2, "Not connected to WiFi", UI_COLOR_TEXT);
        ui_print_center(4, "Connect first via", UI_COLOR_DIMMED);
        ui_print_center(5, "Network Attacks menu", UI_COLOR_DIMMED);
        ui_draw_status("ESC:Back");
        return;
    }
    
    if (data->scanning) {
        ui_print_center(3, "Scanning network...", UI_COLOR_DIMMED);
    } else if (data->host_count == 0) {
        ui_print_center(3, "No hosts found", UI_COLOR_DIMMED);
    } else {
        draw_hosts_list(data);
    }
    
    ui_draw_status("UP/DOWN:Navigate ENTER:Attack ESC:Back");
}

static void draw_hosts_list(arp_hosts_data_t *data)
{
    int visible_end = data->scroll_offset + VISIBLE_ITEMS;
    if (visible_end > data->host_count) {
        visible_end = data->host_count;
    }
    
    for (int i = data->scroll_offset; i < visible_end; i++) {
        int row = (i - data->scroll_offset) + 1;
        bool selected = (i == data->selected_index);
        
        char display_text[32];
        format_host_label(&data->hosts[i], display_text, sizeof(display_text));
        
        ui_draw_menu_item(row, display_text, selected, false, false);
    }
}

// Optimized: redraw only affected menu items
static void redraw_two_items(arp_hosts_data_t *data, int old_index, int new_index)
{
    char label[32];
    
    if (old_index >= data->scroll_offset && old_index < data->scroll_offset + VISIBLE_ITEMS) {
        int old_row = (old_index - data->scroll_offset) + 1;
        format_host_label(&data->hosts[old_index], label, sizeof(label));
        ui_draw_menu_item(old_row, label, false, false, false);
    }
    
    if (new_index >= data->scroll_offset && new_index < data->scroll_offset + VISIBLE_ITEMS) {
        int new_row = (new_index - data->scroll_offset) + 1;
        format_host_label(&data->hosts[new_index], label, sizeof(label));
        ui_draw_menu_item(new_row, label, true, false, false);
    }
}

static void on_tick(screen_t *self)
{
    arp_hosts_data_t *data = (arp_hosts_data_t *)self->user_data;
    
    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    arp_hosts_data_t *data = (arp_hosts_data_t *)self->user_data;
    
    if (data->scanning || data->host_count == 0) {
        if (key == KEY_ESC || key == KEY_BACKSPACE) {
            screen_manager_pop();
        }
        return;
    }
    
    switch (key) {
        case KEY_UP:
            if (data->selected_index > 0) {
                int old_index = data->selected_index;
                
                // Check if at first visible item on page - do page jump
                if (data->selected_index == data->scroll_offset && data->scroll_offset > 0) {
                    // Page up - jump by VISIBLE_ITEMS
                    data->scroll_offset -= VISIBLE_ITEMS;
                    if (data->scroll_offset < 0) data->scroll_offset = 0;
                    // Land on last item of previous page
                    data->selected_index = data->scroll_offset + VISIBLE_ITEMS - 1;
                    if (data->selected_index >= data->host_count) {
                        data->selected_index = data->host_count - 1;
                    }
                    draw_screen(self);
                } else {
                    // Just moved within page
                    data->selected_index--;
                    redraw_two_items(data, old_index, data->selected_index);
                }
            }
            break;
            
        case KEY_DOWN:
            if (data->selected_index < data->host_count - 1) {
                int old_index = data->selected_index;
                data->selected_index++;
                
                // Check if we need to scroll
                if (data->selected_index >= data->scroll_offset + VISIBLE_ITEMS) {
                    // Page down - jump by VISIBLE_ITEMS - don't adjust back for partial pages
                    data->scroll_offset += VISIBLE_ITEMS;
                    data->selected_index = data->scroll_offset;
                    draw_screen(self);
                } else {
                    // Just moved within page
                    redraw_two_items(data, old_index, data->selected_index);
                }
            }
            break;
            
        case KEY_ENTER:
        case KEY_SPACE:
            if (data->selected_index >= 0 && data->selected_index < data->host_count) {
                host_entry_t *host = &data->hosts[data->selected_index];
                
                // Create params with full host info
                arp_attack_params_t *params = malloc(sizeof(arp_attack_params_t));
                if (params) {
                    strncpy(params->ip, host->ip, sizeof(params->ip) - 1);
                    params->ip[sizeof(params->ip) - 1] = '\0';
                    strncpy(params->mac, host->mac, sizeof(params->mac) - 1);
                    params->mac[sizeof(params->mac) - 1] = '\0';
                    strncpy(params->vendor, host->vendor, sizeof(params->vendor) - 1);
                    params->vendor[sizeof(params->vendor) - 1] = '\0';
                    
                    screen_manager_push(arp_attack_screen_create, params);
                }
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
    uart_clear_line_callback();
    
    if (self->user_data) {
        free(self->user_data);
    }
}

static void on_resume(screen_t *self)
{
    // Redraw when returning from attack screen
    draw_screen(self);
}

screen_t* arp_hosts_screen_create(void *params)
{
    (void)params;
    
    ESP_LOGI(TAG, "Creating ARP hosts screen...");
    
    screen_t *screen = screen_alloc();
    if (!screen) return NULL;
    
    // Allocate user data
    arp_hosts_data_t *data = calloc(1, sizeof(arp_hosts_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }
    
    data->self = screen;
    
    // Check if WiFi is connected
    if (!uart_is_wifi_connected()) {
        data->not_connected = true;
        data->scanning = false;
    } else {
        data->scanning = true;
    }
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;
    
    // Draw initial screen
    draw_screen(screen);
    
    // Only start scan if connected
    if (!data->not_connected) {
        uart_register_line_callback(uart_line_callback, data);
        uart_send_command("list_hosts_vendor");
    }
    
    ESP_LOGI(TAG, "ARP hosts screen created");
    return screen;
}
