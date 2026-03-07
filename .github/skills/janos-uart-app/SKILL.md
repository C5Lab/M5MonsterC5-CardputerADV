---
name: janos-uart-app
description: Build LVGL touchscreen applications that communicate with JanOS ESP32C5 firmware over UART. Use when creating screens for Tab5, CoreS3, Cardputer, or any ESP-IDF device that sends UART commands like scan_networks, start_deauth, list_hosts, show_pass. Covers screen building, UART parsing, attack flows, and navigation patterns.
---

# JanOS UART Application Builder

Guide for building ESP-IDF + LVGL applications that control JanOS firmware on ESP32C5 via UART.
Reference implementation: `/Users/janulrich/kod/dev/tabgit/M5MonsterC5-Tab5/main/main.c`

For the complete command list, see [commands-reference.md](commands-reference.md).

## Architecture

```
┌────────────────────────────────┐        UART (115200 8N1)        ┌──────────────────────┐
│  UI Device (Tab5/CoreS3/etc)   │ ──────────────────────────────> │  JanOS on ESP32C5    │
│  - LVGL touchscreen            │ <────────────────────────────── │  - WiFi radio        │
│  - Sends text commands         │        Text responses           │  - BLE radio         │
│  - Parses text responses       │                                 │  - SD card           │
│  - FreeRTOS tasks for I/O      │                                 │  - GPS (optional)    │
└────────────────────────────────┘                                 └──────────────────────┘
```

JanOS exposes a text CLI over UART. The UI app sends commands like `scan_networks\r\n` and parses the multi-line text responses to update the screen.

## UART Setup

```c
#define UART_NUM          UART_NUM_1
#define UART_BAUD_RATE    115200
#define UART_BUF_SIZE     4096

static void uart_init(void) {
    const uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_NUM, UART_BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART_NUM, &uart_config);
    uart_set_pin(UART_NUM, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}
```

**Pin assignments** (configurable, stored in NVS):
| Connector | TX  | RX  | Usage |
|-----------|-----|-----|-------|
| Grove     | 53  | 54  | UART1 (primary) |
| M5Bus     | 37  | 38  | UART2 (Kraken secondary) |

For CoreS3: use the appropriate GPIO for your board. The Tab5 reference stores pin config in NVS and reads with `get_uart1_pins()`.

## Sending Commands

Always append `\r\n`. Log every command sent.

```c
static void uart_send_command(const char *cmd) {
    uart_write_bytes(UART_NUM, cmd, strlen(cmd));
    uart_write_bytes(UART_NUM, "\r\n", 2);
    ESP_LOGI(TAG, "Sent: %s", cmd);
}
```

## Reading & Parsing Responses

### Core Pattern: Line-by-Line Accumulation

All UART parsing follows the same pattern -- read bytes, accumulate into a line buffer character by character, parse on newline:

```c
char rx_buffer[UART_BUF_SIZE];
char line_buffer[512];
int line_pos = 0;

while (!done && !timed_out) {
    int len = uart_read_bytes(UART_NUM, rx_buffer, UART_BUF_SIZE - 1, pdMS_TO_TICKS(100));
    if (len > 0) {
        rx_buffer[len] = '\0';
        for (int i = 0; i < len; i++) {
            char c = rx_buffer[i];
            if (c == '\n' || c == '\r') {
                if (line_pos > 0) {
                    line_buffer[line_pos] = '\0';

                    // === PARSE THIS LINE ===
                    if (strstr(line_buffer, "COMPLETION_MARKER")) {
                        done = true;
                        break;
                    }
                    parse_data_line(line_buffer);

                    line_pos = 0;
                }
            } else if (line_pos < sizeof(line_buffer) - 1) {
                line_buffer[line_pos++] = c;
            }
        }
    }
}
```

### Completion Markers

Every command response has a known end marker. Wait for it before proceeding:

