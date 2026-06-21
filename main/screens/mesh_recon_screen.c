/**
 * @file mesh_recon_screen.c
 * @brief Mesh Recon (802.15.4 / Zigbee / Thread) live screen.
 *
 * Protocol (board firmware unchanged):
 *   start_zig_recon all 250        -> start channel-hopping recon (250ms dwell)
 *   zig_recon_status               -> one-line status
 *   zig_recon_list all             -> PAN list
 *   zig_recon_nodes all            -> nodes + edges
 *   stop / zig_recon_clear         -> stop and clear on exit
 *
 * All recon lines start with "[ZIG]"; parsed by iot_recon (see iot_recon.h).
 * Two views: PAN list, and the expanded PAN's nodes with a "locate" panel for
 * a tracked node (live RSSI + getting-stronger/weaker trend).
 *
 * Data accumulates via upsert; entries older than AGE_OUT_MS (by the board's
 * age_ms) are filtered out of the views so stale PANs/nodes disappear. SPACE
 * pauses/resumes polling for calm browsing.
 */

#include "mesh_recon_screen.h"
#include "iot_recon.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "MESH_RECON";

#define POLL_INTERVAL_US  1000000   // 1 Hz
#define PAN_VISIBLE       5
#define NODE_VISIBLE      4
#define AGE_OUT_MS        60000     // hide entries not seen for >60s

typedef enum { VIEW_PANS = 0, VIEW_NODES } mesh_view_t;

typedef struct {
    iot_recon_t recon;
    mesh_view_t view;

    int  pan_sel;               // index into the age-filtered PAN list
    int  pan_scroll;
    char expanded_pan[12];

    int  node_sel;              // index into the age-filtered node list
    int  node_scroll;

    char tracked_pan[12];
    char tracked_addr[24];
    int  tracked_last_rssi;
    int  trend;                 // -1 weaker, 0 same, +1 stronger

    bool blocked;                // wardrive/anti-surv active -> cannot start
    bool start_pending;
    int64_t start_due_us;
    int  idle_status_count;
    int  restart_attempts;
    bool owns_radio;
    bool start_failed;
    bool paused;                // polling paused (SPACE) for static browsing
    bool needs_redraw;
    bool ui_initialized;
    int  drawn_pan_count;       // PAN view repaints list only when this changes
    int  drawn_node_count;      // node view repaints list only when this changes
    esp_timer_handle_t timer;
    screen_t *self;
} mesh_data_t;

static void draw_screen(screen_t *self);

// ---- age-filtered PAN access ------------------------------------------------

static int visible_pan_count(mesh_data_t *d)
{
    int n = 0;
    for (int i = 0; i < d->recon.pan_count; i++) {
        if (d->recon.pans[i].age_ms <= AGE_OUT_MS) n++;
    }
    return n;
}

// recon.pans index of the k-th fresh PAN, or -1.
static int visible_pan_index(mesh_data_t *d, int k)
{
    int n = 0;
    for (int i = 0; i < d->recon.pan_count; i++) {
        if (d->recon.pans[i].age_ms <= AGE_OUT_MS) {
            if (n == k) return i;
            n++;
        }
    }
    return -1;
}

// ---- age-filtered node access (by expanded PAN) -----------------------------

static int pan_node_count(mesh_data_t *d)
{
    int n = 0;
    for (int i = 0; i < d->recon.node_count; i++) {
        if (d->recon.nodes[i].age_ms <= AGE_OUT_MS &&
            strcmp(d->recon.nodes[i].pan_id, d->expanded_pan) == 0) n++;
    }
    return n;
}

static int pan_node_index(mesh_data_t *d, int k)
{
    int n = 0;
    for (int i = 0; i < d->recon.node_count; i++) {
        if (d->recon.nodes[i].age_ms <= AGE_OUT_MS &&
            strcmp(d->recon.nodes[i].pan_id, d->expanded_pan) == 0) {
            if (n == k) return i;
            n++;
        }
    }
    return -1;
}

// ---- UART stream ------------------------------------------------------------

