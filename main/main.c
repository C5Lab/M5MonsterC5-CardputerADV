/**
 * @file main.c
 * @brief Main entry point for Cardputer-ADV WiFi Attack application
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "display.h"
#include "keyboard.h"
#include "uart_handler.h"
#include "screen_manager.h"
#include "home_screen.h"
#include "battery.h"
#include "settings.h"
#include "buzzer.h"
#include "text_ui.h"

#include "version.h"

static const char *TAG = "MAIN";

static volatile bool board_sd_missing = false;
static volatile bool board_sd_check_pending = false;
static volatile bool board_sd_ok_received = false;
static int64_t board_sd_check_start_ms = 0;

#define SD_CHECK_TIMEOUT_MS    3000
#define SD_CHECK_RETRIES       2
#define SD_CHECK_BOOT_DELAY_MS 1000

bool is_board_sd_missing(void)
{
    return board_sd_missing;
}

static void uart_sd_check_line_callback(const char *line, void *user_data)
{
    (void)user_data;
    if (!line || !board_sd_check_pending) {
        return;
    }
    if (strcmp(line, "SD_OK") == 0) {
        board_sd_ok_received = true;
        board_sd_check_pending = false;
    } else if (strcmp(line, "SD_NONE") == 0) {
        board_sd_missing = true;
        board_sd_check_pending = false;
    }
}

// ─── Boot screen ─────────────────────────────────────────────────────────────

typedef enum {
    BSTATE_PENDING = 0,
    BSTATE_RUNNING,
    BSTATE_OK,
    BSTATE_WARN,
} boot_state_t;

typedef struct {
    const char  *label;
    boot_state_t state;
    char         detail[40];
} boot_step_t;

#define BOOT_STEPS 2
static boot_step_t s_steps[BOOT_STEPS] = {
    { "1/2  Booting",        BSTATE_PENDING, "" },
    { "2/2  Monster Grove",  BSTATE_PENDING, "" },
};

#define BOOT_ROW_START  2   // grid row where first step is drawn
#define FONT_H          16  // matches font8x16

static void boot_draw_step(int i)
{
    const char *icon;
    uint16_t    color;
    switch (s_steps[i].state) {
        case BSTATE_OK:      icon = "[OK]"; color = UI_COLOR_TITLE;  break;
        case BSTATE_WARN:    icon = "[!!]"; color = COLOR_YELLOW;    break;
        case BSTATE_RUNNING: icon = "[..]"; color = UI_COLOR_TEXT;   break;
        default:             icon = "[  ]"; color = UI_COLOR_DIMMED; break;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%s %s", icon, s_steps[i].label);

    int y = (BOOT_ROW_START + i) * FONT_H;
    display_fill_rect(0, y, DISPLAY_WIDTH, FONT_H, UI_COLOR_BG);
    ui_print(0, BOOT_ROW_START + i, buf, color);
}

// Draw the full boot screen once — called only at startup.
static void boot_screen_init(void)
{
    ui_clear();
    ui_draw_title("  M5MONSTER  ADV  ");
    for (int i = 0; i < BOOT_STEPS; i++) {
        boot_draw_step(i);
    }
    ui_draw_status("");
    display_flush();
}

// Update a single step without touching the title or other rows.
static void boot_set(int step, boot_state_t state, const char *detail)
{
    s_steps[step].state = state;
    if (detail) {
        strncpy(s_steps[step].detail, detail, sizeof(s_steps[step].detail) - 1);
        s_steps[step].detail[sizeof(s_steps[step].detail) - 1] = '\0';
    }
    boot_draw_step(step);

    // Show detail of the last active step
    const char *status_text = "";
    for (int i = BOOT_STEPS - 1; i >= 0; i--) {
        if (s_steps[i].state != BSTATE_PENDING && s_steps[i].detail[0]) {
            status_text = s_steps[i].detail;
            break;
        }
    }
    ui_draw_status(status_text);
    display_flush();
}

static void boot_wait_for_monster_sd_continue(void)
{
    ESP_LOGW(TAG, "Monster SD card not detected, waiting for user confirmation");

    display_set_backlight(settings_get_screen_brightness());
    ui_clear();
    ui_show_message_tall("Warning",
                         "No SD in MonsterC5\n"
                         "Insert SD and reboot\n"
                         "Press Enter to continue\n"
                         "Some functions limited");
    display_flush();

    while (true) {
        keyboard_process();
        key_code_t key = keyboard_get_key();
        if (key == KEY_ENTER || key == KEY_SPACE || key == KEY_ESC) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ui_clear();
    display_flush();
}

// ─── app_main ────────────────────────────────────────────────────────────────

void app_main(void)
{
    ESP_LOGI(TAG, "Cardputer-ADV WiFi Attack Application Starting...");

    // Settings and display must succeed before we can show anything
    if (settings_init() != ESP_OK) {
        ESP_LOGE(TAG, "Settings init failed");
        return;
    }
    if (display_init() != ESP_OK) {
        ESP_LOGE(TAG, "Display init failed");
        return;
    }
    display_set_backlight(settings_get_screen_brightness());

    // Draw boot screen once — subsequent boot_set() calls update only changed rows
    boot_screen_init();

    // ── Step 1: Booting ──────────────────────────────────────────────────────
    boot_set(0, BSTATE_RUNNING, "Initializing...");

    battery_init(); // optional

    if (keyboard_init() != ESP_OK) {
        ESP_LOGE(TAG, "Keyboard init failed");
        return;
    }

    boot_set(0, BSTATE_OK, "");

    // ── Step 2: Monster Grove ────────────────────────────────────────────────
    boot_set(1, BSTATE_RUNNING, "Searching... ESC to skip");

    if (uart_handler_init() != ESP_OK) {
        ESP_LOGE(TAG, "UART handler init failed");
        return;
    }
    uart_register_monitor_callback(uart_sd_check_line_callback, NULL);

    bool board_detected = uart_check_board_ping(500);
    bool popup_dismissed = false;

    // Retry until board found or user skips
    while (!board_detected && !popup_dismissed) {
        for (int i = 0; i < 100 && !popup_dismissed; i++) {
            keyboard_process();
            if (keyboard_get_key() == KEY_ESC) {
                popup_dismissed = true;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (!popup_dismissed) {
            board_detected = uart_check_board_ping(500);
        }
    }

    if (!board_detected) {
        // User pressed ESC — skip Grove check
        boot_set(1, BSTATE_WARN, "Not detected (skipped)");
    } else {
        // Board found — now check its SD
        boot_set(1, BSTATE_RUNNING, "Checking Monster SD...");
        vTaskDelay(pdMS_TO_TICKS(SD_CHECK_BOOT_DELAY_MS));

        for (int attempt = 1; attempt <= SD_CHECK_RETRIES; attempt++) {
            board_sd_ok_received = false;
            board_sd_missing     = false;
            board_sd_check_pending = true;
            board_sd_check_start_ms = esp_timer_get_time() / 1000;
            uart_send_command("sd_status");

            while (board_sd_check_pending) {
                int64_t now_ms = esp_timer_get_time() / 1000;
                if ((now_ms - board_sd_check_start_ms) > SD_CHECK_TIMEOUT_MS) {
                    board_sd_check_pending = false;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            if (board_sd_ok_received || board_sd_missing) break;

            if (attempt < SD_CHECK_RETRIES) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        }

        if (board_sd_ok_received) {
            boot_set(1, BSTATE_OK, "Monster SD ready");
        } else if (board_sd_missing) {
            boot_set(1, BSTATE_WARN, "Monster SD missing");
            boot_wait_for_monster_sd_continue();
        } else {
            boot_set(1, BSTATE_WARN, "Monster SD unknown");
        }
    }

    // Pause so user can read the boot results
    vTaskDelay(pdMS_TO_TICKS(2000));

    // ── Finish boot ──────────────────────────────────────────────────────────
    if (buzzer_init() != ESP_OK) {
        ESP_LOGW(TAG, "Buzzer init failed - audio disabled");
    }

    screen_manager_init();
    screen_manager_push(home_screen_create, NULL);

    ESP_LOGI(TAG, "Application started successfully!");

    // Main loop
    int     tick_counter      = 0;
    bool    screen_dimmed     = false;
    int64_t last_activity_time = esp_timer_get_time() / 1000;

    while (1) {
        keyboard_process();

        key_code_t key = keyboard_get_key();
        if (key != KEY_NONE) {
            last_activity_time = esp_timer_get_time() / 1000;
            if (screen_dimmed) {
                display_set_backlight(settings_get_screen_brightness());
                screen_dimmed = false;
            }
        }

        uint32_t timeout = settings_get_screen_timeout_ms();
        int64_t  now     = esp_timer_get_time() / 1000;
        if (!screen_dimmed && timeout > 0 && (now - last_activity_time) > timeout) {
            display_set_backlight(0);
            screen_dimmed = true;
        }

        if (++tick_counter >= 50) {
            tick_counter = 0;
            screen_manager_tick();
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