| Command | Completion Marker |
|---------|-------------------|
| `scan_networks` | `"Scan results printed"` |
| `wifi_connect` | `"SUCCESS"` or `"FAILED"` or `"Error"` |
| `list_hosts` | `"Discovered Hosts"` (header line, data follows) |
| `list_sd` | `"HTML files found"` (header, data follows), or timeout |
| `show_pass` | Timeout (no explicit end marker) |
| `list_probes` | Timeout (no explicit end marker) |
| `wpasec_upload` | `"Done:"` |

For commands without explicit end markers, use a timeout with empty-read detection (e.g., 3 consecutive empty reads of 500ms each).

### Key Parsing Recipes

**Network scan CSV** -- fields: index, SSID, (empty), BSSID, channel, security, RSSI, band:
```c
// Line: "1","AX3","","C4:2B:44:12:29:20","112","WPA2","-59","5GHz"
static bool parse_network_line(const char *line, wifi_network_t *net) {
    if (line[0] != '"') return false;
    char temp[256];
    strncpy(temp, line, sizeof(temp) - 1);
    char *fields[8] = {NULL};
    int field_idx = 0;
    char *p = temp;
    while (*p && field_idx < 8) {
        if (*p == '"') {
            p++;
            fields[field_idx] = p;
            while (*p && *p != '"') p++;
            if (*p == '"') { *p = '\0'; p++; }
            field_idx++;
            if (*p == ',') p++;
        } else { p++; }
    }
    if (field_idx < 8) return false;
    net->index = atoi(fields[0]);
    strncpy(net->ssid, fields[1], sizeof(net->ssid) - 1);
    strncpy(net->bssid, fields[3], sizeof(net->bssid) - 1);
    net->channel = atoi(fields[4]);
    strncpy(net->security, fields[5], sizeof(net->security) - 1);
    net->rssi = atoi(fields[6]);
    strncpy(net->band, fields[7], sizeof(net->band) - 1);
    return true;
}
```

**Host list** -- parse `"  IP  ->  MAC"` lines:
```c
if (strstr(line, "Our IP:") != NULL) {
    // Extract IP after "Our IP: " up to ","
} else if (strstr(line, "->") != NULL) {
    char *p = line;
    while (*p == ' ') p++;
    // read IP until space, skip " -> ", read MAC
}
```

**Evil twin passwords** -- `"SSID", "password"`:
```c
if (line[0] == '"') {
    char *ssid_start = line + 1;
    char *ssid_end = strchr(ssid_start, '"');
    if (ssid_end && *(ssid_end+1) == ',' && *(ssid_end+3) == '"') {
        *ssid_end = '\0';
        char *pass_start = ssid_end + 4;
        char *pass_end = strchr(pass_start, '"');
        if (pass_end) { *pass_end = '\0'; /* ssid_start and pass_start are your values */ }
    }
}
```

**HTML file list** -- `"N filename.html"`:
```c
if (strstr(line_buffer, "HTML files found") != NULL) {
    header_found = true;
} else if (header_found && line_pos > 2) {
    int file_num;
    char filename[64];
    if (sscanf(line_buffer, "%d %63s", &file_num, filename) == 2) {
        // store filename at index file_num
    }
}
```

**Probe list** -- `"N SSID"`:
```c
char *p = line;
while (*p == ' ') p++;
if (isdigit((unsigned char)*p)) {
    int idx = 0;
    while (isdigit((unsigned char)*p)) { idx = idx * 10 + (*p - '0'); p++; }
    while (*p == ' ') p++;
    if (*p != '\0') {
        char ssid[33];
        strncpy(ssid, p, sizeof(ssid) - 1);
        // trim trailing whitespace
    }
}
```

