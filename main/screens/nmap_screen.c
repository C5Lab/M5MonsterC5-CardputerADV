/**
 * @file nmap_screen.c
 * @brief Nmap port scanner screen implementation
 *
 * Flow: Check WiFi -> Target method (All/Select) -> [Host scan -> Pick host]
 *       -> Scan type (quick/medium/heavy) -> Scanning ports -> Results
 */

#include "nmap_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "NMAP";

#define MAX_HOSTS       32
#define MAX_PORTS       64
#define VISIBLE_ITEMS   5

/* ---- Data structures ---- */

typedef enum {
    STATE_CHECK_WIFI,
    STATE_TARGET_METHOD,
    STATE_SCANNING_HOSTS,
    STATE_SELECT_HOST,
    STATE_SELECT_SCAN_TYPE,
    STATE_SCANNING_PORTS,
    STATE_RESULTS
} nmap_state_t;

typedef struct {
    int port;
    char service[16];
} open_port_t;

typedef struct {
    char ip[16];
    char mac[18];
    open_port_t ports[MAX_PORTS];
    int port_count;
    bool no_open_ports;
} nmap_host_t;

typedef struct {
    char ip[16];
    char mac[18];
} discovered_host_t;

typedef struct {
    nmap_state_t state;
    screen_t *self;
    bool needs_redraw;

    /* Target method: 0=All hosts, 1=Scan & select */
    int method_index;

    /* Host discovery */
    discovered_host_t hosts[MAX_HOSTS];
    int host_count;
    bool host_scan_started;
    bool host_scan_done;
    int selected_host;
    int host_scroll;
    int animation_frame;
    esp_timer_handle_t update_timer;

    /* Selected target */
    char target_ip[16];   /* empty = all hosts */

    /* Scan type: 0=quick, 1=medium, 2=heavy */
    int scan_type_index;

    /* Nmap scan progress */
    char current_scan_ip[16];
    int current_progress;     /* 0-100 for current host */
    int total_hosts_scanned;
    int total_ports_found;

    /* Results */
    nmap_host_t results[MAX_HOSTS];
    int result_count;
    int current_result_host;  /* index into results[] being populated */
    bool scan_complete;
    int result_scroll;
    int result_selected;
    int result_total_lines;   /* total lines in result view */
} nmap_data_t;

/* Forward declarations */
static void draw_screen(screen_t *self);
static void stop_timer(nmap_data_t *data);

/* ---- Scan type helpers ---- */

static const char* scan_type_str(int idx)
{
    switch (idx) {
        case 0: return "quick";
        case 1: return "medium";
        case 2: return "heavy";
        default: return "quick";
    }
}

static const char* scan_type_label(int idx)
{
    switch (idx) {
        case 0: return "Quick  FTP,SSH,HTTP,SMB,RDP";
        case 1: return "Medium LDAP,MQTT,Docker,Redis";
        case 2: return "Heavy  TFTP,BGP,Modbus,MongoDB";
        default: return "";
    }
}

/* ---- UART callback for host discovery (list_hosts) ---- */

static void host_scan_line_cb(const char *line, void *user_data)
{
    nmap_data_t *data = (nmap_data_t *)user_data;
    if (!data || data->state != STATE_SCANNING_HOSTS) return;

    /* Start marker */
    if (strstr(line, "=== Discovered Hosts ===") != NULL) {
        data->host_scan_started = true;
        data->host_count = 0;
        return;
    }

    /* End marker: "Found N hosts" */
    if (strstr(line, "Found") != NULL && strstr(line, "hosts") != NULL) {
        data->host_scan_done = true;
        data->needs_redraw = true;
        return;
    }

    /* Parse host line: "  IP  ->  MAC" */
    if (data->host_scan_started && data->host_count < MAX_HOSTS) {
        const char *arrow = strstr(line, "->");
        if (!arrow) return;

        const char *ip_start = line;
        while (*ip_start == ' ') ip_start++;
        const char *ip_end = ip_start;
        while (*ip_end && *ip_end != ' ') ip_end++;
        size_t ip_len = ip_end - ip_start;
        if (ip_len == 0 || ip_len >= sizeof(data->hosts[0].ip)) return;

        discovered_host_t *h = &data->hosts[data->host_count];
        memcpy(h->ip, ip_start, ip_len);
        h->ip[ip_len] = '\0';

        const char *mac_start = arrow + 2;
        while (*mac_start == ' ') mac_start++;
        const char *mac_end = mac_start;
        while (*mac_end && *mac_end != ' ' && *mac_end != '[') mac_end++;
        size_t mac_len = mac_end - mac_start;
        if (mac_len >= sizeof(h->mac)) mac_len = sizeof(h->mac) - 1;
        memcpy(h->mac, mac_start, mac_len);
        h->mac[mac_len] = '\0';

        data->host_count++;
    }
}

