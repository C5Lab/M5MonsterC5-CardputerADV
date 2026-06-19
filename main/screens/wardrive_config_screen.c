/**
 * @file wardrive_config_screen.c
 * @brief Wardrive 2.0 setup screen. See header.
 *
 * Protocol (board firmware unchanged):
 *   get_wardrive_config                 -> [WDCFG] ... lines, [WDCFG] END
 *   set_wardrive_bands <csv>            (wifi24,wifi5,ble; omit disabled)
 *   set_wardrive_channels popular|all|custom <colon-list>
 *   set_wardrive_rssi_delta wifi|ble <int>
 *   set_wardrive_memcap <int>
 *   set_wardrive_cooldown <int>
 *   set_antisurv_sensitivity low|med|high
 *   gps_set atgm|m5|cap
 *
 * Editing uses the repo's LEFT/RIGHT cycle pattern; custom channel list is
 * entered via the shared text_input_screen.
 */

#include "wardrive_config_screen.h"
#include "wardrive_config.h"
#include "text_input_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "settings.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "WD_CONFIG";

#define VISIBLE_ROWS        6
#define LOAD_TIMEOUT_TICKS  30

// Edit limits / steps
#define RSSI_MIN   0
#define RSSI_MAX   40
#define MEMCAP_MIN 100
#define MEMCAP_MAX 100000
#define MEMCAP_STEP 100
#define COOLDOWN_MIN 0
#define COOLDOWN_MAX 600

typedef enum {
    F_BAND_24 = 0,
    F_BAND_5,
    F_BAND_BLE,
    F_CH_MODE,
    F_CH_CUSTOM,
    F_WIFI_DBM,
    F_BLE_DBM,
    F_MEMCAP,
    F_COOLDOWN,
    F_ANTISURV,
    F_GPS,
    F_LOAD,
    F_APPLY,
    F_COUNT
} wd_field_t;

typedef struct {
    wardrive_config_t cfg;
    int gps_type;           // mirrors settings gps_type_t (0=atgm,1=m5,2=cap)
    int selected;
    int scroll_offset;
    bool loading;
    int  load_ticks;
    char status[40];
    bool needs_redraw;
    screen_t *self;
} wd_config_data_t;

static void draw_screen(screen_t *self);

static const char *ch_mode_str(wardrive_channel_mode_t m)
{
    switch (m) {
        case WARDRIVE_CH_POPULAR: return "popular";
        case WARDRIVE_CH_ALL:     return "all";
        case WARDRIVE_CH_CUSTOM:  return "custom";
    }
    return "?";
}

static const char *antisurv_str(wardrive_antisurv_sens_t s)
{
    switch (s) {
        case WARDRIVE_ANTISURV_LOW:  return "low";
        case WARDRIVE_ANTISURV_MED:  return "med";
        case WARDRIVE_ANTISURV_HIGH: return "high";
    }
    return "?";
}

static const char *gps_str(int t)
{
    switch (t) {
        case GPS_TYPE_ATGM: return "atgm";
        case GPS_TYPE_M5:   return "m5";
        case GPS_TYPE_CAP:  return "cap";
    }
    return "?";
}

// Render one field's label+value into buf (<=30 cols).
static void field_label(wd_config_data_t *d, wd_field_t f, char *buf, size_t len)
{
    const wardrive_config_t *c = &d->cfg;
    switch (f) {
        case F_BAND_24:  snprintf(buf, len, "2.4GHz: %s", (c->bands & WARDRIVE_BAND_WIFI24) ? "On" : "Off"); break;
        case F_BAND_5:   snprintf(buf, len, "5GHz:   %s", (c->bands & WARDRIVE_BAND_WIFI5)  ? "On" : "Off"); break;
        case F_BAND_BLE: snprintf(buf, len, "BLE:    %s", (c->bands & WARDRIVE_BAND_BLE)    ? "On" : "Off"); break;
        case F_CH_MODE:  snprintf(buf, len, "Channels: %s", ch_mode_str(c->channel_mode)); break;
        case F_CH_CUSTOM:snprintf(buf, len, "Custom: %.20s", c->custom_channels[0] ? c->custom_channels : "(set)"); break;
        case F_WIFI_DBM: snprintf(buf, len, "WiFi dBm: %d", c->wifi_rssi_delta); break;
        case F_BLE_DBM:  snprintf(buf, len, "BLE dBm: %d", c->ble_rssi_delta); break;
        case F_MEMCAP:   snprintf(buf, len, "MemCap: %d", c->mem_cap); break;
        case F_COOLDOWN: snprintf(buf, len, "Cooldown: %d", c->startup_cooldown); break;
        case F_ANTISURV: snprintf(buf, len, "AntiSurv: %s", antisurv_str(c->antisurv_sensitivity)); break;
        case F_GPS:      snprintf(buf, len, "GPS: %s", gps_str(d->gps_type)); break;
        case F_LOAD:     snprintf(buf, len, "Load current"); break;
        case F_APPLY:    snprintf(buf, len, "Apply"); break;
        default:         buf[0] = '\0'; break;
    }
}