**Deauth detector** -- `"[DEAUTH] CH: N | AP: name (BSSID) | RSSI: N"`:
```c
static bool parse_deauth_line(const char *line, deauth_entry_t *entry) {
    if (!strstr(line, "[DEAUTH]")) return false;
    const char *ch = strstr(line, "CH:");
    entry->channel = atoi(ch + 3);
    const char *ap = strstr(line, "AP:"); ap += 3; while (*ap == ' ') ap++;
    const char *paren = strchr(ap, '(');
    size_t ap_len = paren - ap;
    while (ap_len > 0 && ap[ap_len-1] == ' ') ap_len--;
    memcpy(entry->ap_name, ap, ap_len);
    const char *paren_end = strchr(paren + 1, ')');
    memcpy(entry->bssid, paren + 1, paren_end - paren - 1);
    entry->rssi = atoi(strstr(line, "RSSI:") + 5);
    return true;
}
```

**Handshake success**:
```c
if (strstr(line, "HANDSHAKE IS COMPLETE AND VALID")) { /* handshake validated */ }
if (strstr(line, "PCAP saved:")) { /* extract filename from path */ }
if (strstr(line, "handshake saved for SSID:")) { /* extract SSID after "SSID: " */ }
```

**Evil twin password capture**:
```c
if (strstr(line, "connected to SSID=")) { /* extract SSID between ' quotes */ }
if (strstr(line, "password=")) { /* extract password after = */ }
if (strstr(line, "Password verified!")) { /* attack succeeded */ }
```

**wifi_connect result**:
```c
if (strstr(rx_buffer, "SUCCESS")) { connected = true; }
if (strstr(rx_buffer, "FAILED") || strstr(rx_buffer, "Error")) { failed = true; }
```

## Screen Building

### Screen Lifecycle

```
show_*_page()
  ├── Create LVGL objects (container, labels, buttons, table)
  ├── Register event callbacks
  ├── Start background UART monitor task (if needed)
  └── Set as current visible page

[User interacts / UART data arrives]

cleanup (on back/close):
  ├── Set monitoring_active = false
  ├── Send "stop" via UART
  ├── Wait for task to finish (furi_thread_join or vTaskDelete)
  ├── Delete LVGL objects (lv_obj_del)
  ├── NULL all pointers
  └── Free allocated memory
```

### Tile (Button) Creation

```c
static lv_obj_t *create_tile(lv_obj_t *parent, const char *icon_text,
                              const char *label_text, lv_color_t bg_color,
                              lv_event_cb_t cb, const char *user_data) {
    lv_obj_t *tile = lv_btn_create(parent);
    lv_obj_set_size(tile, TILE_WIDTH, TILE_HEIGHT);
    lv_obj_set_style_bg_color(tile, bg_color, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(tile, lv_color_lighten(bg_color, 50), LV_STATE_PRESSED);
    lv_obj_set_style_radius(tile, 12, 0);
    lv_obj_set_style_shadow_width(tile, 0, 0);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *icon = lv_label_create(tile);
    lv_label_set_text(icon, icon_text);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(icon, lv_color_white(), 0);

    lv_obj_t *label = lv_label_create(tile);
    lv_label_set_text(label, label_text);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);

    if (cb) lv_obj_add_event_cb(tile, cb, LV_EVENT_CLICKED, (void *)user_data);
    return tile;
}
```

Scale `TILE_WIDTH` and `TILE_HEIGHT` based on display resolution. Tab5 uses 230x140 for a 1280x720 display. For CoreS3 (320x240), use ~100x80 or similar.

### Navigation via Event Callbacks

```c
static void main_tile_event_cb(lv_event_t *e) {
    const char *name = (const char *)lv_event_get_user_data(e);
    if (strcmp(name, "WiFi Scan & Attack") == 0)  show_scan_page();
    else if (strcmp(name, "Global WiFi Attacks") == 0) show_global_attacks_page();
    else if (strcmp(name, "Bluetooth") == 0) show_bluetooth_menu_page();
    // ...
}
```

### Popup Pattern

```c
// Semi-transparent overlay covering the whole screen
overlay = lv_obj_create(lv_scr_act());
lv_obj_remove_style_all(overlay);
lv_obj_set_size(overlay, lv_pct(100), lv_pct(100));
lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);
lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);  // block clicks to background

// Centered popup
popup = lv_obj_create(overlay);
lv_obj_set_size(popup, POPUP_W, POPUP_H);
lv_obj_center(popup);
// ... add content to popup ...
```