/* ---- UART callback for nmap scan ---- */

static void nmap_scan_line_cb(const char *line, void *user_data)
{
    nmap_data_t *data = (nmap_data_t *)user_data;
    if (!data || data->state != STATE_SCANNING_PORTS) return;

    /* Skip log/debug lines */
    if (strstr(line, "[MEM]") != NULL) return;

    /* Trim leading spaces */
    const char *trimmed = line;
    while (*trimmed == ' ') trimmed++;

    /* New host: "Host: 192.168.0.4  (00:C0:CA:B4:E6:3F)" or "Host: 192.168.0.5  (MAC unknown)" */
    if (strncmp(trimmed, "Host:", 5) == 0) {
        if (data->result_count < MAX_HOSTS) {
            nmap_host_t *h = &data->results[data->result_count];
            memset(h, 0, sizeof(nmap_host_t));

            char ip[16] = {0};
            char mac[18] = {0};
            if (sscanf(trimmed, "Host: %15s (%17[^)])", ip, mac) >= 1) {
                strncpy(h->ip, ip, sizeof(h->ip) - 1);
                if (strstr(trimmed, "MAC unknown") != NULL) {
                    strcpy(h->mac, "unknown");
                } else {
                    strncpy(h->mac, mac, sizeof(h->mac) - 1);
                }
                data->current_result_host = data->result_count;
                data->result_count++;
            }
        }
        data->needs_redraw = true;
        return;
    }

    /* Progress: "  Scanning 192.168.0.4 ports 21-143 [1/100] ..." */
    if (strstr(line, "Scanning") != NULL && strstr(line, "ports") != NULL && strchr(line, '[')) {
        char ip[16] = {0};
        int port_from = 0, port_to = 0, current = 0, total = 0;
        if (sscanf(trimmed, "Scanning %15s ports %d-%d [%d/%d]",
                   ip, &port_from, &port_to, &current, &total) == 5 && total > 0) {
            strncpy(data->current_scan_ip, ip, sizeof(data->current_scan_ip) - 1);
            data->current_progress = (current * 100) / total;
        }
        data->needs_redraw = true;
        return;
    }

    /* Open port: "    80/tcp  open  HTTP" */
    {
        int port = 0;
        char service[16] = {0};
        if (sscanf(trimmed, "%d/tcp open %15s", &port, service) == 2 ||
            sscanf(trimmed, "%d/tcp  open  %15s", &port, service) == 2) {
            if (data->result_count > 0) {
                nmap_host_t *h = &data->results[data->current_result_host];
                if (h->port_count < MAX_PORTS) {
                    h->ports[h->port_count].port = port;
                    strncpy(h->ports[h->port_count].service, service, sizeof(h->ports[0].service) - 1);
                    h->port_count++;
                }
            }
            data->needs_redraw = true;
            return;
        }
    }

    /* No open ports marker */
    if (strstr(trimmed, "(no open ports)") != NULL) {
        if (data->result_count > 0) {
            data->results[data->current_result_host].no_open_ports = true;
        }
        data->needs_redraw = true;
        return;
    }

    /* Completion: "Scanned N hosts, found M open ports" */
    if (strstr(line, "Scanned") != NULL && strstr(line, "open ports") != NULL) {
        sscanf(trimmed, "Scanned %d hosts, found %d open ports",
               &data->total_hosts_scanned, &data->total_ports_found);
        data->scan_complete = true;
        data->needs_redraw = true;
        return;
    }

    /* Host discovery phases (informational - update display) */
    if (strstr(line, "ARP scan") != NULL || strstr(line, "ICMP ping") != NULL ||
        strstr(line, "hosts discovered") != NULL) {
        data->needs_redraw = true;
    }
}

