/**
 * @file keyboard_k132.c
 * @brief Keyboard driver for M5Stack Cardputer (K132) using 74HC138
 */

#include "keyboard.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG = "KEYBOARD";

// K132 keyboard wiring (from M5Stack Cardputer docs)
#define K132_ADDR_A0 8
#define K132_ADDR_A1 9
#define K132_ADDR_A2 11

#define K132_ROWS 8
#define K132_COLS 7

static const gpio_num_t k132_col_pins[K132_COLS] = {
    13, 15, 3, 4, 5, 6, 7
};

// Key event queue
static QueueHandle_t key_queue = NULL;
static key_event_callback_t key_callback = NULL;
static key_code_t last_key = KEY_NONE;
static bool keyboard_initialized = false;

// Modifier key states
static bool fn_held = false;
static bool shift_held = false;
static bool ctrl_held = false;
static bool capslock_state = false;

static bool text_input_mode = false;
static bool key_state[K132_ROWS][K132_COLS];

/**
 * @brief Keyboard layout map (4 rows x 14 columns)
 */
static const key_code_t key_value_map[4][14] = {
    {KEY_GRAVE, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0, KEY_MINUS, KEY_EQUAL, KEY_BACKSPACE},
    {KEY_TAB, KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P, KEY_LBRACKET, KEY_RBRACKET, KEY_BACKSLASH},
    {KEY_FN, KEY_SHIFT, KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L, KEY_SEMICOLON, KEY_APOSTROPHE, KEY_ENTER},
    {KEY_CTRL, KEY_OPT, KEY_ALT, KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M, KEY_COMMA, KEY_DOT, KEY_SLASH, KEY_SPACE}
};

typedef struct {
    uint8_t x_1;
    uint8_t x_2;
} x_map_t;

// Matches M5Cardputer IOMatrix mapping.
static const x_map_t x_map_chart[7] = {
    {0, 1}, {2, 3}, {4, 5}, {6, 7}, {8, 9}, {10, 11}, {12, 13}
};

static void set_row_select(uint8_t row)
{
    gpio_set_level(K132_ADDR_A0, (row >> 0) & 0x01);
    gpio_set_level(K132_ADDR_A1, (row >> 1) & 0x01);
    gpio_set_level(K132_ADDR_A2, (row >> 2) & 0x01);
}

static void update_modifier_state(key_code_t key, bool pressed)
{
    switch (key) {
        case KEY_FN:
            fn_held = pressed;
            break;
        case KEY_SHIFT:
            shift_held = pressed;
            break;
        case KEY_CTRL:
            ctrl_held = pressed;
            break;
        case KEY_CAPSLOCK:
            capslock_state = pressed;
            break;
        default:
            break;
    }
}

static bool is_modifier_key(key_code_t key)
{
    return (key == KEY_FN || key == KEY_OPT || key == KEY_SHIFT || key == KEY_CTRL || key == KEY_ALT || key == KEY_CAPSLOCK);
}

static bool raw_to_xy(uint8_t raw_row, uint8_t raw_col, uint8_t *x, uint8_t *y)
{
    if (raw_row >= K132_ROWS || raw_col >= K132_COLS) {
        return false;
    }

    // Equivalent of M5 IOMatrixKeyboardReader:
    // coor.x = (raw_row > 3) ? x_map_chart[j].x_1 : x_map_chart[j].x_2;
    // coor.y = (raw_row > 3) ? (raw_row - 4) : raw_row;
    // coor.y = -coor.y + 3;
    uint8_t row = (raw_row > 3) ? (raw_row - 4) : raw_row;
    row = (uint8_t)(3 - row);

    uint8_t col = (raw_row > 3) ? x_map_chart[raw_col].x_1 : x_map_chart[raw_col].x_2;

    if (row >= 4 || col >= 14) {
        return false;
    }

    *x = col;
    *y = row;
    return true;
}

static void handle_key_event(uint8_t raw_row, uint8_t raw_col, bool pressed)
{
    uint8_t x = 0;
    uint8_t y = 0;
    if (!raw_to_xy(raw_row, raw_col, &x, &y)) return;

    key_code_t key = key_value_map[y][x];

    update_modifier_state(key, pressed);

    // Skip sending modifier keys as events
    if (is_modifier_key(key)) {
        return;
    }

    if (!pressed) {
        return;
    }

    // ESC and arrow key behavior matches ADV driver
    if (key == KEY_GRAVE) {
        bool esc_active = text_input_mode ? fn_held : true;
        if (esc_active) {
            key = KEY_ESC;
        }
    } else {
        bool arrow_active = text_input_mode ? fn_held : true;
        if (arrow_active) {
            if (key == KEY_SEMICOLON) key = KEY_UP;
            else if (key == KEY_DOT) key = KEY_DOWN;
            else if (key == KEY_COMMA) key = KEY_LEFT;
            else if (key == KEY_SLASH) key = KEY_RIGHT;
        }
    }

    if (key == KEY_NONE) return;

    last_key = key;
    xQueueSend(key_queue, &key, 0);

    if (key_callback) {
        key_callback(key, true);
    }
}

static void scan_keyboard(void)
{
    if (!keyboard_initialized) return;

    for (uint8_t row = 0; row < K132_ROWS; row++) {
        set_row_select(row);
        esp_rom_delay_us(30);

        for (uint8_t col = 0; col < K132_COLS; col++) {
            bool pressed = (gpio_get_level(k132_col_pins[col]) == 0);
            if (pressed != key_state[row][col]) {
                key_state[row][col] = pressed;
                handle_key_event(row, col, pressed);
            }
        }
    }
}

esp_err_t keyboard_init(void)
{
    ESP_LOGI(TAG, "Initializing Cardputer K132 keyboard (74HC138)...");

    // Configure address pins as outputs
    gpio_config_t addr_conf = {
        .pin_bit_mask = (1ULL << K132_ADDR_A0) | (1ULL << K132_ADDR_A1) | (1ULL << K132_ADDR_A2),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&addr_conf);
    set_row_select(0);

    // Configure column pins as inputs with pull-ups
    uint64_t col_mask = 0;
    for (int i = 0; i < K132_COLS; i++) {
        col_mask |= (1ULL << k132_col_pins[i]);
    }
    gpio_config_t col_conf = {
        .pin_bit_mask = col_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&col_conf);

    memset(key_state, 0, sizeof(key_state));

    key_queue = xQueueCreate(16, sizeof(key_code_t));
    if (key_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create key queue");
        return ESP_FAIL;
    }

    keyboard_initialized = true;
    ESP_LOGI(TAG, "Keyboard initialized successfully");
    return ESP_OK;
}

void keyboard_process(void)
{
    scan_keyboard();
}

void keyboard_register_callback(key_event_callback_t callback)
{
    key_callback = callback;
}

key_code_t keyboard_get_key(void)
{
    key_code_t key = KEY_NONE;
    if (xQueueReceive(key_queue, &key, 0) == pdTRUE) {
        return key;
    }
    return KEY_NONE;
}

bool keyboard_is_pressed(key_code_t key)
{
    return (last_key == key);
}

bool keyboard_is_shift_held(void)
{
    return shift_held;
}

bool keyboard_is_ctrl_held(void)
{
    return ctrl_held;
}

bool keyboard_is_capslock_held(void)
{
    return capslock_state;
}

bool keyboard_is_fn_held(void)
{
    return fn_held;
}

void keyboard_set_text_input_mode(bool enabled)
{
    text_input_mode = enabled;
    ESP_LOGI(TAG, "Text input mode: %s", enabled ? "ON" : "OFF");
}