Close popup: `lv_obj_del(overlay)` deletes both overlay and popup (child).

### Thread-Safe LVGL Updates

LVGL is NOT thread-safe. Any LVGL call from a background UART task must be wrapped:

```c
if (bsp_display_lock(0)) {
    lv_label_set_text(status_label, "Connected!");
    bsp_display_unlock();
}
```

If `bsp_display_lock` is not available, use a mutex or LVGL's built-in `lv_lock()`/`lv_unlock()` (LVGL 9.x).

### Memory Management

- Use **PSRAM** (`heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`) for large data structures (network lists, probe arrays, host tables).
- Use **internal RAM** for small UI state structs and LVGL objects (LVGL manages its own allocator).
- Always free PSRAM allocations in cleanup functions.
- Keep network scan results in a global structure so they survive screen transitions.

## Common Workflows

### 1. Scan-Select-Attack

```
uart_send("scan_networks")
  → wait for "Scan results printed"
  → parse CSV lines into network array
  → show network list with checkboxes
  → user selects networks and taps attack button

uart_send("select_networks 1 3 5")    // 1-based indices
uart_send("start_deauth")             // or start_evil_twin, start_handshake, etc.
  → start monitor task to parse UART output
  → update screen with attack status

// On back/stop:
uart_send("stop")
```

### 2. Evil Twin Flow

```
scan_networks → select_networks → list_sd → user picks HTML
→ select_html <index> → start_evil_twin
→ monitor for: "Client connected", "Password received", "Password verified!"
→ stop
```

### 3. Connect-ListHosts-ARPBan

```
// Check if password known via show_pass evil, or ask user
wifi_connect <SSID> <password>
  → wait for "SUCCESS" / "FAILED"

list_hosts
  → wait for "Discovered Hosts", parse IP->MAC lines

// User taps a host:
arp_ban <MAC> [IP]
  → show "ARP Poisoning Active"

// On back:
stop
```

### 4. Portal/Karma Setup

```
list_sd → user picks HTML → select_html <index>
start_portal <SSID>         // or start_karma <probe_index>
  → monitor for client connections and form submissions
  → stop
```

### 5. Wardrive

```
start_wardrive   // or start_wardrive_promisc
  → wait for "GPS fix obtained" (show "Acquiring GPS Fix..." until then)
  → parse CSV network lines as they arrive
  → show "Logged N networks to ..."
  → handle "GPS fix lost!" / "GPS fix recovered:"
  → stop
```

### 6. Bluetooth Locate

```
scan_bt → parse device list → show scrollable list
→ user taps device → scan_bt <MAC> → continuous RSSI updates
→ stop
```

## Adapting for Smaller Screens (CoreS3)

When porting from Tab5 (1280x720) to CoreS3 (320x240):

- Reduce tile grid: 2x3 instead of 3x4. Tile size ~100x80.
- Use smaller fonts: `lv_font_montserrat_14` for body, `lv_font_montserrat_18` for titles (instead of 20/28).
- Popups should be nearly full-screen (280x200) since the display is small.
- Scrollable lists are essential -- use `lv_table` or `lv_list` with small row heights.
- Consider a single UART (no Kraken dual-UART mode) to keep complexity low.
- The home button at top should be compact (icon only, no text).
- Tab bar (UART1/UART2/INTERNAL) may not fit; use a dropdown or eliminate tabs for single-UART configs.

## Checklist for New Screens

1. Create `show_<feature>_page()` function
2. Build UI: container, title, content area, back button
3. Send UART command(s)
4. Start background monitor task if response is continuous
5. Parse UART response using line-by-line accumulation pattern
6. Update UI inside `bsp_display_lock()`/`unlock()` from background tasks
7. Implement cleanup: set `monitoring = false`, send `"stop"`, delete LVGL objects, free memory
8. Wire up the back/home button to call cleanup and return to parent screen