static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// LEFT/RIGHT edit on the selected field. dir = -1 or +1.
static void edit_field(wd_config_data_t *d, wd_field_t f, int dir)
{
    wardrive_config_t *c = &d->cfg;
    switch (f) {
        case F_BAND_24:  c->bands ^= WARDRIVE_BAND_WIFI24; break;
        case F_BAND_5:   c->bands ^= WARDRIVE_BAND_WIFI5;  break;
        case F_BAND_BLE: c->bands ^= WARDRIVE_BAND_BLE;    break;
        case F_CH_MODE: {
            int m = (int)c->channel_mode + dir;
            if (m < 0) m = WARDRIVE_CH_CUSTOM;
            if (m > WARDRIVE_CH_CUSTOM) m = WARDRIVE_CH_POPULAR;
            c->channel_mode = (wardrive_channel_mode_t)m;
            break;
        }
        case F_WIFI_DBM: c->wifi_rssi_delta = clampi(c->wifi_rssi_delta + dir, RSSI_MIN, RSSI_MAX); break;
        case F_BLE_DBM:  c->ble_rssi_delta  = clampi(c->ble_rssi_delta + dir, RSSI_MIN, RSSI_MAX); break;
        case F_MEMCAP:   c->mem_cap = clampi(c->mem_cap + dir * MEMCAP_STEP, MEMCAP_MIN, MEMCAP_MAX); break;
        case F_COOLDOWN: c->startup_cooldown = clampi(c->startup_cooldown + dir, COOLDOWN_MIN, COOLDOWN_MAX); break;
        case F_ANTISURV: {
            int s = (int)c->antisurv_sensitivity + dir;
            if (s < 0) s = WARDRIVE_ANTISURV_HIGH;
            if (s > WARDRIVE_ANTISURV_HIGH) s = WARDRIVE_ANTISURV_LOW;
            c->antisurv_sensitivity = (wardrive_antisurv_sens_t)s;
            break;
        }
        case F_GPS: {
            int g = d->gps_type + dir;
            if (g < 0) g = GPS_TYPE_CAP;
            if (g > GPS_TYPE_CAP) g = GPS_TYPE_ATGM;
            d->gps_type = g;
            break;
        }
        default: break;
    }
    d->status[0] = '\0';
}

// ---- Load (get_wardrive_config) -------------------------------------------

static void uart_line_callback(const char *line, void *user_data)
{
    wd_config_data_t *d = (wd_config_data_t *)user_data;
    if (!d || !line) return;
    // Only consume config lines while a Load is in progress. Otherwise the
    // [WDCFG] dump the board emits after every set_* (during Apply) would
    // trigger a redraw storm.
    if (!d->loading) return;
    if (wardrive_config_parse_line(&d->cfg, line)) {
        d->loading = false;            // saw [WDCFG] END
        snprintf(d->status, sizeof(d->status), "Loaded");
    }
    d->needs_redraw = true;
}

static void do_load(wd_config_data_t *d)
{
    d->loading = true;
    d->load_ticks = 0;
    snprintf(d->status, sizeof(d->status), "Loading...");
    uart_flush_rx();
    uart_send_command("get_wardrive_config");
    d->needs_redraw = true;
}

// ---- Apply (walk set_* commands) ------------------------------------------

