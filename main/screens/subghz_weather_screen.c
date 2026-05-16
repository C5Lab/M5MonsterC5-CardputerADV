/**
 * @file subghz_weather_screen.c
 * @brief Sub-GHz Weather screen
 *
 * UART behaviour cloned from coreS3/main/screens/subghz_weather_screen.c:
 *   on open:   send "subghz_weather"
 *   line cb:   "[SUBGHZ_WEATHER_START] freq=..."  -> remember listen freq
 *              "[SUBGHZ_WEATHER] proto=... id=0x... ch=... temp=... hum=... batt=..."
 *              -> upsert into an 8-slot LRU sensor table (keyed on proto+id+ch)
 *   on back:   send "subghz_stop", drop line callback
 *
 * Cardputer rendering: 5 visible sensor rows, scrollable through the 8 slots,
 * status row at the bottom shows freq + active sensor count.
 */

#include "subghz_weather_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "SUBGHZ_WX";

#define MAX_SENSORS     8
#define VISIBLE_ROWS    5
#define UI_TICK_PERIOD  10   /* main loop calls on_tick every ~500ms; 10 ticks -> ~5s.
                                We poll on every tick because needs_redraw drives renders. */

typedef struct {
    bool     used;
    char     proto[24];
    unsigned long id;
    char     ch[8];
    char     temp[16];
    char     hum[8];
    char     batt[8];
    int64_t  last_seen_us;
} weather_sensor_t;

typedef struct {
    weather_sensor_t sensors[MAX_SENSORS];
    SemaphoreHandle_t mtx;

    float listen_freq;
    int   selected_index;     /* index into sensors[], including empty slots */
    int   scroll_offset;
    bool  needs_redraw;
    screen_t *self;
} subghz_wx_data_t;

static subghz_wx_data_t *s_current = NULL;

static void draw_screen(screen_t *self);
static void redraw_sensor_row(subghz_wx_data_t *data, int slot_idx);
static void redraw_list_window(subghz_wx_data_t *data);

static int find_slot_locked(subghz_wx_data_t *data, const char *proto,
                            unsigned long id, const char *ch)
{
    for (int i = 0; i < MAX_SENSORS; i++) {
        weather_sensor_t *s = &data->sensors[i];
        if (s->used && s->id == id &&
            strncmp(s->proto, proto, sizeof(s->proto)) == 0 &&
            strncmp(s->ch, ch, sizeof(s->ch)) == 0) {
            return i;
        }
    }
    return -1;
}

static int pick_free_or_lru_locked(subghz_wx_data_t *data)
{
    for (int i = 0; i < MAX_SENSORS; i++) {
        if (!data->sensors[i].used) return i;
    }
    int lru = 0;
    for (int i = 1; i < MAX_SENSORS; i++) {
        if (data->sensors[i].last_seen_us < data->sensors[lru].last_seen_us)
            lru = i;
    }
    return lru;
}

static void upsert_sensor(subghz_wx_data_t *data, const char *proto,
                          unsigned long id, const char *ch,
                          const char *temp, const char *hum, const char *batt)
{
    xSemaphoreTake(data->mtx, portMAX_DELAY);
    int slot = find_slot_locked(data, proto, id, ch);
    if (slot < 0) slot = pick_free_or_lru_locked(data);

    weather_sensor_t *s = &data->sensors[slot];
    s->used = true;
    s->id = id;
    snprintf(s->proto, sizeof(s->proto), "%s", proto);
    snprintf(s->ch,    sizeof(s->ch),    "%s", ch);
    snprintf(s->temp,  sizeof(s->temp),  "%s", temp);
    snprintf(s->hum,   sizeof(s->hum),   "%s", hum);
    snprintf(s->batt,  sizeof(s->batt),  "%s", batt);
    s->last_seen_us = esp_timer_get_time();
    xSemaphoreGive(data->mtx);

    data->needs_redraw = true;
}

