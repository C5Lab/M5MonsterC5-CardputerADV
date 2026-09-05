/**
 * @file network_attacks_screen.c
 * @brief Network attacks menu screen implementation
 */

#include "network_attacks_screen.h"
#include "wifi_connect_screen.h"
#include "arp_hosts_screen.h"
#include "wpasec_upload_screen.h"
#include "mitm_sniffer_screen.h"
#include "nmap_screen.h"
#include "gitm_screen.h"
#include "rogue_gitm_screen.h"
#include "settings.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "NET_ATTACKS";

// Menu item IDs
#define MENU_WIFI         0
#define MENU_ARP          1
#define MENU_WPASEC       2
#define MENU_MITM         3
#define MENU_NMAP         4
#define MENU_GITM         5
#define MENU_ROGUE_GITM   6

#define MAX_MENU_ITEMS 7
#define VISIBLE_ITEMS  6

// Screen user data
typedef struct {
    int selected_index;
    int scroll_offset;
    int menu_count;
    int items[MAX_MENU_ITEMS];   // Ordered list of visible menu item IDs
    bool show_arp;
} network_attacks_data_t;

static const char* get_menu_text(int item_id)
{
    switch (item_id) {
        case MENU_WIFI:       return uart_is_wifi_connected() ? "Disconnect from WiFi" : "Connect to WiFi";
        case MENU_ARP:        return "ARP Poisoning";
        case MENU_WPASEC:     return "WPA-SEC Upload";
        case MENU_MITM:       return "MITM Sniffer";
        case MENU_NMAP:       return "Nmap Scanner";
        case MENU_GITM:       return "GITM";
        case MENU_ROGUE_GITM: return "Rogue GITM";
        default:              return "";
    }
}

static void draw_menu_row(network_attacks_data_t *data, int idx)
{
    int row_on_screen = idx - data->scroll_offset;
    if (row_on_screen < 0 || row_on_screen >= VISIBLE_ITEMS) return;
    if (idx < 0 || idx >= data->menu_count) return;

    ui_draw_menu_item(1 + row_on_screen, get_menu_text(data->items[idx]),
                      idx == data->selected_index, false, false);
}

static void redraw_two_rows(network_attacks_data_t *data, int old_idx, int new_idx)
{
    draw_menu_row(data, old_idx);
    draw_menu_row(data, new_idx);
}

static void redraw_menu_list(network_attacks_data_t *data)
{
    ui_draw_title(settings_get_red_team_enabled() ? "Network Attacks" : "Network Tests");

    for (int i = 0; i < VISIBLE_ITEMS; i++) {
        int idx = data->scroll_offset + i;
        if (idx < data->menu_count) {
            draw_menu_row(data, idx);
        } else {
            int y = (1 + i) * 16;
            display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
        }
    }

    display_fill_rect(DISPLAY_WIDTH - 16, 1 * 16, 16, 16, UI_COLOR_BG);
    display_fill_rect(DISPLAY_WIDTH - 16, VISIBLE_ITEMS * 16, 16, 16, UI_COLOR_BG);
    if (data->scroll_offset > 0) {
        ui_print(UI_COLS - 2, 1, "^", UI_COLOR_DIMMED);
    }
    if (data->scroll_offset + VISIBLE_ITEMS < data->menu_count) {
        ui_print(UI_COLS - 2, VISIBLE_ITEMS, "v", UI_COLOR_DIMMED);
    }
}

static void draw_screen(screen_t *self)
{
    network_attacks_data_t *data = (network_attacks_data_t *)self->user_data;

    ui_clear();
    redraw_menu_list(data);
    ui_draw_status("UP/DOWN:Navigate ENTER:Select ESC:Back");
}