static void uart_line_callback(const char *line, void *user_data)
{
    mesh_data_t *d = (mesh_data_t *)user_data;
    if (!d || !line) return;

    zig_line_kind_t kind = zig_line_kind(line);
    if (kind == ZIG_LINE_NONE) return;

    iot_recon_feed_line(&d->recon, line);

    if (kind == ZIG_LINE_STATUS && !d->blocked && !d->start_pending && !d->paused) {
        if (d->recon.status.active) {
            d->idle_status_count = 0;
            d->restart_attempts = 0;
        } else if (++d->idle_status_count >= 3) {
            d->idle_status_count = 0;
            if (d->timer) esp_timer_stop(d->timer);
            if (d->restart_attempts < 3) {
                d->restart_attempts++;
                d->start_pending = true;
                d->start_due_us = esp_timer_get_time() + 300000;
                uart_send_command("stop");
            } else {
                d->start_failed = true;
            }
        }
    }

    // Update locate trend when a sample for the tracked node arrives.
    if (kind == ZIG_LINE_NODE && d->tracked_addr[0] && d->recon.node_count > 0) {
        iot_node_t *n = &d->recon.nodes[d->recon.node_count - 1];
        if (strcmp(n->addr, d->tracked_addr) == 0 &&
            strcmp(n->pan_id, d->tracked_pan) == 0) {
            if (n->last_rssi > d->tracked_last_rssi) d->trend = 1;
            else if (n->last_rssi < d->tracked_last_rssi) d->trend = -1;
            else d->trend = 0;
            d->tracked_last_rssi = n->last_rssi;
        }
    }

    d->needs_redraw = true;
}

// ---- polling timer ----------------------------------------------------------

static void poll_timer_cb(void *arg)
{
    mesh_data_t *d = (mesh_data_t *)arg;
    if (!d || d->blocked || d->start_pending) return;

    uart_send_command("zig_recon_status");
    uart_send_command("zig_recon_list all");

    if (d->view == VIEW_NODES) {
        uart_send_command("zig_recon_nodes all");
    }
}

static void set_paused(mesh_data_t *d, bool paused)
{
    d->paused = paused;
    if (d->timer) {
        if (paused || d->start_failed) esp_timer_stop(d->timer);
        else                           esp_timer_start_periodic(d->timer, POLL_INTERVAL_US);
    }
    d->ui_initialized = false;   // force a full retitle/redraw
    draw_screen(d->self);
}

// ---- drawing ----------------------------------------------------------------

// Live header line (row 1) — cheap single-row refresh, no list flicker.
static void draw_pan_header(mesh_data_t *d)
{
    iot_status_t *st = &d->recon.status;
    const char *mode = d->paused ? "PAUSE" : (st->active ? "scan" : "idle");
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "%s ch%d pk%d PAN%d dr%d",
             mode, st->channel, st->packets, visible_pan_count(d), st->dropped);
    display_fill_rect(0, 1 * 16, DISPLAY_WIDTH, 16, UI_COLOR_BG);
    ui_print(0, 1, hdr, UI_COLOR_DIMMED);
}

// Map the raw proto token to a short, readable label (<=6 cols).
static const char *proto_label(const char *proto)
{
    if (strstr(proto, "zigbee")) return "Zigbee";
    if (strstr(proto, "thread")) return "Thread";
    if (strstr(proto, "802154") || strstr(proto, "ieee")) return "802.15";
    return proto;
}

