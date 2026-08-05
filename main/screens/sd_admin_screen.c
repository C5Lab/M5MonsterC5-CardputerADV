/**
 * @file sd_admin_screen.c
 * @brief SD Card Admin / File Manager - drives the JanOS admin portal over UART
 *
 * Mirrors the Tab5 "Monster SD Admin" page. The connected JanOS board exposes
 * its SD card through a local WPA2 AP:
 *   start_admin_portal <password>  -> AP "JanOS-Admin", panel on 172.0.0.1
 *   stop                           -> shuts the portal down
 *
 * The WPA2 password is kept in NVS so it survives a reboot, exactly like the
 * Tab5 page does. UART replies arrive on the uart_rx task, so the line callback
 * only updates shared state under a critical section; every draw call is made
 * later from on_tick/on_key, which run on the application loop.
 */

#include "sd_admin_screen.h"
#include "text_input_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "nvs.h"
#include "qrcode.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>

// Provided by main.c - true when the board reported a missing SD card at boot
extern bool is_board_sd_missing(void);

static const char *TAG = "SD_ADMIN";

#define SCREEN_TITLE            "Monster SD Admin"
#define ADMIN_SSID              "JanOS-Admin"
#define ADMIN_URL               "http://172.0.0.1"
#define QUICK_START_PASSWORD    "12345678"
#define PASSWORD_MIN_LEN        8
#define PASSWORD_MAX_LEN        63

// Same NVS layout as the Tab5 build
#define SD_ADMIN_NVS_NAMESPACE      "sd_admin"
#define SD_ADMIN_NVS_PASSWORD_KEY   "wpa2_pass"

// Controlled timeouts: the UI never waits on the board forever
#define START_TIMEOUT_MS    12000
#define STOP_TIMEOUT_MS     6000

// One status line is one 30-column row
#define STATUS_MAX_LEN      30

// Content rows (row 0 is the title bar, row 7 is the status bar)
#define ROW_STATUS          1
#define ROW_INFO            2
#define ROW_ITEM_FIRST      3

typedef enum {
    ADMIN_STOPPED = 0,
    ADMIN_STARTING,
    ADMIN_RUNNING,
    ADMIN_STOPPING,
} admin_state_t;

typedef enum {
    ITEM_PASSWORD = 0,
    ITEM_START,
    ITEM_QUICK_START,
    ITEM_STOP,
    ITEM_COUNT,
} menu_item_t;

// Shared between the UART task and the UI loop
typedef struct {
    admin_state_t state;
    char          status[STATUS_MAX_LEN + 1];
    bool          changed;
    int64_t       deadline_us;
} admin_shared_t;

// Kept across screen instances so "leave the portal running" survives a back-out
static admin_shared_t s_admin = {
    .state  = ADMIN_STOPPED,
    .status = "Enter a password or Quick.",
};
static portMUX_TYPE s_admin_lock = portMUX_INITIALIZER_UNLOCKED;

// Password the running portal was started with, shown so the AP can be joined
static char s_active_password[PASSWORD_MAX_LEN + 1];

typedef struct {
    int  selected;
    bool confirm_leave;                     // back pressed while the portal is up
    int  confirm_selected;                  // 0 = leave running, 1 = stop
    bool show_qr;                           // full-screen Wi-Fi join code
    char password[PASSWORD_MAX_LEN + 1];
} sd_admin_data_t;

static void draw_screen(screen_t *self);

/* ---- Stored password ---- */