static void on_key(screen_t *self, key_code_t key)
{
    network_attacks_data_t *data = (network_attacks_data_t *)self->user_data;

    switch (key) {
        case KEY_UP:
            if (data->menu_count <= 0) break;
            if (data->selected_index > 0) {
                int old_idx = data->selected_index;
                if (data->selected_index == data->scroll_offset && data->scroll_offset > 0) {
                    data->scroll_offset -= VISIBLE_ITEMS;
                    if (data->scroll_offset < 0) data->scroll_offset = 0;
                    data->selected_index = data->scroll_offset + VISIBLE_ITEMS - 1;
                    if (data->selected_index >= data->menu_count)
                        data->selected_index = data->menu_count - 1;
                    redraw_menu_list(data);
                } else {
                    data->selected_index--;
                    redraw_two_rows(data, old_idx, data->selected_index);
                }
            } else {
                data->selected_index = data->menu_count - 1;
                data->scroll_offset = data->selected_index - VISIBLE_ITEMS + 1;
                if (data->scroll_offset < 0) data->scroll_offset = 0;
                redraw_menu_list(data);
            }
            break;

        case KEY_DOWN:
            if (data->menu_count <= 0) break;
            if (data->selected_index < data->menu_count - 1) {
                int old_idx = data->selected_index;
                if (data->selected_index == data->scroll_offset + VISIBLE_ITEMS - 1) {
                    data->scroll_offset += VISIBLE_ITEMS;
                    data->selected_index = data->scroll_offset;
                    redraw_menu_list(data);
                } else {
                    data->selected_index++;
                    redraw_two_rows(data, old_idx, data->selected_index);
                }
            } else {
                data->selected_index = 0;
                data->scroll_offset = 0;
                redraw_menu_list(data);
            }
            break;

        case KEY_ENTER:
        case KEY_SPACE: {
            int item_id = data->items[data->selected_index];
            if (item_id == MENU_WIFI) {
                if (uart_is_wifi_connected()) {
                    uart_send_command("wifi_disconnect");
                    uart_set_wifi_connected(false);
                    draw_screen(self);
                } else {
                    screen_manager_push(wifi_connect_screen_create, NULL);
                }
            } else if (item_id == MENU_ARP) {
                screen_manager_push(arp_hosts_screen_create, NULL);
            } else if (item_id == MENU_MITM) {
                screen_manager_push(mitm_sniffer_screen_create, NULL);
            } else if (item_id == MENU_WPASEC) {
                screen_manager_push(wpasec_upload_screen_create, NULL);
            } else if (item_id == MENU_NMAP) {
                screen_manager_push(nmap_screen_create, NULL);
            } else if (item_id == MENU_GITM) {
                screen_manager_push(gitm_screen_create, NULL);
            } else if (item_id == MENU_ROGUE_GITM) {
                screen_manager_push(rogue_gitm_screen_create, NULL);
            }
            break;
        }

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
    if (self->user_data) {
        free(self->user_data);
    }
}

static void on_resume(screen_t *self)
{
    draw_screen(self);
}

screen_t* network_attacks_screen_create(void *params)
{
    (void)params;

    ESP_LOGI(TAG, "Creating network attacks screen...");

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    network_attacks_data_t *data = calloc(1, sizeof(network_attacks_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }

    bool red_team = settings_get_red_team_enabled();
    data->show_arp = red_team;
    int idx = 0;
    data->items[idx++] = MENU_WIFI;
    if (red_team) {
        data->items[idx++] = MENU_ARP;
        data->items[idx++] = MENU_MITM;
        data->items[idx++] = MENU_ROGUE_GITM;
    }
    data->items[idx++] = MENU_GITM;
    data->items[idx++] = MENU_NMAP;
    data->items[idx++] = MENU_WPASEC;
    data->menu_count = idx;

    ESP_LOGI(TAG, "Network menu: show_arp=%d, menu_count=%d", data->show_arp, data->menu_count);

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;

    draw_screen(screen);

    ESP_LOGI(TAG, "Network attacks screen created");
    return screen;
}