static void draw_pan_list(mesh_data_t *d)
{
    iot_status_t *st = &d->recon.status;
    int total = visible_pan_count(d);

    // Keep the selection within the (possibly shrunken) visible list.
    if (d->pan_sel >= total) d->pan_sel = (total > 0) ? total - 1 : 0;
    if (d->pan_sel < d->pan_scroll) d->pan_scroll = d->pan_sel;
    if (d->pan_sel >= d->pan_scroll + PAN_VISIBLE)
        d->pan_scroll = d->pan_sel - PAN_VISIBLE + 1;

    draw_pan_header(d);

    if (d->blocked) {
        ui_print_center(2, "Stop active radio first", UI_COLOR_HIGHLIGHT);
        ui_print_center(4, "Wardrive/Anti-Surv busy", UI_COLOR_DIMMED);
    } else if (d->start_failed) {
        ui_print_center(2, "Mesh start failed", UI_COLOR_HIGHLIGHT);
        ui_print_center(4, "Back and try again", UI_COLOR_DIMMED);
    } else if (d->start_pending) {
        ui_print_center(3, "Starting Mesh Recon...", UI_COLOR_HIGHLIGHT);
    } else if (total == 0) {
        ui_print_center(3, st->active ? "Scanning for mesh..." : "No Mesh networks yet",
                        st->active ? UI_COLOR_DIMMED : UI_COLOR_HIGHLIGHT);
    } else {
        for (int r = 0; r < PAN_VISIBLE; r++) {
            int k = d->pan_scroll + r;
            int row = r + 2;
            int pi = (k < total) ? visible_pan_index(d, k) : -1;
            if (pi >= 0) {
                iot_pan_t *p = &d->recon.pans[pi];
                char age[12];
                iot_recon_humanize_age(p->age_ms, age, sizeof(age));
                char label[64];
                snprintf(label, sizeof(label), "%-6.6s n%d %d %s",
                         proto_label(p->proto), p->nodes, p->last_rssi, age);
                ui_draw_menu_item(row, label, k == d->pan_sel, false, false);
            } else {
                display_fill_rect(0, row * 16, DISPLAY_WIDTH, 16, UI_COLOR_BG);
            }
        }
        if (d->pan_scroll > 0) ui_print(UI_COLS - 2, 2, "^", UI_COLOR_DIMMED);
        if (d->pan_scroll + PAN_VISIBLE < total)
            ui_print(UI_COLS - 2, 1 + PAN_VISIBLE, "v", UI_COLOR_DIMMED);
    }

    d->drawn_pan_count = total;
    if (d->blocked || d->start_failed) {
        ui_draw_status("ESC:Back");
    } else {
        ui_draw_status(d->paused ? "PAUSED SPC:Resume ESC" : "UP/DN ENTER SPC:Pause");
    }
}

// Locate panel (row 6) for the tracked node — refreshed live while in nodes
// view so you can walk toward the device.
static void draw_locate_panel(mesh_data_t *d)
{
    display_fill_rect(0, 6 * 16, DISPLAY_WIDTH, 16, UI_COLOR_BG);
    if (d->tracked_addr[0] && strcmp(d->tracked_pan, d->expanded_pan) == 0) {
        const char *arrow = d->trend > 0 ? "STRONGER" : (d->trend < 0 ? "weaker" : "steady");
        char panel[40];
        snprintf(panel, sizeof(panel), "TRK %.9s %d %s",
                 d->tracked_addr, d->tracked_last_rssi, arrow);
        ui_print(0, 6, panel, d->trend > 0 ? UI_COLOR_HIGHLIGHT : UI_COLOR_TEXT);
    }
}

static void draw_node_view(mesh_data_t *d)
{
    int total = pan_node_count(d);

    if (d->node_sel >= total) d->node_sel = (total > 0) ? total - 1 : 0;
    if (d->node_sel < d->node_scroll) d->node_scroll = d->node_sel;
    if (d->node_sel >= d->node_scroll + NODE_VISIBLE)
        d->node_scroll = d->node_sel - NODE_VISIBLE + 1;

    char hdr[40];
    snprintf(hdr, sizeof(hdr), "PAN %s nodes:%d", d->expanded_pan, total);
    display_fill_rect(0, 1 * 16, DISPLAY_WIDTH, 16, UI_COLOR_BG);
    ui_print(0, 1, hdr, UI_COLOR_DIMMED);

    if (total == 0) {
        ui_print_center(3, "No nodes yet", UI_COLOR_DIMMED);
    } else {
        for (int r = 0; r < NODE_VISIBLE; r++) {
            int k = d->node_scroll + r;
            int row = r + 2;
            int ni = (k < total) ? pan_node_index(d, k) : -1;
            if (ni >= 0) {
                iot_node_t *n = &d->recon.nodes[ni];
                bool tracked = (d->tracked_addr[0] &&
                                strcmp(n->addr, d->tracked_addr) == 0 &&
                                strcmp(n->pan_id, d->tracked_pan) == 0);
                char label[40];
                snprintf(label, sizeof(label), "%c%-5.5s %-9.9s %d",
                         tracked ? '*' : ' ', n->role, n->addr, n->last_rssi);
                ui_draw_menu_item(row, label, k == d->node_sel, false, false);
            } else {
                display_fill_rect(0, row * 16, DISPLAY_WIDTH, 16, UI_COLOR_BG);
            }
        }
        if (d->node_scroll > 0) ui_print(UI_COLS - 2, 2, "^", UI_COLOR_DIMMED);
        if (d->node_scroll + NODE_VISIBLE < total)
            ui_print(UI_COLS - 2, 1 + NODE_VISIBLE, "v", UI_COLOR_DIMMED);
    }

    draw_locate_panel(d);
    d->drawn_node_count = total;
    ui_draw_status(d->paused ? "PAUSED SPC:Resume ESC" : "ENTER:Track SPC:Pause");
}

