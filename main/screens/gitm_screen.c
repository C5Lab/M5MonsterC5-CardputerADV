/**
 * @file gitm_screen.c
 * @brief Clean GITM wizard: force WiFi → SoftAP SSID → SoftAP pass → session
 */

#include "gitm_screen.h"
#include "gitm_session_screen.h"
#include "wifi_connect_screen.h"
#include "text_input_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "GITM";

typedef enum {
    STATE_WAIT_WIFI,
    STATE_SETUP,
} gitm_state_t;

typedef struct {
    gitm_state_t state;
    char softap_ssid[33];
    char softap_pass[64];
    bool needs_push_wifi;
    bool needs_push_ssid;
    bool needs_push_pass;
    bool needs_start_session;
    screen_t *self;
} gitm_data_t;

static void draw_screen(screen_t *self);
static void on_ssid_submitted(const char *text, void *user_data);
static void on_pass_submitted(const char *text, void *user_data);

static void draw_screen(screen_t *self)
{
    gitm_data_t *data = (gitm_data_t *)self->user_data;
    ui_clear();
    ui_draw_title("GITM");

    if (data->state == STATE_WAIT_WIFI) {
        ui_print_center(2, "WiFi required", UI_COLOR_HIGHLIGHT);
        ui_print_center(3, "Connect upstream first", UI_COLOR_DIMMED);
        ui_print_center(5, "ENTER: Connect", UI_COLOR_TEXT);
        ui_draw_status("ENTER:WiFi ESC:Back");
        return;
    }

    ui_print_center(3, "Configure SoftAP...", UI_COLOR_DIMMED);
    ui_draw_status("Please wait");
}

static void on_ssid_submitted(const char *text, void *user_data)
{
    gitm_data_t *data = (gitm_data_t *)user_data;
    if (!data || !text || !text[0]) return;

    strncpy(data->softap_ssid, text, sizeof(data->softap_ssid) - 1);
    data->softap_ssid[sizeof(data->softap_ssid) - 1] = '\0';
    data->needs_push_pass = true;
    screen_manager_pop();
}

static void on_pass_submitted(const char *text, void *user_data)
{
    gitm_data_t *data = (gitm_data_t *)user_data;
    if (!data) return;

    size_t len = text ? strlen(text) : 0;
    if (len > 0 && (len < 8 || len > 63)) {
        /* Reject short password — re-prompt */
        data->needs_push_pass = true;
        screen_manager_pop();
        return;
    }

    if (text) {
        strncpy(data->softap_pass, text, sizeof(data->softap_pass) - 1);
        data->softap_pass[sizeof(data->softap_pass) - 1] = '\0';
    } else {
        data->softap_pass[0] = '\0';
    }
    data->needs_start_session = true;
    screen_manager_pop();
}

static void push_ssid_input(gitm_data_t *data)
{
    text_input_params_t *p = calloc(1, sizeof(text_input_params_t));
    if (!p) return;
    p->title = "SoftAP SSID";
    p->hint = "Capture network name";
    p->on_submit = on_ssid_submitted;
    p->user_data = data;
    p->allow_empty = false;
    p->max_length = 32;
    screen_manager_push(text_input_screen_create, p);
}

static void push_pass_input(gitm_data_t *data)
{
    text_input_params_t *p = calloc(1, sizeof(text_input_params_t));
    if (!p) return;
    p->title = "SoftAP Password";
    p->hint = "Empty=open, else 8-63";
    p->on_submit = on_pass_submitted;
    p->user_data = data;
    p->allow_empty = true;
    p->max_length = 63;
    p->masked = true;
    screen_manager_push(text_input_screen_create, p);
}

static void start_session(gitm_data_t *data)
{
    gitm_session_params_t *sp = calloc(1, sizeof(gitm_session_params_t));
    if (!sp) return;
    sp->mode = GITM_MODE_CLEAN;
    strncpy(sp->ssid, data->softap_ssid, sizeof(sp->ssid) - 1);
    strncpy(sp->password, data->softap_pass, sizeof(sp->password) - 1);
    sp->victim_index = 0;
    screen_manager_replace(gitm_session_screen_create, sp);
}

static void begin_setup(gitm_data_t *data)
{
    data->state = STATE_SETUP;
    data->needs_push_ssid = true;
    draw_screen(data->self);
}

static void on_tick(screen_t *self)
{
    gitm_data_t *data = (gitm_data_t *)self->user_data;
    if (!data) return;

    if (data->state == STATE_WAIT_WIFI && uart_is_wifi_connected()) {
        begin_setup(data);
        return;
    }

    if (data->needs_push_wifi) {
        data->needs_push_wifi = false;
        wifi_connect_params_t *w = malloc(sizeof(wifi_connect_params_t));
        if (w) {
            w->auto_close_on_success = true;
            screen_manager_push(wifi_connect_screen_create, w);
        } else {
            screen_manager_push(wifi_connect_screen_create, NULL);
        }
        return;
    }

    if (data->needs_push_ssid) {
        data->needs_push_ssid = false;
        push_ssid_input(data);
        return;
    }

    if (data->needs_push_pass) {
        data->needs_push_pass = false;
        push_pass_input(data);
        return;
    }

    if (data->needs_start_session) {
        data->needs_start_session = false;
        start_session(data);
        return;
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    gitm_data_t *data = (gitm_data_t *)self->user_data;
    if (!data) return;

    switch (key) {
        case KEY_ENTER:
        case KEY_SPACE:
            if (data->state == STATE_WAIT_WIFI) {
                data->needs_push_wifi = true;
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

static void on_resume(screen_t *self)
{
    gitm_data_t *data = (gitm_data_t *)self->user_data;
    if (!data) return;

    if (data->state == STATE_WAIT_WIFI && uart_is_wifi_connected()) {
        begin_setup(data);
        return;
    }
    draw_screen(self);
}

static void on_destroy(screen_t *self)
{
    if (self->user_data) {
        free(self->user_data);
    }
}

screen_t *gitm_screen_create(void *params)
{
    (void)params;

    ESP_LOGI(TAG, "Creating GITM wizard");

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    gitm_data_t *data = calloc(1, sizeof(gitm_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }

    data->self = screen;
    if (uart_is_wifi_connected()) {
        data->state = STATE_SETUP;
        data->needs_push_ssid = true;
    } else {
        data->state = STATE_WAIT_WIFI;
    }

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;

    draw_screen(screen);
    return screen;
}
