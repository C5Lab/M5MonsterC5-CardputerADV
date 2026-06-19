/**
 * @file home_screen.c
 * @brief Home screen implementation with main menu
 */

#include "home_screen.h"
#include "wifi_scan_screen.h"
#include "global_attacks_screen.h"
#include "sniff_karma_menu_screen.h"
#include "bt_menu_screen.h"
#include "deauth_detector_screen.h"
#include "compromised_menu_screen.h"
#include "network_attacks_screen.h"
#include "subghz_menu_screen.h"
#include "mesh_recon_screen.h"
#include "wardrive_menu_screen.h"
#include "settings_screen.h"
#include "placeholder_screen.h"
#include "settings.h"
#include "text_ui.h"
#include "uart_handler.h"
#include "esp_log.h"
#include <string.h>
#include "version.h"

static const char *TAG = "HOME_SCREEN";

// Menu items with both attack and test versions of titles
typedef struct {
    const char *title_attack;   // Title when red team enabled
    const char *title_test;     // Title when red team disabled
    screen_create_fn create_fn;
    const char *placeholder_title;
    bool        subghz_only;    // If true, only shown when subghz module probe succeeded
} menu_item_t;

// All possible menu candidates, in display order. Sub-GHz sits right after
// Network Tools and is only included when uart_is_subghz_available() returns
// true (probed once at boot).
static const menu_item_t all_menu_items[] = {
    {"WiFi Scan & Attack", "WiFi Scan & Test", wifi_scan_screen_create, NULL, false},
    {"Bluetooth", "Bluetooth", bt_menu_screen_create, NULL, false},
    {"Compromised data", "Compromised data", compromised_menu_screen_create, NULL, false},
    {"Deauth Detector", "Deauth Detector", deauth_detector_screen_create, NULL, false},
    {"Global WiFi Attacks", "Global WiFi Tests", global_attacks_screen_create, NULL, false},
    {"Mesh Recon", "Mesh Recon", mesh_recon_screen_create, NULL, false},
    {"Network Tools", "Network Tools", network_attacks_screen_create, NULL, false},
    {"Sub-GHz", "Sub-GHz", subghz_menu_screen_create, NULL, true},
    {"Wardrive", "Wardrive", wardrive_menu_screen_create, NULL, false},
    {"WiFi Sniff&Karma", "WiFi Sniff&Karma", sniff_karma_menu_screen_create, NULL, false},
    {"Settings", "Settings", settings_screen_create, NULL, false},
};

#define ALL_MENU_COUNT (sizeof(all_menu_items) / sizeof(all_menu_items[0]))
#define VISIBLE_ITEMS 6

// Screen user data
typedef struct {
    int  selected_index;
    int  scroll_offset;
    int  visible_count;
    // Indices into all_menu_items[] that are currently visible.
    int  visible[ALL_MENU_COUNT];
} home_screen_data_t;

static void rebuild_visible(home_screen_data_t *data)
{
    data->visible_count = 0;
    for (int i = 0; i < (int)ALL_MENU_COUNT; i++) {
        if (all_menu_items[i].subghz_only && !uart_is_subghz_available()) {
            continue;
        }
        data->visible[data->visible_count++] = i;
    }
    if (data->selected_index >= data->visible_count) {
        data->selected_index = data->visible_count > 0 ? data->visible_count - 1 : 0;
    }
    if (data->scroll_offset > data->visible_count - VISIBLE_ITEMS) {
        data->scroll_offset = data->visible_count - VISIBLE_ITEMS;
    }
    if (data->scroll_offset < 0) data->scroll_offset = 0;
}

// Helper to get the correct title based on red team setting
static const char* get_visible_title(const home_screen_data_t *data, int visible_idx)
{
    const menu_item_t *item = &all_menu_items[data->visible[visible_idx]];
    return settings_get_red_team_enabled()
        ? item->title_attack
        : item->title_test;
}

static void draw_screen(screen_t *self)
{
    home_screen_data_t *data = (home_screen_data_t *)self->user_data;

    ui_clear();

    // Draw title
    ui_draw_title("LAB5 v" JANOS_ADV_VERSION);

    int visible_end = data->scroll_offset + VISIBLE_ITEMS;
    if (visible_end > data->visible_count) {
        visible_end = data->visible_count;
    }

    for (int i = data->scroll_offset; i < visible_end; i++) {
        int row = (i - data->scroll_offset) + 1;
        ui_draw_menu_item(row, get_visible_title(data, i), i == data->selected_index, false, false);
    }

    if (data->scroll_offset > 0) {
        ui_print(UI_COLS - 2, 1, "^", UI_COLOR_DIMMED);
    }
    if (data->scroll_offset + VISIBLE_ITEMS < data->visible_count) {
        ui_print(UI_COLS - 2, VISIBLE_ITEMS, "v", UI_COLOR_DIMMED);
    }

    ui_draw_status("UP/DOWN:Navigate ENTER:Select");
}