/* ---- Timer callback for animations ---- */

static void timer_callback(void *arg)
{
    nmap_data_t *data = (nmap_data_t *)arg;

    if (data->state == STATE_SCANNING_HOSTS) {
        if (data->host_scan_done) {
            stop_timer(data);
            if (data->host_count > 0) {
                data->state = STATE_SELECT_HOST;
                data->selected_host = 0;
                data->host_scroll = 0;
            } else {
                /* No hosts found - go back to method select */
                data->state = STATE_TARGET_METHOD;
            }
            data->needs_redraw = true;
            return;
        }
        data->animation_frame = (data->animation_frame + 1) % 4;
        const char *spinner[] = {"|", "/", "-", "\\"};
        int y3 = 3 * 16;
        display_fill_rect(0, y3, DISPLAY_WIDTH, 16, UI_COLOR_BG);
        char status[32];
        snprintf(status, sizeof(status), "Scanning hosts... %s", spinner[data->animation_frame]);
        ui_print_center(3, status, UI_COLOR_TEXT);
        return;
    }

    if (data->state == STATE_SCANNING_PORTS) {
        if (data->scan_complete) {
            stop_timer(data);
            uart_clear_line_callback();
            data->state = STATE_RESULTS;
            data->result_scroll = 0;
            data->result_selected = 0;
            data->needs_redraw = true;
            return;
        }
        /* Refresh screen periodically during scan */
        data->needs_redraw = true;
    }
}

static void stop_timer(nmap_data_t *data)
{
    if (data->update_timer) {
        esp_timer_stop(data->update_timer);
        esp_timer_delete(data->update_timer);
        data->update_timer = NULL;
    }
}

static void start_timer(nmap_data_t *data)
{
    stop_timer(data);
    esp_timer_create_args_t timer_args = {
        .callback = timer_callback,
        .arg = data,
        .name = "nmap_timer"
    };
    esp_timer_create(&timer_args, &data->update_timer);
    esp_timer_start_periodic(data->update_timer, 300000); /* 300ms */
}

/* ---- Start host scan ---- */

static void start_host_scan(nmap_data_t *data)
{
    data->state = STATE_SCANNING_HOSTS;
    data->host_count = 0;
    data->host_scan_started = false;
    data->host_scan_done = false;
    data->animation_frame = 0;

    draw_screen(data->self);

    uart_register_line_callback(host_scan_line_cb, data);
    uart_send_command("list_hosts");
    start_timer(data);
}

/* ---- Start nmap scan ---- */

static void start_nmap_scan(nmap_data_t *data)
{
    data->state = STATE_SCANNING_PORTS;
    data->result_count = 0;
    data->current_result_host = -1;
    data->current_progress = 0;
    data->current_scan_ip[0] = '\0';
    data->total_hosts_scanned = 0;
    data->total_ports_found = 0;
    data->scan_complete = false;

    draw_screen(data->self);

    uart_register_line_callback(nmap_scan_line_cb, data);

    char cmd[64];
    if (data->target_ip[0] != '\0') {
        snprintf(cmd, sizeof(cmd), "start_nmap %s %s", data->target_ip, scan_type_str(data->scan_type_index));
    } else {
        snprintf(cmd, sizeof(cmd), "start_nmap %s", scan_type_str(data->scan_type_index));
    }
    uart_send_command(cmd);
    start_timer(data);
}

