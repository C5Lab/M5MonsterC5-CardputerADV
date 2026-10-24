/** Opt-in JanOS client list. UART callbacks only update data; UI owns navigation. */
#include "sniffer_results_screen.h"
#include "sniffer_clients_model.h"
#include "station_deauth_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "buzzer.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VISIBLE 6
#define START_ROW 1
#define LINE_COLS 30
#define START_TIMEOUT_US (15000000LL)
#define LIST_IDLE_TIMEOUT_US (5000000LL)
#define LIST_TOTAL_TIMEOUT_US (300000000LL)

typedef struct {
    sc_model_t model;
    SemaphoreHandle_t mutex;
    unsigned selected, offset, view_count;
    uint16_t ap_clients[SC_MAX_APS];
    bool loading, dirty, pending, started;
    uint8_t pending_ap[6], pending_station[6];
    char pending_name[33], message[48];
    int64_t deadline, list_start;
} sniffer_results_data_t;

typedef struct {
    bool is_ap;
    unsigned ap, client;
} view_item_t;

static void print_row(int row, const char *text, uint16_t fg)
{
    char line[LINE_COLS + 1];
    snprintf(line, sizeof(line), "%-*.*s", LINE_COLS, LINE_COLS, text ? text : "");
    ui_print(0, row, line, fg);
}
static void rebuild_view(sniffer_results_data_t *d)
{
    memset(d->ap_clients, 0, sizeof(d->ap_clients));
    for(unsigned i = 0; i < d->model.client_count; ++i) {
        unsigned ap = d->model.clients[i].ap;
        if(ap < SC_MAX_APS) ++d->ap_clients[ap];
    }
    d->view_count = d->model.ap_count + d->model.client_count;
}
static bool view_item(const sniffer_results_data_t *d, unsigned view, view_item_t *out)
{
    unsigned v = 0, client = 0;
    for(unsigned a = 0; a < d->model.ap_count; ++a) {
        unsigned n = d->ap_clients[a];
        if(v == view) { out->is_ap = true; out->ap = a; out->client = 0; return true; }
        ++v;
        if(view < v + n) {
            out->is_ap = false; out->ap = a; out->client = client + (view - v);
            return true;
        }
        v += n;
        client += n;
    }
    return false;
}
static bool is_client_view(const sniffer_results_data_t *d, unsigned view)
{
    view_item_t item;
    return view_item(d, view, &item) && !item.is_ap;
}
static unsigned first_client_view(const sniffer_results_data_t *d)
{
    for(unsigned v = 0; v < d->view_count; ++v) if(is_client_view(d, v)) return v;
    return 0;
}
static unsigned next_client_view(const sniffer_results_data_t *d, unsigned from, int dir)
{
    if(!d->view_count) return from;
    unsigned v = from;
    for(unsigned i = 0; i < d->view_count; ++i) {
        if(dir > 0) v = (v + 1) % d->view_count;
        else v = v ? v - 1 : d->view_count - 1;
        if(is_client_view(d, v)) return v;
    }
    return from;
}
static unsigned client_in_window(const sniffer_results_data_t *d, bool last)
{
    unsigned found = d->selected, end = d->offset + VISIBLE, seen = 0;
    if(end > d->view_count) end = d->view_count;
    for(unsigned v = d->offset; v < end; ++v) {
        if(!is_client_view(d, v)) continue;
        if(!seen || !last) found = v;
        else found = v;
        seen = 1;
        if(!last && seen) break;
    }
    if(!seen) return next_client_view(d, d->offset ? d->offset - 1 : 0, 1);
    return found;
}
static void finish_snapshot(sniffer_results_data_t *d)
{
    rebuild_view(d);
    d->offset = 0;
    d->selected = first_client_view(d);
}
static void format_ap_line(const sniffer_results_data_t *d, unsigned ap, char *out, size_t size)
{
    const sc_ap_t *a = &d->model.aps[ap];
    char label[33];
    sc_ap_label(a, label, sizeof(label));
    if(a->ssid_len) snprintf(out, size, "%s, CH%u: %u", label, a->channel, d->ap_clients[ap]);
    else snprintf(out, size, "%s: %u", label, d->ap_clients[ap]);
}
static void draw_view_row(sniffer_results_data_t *d, unsigned view)
{
    if(view < d->offset || view >= d->offset + VISIBLE) return;
    int row = START_ROW + (int)(view - d->offset);
    if(view >= d->view_count) { print_row(row, "", UI_COLOR_TEXT); return; }
    view_item_t item;
    if(!view_item(d, view, &item)) { print_row(row, "", UI_COLOR_TEXT); return; }
    char text[48];
    if(item.is_ap) {
        format_ap_line(d, item.ap, text, sizeof(text));
        print_row(row, text, UI_COLOR_HIGHLIGHT);
    } else {
        char mac[18];
        sc_mac_text(mac, d->model.clients[item.client].mac);
        snprintf(text, sizeof(text), "%c%s", view == d->selected ? '>' : ' ', mac);
        print_row(row, text, view == d->selected ? UI_COLOR_SELECTED : UI_COLOR_DIMMED);
    }
}
static void draw_view_window(sniffer_results_data_t *d)
{
    for(unsigned i = 0; i < VISIBLE; ++i) draw_view_row(d, d->offset + i);
}
static void draw_locked(sniffer_results_data_t *d)
{
    ui_clear(); ui_draw_title("Sniffer Clients");
    if(d->loading) ui_print_center(3,"Loading...",UI_COLOR_DIMMED);
    else if(d->model.failed) {
        ui_print_center(3,d->model.error,UI_COLOR_TEXT);
        ui_print_center(5,"R: Retry",UI_COLOR_DIMMED);
    } else if(!d->model.client_count) ui_print_center(3,"No clients",UI_COLOR_DIMMED);
    else draw_view_window(d);
    if(d->pending) ui_draw_status("Starting... ESC: Cancel");
    else if(d->message[0]) ui_draw_status(d->message);
    else ui_draw_status("UP/DN:Sel D:Deauth R:Reload");
}
static void draw_screen(screen_t *self)
{
    sniffer_results_data_t *d=self->user_data;
    xSemaphoreTake(d->mutex,portMAX_DELAY); draw_locked(d); xSemaphoreGive(d->mutex);
}
static void uart_line_callback(const char *line,void *user_data)
{
    sniffer_results_data_t *d=user_data;
    xSemaphoreTake(d->mutex,portMAX_DELAY);
    /* Once a list has timed out/failed, discard its remaining UART records. */
    sc_event_t event=SC_IGNORED;
    if(d->loading || d->pending)
        event=sc_model_line(&d->model,line,d->pending ? d->pending_ap : NULL,d->pending ? d->pending_station : NULL);
    if(event==SC_UPDATED) {
        d->deadline=esp_timer_get_time()+LIST_IDLE_TIMEOUT_US;
        if(d->model.complete) { d->loading=false; finish_snapshot(d); }
        else if(d->model.failed) d->loading=false;
        d->dirty=true;
    } else if(event==SC_STARTED) {
        d->pending=false; d->started=true; d->dirty=true;
    } else if(event==SC_REFUSED) {
        d->pending=false;
        snprintf(d->message,sizeof(d->message),"%.30s",d->model.error);
        d->dirty=true;
    }
    xSemaphoreGive(d->mutex);
}
static void on_tick(screen_t *self)
{
    sniffer_results_data_t *d=self->user_data;
    bool stop=false, started=false;
    station_deauth_params_t *params=NULL;
    xSemaphoreTake(d->mutex,portMAX_DELAY);
    int64_t now=esp_timer_get_time();
    if(d->pending && now>d->deadline) {
        d->pending=false; stop=true; d->dirty=true;
        d->model.failed=true;
        snprintf(d->model.error,sizeof(d->model.error),"Start timeout; R to reload");
        snprintf(d->message,sizeof(d->message),"Stopped; R: Reload");
    }
    if(d->loading && (now>d->deadline || now-d->list_start>LIST_TOTAL_TIMEOUT_US)) {
        d->loading=false; d->model.failed=true; d->dirty=true;
        snprintf(d->model.error,sizeof(d->model.error),"No list; update JanOS / R");
    }
    if(d->started) {
        d->started=false; started=true;
        params=calloc(1,sizeof(*params));
        if(params) {
            sc_mac_text(params->mac,d->pending_station);
            sc_mac_text(params->bssid,d->pending_ap);
            snprintf(params->ssid,sizeof(params->ssid),"%s",d->pending_name);
        } else {
            stop=true; snprintf(d->message,sizeof(d->message),"Not enough memory; stopped");
        }
    }
    if(d->dirty) { d->dirty=false; draw_locked(d); }
    xSemaphoreGive(d->mutex);
    if(stop) uart_send_command("stop");
    if(started && params) {
        /* Never navigate/register/clear from the UART dispatch task. */
        uart_clear_line_callback();
        if(screen_manager_push(station_deauth_screen_create,params)!=ESP_OK) {
            uart_send_command("stop");
            uart_register_line_callback(uart_line_callback,d);
            xSemaphoreTake(d->mutex,portMAX_DELAY);
            snprintf(d->message,sizeof(d->message),"Screen error; stopped"); d->dirty=true;
            xSemaphoreGive(d->mutex);
        } else buzzer_beep_attack();
    }
}
static void on_key(screen_t *self,key_code_t key)
{
    sniffer_results_data_t *d=self->user_data;
    char command[96]={0}; bool leave=false, stop=false, reload=false, dirty=true;
    xSemaphoreTake(d->mutex,portMAX_DELAY);
    if(key==KEY_ESC || key==KEY_Q || key==KEY_BACKSPACE) {
        leave=true; stop=d->pending || d->started;
        d->pending=false; d->started=false;
    } else if(key==KEY_R && !d->pending && !d->started && !d->loading) {
        sc_model_free(&d->model); d->loading=true; d->selected=0; d->offset=0; d->view_count=0;
        memset(d->ap_clients,0,sizeof(d->ap_clients));
        d->message[0]=0; d->deadline=esp_timer_get_time()+LIST_IDLE_TIMEOUT_US;
        d->list_start=esp_timer_get_time(); reload=true;
    } else if(!d->loading && d->model.complete && !d->model.failed && !d->pending && !d->started && d->model.client_count) {
        if(key==KEY_UP || key==KEY_DOWN) {
            unsigned old=d->selected, old_offset=d->offset;
            unsigned first=first_client_view(d);
            unsigned top=client_in_window(d,false), bottom=client_in_window(d,true);
            if(key==KEY_UP) {
                if(d->selected==first) {
                    d->selected=next_client_view(d,first,-1);
                    d->offset=(d->selected/VISIBLE)*VISIBLE;
                } else if(d->selected==top && d->offset>0) {
                    d->offset=d->offset>VISIBLE ? d->offset-VISIBLE : 0;
                    d->selected=client_in_window(d,true);
                } else d->selected=next_client_view(d,d->selected,-1);
            } else {
                if(d->selected==next_client_view(d,first,-1)) {
                    d->selected=first; d->offset=0;
                } else if(d->selected==bottom && d->offset+VISIBLE<d->view_count) {
                    d->offset+=VISIBLE;
                    d->selected=client_in_window(d,false);
                } else d->selected=next_client_view(d,d->selected,1);
            }
            if(d->offset!=old_offset) draw_view_window(d);
            else { draw_view_row(d, old); draw_view_row(d, d->selected); }
            dirty=false;
        } else if(key==KEY_D) {
            view_item_t item;
            if(view_item(d,d->selected,&item) && !item.is_ap) {
                sc_client_t *client=&d->model.clients[item.client];
                sc_ap_t *ap=&d->model.aps[client->ap];
                char bssid[18],station[18]; sc_mac_text(bssid,ap->bssid); sc_mac_text(station,client->mac);
                memcpy(d->pending_ap,ap->bssid,6); memcpy(d->pending_station,client->mac,6);
                sc_ap_label(ap,d->pending_name,sizeof(d->pending_name));
                snprintf(command,sizeof(command),"start_deauth_station %s %s",bssid,station);
                d->pending=true; d->message[0]=0; d->deadline=esp_timer_get_time()+START_TIMEOUT_US;
            }
        }
    }
    if(dirty) d->dirty=true;
    xSemaphoreGive(d->mutex);
    if(stop) uart_send_command("stop");
    if(reload) uart_send_command("show_sniffer_clients");
    if(command[0] && uart_send_command(command)!=ESP_OK) {
        xSemaphoreTake(d->mutex,portMAX_DELAY); d->pending=false;
        snprintf(d->message,sizeof(d->message),"UART send failed"); xSemaphoreGive(d->mutex);
    }
    if(leave) screen_manager_pop();
}
static void on_resume(screen_t *self)
{
    sniffer_results_data_t *d=self->user_data;
    uart_register_line_callback(uart_line_callback,d);
    draw_screen(self);
}
static void on_destroy(screen_t *self)
{
    sniffer_results_data_t *d=self->user_data;
    uart_clear_line_callback();
    if(d) { sc_model_free(&d->model); vSemaphoreDelete(d->mutex); free(d); }
}
screen_t *sniffer_results_screen_create(void *params)
{
    (void)params;
    screen_t *screen=screen_alloc(); if(!screen) return NULL;
    sniffer_results_data_t *d=calloc(1,sizeof(*d));
    if(!d) { free(screen); return NULL; }
    d->mutex=xSemaphoreCreateMutex();
    if(!d->mutex) { free(d); free(screen); return NULL; }
    screen->user_data=d; screen->on_draw=draw_screen; screen->on_tick=on_tick;
    screen->on_key=on_key; screen->on_destroy=on_destroy; screen->on_resume=on_resume;
    d->loading=true; d->deadline=esp_timer_get_time()+LIST_IDLE_TIMEOUT_US; d->list_start=esp_timer_get_time();
    uart_register_line_callback(uart_line_callback,d);
    uart_send_command("show_sniffer_clients"); draw_screen(screen); return screen;
}