static void do_apply(wd_config_data_t *d)
{
    const wardrive_config_t *c = &d->cfg;
    char cmd[128];
    int n = 0;

    // Bands CSV (omit disabled). Send even if empty so the board can reject.
    char bands[32] = {0};
    if (c->bands & WARDRIVE_BAND_WIFI24) strcat(bands, bands[0] ? ",wifi24" : "wifi24");
    if (c->bands & WARDRIVE_BAND_WIFI5)  strcat(bands, bands[0] ? ",wifi5"  : "wifi5");
    if (c->bands & WARDRIVE_BAND_BLE)    strcat(bands, bands[0] ? ",ble"    : "ble");
    snprintf(cmd, sizeof(cmd), "set_wardrive_bands %s", bands);
    uart_send_command(cmd); n++;

    if (c->channel_mode == WARDRIVE_CH_CUSTOM && c->custom_channels[0]) {
        snprintf(cmd, sizeof(cmd), "set_wardrive_channels custom %s", c->custom_channels);
    } else {
        snprintf(cmd, sizeof(cmd), "set_wardrive_channels %s",
                 c->channel_mode == WARDRIVE_CH_ALL ? "all" : "popular");
    }
    uart_send_command(cmd); n++;

    snprintf(cmd, sizeof(cmd), "set_wardrive_rssi_delta wifi %d", c->wifi_rssi_delta);
    uart_send_command(cmd); n++;
    snprintf(cmd, sizeof(cmd), "set_wardrive_rssi_delta ble %d", c->ble_rssi_delta);
    uart_send_command(cmd); n++;
    snprintf(cmd, sizeof(cmd), "set_wardrive_memcap %d", c->mem_cap);
    uart_send_command(cmd); n++;
    snprintf(cmd, sizeof(cmd), "set_wardrive_cooldown %d", c->startup_cooldown);
    uart_send_command(cmd); n++;
    snprintf(cmd, sizeof(cmd), "set_antisurv_sensitivity %s",
             antisurv_str(c->antisurv_sensitivity));
    uart_send_command(cmd); n++;

    snprintf(cmd, sizeof(cmd), "gps_set %s", gps_str(d->gps_type));
    uart_send_command(cmd); n++;
    settings_set_gps_type((gps_type_t)d->gps_type);

    snprintf(d->status, sizeof(d->status), "Applied %d cmds", n);
    ESP_LOGI(TAG, "Applied %d config commands", n);
}

// ---- Custom channels input -------------------------------------------------

static void on_custom_channels(const char *text, void *user_data)
{
    wd_config_data_t *d = (wd_config_data_t *)user_data;
    if (d) {
        snprintf(d->cfg.custom_channels, sizeof(d->cfg.custom_channels), "%s", text);
        d->cfg.channel_mode = WARDRIVE_CH_CUSTOM;
        d->status[0] = '\0';
    }
    screen_manager_pop();   // back to config; on_resume redraws
}

// ---- Screen callbacks ------------------------------------------------------

// Redraw a single field row in place (no full clear). Avoids flicker.
static void redraw_field_row(wd_config_data_t *d, int idx)
{
    int row = (idx - d->scroll_offset) + 1;
    if (row < 1 || row > VISIBLE_ROWS) return;
    char label[31];
    field_label(d, (wd_field_t)idx, label, sizeof(label));
    ui_draw_menu_item(row, label, idx == d->selected, false, false);
}

static void redraw_status(wd_config_data_t *d)
{
    ui_draw_status(d->status[0] ? d->status : "L/R:Edit ENTER ESC");
}

// Redraw only the content rows + scroll arrows (title/status untouched).
// Used on page scroll so the whole screen doesn't flash.
static void redraw_content(wd_config_data_t *d)
{
    for (int r = 0; r < VISIBLE_ROWS; r++) {
        int idx = d->scroll_offset + r;
        int row = r + 1;
        if (idx < F_COUNT) {
            char label[31];
            field_label(d, (wd_field_t)idx, label, sizeof(label));
            ui_draw_menu_item(row, label, idx == d->selected, false, false);
        } else {
            display_fill_rect(0, row * 16, DISPLAY_WIDTH, 16, UI_COLOR_BG);
        }
    }
    if (d->scroll_offset > 0) {
        ui_print(UI_COLS - 2, 1, "^", UI_COLOR_DIMMED);
    }
    if (d->scroll_offset + VISIBLE_ROWS < F_COUNT) {
        ui_print(UI_COLS - 2, VISIBLE_ROWS, "v", UI_COLOR_DIMMED);
    }
}

// Move selection; return true if the visible page scrolled (needs full redraw).
static bool move_selection(wd_config_data_t *d, int dir)
{
    int old_scroll = d->scroll_offset;
    int s = d->selected + dir;
    if (s < 0) s = F_COUNT - 1;
    if (s >= F_COUNT) s = 0;
    d->selected = s;
    if (d->selected < d->scroll_offset) d->scroll_offset = d->selected;
    if (d->selected >= d->scroll_offset + VISIBLE_ROWS)
        d->scroll_offset = d->selected - VISIBLE_ROWS + 1;
    return d->scroll_offset != old_scroll;
}

