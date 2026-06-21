/**
 * @file subghz_settings_screen.c
 * @brief Global Sub-GHz Settings screen (CC1101 frequency correction).
 *
 * UART behaviour:
 *   on entry:  subghz_get_freq_correction  -> "[SUBGHZ_FREQ_CORRECTION] +X.XX"
 *   on Apply:  subghz_set_freq_correction <+/-X.XX>
 *
 * The displayed value lives in centi-MHz (int, range -500..+500) so the
 * arrow keys can adjust it without floating-point drift. The firmware
 * persists this value in its own NVS ("subghz" / "freq_corr_cc"); the
 * display only sends/receives it via UART.
 *
 * Keys:
 *   UP/DOWN     - move selection between [value] and [Apply]
 *   LEFT/RIGHT  - on value row: -/+ 0.01 MHz (SHIFT: -/+ 0.10 MHz)
 *   ENTER       - on Apply row: send subghz_set_freq_correction
 *   ESC         - back to Sub-GHz menu
 */

#include "subghz_settings_screen.h"
#include "uart_handler.h"
#include "keyboard.h"
#include "text_ui.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

static const char *TAG = "SUBGHZ_SET";

#define VALUE_MIN_CC     (-500)   /* -5.00 MHz */
#define VALUE_MAX_CC     ( 500)   /* +5.00 MHz */
#define VALUE_STEP_FINE  ( 1)
#define VALUE_STEP_COARSE (10)

typedef enum {
    MENU_VALUE = 0,
    MENU_APPLY,
    MENU_COUNT,
} menu_item_t;

#define TOAST_BUF_LEN 32

typedef struct {
    int  value_cc;      /* current displayed correction in centi-MHz */
    bool value_loaded;  /* true once we've received a reply from firmware */
    int  selected;

    /* Pending UART update from the UART task — applied in on_tick. */
    SemaphoreHandle_t mtx;
    bool pending_update;
    int  pending_value_cc;

    char toast[TOAST_BUF_LEN];
    bool toast_visible;
    int  toast_ttl_ticks;   /* on_tick counts down; clear at 0 */
} subghz_settings_data_t;

#define ROW_LABEL  2
#define ROW_VALUE  3
#define ROW_APPLY  5
#define ROW_TOAST  6

static void draw_value_row(subghz_settings_data_t *data, bool selected);
static void draw_apply_row(subghz_settings_data_t *data, bool selected);
static void draw_toast_row(subghz_settings_data_t *data);

static void format_value(int cc, char *out, size_t n)
{
    int abs_cc = cc < 0 ? -cc : cc;
    int whole = abs_cc / 100;
    int frac  = abs_cc % 100;
    snprintf(out, n, "%c%d.%02d MHz", cc < 0 ? '-' : '+', whole, frac);
}

static void uart_line_cb(const char *line, void *user_data)
{
    subghz_settings_data_t *data = (subghz_settings_data_t *)user_data;
    if (!data || !line) return;

    const char *tag = strstr(line, "[SUBGHZ_FREQ_CORRECTION]");
    if (!tag) return;

    const char *p = tag + strlen("[SUBGHZ_FREQ_CORRECTION]");
    while (*p == ' ') p++;

    float v = strtof(p, NULL);
    if (v >  5.0f) v =  5.0f;
    if (v < -5.0f) v = -5.0f;

    int cc = (int)(v * 100.0f + (v >= 0 ? 0.5f : -0.5f));
    if (cc < VALUE_MIN_CC) cc = VALUE_MIN_CC;
    if (cc > VALUE_MAX_CC) cc = VALUE_MAX_CC;

    if (data->mtx) xSemaphoreTake(data->mtx, portMAX_DELAY);
    data->pending_value_cc = cc;
    data->pending_update   = true;
    if (data->mtx) xSemaphoreGive(data->mtx);
}

static void draw_screen(screen_t *self)
{
    subghz_settings_data_t *data = (subghz_settings_data_t *)self->user_data;

    ui_clear();
    ui_draw_title("Sub-GHz Settings");

    ui_print(0, ROW_LABEL, "Freq Correction:", UI_COLOR_TEXT);

    draw_value_row(data, data->selected == MENU_VALUE);
    draw_apply_row(data, data->selected == MENU_APPLY);
    draw_toast_row(data);

    ui_draw_status("L/R:Adj ENT:Apply ESC:Back");
}

static void draw_value_row(subghz_settings_data_t *data, bool selected)
{
    int y = ROW_VALUE * 16;
    display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);

    char buf[24];
    if (data->value_loaded) {
        format_value(data->value_cc, buf, sizeof(buf));
    } else {
        snprintf(buf, sizeof(buf), "Loading...");
    }

    /* Indent so the row reads as a value, not a label. */
    char line[32];
    snprintf(line, sizeof(line), "  %s", buf);
    ui_draw_menu_item(ROW_VALUE, line, selected, false, false);
}

static void draw_apply_row(subghz_settings_data_t *data, bool selected)
{
    int y = ROW_APPLY * 16;
    display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
    ui_draw_menu_item(ROW_APPLY, "  [Apply]", selected, false, false);
}

static void draw_toast_row(subghz_settings_data_t *data)
{
    int y = ROW_TOAST * 16;
    display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
    if (data->toast_visible) {
        ui_print_center(ROW_TOAST, data->toast, UI_COLOR_HIGHLIGHT);
    }
}