/* ---- Drawing ---- */

static void draw_host_row(nmap_data_t *data, int host_idx)
{
    int row_on_screen = host_idx - data->host_scroll;
    if (row_on_screen < 0 || row_on_screen >= VISIBLE_ITEMS) return;

    char label[30];
    snprintf(label, sizeof(label), "%s", data->hosts[host_idx].ip);
    bool selected = (host_idx == data->selected_host);
    ui_draw_menu_item(1 + row_on_screen, label, selected, false, false);
}

static void redraw_host_list(nmap_data_t *data)
{
    ui_draw_title("Select Host");
    for (int i = 0; i < VISIBLE_ITEMS; i++) {
        int idx = data->host_scroll + i;
        if (idx < data->host_count) {
            draw_host_row(data, idx);
        } else {
            int y = (1 + i) * 16;
            display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
        }
    }
    /* Scroll indicators */
    if (data->host_scroll > 0) {
        ui_print(UI_COLS - 2, 1, "^", UI_COLOR_DIMMED);
    }
    if (data->host_scroll + VISIBLE_ITEMS < data->host_count) {
        ui_print(UI_COLS - 2, VISIBLE_ITEMS, "v", UI_COLOR_DIMMED);
    }
}

static int count_result_lines(nmap_data_t *data)
{
    int lines = 1; /* summary line */
    for (int i = 0; i < data->result_count; i++) {
        lines++; /* host header */
        if (data->results[i].port_count > 0) {
            lines += data->results[i].port_count;
        } else {
            lines++; /* "(no open ports)" */
        }
    }
    return lines;
}

static void draw_results(nmap_data_t *data)
{
    ui_draw_title("Nmap Results");

    /* Build flat list of result lines and draw visible window */
    int line_idx = 0;
    int draw_row = 1;
    int max_rows = UI_ROWS - 2; /* rows 1..6, leave 0 for title and 7 for status */

    /* Summary line */
    if (line_idx >= data->result_scroll && draw_row <= max_rows) {
        char summary[30];
        snprintf(summary, sizeof(summary), "%d hosts, %d open ports",
                 data->total_hosts_scanned, data->total_ports_found);
        ui_print(0, draw_row, summary, UI_COLOR_HIGHLIGHT);
        draw_row++;
    }
    line_idx++;

    for (int i = 0; i < data->result_count && draw_row <= max_rows; i++) {
        nmap_host_t *h = &data->results[i];

        /* Host header */
        if (line_idx >= data->result_scroll && draw_row <= max_rows) {
            char hdr[30];
            snprintf(hdr, sizeof(hdr), "%s", h->ip);
            ui_print(0, draw_row, hdr, UI_COLOR_TEXT);
            draw_row++;
        }
        line_idx++;

        if (h->port_count > 0) {
            for (int p = 0; p < h->port_count && draw_row <= max_rows; p++) {
                if (line_idx >= data->result_scroll && draw_row <= max_rows) {
                    char port_line[30];
                    snprintf(port_line, sizeof(port_line), "  %d/%s",
                             h->ports[p].port, h->ports[p].service);
                    ui_print(0, draw_row, port_line, UI_COLOR_HIGHLIGHT);
                    draw_row++;
                }
                line_idx++;
            }
        } else {
            if (line_idx >= data->result_scroll && draw_row <= max_rows) {
                ui_print(0, draw_row, "  (no open ports)", UI_COLOR_DIMMED);
                draw_row++;
            }
            line_idx++;
        }
    }

    /* Clear remaining rows */
    while (draw_row <= max_rows) {
        display_fill_rect(0, draw_row * 16, DISPLAY_WIDTH, 16, UI_COLOR_BG);
        draw_row++;
    }

    /* Scroll indicators */
    if (data->result_scroll > 0) {
        ui_print(UI_COLS - 2, 1, "^", UI_COLOR_DIMMED);
    }
    data->result_total_lines = count_result_lines(data);
    int max_visible = UI_ROWS - 2;
    if (data->result_scroll + max_visible < data->result_total_lines) {
        ui_print(UI_COLS - 2, max_rows, "v", UI_COLOR_DIMMED);
    }
}