static void draw_screen(screen_t *self)
{
    wd_config_data_t *d = (wd_config_data_t *)self->user_data;

    ui_clear();
    ui_draw_title("Wardrive Setup");

    if (d->loading) {
        ui_print_center(3, "Loading...", UI_COLOR_DIMMED);
        ui_draw_status("Please wait");
        return;
    }

    redraw_content(d);

    if (d->status[0]) {
        ui_draw_status(d->status);
    } else {
        ui_draw_status("L/R:Edit ENTER ESC");
    }
}

static void open_custom_input(wd_config_data_t *d)
{
    text_input_params_t *p = calloc(1, sizeof(text_input_params_t));
    if (!p) return;
    p->title = "Custom channels";
    p->hint = "e.g. 1:6:11:36";
    p->on_submit = on_custom_channels;
    p->user_data = d;
    p->allow_empty = false;
    screen_manager_push(text_input_screen_create, p);
}

static void on_key(screen_t *self, key_code_t key)
{
    wd_config_data_t *d = (wd_config_data_t *)self->user_data;
    if (!d) return;
    if (d->loading) {
        if (key == KEY_ESC || key == KEY_Q || key == KEY_BACKSPACE) {
            d->loading = false;
            draw_screen(self);
        }
        return;
    }

    switch (key) {
        case KEY_UP:
        case KEY_DOWN: {
            int prev = d->selected;
            bool scrolled = move_selection(d, key == KEY_UP ? -1 : +1);
            if (scrolled) {
                redraw_content(d);          // page changed: content only, no flash
            } else {
                redraw_field_row(d, prev);  // deselect old
                redraw_field_row(d, d->selected); // select new
            }
            break;
        }
        case KEY_LEFT:
        case KEY_RIGHT: {
            bool had_status = d->status[0] != '\0';
            edit_field(d, (wd_field_t)d->selected, key == KEY_LEFT ? -1 : +1);
            redraw_field_row(d, d->selected);
            if (had_status) redraw_status(d); // edit cleared a transient status
            break;
        }
        case KEY_ENTER:
        case KEY_SPACE:
            switch ((wd_field_t)d->selected) {
                case F_CH_CUSTOM: open_custom_input(d); break;
                case F_LOAD:      do_load(d); draw_screen(self); break;
                case F_APPLY:     do_apply(d); draw_screen(self); break;
                default: {
                    bool had_status = d->status[0] != '\0';
                    edit_field(d, (wd_field_t)d->selected, +1);
                    redraw_field_row(d, d->selected);
                    if (had_status) redraw_status(d);
                    break;
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

static void on_tick(screen_t *self)
{
    wd_config_data_t *d = (wd_config_data_t *)self->user_data;
    if (!d) return;
    if (d->loading) {
        d->load_ticks++;
        if (d->load_ticks >= LOAD_TIMEOUT_TICKS) {
            d->loading = false;
            snprintf(d->status, sizeof(d->status), "No response");
            d->needs_redraw = true;
        }
    }
    if (d->needs_redraw) {
        d->needs_redraw = false;
        draw_screen(self);
    }
}

static void on_resume(screen_t *self)
{
    wd_config_data_t *d = (wd_config_data_t *)self->user_data;
    if (!d) return;
    uart_register_line_callback(uart_line_callback, d);
    draw_screen(self);
}

static void on_destroy(screen_t *self)
{
    uart_clear_line_callback();
    if (self->user_data) free(self->user_data);
}

screen_t* wardrive_config_screen_create(void *params)
{
    (void)params;
    ESP_LOGI(TAG, "Creating wardrive config screen...");

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    wd_config_data_t *d = calloc(1, sizeof(wd_config_data_t));
    if (!d) {
        free(screen);
        return NULL;
    }
    d->self = screen;
    wardrive_config_set_defaults(&d->cfg);
    d->gps_type = (int)settings_get_gps_type();
    d->selected = 0;

    screen->user_data = d;
    screen->on_draw = draw_screen;
    screen->on_key = on_key;
    screen->on_tick = on_tick;
    screen->on_resume = on_resume;
    screen->on_destroy = on_destroy;

    uart_register_line_callback(uart_line_callback, d);
    do_load(d);             // reflect the board's real config, not our defaults
    draw_screen(screen);

    ESP_LOGI(TAG, "Wardrive config screen created");
    return screen;
}