static void uart_line_cb(const char *line, void *user_data)
{
    subghz_wx_data_t *data = (subghz_wx_data_t *)user_data;
    if (!data || !line) return;

    if (strstr(line, "[SUBGHZ_WEATHER_START]")) {
        float f = 0.0f;
        const char *p = strstr(line, "freq=");
        if (p && sscanf(p, "freq=%f", &f) == 1) {
            data->listen_freq = f;
        }
        data->needs_redraw = true;
        return;
    }

    const char *p = strstr(line, "[SUBGHZ_WEATHER] proto=");
    if (!p) return;

    char proto[24] = {0};
    char ch[8]     = {0};
    char temp[16]  = {0};
    char hum[8]    = {0};
    char batt[8]   = {0};
    unsigned long id = 0;

    if (sscanf(p,
               "[SUBGHZ_WEATHER] proto=%23s id=0x%lX ch=%7s temp=%15s hum=%7s batt=%7s",
               proto, &id, ch, temp, hum, batt) == 6) {
        upsert_sensor(data, proto, id, ch, temp, hum, batt);
    }
}

static void format_age(int64_t now_us, int64_t last_us, char *buf, size_t n)
{
    if (last_us <= 0) { snprintf(buf, n, "--"); return; }
    int64_t secs = (now_us - last_us) / 1000000;
    if (secs < 0) secs = 0;
    if (secs < 60)         snprintf(buf, n, "%llds",   (long long)secs);
    else if (secs < 3600)  snprintf(buf, n, "%lldm",   (long long)(secs / 60));
    else if (secs < 86400) snprintf(buf, n, "%lldh",   (long long)(secs / 3600));
    else                   snprintf(buf, n, "%lldd",   (long long)(secs / 86400));
}

/* Format a single row to fit in 30 cols. Free slots show "--". */
static void format_row(const weather_sensor_t *s, int64_t now_us,
                       char *out, size_t n)
{
    if (!s->used) {
        snprintf(out, n, "--");
        return;
    }
    char age[8];
    format_age(now_us, s->last_seen_us, age, sizeof(age));

    const char *temp = (strcmp(s->temp, "-") == 0) ? "--" : s->temp;
    const char *hum  = (strcmp(s->hum,  "-") == 0) ? "--" : s->hum;

    /* "<proto:7> <temp:5>C <hum:3>% <batt:3> <age:3>" -> 30 chars budget */
    snprintf(out, n, "%-7.7s %.5sC %.3s%% %.3s %s",
             s->proto, temp, hum, s->batt, age);
}

static void redraw_sensor_row(subghz_wx_data_t *data, int slot_idx)
{
    int row_on_screen = slot_idx - data->scroll_offset;
    if (row_on_screen < 0 || row_on_screen >= VISIBLE_ROWS) return;
    int ui_row = 1 + row_on_screen;
    int y = ui_row * 16;
    display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
    if (slot_idx >= MAX_SENSORS) return;

    char buf[64];
    int64_t now_us = esp_timer_get_time();
    weather_sensor_t snap;
    xSemaphoreTake(data->mtx, portMAX_DELAY);
    snap = data->sensors[slot_idx];
    xSemaphoreGive(data->mtx);

    format_row(&snap, now_us, buf, sizeof(buf));
    bool selected = (slot_idx == data->selected_index);
    uint16_t fg = snap.used ? UI_COLOR_TEXT : UI_COLOR_DIMMED;
    if (selected) {
        ui_draw_menu_item(ui_row, buf, true, false, false);
    } else {
        ui_print(0, ui_row, buf, fg);
    }
}

static void redraw_list_window(subghz_wx_data_t *data)
{
    for (int i = 0; i < VISIBLE_ROWS; i++) {
        int slot_idx = data->scroll_offset + i;
        redraw_sensor_row(data, slot_idx);
    }

    display_fill_rect(DISPLAY_WIDTH - 16, 1 * 16, 16, 16, UI_COLOR_BG);
    display_fill_rect(DISPLAY_WIDTH - 16, (1 + VISIBLE_ROWS - 1) * 16, 16, 16, UI_COLOR_BG);
    if (data->scroll_offset > 0)
        ui_print(UI_COLS - 2, 1, "^", UI_COLOR_DIMMED);
    if (data->scroll_offset + VISIBLE_ROWS < MAX_SENSORS)
        ui_print(UI_COLS - 2, 1 + VISIBLE_ROWS - 1, "v", UI_COLOR_DIMMED);
}