static void show_toast(subghz_settings_data_t *data, const char *fmt, ...)
                       __attribute__((format(printf, 2, 3)));

static void show_toast(subghz_settings_data_t *data, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(data->toast, sizeof(data->toast), fmt, ap);
    va_end(ap);
    data->toast_visible = true;
    /* on_tick fires roughly every 500 ms — 4 ticks = ~2 s. */
    data->toast_ttl_ticks = 4;
}

static void clear_toast(subghz_settings_data_t *data)
{
    data->toast_visible = false;
    data->toast_ttl_ticks = 0;
    data->toast[0] = '\0';
}

static void adjust_value(subghz_settings_data_t *data, int delta_cc)
{
    if (!data->value_loaded) return;
    int v = data->value_cc + delta_cc;
    if (v < VALUE_MIN_CC) v = VALUE_MIN_CC;
    if (v > VALUE_MAX_CC) v = VALUE_MAX_CC;
    if (v == data->value_cc) return;
    data->value_cc = v;
    clear_toast(data);
    draw_value_row(data, data->selected == MENU_VALUE);
    draw_toast_row(data);
}

static void send_apply(subghz_settings_data_t *data)
{
    if (!data->value_loaded) return;
    char val[16];
    format_value(data->value_cc, val, sizeof(val));
    /* strip trailing " MHz" before passing to the CLI. */
    char *space = strchr(val, ' ');
    if (space) *space = '\0';

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "subghz_set_freq_correction %s", val);
    uart_send_command(cmd);
    ESP_LOGI(TAG, "Sent: %s", cmd);

    show_toast(data, "Saved %s MHz", val);
    draw_toast_row(data);
}

static void on_tick(screen_t *self)
{
    subghz_settings_data_t *data = (subghz_settings_data_t *)self->user_data;
    if (!data) return;

    /* Apply async UART update. */
    bool need_redraw = false;
    if (data->mtx) xSemaphoreTake(data->mtx, portMAX_DELAY);
    if (data->pending_update) {
        bool first_load = !data->value_loaded;
        data->value_cc = data->pending_value_cc;
        data->value_loaded = true;
        data->pending_update = false;
        need_redraw = true;
        (void)first_load;
    }
    if (data->mtx) xSemaphoreGive(data->mtx);

    if (need_redraw) {
        draw_value_row(data, data->selected == MENU_VALUE);
    }

    /* Toast TTL — countdown only when visible. */
    if (data->toast_visible && data->toast_ttl_ticks > 0) {
        data->toast_ttl_ticks--;
        if (data->toast_ttl_ticks == 0) {
            data->toast_visible = false;
            draw_toast_row(data);
        }
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    subghz_settings_data_t *data = (subghz_settings_data_t *)self->user_data;

    switch (key) {
        case KEY_UP: {
            int old = data->selected;
            data->selected = (old + MENU_COUNT - 1) % MENU_COUNT;
            draw_value_row(data, data->selected == MENU_VALUE);
            draw_apply_row(data, data->selected == MENU_APPLY);
            break;
        }

        case KEY_DOWN: {
            int old = data->selected;
            data->selected = (old + 1) % MENU_COUNT;
            draw_value_row(data, data->selected == MENU_VALUE);
            draw_apply_row(data, data->selected == MENU_APPLY);
            break;
        }

        case KEY_LEFT:
            if (data->selected == MENU_VALUE) {
                int step = keyboard_is_shift_held() ? -VALUE_STEP_COARSE
                                                    : -VALUE_STEP_FINE;
                adjust_value(data, step);
            }
            break;

        case KEY_RIGHT:
            if (data->selected == MENU_VALUE) {
                int step = keyboard_is_shift_held() ? VALUE_STEP_COARSE
                                                   : VALUE_STEP_FINE;
                adjust_value(data, step);
            }
            break;

        case KEY_ENTER:
        case KEY_SPACE:
            if (data->selected == MENU_APPLY) {
                send_apply(data);
            } else if (data->selected == MENU_VALUE) {
                /* Hop to Apply so a quick ENTER-ENTER applies. */
                data->selected = MENU_APPLY;
                draw_value_row(data, false);
                draw_apply_row(data, true);
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
    subghz_settings_data_t *data = (subghz_settings_data_t *)self->user_data;
    if (!data) return;

    uart_clear_line_callback();
    if (data->mtx) {
        vSemaphoreDelete(data->mtx);
        data->mtx = NULL;
    }
    free(data);
    self->user_data = NULL;
}

static void on_resume(screen_t *self)
{
    draw_screen(self);
}

screen_t* subghz_settings_screen_create(void *params)
{
    (void)params;

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    subghz_settings_data_t *data = calloc(1, sizeof(subghz_settings_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }

    data->mtx = xSemaphoreCreateMutex();
    data->selected = MENU_VALUE;
    data->value_cc = 0;
    data->value_loaded = false;

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;

    draw_screen(screen);

    /* Ask firmware for the current correction value. The reply comes in on
     * uart_line_cb and is consumed in on_tick. */
    uart_register_line_callback(uart_line_cb, data);
    uart_send_command("subghz_get_freq_correction");

    ESP_LOGI(TAG, "Sub-GHz settings screen created");
    return screen;
}