static void draw_screen(screen_t *self)
{
    nmap_data_t *data = (nmap_data_t *)self->user_data;
    ui_clear();

    switch (data->state) {
        case STATE_CHECK_WIFI:
            ui_draw_title("Nmap Scanner");
            ui_print_center(2, "Not connected to WiFi", UI_COLOR_TEXT);
            ui_print_center(4, "Connect first via", UI_COLOR_DIMMED);
            ui_print_center(5, "Network Attacks menu", UI_COLOR_DIMMED);
            ui_draw_status("ESC:Back");
            break;

        case STATE_TARGET_METHOD:
            ui_draw_title("Nmap Scanner");
            ui_draw_menu_item(1, "All hosts", data->method_index == 0, false, false);
            ui_draw_menu_item(2, "Scan & select host", data->method_index == 1, false, false);
            ui_draw_status("UP/DOWN ENTER:Select ESC:Back");
            break;

        case STATE_SCANNING_HOSTS:
            ui_draw_title("Host Discovery");
            ui_print_center(3, "Scanning hosts...", UI_COLOR_TEXT);
            ui_draw_status("ESC:Cancel");
            break;

        case STATE_SELECT_HOST:
            ui_clear();
            redraw_host_list(data);
            ui_draw_status("ENTER:Select ESC:Back");
            break;

        case STATE_SELECT_SCAN_TYPE:
            ui_draw_title("Scan Type");
            for (int i = 0; i < 3; i++) {
                ui_draw_menu_item(i + 1, scan_type_label(i),
                                  data->scan_type_index == i, false, false);
            }
            ui_draw_status("UP/DOWN ENTER:Select ESC:Back");
            break;

        case STATE_SCANNING_PORTS: {
            ui_draw_title("Nmap Scanning");

            /* Show what we're scanning */
            if (data->current_scan_ip[0]) {
                char info[30];
                snprintf(info, sizeof(info), "Host: %s", data->current_scan_ip);
                ui_print(0, 2, info, UI_COLOR_TEXT);
            } else {
                ui_print_center(2, "Starting scan...", UI_COLOR_DIMMED);
            }

            /* Progress bar */
            char pct_text[16];
            snprintf(pct_text, sizeof(pct_text), "%d%%", data->current_progress);
            ui_draw_progress(3, data->current_progress, pct_text);

            /* Stats line */
            char stats[30];
            int found = 0;
            for (int i = 0; i < data->result_count; i++) {
                found += data->results[i].port_count;
            }
            snprintf(stats, sizeof(stats), "Hosts:%d Ports:%d", data->result_count, found);
            ui_print(0, 5, stats, UI_COLOR_DIMMED);

            ui_draw_status("ESC:Stop");
            break;
        }

        case STATE_RESULTS:
            draw_results(data);
            ui_draw_status("UP/DOWN:Scroll ESC:Back");
            break;
    }
}

/* ---- Key handler ---- */