// Redraw only the content rows + scroll arrows (title/status stay put).
// Used on page scroll so the whole screen doesn't flash.
static void redraw_content(home_screen_data_t *data)
{
    int visible_end = data->scroll_offset + VISIBLE_ITEMS;
    if (visible_end > data->visible_count) visible_end = data->visible_count;
    for (int r = 0; r < VISIBLE_ITEMS; r++) {
        int i = data->scroll_offset + r;
        int row = r + 1;
        if (i < visible_end) {
            ui_draw_menu_item(row, get_visible_title(data, i), i == data->selected_index, false, false);
        } else {
            display_fill_rect(0, row * 16, DISPLAY_WIDTH, 16, UI_COLOR_BG);
        }
    }
    if (data->scroll_offset > 0) {
        ui_print(UI_COLS - 2, 1, "^", UI_COLOR_DIMMED);
    }
    if (data->scroll_offset + VISIBLE_ITEMS < data->visible_count) {
        ui_print(UI_COLS - 2, VISIBLE_ITEMS, "v", UI_COLOR_DIMMED);
    }
}

// Optimized: redraw only two changed rows
static void redraw_two_items(home_screen_data_t *data, int old_index, int new_index)
{
    if (old_index >= data->scroll_offset && old_index < data->scroll_offset + VISIBLE_ITEMS) {
        int old_row = (old_index - data->scroll_offset) + 1;
        ui_draw_menu_item(old_row, get_visible_title(data, old_index), false, false, false);
    }
    if (new_index >= data->scroll_offset && new_index < data->scroll_offset + VISIBLE_ITEMS) {
        int new_row = (new_index - data->scroll_offset) + 1;
        ui_draw_menu_item(new_row, get_visible_title(data, new_index), true, false, false);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    home_screen_data_t *data = (home_screen_data_t *)self->user_data;

    switch (key) {
        case KEY_UP:
            if (data->selected_index > 0) {
                int old_index = data->selected_index;

                if (data->selected_index == data->scroll_offset && data->scroll_offset > 0) {
                    data->scroll_offset -= VISIBLE_ITEMS;
                    if (data->scroll_offset < 0) data->scroll_offset = 0;
                    data->selected_index = data->scroll_offset + VISIBLE_ITEMS - 1;
                    if (data->selected_index >= data->visible_count) {
                        data->selected_index = data->visible_count - 1;
                    }
                    redraw_content(data);
                } else {
                    data->selected_index--;
                    redraw_two_items(data, old_index, data->selected_index);
                }
            } else if (data->visible_count > 0) {
                data->selected_index = data->visible_count - 1;
                data->scroll_offset = (data->selected_index / VISIBLE_ITEMS) * VISIBLE_ITEMS;
                redraw_content(data);
            }
            break;

        case KEY_DOWN:
            if (data->selected_index < data->visible_count - 1) {
                int old_index = data->selected_index;
                data->selected_index++;

                if (data->selected_index >= data->scroll_offset + VISIBLE_ITEMS) {
                    data->scroll_offset += VISIBLE_ITEMS;
                    data->selected_index = data->scroll_offset;
                    draw_screen(self);
                } else {
                    redraw_two_items(data, old_index, data->selected_index);
                }
            } else if (data->visible_count > 0) {
                data->selected_index = 0;
                data->scroll_offset = 0;
                draw_screen(self);
            }
            break;

        case KEY_ENTER:
        case KEY_SPACE:
            {
                if (data->visible_count <= 0) break;
                const menu_item_t *item = &all_menu_items[data->visible[data->selected_index]];
                if (item->create_fn) {
                    screen_manager_push(item->create_fn, NULL);
                } else {
                    screen_manager_push(placeholder_screen_create, (void*)item->placeholder_title);
                }
            }
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
    home_screen_data_t *data = (home_screen_data_t *)self->user_data;
    rebuild_visible(data);
    draw_screen(self);
}

screen_t* home_screen_create(void *params)
{
    (void)params;

    ESP_LOGI(TAG, "Creating home screen...");

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    home_screen_data_t *data = calloc(1, sizeof(home_screen_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }

    rebuild_visible(data);

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;

    draw_screen(screen);

    ESP_LOGI(TAG, "Home screen created");
    return screen;
}