static int count_active(subghz_wx_data_t *data)
{
    int n = 0;
    xSemaphoreTake(data->mtx, portMAX_DELAY);
    for (int i = 0; i < MAX_SENSORS; i++) {
        if (data->sensors[i].used) n++;
    }
    xSemaphoreGive(data->mtx);
    return n;
}

static void draw_screen(screen_t *self)
{
    subghz_wx_data_t *data = (subghz_wx_data_t *)self->user_data;
    int active = count_active(data);

    ui_clear();

    char title[24];
    if (data->listen_freq > 0.0f) {
        int whole = (int)data->listen_freq;
        int frac  = ((int)(data->listen_freq * 100.0f + 0.5f)) % 100;
        snprintf(title, sizeof(title), "Weather %d.%02d", whole, frac);
    } else {
        snprintf(title, sizeof(title), "Weather");
    }
    ui_draw_title(title);

    redraw_list_window(data);

    char status[36];
    snprintf(status, sizeof(status), "%d sensor%s   ESC:Back",
             active, active == 1 ? "" : "s");
    ui_draw_status(status);
}

static void on_tick(screen_t *self)
{
    subghz_wx_data_t *data = (subghz_wx_data_t *)self->user_data;
    if (!data->needs_redraw) return;
    data->needs_redraw = false;
    /* Full redraw: data + age changed. Cheap enough at our refresh cadence. */
    draw_screen(self);
}

static void redraw_two_rows(subghz_wx_data_t *data, int old_idx, int new_idx)
{
    if (old_idx >= 0) redraw_sensor_row(data, old_idx);
    if (new_idx >= 0) redraw_sensor_row(data, new_idx);
}

static void on_key(screen_t *self, key_code_t key)
{
    subghz_wx_data_t *data = (subghz_wx_data_t *)self->user_data;

    switch (key) {
        case KEY_UP:
            if (data->selected_index > 0) {
                int old = data->selected_index;
                int new_idx = old - 1;
                if (new_idx < data->scroll_offset) {
                    data->scroll_offset = new_idx;
                    data->selected_index = new_idx;
                    redraw_list_window(data);
                } else {
                    data->selected_index = new_idx;
                    redraw_two_rows(data, old, new_idx);
                }
            }
            break;

        case KEY_DOWN:
            if (data->selected_index < MAX_SENSORS - 1) {
                int old = data->selected_index;
                int new_idx = old + 1;
                if (new_idx >= data->scroll_offset + VISIBLE_ROWS) {
                    data->scroll_offset = new_idx - VISIBLE_ROWS + 1;
                    data->selected_index = new_idx;
                    redraw_list_window(data);
                } else {
                    data->selected_index = new_idx;
                    redraw_two_rows(data, old, new_idx);
                }
            }
            break;

        case KEY_R:
            data->needs_redraw = true;
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
    subghz_wx_data_t *data = (subghz_wx_data_t *)self->user_data;
    if (data) {
        uart_clear_line_callback();
        uart_send_command("subghz_stop");
        if (data->mtx) {
            vSemaphoreDelete(data->mtx);
            data->mtx = NULL;
        }
        if (s_current == data) s_current = NULL;
        free(data);
        self->user_data = NULL;
    }
}

static void on_resume(screen_t *self)
{
    draw_screen(self);
}

screen_t* subghz_weather_screen_create(void *params)
{
    (void)params;

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    subghz_wx_data_t *data = calloc(1, sizeof(subghz_wx_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }

    data->mtx = xSemaphoreCreateMutex();
    data->listen_freq = 0.0f;
    data->self = screen;
    s_current = data;

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;

    /* Defensive: stop any prior subghz operation, install our line callback,
     * then ask the firmware to start the weather decoder. */
    uart_send_command("subghz_stop");
    uart_register_line_callback(uart_line_cb, data);
    uart_send_command("subghz_weather");

    draw_screen(screen);

    ESP_LOGI(TAG, "Weather screen created");
    return screen;
}