static void draw_screen(screen_t *self)
{
    mesh_data_t *d = (mesh_data_t *)self->user_data;

    if (!d->ui_initialized) {
        ui_clear();
        ui_draw_title(d->view == VIEW_PANS ? "Mesh Recon" : "Mesh Nodes");
        d->ui_initialized = true;
    }

    // Clear content band before redrawing the active view.
    display_fill_rect(0, 19, DISPLAY_WIDTH, 98, UI_COLOR_BG);

    if (d->view == VIEW_PANS) draw_pan_list(d);
    else draw_node_view(d);
}

// ---- input ------------------------------------------------------------------

static void open_selected_pan(mesh_data_t *d)
{
    int pi = visible_pan_index(d, d->pan_sel);
    if (pi < 0) return;
    snprintf(d->expanded_pan, sizeof(d->expanded_pan), "%s", d->recon.pans[pi].pan_id);
    d->view = VIEW_NODES;
    d->node_sel = 0;
    d->node_scroll = 0;
    d->ui_initialized = false;   // retitle "Mesh Nodes" + full clear
    if (!d->paused) uart_send_command("zig_recon_nodes all");
    draw_screen(d->self);
}

static void track_selected_node(mesh_data_t *d)
{
    int ni = pan_node_index(d, d->node_sel);
    if (ni < 0) return;
    iot_node_t *n = &d->recon.nodes[ni];
    snprintf(d->tracked_pan, sizeof(d->tracked_pan), "%s", n->pan_id);
    snprintf(d->tracked_addr, sizeof(d->tracked_addr), "%s", n->addr);
    d->tracked_last_rssi = n->last_rssi;
    d->trend = 0;
    d->needs_redraw = true;
}

static void on_key(screen_t *self, key_code_t key)
{
    mesh_data_t *d = (mesh_data_t *)self->user_data;
    if (!d) return;

    if (key == KEY_SPACE) {          // pause/resume polling in either view
        set_paused(d, !d->paused);
        return;
    }

    if (d->view == VIEW_PANS) {
        int total = visible_pan_count(d);
        switch (key) {
            case KEY_UP:
                if (total > 0) {
                    d->pan_sel = (d->pan_sel > 0) ? d->pan_sel - 1 : total - 1;
                    d->pan_scroll = (d->pan_sel / PAN_VISIBLE) * PAN_VISIBLE;
                    draw_screen(self);
                }
                break;
            case KEY_DOWN:
                if (total > 0) {
                    d->pan_sel = (d->pan_sel < total - 1) ? d->pan_sel + 1 : 0;
                    d->pan_scroll = (d->pan_sel / PAN_VISIBLE) * PAN_VISIBLE;
                    draw_screen(self);
                }
                break;
            case KEY_ENTER:
                open_selected_pan(d);
                break;
            case KEY_ESC:
            case KEY_Q:
            case KEY_BACKSPACE:
                screen_manager_pop();
                break;
            default:
                break;
        }
    } else { // VIEW_NODES
        int total = pan_node_count(d);
        switch (key) {
            case KEY_UP:
                if (total > 0) {
                    d->node_sel = (d->node_sel > 0) ? d->node_sel - 1 : total - 1;
                    d->node_scroll = (d->node_sel / NODE_VISIBLE) * NODE_VISIBLE;
                    draw_screen(self);
                }
                break;
            case KEY_DOWN:
                if (total > 0) {
                    d->node_sel = (d->node_sel < total - 1) ? d->node_sel + 1 : 0;
                    d->node_scroll = (d->node_sel / NODE_VISIBLE) * NODE_VISIBLE;
                    draw_screen(self);
                }
                break;
            case KEY_ENTER:
                track_selected_node(d);
                draw_screen(self);
                break;
            case KEY_ESC:
            case KEY_Q:
            case KEY_BACKSPACE:
                d->view = VIEW_PANS;
                d->ui_initialized = false; // retitle + full clear
                draw_screen(self);
                break;
            default:
                break;
        }
    }
}