static void on_key(screen_t *self, key_code_t key)
{
    nmap_data_t *data = (nmap_data_t *)self->user_data;

    switch (data->state) {
        case STATE_CHECK_WIFI:
            if (key == KEY_ESC || key == KEY_BACKSPACE) {
                screen_manager_pop();
            }
            break;

        case STATE_TARGET_METHOD:
            switch (key) {
                case KEY_UP:
                    if (data->method_index > 0) {
                        int old = data->method_index;
                        data->method_index--;
                        ui_draw_menu_item(old + 1, old == 0 ? "All hosts" : "Scan & select host",
                                          false, false, false);
                        ui_draw_menu_item(data->method_index + 1,
                                          data->method_index == 0 ? "All hosts" : "Scan & select host",
                                          true, false, false);
                    } else {
                        int old = data->method_index;
                        data->method_index = 1;
                        ui_draw_menu_item(old + 1, "All hosts", false, false, false);
                        ui_draw_menu_item(2, "Scan & select host", true, false, false);
                    }
                    break;
                case KEY_DOWN:
                    if (data->method_index < 1) {
                        int old = data->method_index;
                        data->method_index++;
                        ui_draw_menu_item(old + 1, "All hosts", false, false, false);
                        ui_draw_menu_item(data->method_index + 1, "Scan & select host",
                                          true, false, false);
                    } else {
                        int old = data->method_index;
                        data->method_index = 0;
                        ui_draw_menu_item(old + 1, "Scan & select host", false, false, false);
                        ui_draw_menu_item(1, "All hosts", true, false, false);
                    }
                    break;
                case KEY_ENTER:
                case KEY_SPACE:
                    if (data->method_index == 0) {
                        /* All hosts - go to scan type */
                        data->target_ip[0] = '\0';
                        data->state = STATE_SELECT_SCAN_TYPE;
                        data->scan_type_index = 0;
                        draw_screen(self);
                    } else {
                        /* Scan & select host */
                        start_host_scan(data);
                    }
                    break;
                case KEY_ESC:
                case KEY_BACKSPACE:
                    screen_manager_pop();
                    break;
                default:
                    break;
            }
            break;

        case STATE_SCANNING_HOSTS:
            if (key == KEY_ESC || key == KEY_BACKSPACE) {
                stop_timer(data);
                uart_clear_line_callback();
                data->state = STATE_TARGET_METHOD;
                draw_screen(self);
            }
            break;

        case STATE_SELECT_HOST:
            switch (key) {
                case KEY_UP:
                    if (data->selected_host > 0) {
                        int old = data->selected_host;
                        if (data->selected_host == data->host_scroll && data->host_scroll > 0) {
                            data->host_scroll -= VISIBLE_ITEMS;
                            if (data->host_scroll < 0) data->host_scroll = 0;
                            data->selected_host = data->host_scroll + VISIBLE_ITEMS - 1;
                            if (data->selected_host >= data->host_count)
                                data->selected_host = data->host_count - 1;
                            ui_clear();
                            redraw_host_list(data);
                            ui_draw_status("ENTER:Select ESC:Back");
                        } else {
                            data->selected_host--;
                            draw_host_row(data, old);
                            draw_host_row(data, data->selected_host);
                        }
                    } else {
                        data->selected_host = data->host_count - 1;
                        data->host_scroll = data->selected_host - VISIBLE_ITEMS + 1;
                        if (data->host_scroll < 0) data->host_scroll = 0;
                        ui_clear();
                        redraw_host_list(data);
                        ui_draw_status("ENTER:Select ESC:Back");
                    }
                    break;
                case KEY_DOWN:
                    if (data->selected_host < data->host_count - 1) {
                        int old = data->selected_host;
                        if (data->selected_host == data->host_scroll + VISIBLE_ITEMS - 1) {
                            data->host_scroll += VISIBLE_ITEMS;
                            data->selected_host = data->host_scroll;
                            ui_clear();
                            redraw_host_list(data);
                            ui_draw_status("ENTER:Select ESC:Back");
                        } else {
                            data->selected_host++;
                            draw_host_row(data, old);
                            draw_host_row(data, data->selected_host);
                        }
                    } else {
                        data->selected_host = 0;
                        data->host_scroll = 0;
                        ui_clear();
                        redraw_host_list(data);
                        ui_draw_status("ENTER:Select ESC:Back");
                    }
                    break;
                case KEY_ENTER:
                case KEY_SPACE:
                    if (data->selected_host >= 0 && data->selected_host < data->host_count) {
                        strncpy(data->target_ip, data->hosts[data->selected_host].ip,
                                sizeof(data->target_ip) - 1);
                        data->target_ip[sizeof(data->target_ip) - 1] = '\0';
                        data->state = STATE_SELECT_SCAN_TYPE;
                        data->scan_type_index = 0;
                        draw_screen(self);
                    }
                    break;
                case KEY_ESC:
                case KEY_BACKSPACE:
                    data->state = STATE_TARGET_METHOD;
                    draw_screen(self);
                    break;
                default:
                    break;
            }
            break;

        case STATE_SELECT_SCAN_TYPE:
            switch (key) {
                case KEY_UP:
                    if (data->scan_type_index > 0) {
                        int old = data->scan_type_index;
                        data->scan_type_index--;
                        ui_draw_menu_item(old + 1, scan_type_label(old), false, false, false);
                        ui_draw_menu_item(data->scan_type_index + 1,
                                          scan_type_label(data->scan_type_index), true, false, false);
                    } else {
                        int old = data->scan_type_index;
                        data->scan_type_index = 2;
                        ui_draw_menu_item(old + 1, scan_type_label(old), false, false, false);
                        ui_draw_menu_item(3, scan_type_label(2), true, false, false);
                    }
                    break;
                case KEY_DOWN:
                    if (data->scan_type_index < 2) {
                        int old = data->scan_type_index;
                        data->scan_type_index++;
                        ui_draw_menu_item(old + 1, scan_type_label(old), false, false, false);
                        ui_draw_menu_item(data->scan_type_index + 1,
                                          scan_type_label(data->scan_type_index), true, false, false);
                    } else {
                        int old = data->scan_type_index;
                        data->scan_type_index = 0;
                        ui_draw_menu_item(old + 1, scan_type_label(old), false, false, false);
                        ui_draw_menu_item(1, scan_type_label(0), true, false, false);
                    }
                    break;
                case KEY_ENTER:
                case KEY_SPACE:
                    start_nmap_scan(data);
                    break;
                case KEY_ESC:
                case KEY_BACKSPACE:
                    /* Go back to target method or host select */
                    if (data->target_ip[0] != '\0') {
                        data->state = STATE_SELECT_HOST;
                    } else {
                        data->state = STATE_TARGET_METHOD;
                    }
                    draw_screen(self);
                    break;
                default:
                    break;
            }
            break;

        case STATE_SCANNING_PORTS:
            if (key == KEY_ESC || key == KEY_BACKSPACE) {
                uart_send_command("stop");
                stop_timer(data);
                uart_clear_line_callback();
                screen_manager_pop();
            }
            break;

        case STATE_RESULTS: {
            int max_visible = UI_ROWS - 2;
            switch (key) {
                case KEY_UP:
                    if (data->result_scroll > 0) {
                        data->result_scroll--;
                        draw_screen(self);
                    }
                    break;
                case KEY_DOWN:
                    if (data->result_scroll + max_visible < data->result_total_lines) {
                        data->result_scroll++;
                        draw_screen(self);
                    }
                    break;
                case KEY_ESC:
                case KEY_BACKSPACE:
                case KEY_ENTER:
                    screen_manager_pop();
                    break;
                default:
                    break;
            }
            break;
        }
    }
}

/* ---- Tick ---- */

static void on_tick(screen_t *self)
{
    nmap_data_t *data = (nmap_data_t *)self->user_data;

    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

/* ---- Lifecycle ---- */

static void on_destroy(screen_t *self)
{
    nmap_data_t *data = (nmap_data_t *)self->user_data;
    if (data) {
        stop_timer(data);
        uart_clear_line_callback();
        free(data);
    }
}

static void on_resume(screen_t *self)
{
    draw_screen(self);
}

screen_t* nmap_screen_create(void *params)
{
    (void)params;

    ESP_LOGI(TAG, "Creating nmap screen...");

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    nmap_data_t *data = calloc(1, sizeof(nmap_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }

    data->self = screen;

    /* Check WiFi connection */
    if (!uart_is_wifi_connected()) {
        data->state = STATE_CHECK_WIFI;
    } else {
        data->state = STATE_TARGET_METHOD;
    }

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;

    draw_screen(screen);

    ESP_LOGI(TAG, "Nmap screen created");
    return screen;
}