static bool load_saved_password(char *out, size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';

    nvs_handle_t nvs;
    if (nvs_open(SD_ADMIN_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;

    size_t stored_size = out_size;
    esp_err_t err = nvs_get_str(nvs, SD_ADMIN_NVS_PASSWORD_KEY, out, &stored_size);
    nvs_close(nvs);

    if (err != ESP_OK) {
        out[0] = '\0';
        return false;
    }
    return true;
}

static bool save_password(const char *password)
{
    if (!password || !password[0]) return false;

    nvs_handle_t nvs;
    if (nvs_open(SD_ADMIN_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return false;

    esp_err_t err = nvs_set_str(nvs, SD_ADMIN_NVS_PASSWORD_KEY, password);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);

    return err == ESP_OK;
}

/* ---- Shared state ---- */

static void shared_set(admin_state_t state, const char *status, int64_t deadline_us)
{
    // Format outside the critical section, copy inside it
    char buf[STATUS_MAX_LEN + 1];
    snprintf(buf, sizeof(buf), "%s", status ? status : "");

    portENTER_CRITICAL(&s_admin_lock);
    s_admin.state = state;
    memcpy(s_admin.status, buf, sizeof(buf));
    s_admin.deadline_us = deadline_us;
    s_admin.changed = true;
    portEXIT_CRITICAL(&s_admin_lock);
}

// Feedback text only: keeps the state machine and its deadline untouched
static void shared_set_status(const char *status)
{
    char buf[STATUS_MAX_LEN + 1];
    snprintf(buf, sizeof(buf), "%s", status ? status : "");

    portENTER_CRITICAL(&s_admin_lock);
    memcpy(s_admin.status, buf, sizeof(buf));
    s_admin.changed = true;
    portEXIT_CRITICAL(&s_admin_lock);
}

static admin_shared_t shared_get(void)
{
    admin_shared_t copy;
    portENTER_CRITICAL(&s_admin_lock);
    copy = s_admin;
    portEXIT_CRITICAL(&s_admin_lock);
    return copy;
}

// Reading the state must not swallow a pending redraw, so clearing is explicit
static bool shared_take_changed(void)
{
    bool changed;
    portENTER_CRITICAL(&s_admin_lock);
    changed = s_admin.changed;
    s_admin.changed = false;
    portEXIT_CRITICAL(&s_admin_lock);
    return changed;
}

/* ---- UART ---- */

/**
 * @brief Case-insensitive substring search (strcasestr is not available here)
 */
static bool line_has(const char *line, const char *needle)
{
    size_t n = strlen(needle);
    for (const char *p = line; *p; p++) {
        if (strncasecmp(p, needle, n) == 0) return true;
    }
    return false;
}

/**
 * @brief Classify one JanOS response line. Runs on the uart_rx task.
 */
static void uart_line_callback(const char *line, void *user_data)
{
    (void)user_data;
    if (!line || !line[0]) return;

    admin_state_t state = shared_get().state;

    // "Admin portal started. Connect to 'JanOS-Admin' (WPA2) and open http://172.0.0.1"
    if (line_has(line, "Admin portal started")) {
        shared_set(ADMIN_RUNNING, "Running - portal is up.", 0);
        return;
    }

    // "Portal already running. Use 'stop' to stop it first."
    if (line_has(line, "Portal already running")) {
        shared_set(ADMIN_RUNNING, "Already running on board.", 0);
        return;
    }

    if (state == ADMIN_STARTING) {
        // "Failed to initialize SD card. Make sure it is inserted."
        if (strcmp(line, "SD_NONE") == 0 ||
            (line_has(line, "SD card") && (line_has(line, "Failed") || line_has(line, "error")))) {
            shared_set(ADMIN_STOPPED, "SD card error - insert it.", 0);
            return;
        }

        // Rejected passphrase, e.g. "Password must be 8-63 characters"
        if (line_has(line, "password") &&
            (line_has(line, "8-63") || line_has(line, "length") ||
             line_has(line, "short") || line_has(line, "long"))) {
            shared_set(ADMIN_STOPPED, "Password must be 8-63 long.", 0);
            return;
        }

        // AP or HTTP server could not be brought up
        if (line_has(line, "Failed to start") || line_has(line, "Failed to configure") ||
            line_has(line, "Failed to create") || line_has(line, "Failed to init") ||
            line_has(line, "portal start failed")) {
            shared_set(ADMIN_STOPPED, "AP/HTTP portal failed.", 0);
            return;
        }
        return;
    }

    if (state == ADMIN_STOPPING && line_has(line, "stopped")) {
        // s_active_password is cleared by on_tick, on the UI loop
        shared_set(ADMIN_STOPPED, "Portal stopped.", 0);
    }
}

/* ---- Drawing ---- */

static bool item_enabled(menu_item_t item, admin_state_t state)
{
    switch (item) {
        case ITEM_PASSWORD:
            // Doubles as "Show Wi-Fi QR" once the portal answered
            return state == ADMIN_STOPPED ||
                   (state == ADMIN_RUNNING && s_active_password[0] != '\0');
        case ITEM_START:
        case ITEM_QUICK_START:
            return state == ADMIN_STOPPED;
        case ITEM_STOP:
            return state == ADMIN_RUNNING;
        default:
            return false;
    }
}

/**
 * @brief Menu row with a dimmed variant, which ui_draw_menu_item does not have
 */
static void draw_item(int row, const char *text, bool selected, bool enabled)
{
    if (enabled) {
        ui_draw_menu_item(row, text, selected, false, false);
        return;
    }

    int y = row * 16;
    display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
    if (selected) {
        display_draw_vline(0, y, 16, UI_COLOR_DIMMED);
        display_draw_vline(1, y, 16, UI_COLOR_DIMMED);
    }
    ui_draw_text(4, y, text, UI_COLOR_DIMMED, UI_COLOR_BG);
}

/**
 * @brief Password row: masked while stopped, readable while the AP is up so
 *        the passphrase can be typed into the phone that joins it.
 */
static void format_password_item(const sd_admin_data_t *data, admin_state_t state,
                                 char *out, size_t out_len)
{
    if ((state == ADMIN_RUNNING || state == ADMIN_STOPPING) && s_active_password[0]) {
        snprintf(out, out_len, "Show Wi-Fi QR");
        return;
    }

    size_t n = strnlen(data->password, sizeof(data->password));
    if (n == 0) {
        snprintf(out, out_len, "Password: (not set)");
    } else if (n <= 14) {
        char stars[16];
        memset(stars, '*', n);
        stars[n] = '\0';
        snprintf(out, out_len, "Password: %s", stars);
    } else {
        // Too wide for one row - report the length instead of the mask
        snprintf(out, out_len, "Password: %u chars", (unsigned)n);
    }
}

/* ---- Wi-Fi join code ---- */

/**
 * @brief Render the generated code centred on a white screen.
 *
 * Called synchronously from esp_qrcode_generate(). Horizontal runs of dark
 * modules are merged into one rectangle, which keeps the number of SPI
 * transfers (and therefore the repaint) down.
 */
#define QR_CAPTION_H 16     // bottom strip carrying SSID and passphrase

static void qr_display_cb(esp_qrcode_handle_t qr)
{
    int size = esp_qrcode_get_size(qr);
    if (size <= 0) return;

    // "+ 4" keeps at least two modules of quiet zone above and below the code
    int usable = DISPLAY_HEIGHT - QR_CAPTION_H;
    int scale = usable / (size + 4);
    if (scale < 1) scale = usable / size;
    if (scale < 1) return;                  // code larger than the panel

    int px = size * scale;
    int ox = (DISPLAY_WIDTH - px) / 2;
    int oy = (usable - px) / 2;

    display_clear(COLOR_WHITE);

    for (int y = 0; y < size; y++) {
        int run_start = -1;
        for (int x = 0; x <= size; x++) {
            bool dark = (x < size) && esp_qrcode_get_module(qr, x, y);
            if (dark && run_start < 0) {
                run_start = x;
            } else if (!dark && run_start >= 0) {
                display_fill_rect(ox + run_start * scale, oy + y * scale,
                                  (x - run_start) * scale, scale, COLOR_BLACK);
                run_start = -1;
            }
        }
    }

    // Same details in text, for joining the AP without a camera. Built by hand
    // because the row is one column wide: a long passphrase is meant to be cut
    // here, which snprintf() cannot express without tripping -Wformat-truncation.
    char caption[UI_COLS + 1];
    const char *prefix = ADMIN_SSID " / ";
    size_t n = 0;

    for (size_t i = 0; prefix[i] && n < sizeof(caption) - 1; i++) {
        caption[n++] = prefix[i];
    }
    for (size_t i = 0; s_active_password[i] && n < sizeof(caption) - 1; i++) {
        caption[n++] = s_active_password[i];
    }
    caption[n] = '\0';

    int len = (int)n;
    int cx = (DISPLAY_WIDTH - len * 8) / 2;
    if (cx < 0) cx = 0;
    ui_draw_text(cx, DISPLAY_HEIGHT - QR_CAPTION_H, caption, COLOR_BLACK, COLOR_WHITE);
    memset(caption, 0, sizeof(caption));
}

/**
 * @brief Build the WIFI: payload phones understand, escaping the reserved
 *        characters \ ; , : and " as the format requires.
 */
static void build_wifi_payload(char *out, size_t out_len, const char *password)
{
    char escaped[2 * PASSWORD_MAX_LEN + 1];
    size_t j = 0;

    for (size_t i = 0; password[i] && j < sizeof(escaped) - 2; i++) {
        char c = password[i];
        if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') {
            escaped[j++] = '\\';
        }
        escaped[j++] = c;
    }
    escaped[j] = '\0';

    snprintf(out, out_len, "WIFI:T:WPA;S:" ADMIN_SSID ";P:%s;;", escaped);
    memset(escaped, 0, sizeof(escaped));
}

static void draw_qr_screen(void)
{
    char payload[64 + 2 * PASSWORD_MAX_LEN];
    build_wifi_payload(payload, sizeof(payload), s_active_password);

    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func = qr_display_cb;

    if (esp_qrcode_generate(&cfg, payload) != ESP_OK) {
        ui_clear();
        ui_draw_title(SCREEN_TITLE);
        ui_print(0, ROW_STATUS, "QR could not be generated.", UI_COLOR_DIMMED);
        ui_print(0, ROW_INFO, "Join " ADMIN_SSID " by hand.", UI_COLOR_TEXT);
        ui_draw_status("Any key: back");
    }

    memset(payload, 0, sizeof(payload));
}

static void format_item_text(const sd_admin_data_t *data, admin_state_t state,
                             menu_item_t item, char *out, size_t out_len)
{
    switch (item) {
        case ITEM_PASSWORD:
            format_password_item(data, state, out, out_len);
            break;
        case ITEM_START:
            snprintf(out, out_len, "Start Admin Portal");
            break;
        case ITEM_QUICK_START:
            snprintf(out, out_len, "Quick Start " QUICK_START_PASSWORD);
            break;
        case ITEM_STOP:
            snprintf(out, out_len, "Stop Portal");
            break;
        default:
            out[0] = '\0';
            break;
    }
}

// Repaints exactly one menu row, leaving the title and status bars alone
static void draw_menu_row(const sd_admin_data_t *data, admin_state_t state, menu_item_t item)
{
    char text[UI_COLS + 1];
    format_item_text(data, state, item, text, sizeof(text));
    draw_item(ROW_ITEM_FIRST + item, text, data->selected == (int)item,
              item_enabled(item, state));
}

// Only the two option rows; the bars and the question stay untouched
static void redraw_confirm_rows(const sd_admin_data_t *data)
{
    ui_draw_menu_item(ROW_ITEM_FIRST + 1, "Leave portal running",
                      data->confirm_selected == 0, false, false);
    ui_draw_menu_item(ROW_ITEM_FIRST + 2, "Stop portal now",
                      data->confirm_selected == 1, false, false);
}

static void draw_confirmation(sd_admin_data_t *data)
{
    ui_clear();
    ui_draw_title(SCREEN_TITLE);
    ui_print(0, ROW_STATUS, "Portal is still running.", UI_COLOR_HIGHLIGHT);
    ui_print(0, ROW_INFO, "Leave it up or stop it?", UI_COLOR_TEXT);
    redraw_confirm_rows(data);
    ui_draw_status("ENTER:Confirm ESC:Cancel");
}

static void draw_screen(screen_t *self)
{
    sd_admin_data_t *data = (sd_admin_data_t *)self->user_data;

    if (data->confirm_leave) {
        draw_confirmation(data);
        return;
    }

    admin_shared_t snap = shared_get();

    // The join code only makes sense while the portal is actually up
    if (data->show_qr && snap.state == ADMIN_RUNNING && s_active_password[0]) {
        draw_qr_screen();
        return;
    }
    data->show_qr = false;

    bool up = (snap.state == ADMIN_RUNNING || snap.state == ADMIN_STOPPING);

    ui_clear();
    ui_draw_title(SCREEN_TITLE);

    // Current state / last message from the board
    uint16_t status_color = UI_COLOR_DIMMED;
    if (snap.state == ADMIN_RUNNING) status_color = UI_COLOR_HIGHLIGHT;
    else if (snap.state != ADMIN_STOPPED) status_color = UI_COLOR_TEXT;
    ui_print(0, ROW_STATUS, snap.status, status_color);

    if (up) {
        // How to reach the panel once the portal answered
        ui_print(0, ROW_INFO, ADMIN_SSID " > " ADMIN_URL, UI_COLOR_HIGHLIGHT);
    } else {
        // How far the portal's reach goes
        ui_print(0, ROW_INFO, "AP clients only; /sdcard/lab", UI_COLOR_DIMMED);
    }

    for (int i = 0; i < ITEM_COUNT; i++) {
        draw_menu_row(data, snap.state, (menu_item_t)i);
    }

    ui_draw_status("UP/DN ENTER:Select ESC:Back");
}

/* ---- Actions ---- */

static void password_submitted(const char *text, void *user_data)
{
    sd_admin_data_t *data = (sd_admin_data_t *)user_data;

    if (data && text) {
        size_t n = strnlen(text, sizeof(data->password) - 1);
        memcpy(data->password, text, n);
        data->password[n] = '\0';

        if (n < PASSWORD_MIN_LEN) {
            shared_set(ADMIN_STOPPED, "Password needs 8-63 chars.", 0);
        } else {
            shared_set(ADMIN_STOPPED, "Password set. Ready.", 0);
        }
    }

    screen_manager_pop();
}

static void request_password(sd_admin_data_t *data)
{
    text_input_params_t *params = calloc(1, sizeof(*params));
    if (!params) return;

    params->title = "WPA2 Password";
    params->hint = "8-63 chars, no spaces";
    params->on_submit = password_submitted;
    params->user_data = data;
    params->max_length = PASSWORD_MAX_LEN;
    params->masked = true;
    screen_manager_push(text_input_screen_create, params);
}

/**
 * @brief Send `start_admin_portal <password>` and move to the STARTING state
 * @param password Passphrase to use
 * @param persist  true to remember it in NVS (Start), false for Quick Start
 */
static void start_portal(screen_t *self, const char *password, bool persist)
{
    if (shared_get().state != ADMIN_STOPPED) return;

    size_t len = password ? strlen(password) : 0;
    if (len < PASSWORD_MIN_LEN || len > PASSWORD_MAX_LEN) {
        shared_set(ADMIN_STOPPED, "Password needs 8-63 chars.", 0);
        draw_screen(self);
        return;
    }
    // The command is space separated, so the passphrase cannot contain spaces
    if (strchr(password, ' ') || strchr(password, '\r') || strchr(password, '\n')) {
        shared_set(ADMIN_STOPPED, "No spaces in the password.", 0);
        draw_screen(self);
        return;
    }

    if (persist && !save_password(password)) {
        ESP_LOGW(TAG, "Could not persist the WPA2 password");
    }

    // uart_send_sensitive_command appends CRLF and keeps the passphrase out of
    // the UART log.
    char command[32 + PASSWORD_MAX_LEN + 1];
    snprintf(command, sizeof(command), "start_admin_portal %s", password);

    shared_set(ADMIN_STARTING, "Starting portal...",
               esp_timer_get_time() + (int64_t)START_TIMEOUT_MS * 1000);

    esp_err_t err = uart_send_sensitive_command(command);
    memset(command, 0, sizeof(command));

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send start command");
        shared_set(ADMIN_STOPPED, "UART send failed.", 0);
        memset(s_active_password, 0, sizeof(s_active_password));
    } else {
        // Remembered so the running view can show what to type on the phone
        snprintf(s_active_password, sizeof(s_active_password), "%s", password);
        ESP_LOGI(TAG, "Admin portal start requested");
    }

    draw_screen(self);
}

static void stop_portal(screen_t *self)
{
    shared_set(ADMIN_STOPPING, "Stopping portal...",
               esp_timer_get_time() + (int64_t)STOP_TIMEOUT_MS * 1000);

    if (uart_send_command("stop") != ESP_OK) {
        shared_set(ADMIN_STOPPED, "UART send failed.", 0);
    }

    draw_screen(self);
}

/* ---- Screen callbacks ---- */

static void on_key(screen_t *self, key_code_t key)
{
    sd_admin_data_t *data = (sd_admin_data_t *)self->user_data;
    admin_state_t state = shared_get().state;

    // Full-screen join code: any key returns to the menu
    if (data->show_qr) {
        if (key != KEY_NONE) {
            data->show_qr = false;
            draw_screen(self);
        }
        return;
    }

    // Back-out confirmation: never stop the portal without asking
    if (data->confirm_leave) {
        switch (key) {
            case KEY_UP:
            case KEY_DOWN:
                data->confirm_selected = 1 - data->confirm_selected;
                redraw_confirm_rows(data);
                break;

            case KEY_ENTER:
            case KEY_SPACE:
                if (data->confirm_selected == 1) {
                    // Stay on the screen so it can show STOPPING until the board
                    // confirms (or the controlled timeout expires)
                    data->confirm_leave = false;
                    data->selected = ITEM_STOP;
                    stop_portal(self);
                } else {
                    screen_manager_pop();
                }
                break;

            case KEY_ESC:
            case KEY_Q:
            case KEY_BACKSPACE:
                data->confirm_leave = false;
                draw_screen(self);
                break;

            default:
                break;
        }
        return;
    }

    switch (key) {
        // Moving the cursor repaints the two affected rows only, so the title
        // and status bars stay put instead of flashing
        case KEY_UP:
        case KEY_DOWN: {
            int old = data->selected;
            data->selected = (key == KEY_UP)
                ? (data->selected + ITEM_COUNT - 1) % ITEM_COUNT
                : (data->selected + 1) % ITEM_COUNT;
            draw_menu_row(data, state, (menu_item_t)old);
            draw_menu_row(data, state, (menu_item_t)data->selected);
            break;
        }

        case KEY_ENTER:
        case KEY_SPACE:
            // A disabled row says why instead of doing nothing
            if (!item_enabled((menu_item_t)data->selected, state)) {
                shared_set_status(data->selected == ITEM_STOP ? "Portal is not running."
                                                              : "Stop the portal first.");
                draw_screen(self);
                break;
            }
            if (data->selected == ITEM_PASSWORD) {
                if (state == ADMIN_RUNNING) {
                    data->show_qr = true;       // row reads "Show Wi-Fi QR"
                    draw_screen(self);
                } else {
                    request_password(data);
                }
            } else if (data->selected == ITEM_START) {
                // The Password row is this screen's text field, so an empty one
                // opens the editor rather than dead-ending on a warning
                if (data->password[0] == '\0') {
                    shared_set_status("Enter a password to start.");
                    request_password(data);
                } else {
                    start_portal(self, data->password, true);
                }
            } else if (data->selected == ITEM_QUICK_START) {
                start_portal(self, QUICK_START_PASSWORD, false);
            } else if (data->selected == ITEM_STOP) {
                stop_portal(self);
            }
            break;

        case KEY_ESC:
        case KEY_Q:
        case KEY_BACKSPACE:
            if (state == ADMIN_RUNNING || state == ADMIN_STARTING) {
                data->confirm_leave = true;
                data->confirm_selected = 0;
                draw_confirmation(data);
            } else {
                screen_manager_pop();
            }
            break;

        default:
            break;
    }
}

static void on_tick(screen_t *self)
{
    admin_shared_t snap = shared_get();

    if ((snap.state == ADMIN_STARTING || snap.state == ADMIN_STOPPING) &&
        snap.deadline_us && esp_timer_get_time() >= snap.deadline_us) {
        shared_set(ADMIN_STOPPED,
                   snap.state == ADMIN_STARTING ? "Start not confirmed."
                                                : "Stop not confirmed (reset).", 0);
    }

    // Only this loop touches s_active_password, so the UART task cannot tear it
    if (shared_get().state == ADMIN_STOPPED && s_active_password[0]) {
        memset(s_active_password, 0, sizeof(s_active_password));
    }

    if (shared_take_changed()) {
        draw_screen(self);
    }
}

static void on_resume(screen_t *self)
{
    draw_screen(self);
}

static void on_destroy(screen_t *self)
{
    sd_admin_data_t *data = (sd_admin_data_t *)self->user_data;

    uart_clear_line_callback();

    if (data) {
        memset(data, 0, sizeof(*data));   // wipe the working copy of the passphrase
        free(data);
    }
}

screen_t *sd_admin_screen_create(void *params)
{
    (void)params;

    ESP_LOGI(TAG, "Creating SD admin screen...");

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    sd_admin_data_t *data = calloc(1, sizeof(*data));
    if (!data) {
        free(screen);
        return NULL;
    }

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;
    screen->on_resume = on_resume;
    screen->on_destroy = on_destroy;

    // Prefill with the stored passphrase, like the Tab5 page does
    if (load_saved_password(data->password, sizeof(data->password))) {
        data->selected = ITEM_START;
    }

    // Warn early when the board already reported a missing card at boot
    if (shared_get().state == ADMIN_STOPPED && is_board_sd_missing()) {
        shared_set(ADMIN_STOPPED, "Board SD card is missing.", 0);
    }

    uart_register_line_callback(uart_line_callback, NULL);
    shared_take_changed();
    draw_screen(screen);

    ESP_LOGI(TAG, "SD admin screen created");
    return screen;
}