static void on_tick(screen_t *self)
{
    mesh_data_t *d = (mesh_data_t *)self->user_data;
    if (!d) return;

    if (d->start_pending && !d->blocked && esp_timer_get_time() >= d->start_due_us) {
        d->start_pending = false;
        d->idle_status_count = 0;
        iot_recon_reset_all(&d->recon);
        uart_flush_rx();
        uart_send_command("start_zig_recon all 250");
        if (d->timer) {
            esp_timer_stop(d->timer);
            esp_timer_start_periodic(d->timer, POLL_INTERVAL_US);
        }
        d->needs_redraw = true;
    }

    if (!d->needs_redraw) return;
    d->needs_redraw = false;

    if (d->view == VIEW_PANS) {
        // Calm browsing: refresh only the header live; repaint the list only
        // when the visible PAN set changes (or on user nav, via on_key).
        if (d->ui_initialized && visible_pan_count(d) == d->drawn_pan_count) {
            draw_pan_header(d);
        } else {
            draw_screen(self);
        }
    } else {
        // Node list stays calm; only the locate panel refreshes live so the
        // tracked node's RSSI/trend updates while you walk. Repaint the list
        // only when the visible node set changes.
        if (d->ui_initialized && pan_node_count(d) == d->drawn_node_count) {
            draw_locate_panel(d);
        } else {
            draw_screen(self);
        }
    }
}

static void on_destroy(screen_t *self)
{
    mesh_data_t *d = (mesh_data_t *)self->user_data;
    uart_clear_line_callback();
    if (d) {
        if (d->timer) {
            esp_timer_stop(d->timer);
            esp_timer_delete(d->timer);
        }
        if (d->owns_radio) {
            uart_flush_rx();
            uart_send_command("stop");
            uart_send_command("zig_recon_clear");
        }
        free(d);
    }
}

screen_t* mesh_recon_screen_create(void *params)
{
    (void)params;
    ESP_LOGI(TAG, "Creating mesh recon screen...");

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    mesh_data_t *d = calloc(1, sizeof(mesh_data_t));
    if (!d) {
        free(screen);
        return NULL;
    }
    d->self = screen;
    d->view = VIEW_PANS;
    iot_recon_reset_all(&d->recon);
    d->blocked = uart_is_wardrive_active() || uart_is_antisurv_active();
    d->owns_radio = !d->blocked;
    d->start_pending = !d->blocked;
    d->start_due_us = esp_timer_get_time() + 300000;

    screen->user_data = d;
    screen->on_draw = draw_screen;
    screen->on_key = on_key;
    screen->on_tick = on_tick;
    screen->on_destroy = on_destroy;

    uart_register_line_callback(uart_line_callback, d);
    uart_flush_rx();
    if (!d->blocked) {
        // Release a stale board-side attack/sniffer mode before taking 802.15.4.
        uart_send_command("stop");
    }

    esp_timer_create_args_t targs = {
        .callback = poll_timer_cb,
        .arg = d,
        .name = "mesh_poll",
    };
    esp_timer_create(&targs, &d->timer);

    draw_screen(screen);
    ESP_LOGI(TAG, "Mesh recon screen created");
    return screen;
}
