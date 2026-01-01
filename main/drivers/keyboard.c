/**
 * @file keyboard.c
 * @brief Keyboard driver for M5Stack Cardputer ADV
 * 
 * Uses TCA8418 keyboard controller at I2C address 0x34
 * TCA8418 is a I2C/SMBus keyboard controller with interrupt support
 */

#include "keyboard.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG = "KEYBOARD";

// Cardputer ADV I2C configuration
#define KEYBOARD_I2C_PORT       I2C_NUM_0
#define KEYBOARD_I2C_SDA        8
#define KEYBOARD_I2C_SCL        9
#define KEYBOARD_I2C_FREQ       100000
#define KEYBOARD_I2C_ADDR       0x34  // TCA8418 address

// TCA8418 Register addresses
#define TCA8418_REG_CFG             0x01
#define TCA8418_REG_INT_STAT        0x02
#define TCA8418_REG_KEY_LCK_EC      0x03
#define TCA8418_REG_KEY_EVENT_A     0x04
#define TCA8418_REG_KEY_EVENT_B     0x05
#define TCA8418_REG_KEY_EVENT_C     0x06
#define TCA8418_REG_KEY_EVENT_D     0x07
#define TCA8418_REG_KEY_EVENT_E     0x08
#define TCA8418_REG_KEY_EVENT_F     0x09
#define TCA8418_REG_KEY_EVENT_G     0x0A
#define TCA8418_REG_KEY_EVENT_H     0x0B
#define TCA8418_REG_KEY_EVENT_I     0x0C
#define TCA8418_REG_KEY_EVENT_J     0x0D
#define TCA8418_REG_KP_LCK_TIMER    0x0E
#define TCA8418_REG_UNLOCK1         0x0F
#define TCA8418_REG_UNLOCK2         0x10
#define TCA8418_REG_GPIO_INT_STAT1  0x11
#define TCA8418_REG_GPIO_INT_STAT2  0x12
#define TCA8418_REG_GPIO_INT_STAT3  0x13
#define TCA8418_REG_GPIO_DAT_STAT1  0x14
#define TCA8418_REG_GPIO_DAT_STAT2  0x15
#define TCA8418_REG_GPIO_DAT_STAT3  0x16
#define TCA8418_REG_GPIO_DAT_OUT1   0x17
#define TCA8418_REG_GPIO_DAT_OUT2   0x18
#define TCA8418_REG_GPIO_DAT_OUT3   0x19
#define TCA8418_REG_GPIO_INT_EN1    0x1A
#define TCA8418_REG_GPIO_INT_EN2    0x1B
#define TCA8418_REG_GPIO_INT_EN3    0x1C
#define TCA8418_REG_KP_GPIO1        0x1D
#define TCA8418_REG_KP_GPIO2        0x1E
#define TCA8418_REG_KP_GPIO3        0x1F
#define TCA8418_REG_GPI_EM1         0x20
#define TCA8418_REG_GPI_EM2         0x21
#define TCA8418_REG_GPI_EM3         0x22
#define TCA8418_REG_GPIO_DIR1       0x23
#define TCA8418_REG_GPIO_DIR2       0x24
#define TCA8418_REG_GPIO_DIR3       0x25
#define TCA8418_REG_GPIO_INT_LVL1   0x26
#define TCA8418_REG_GPIO_INT_LVL2   0x27
#define TCA8418_REG_GPIO_INT_LVL3   0x28
#define TCA8418_REG_DEBOUNCE_DIS1   0x29
#define TCA8418_REG_DEBOUNCE_DIS2   0x2A
#define TCA8418_REG_DEBOUNCE_DIS3   0x2B
#define TCA8418_REG_GPIO_PULL1      0x2C
#define TCA8418_REG_GPIO_PULL2      0x2D
#define TCA8418_REG_GPIO_PULL3      0x2E

// TCA8418 CFG register bits
#define TCA8418_CFG_AI              0x80
#define TCA8418_CFG_GPI_E_CFG       0x40
#define TCA8418_CFG_OVR_FLOW_M      0x20
#define TCA8418_CFG_INT_CFG         0x10
#define TCA8418_CFG_OVR_FLOW_IEN    0x08
#define TCA8418_CFG_K_LCK_IEN       0x04
#define TCA8418_CFG_GPI_IEN         0x02
#define TCA8418_CFG_KE_IEN          0x01

// Key event register bits
#define TCA8418_KEY_EVENT_CODE_MASK 0x7F
#define TCA8418_KEY_EVENT_PRESSED   0x80

// Key event queue
static QueueHandle_t key_queue = NULL;
static key_event_callback_t key_callback = NULL;
static key_code_t last_key = KEY_NONE;
static bool keyboard_initialized = false;

/**
 * @brief Read a register from TCA8418
 */
static esp_err_t tca8418_read_reg(uint8_t reg, uint8_t *value)
{
    esp_err_t ret = i2c_master_write_read_device(
        KEYBOARD_I2C_PORT,
        KEYBOARD_I2C_ADDR,
        &reg,
        1,
        value,
        1,
        pdMS_TO_TICKS(10)
    );
    return ret;
}

/**
 * @brief Write a register to TCA8418
 */
static esp_err_t tca8418_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    esp_err_t ret = i2c_master_write_to_device(
        KEYBOARD_I2C_PORT,
        KEYBOARD_I2C_ADDR,
        data,
        2,
        pdMS_TO_TICKS(10)
    );
    return ret;
}

/**
 * @brief Initialize TCA8418 keyboard controller
 */
static esp_err_t tca8418_init(void)
{
    esp_err_t ret;
    
    // Configure keyboard matrix - 8 rows x 10 columns
    // Set rows 0-7 as keypad
    ret = tca8418_write_reg(TCA8418_REG_KP_GPIO1, 0xFF);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write KP_GPIO1");
        return ret;
    }
    
    // Set cols 0-7 as keypad
    ret = tca8418_write_reg(TCA8418_REG_KP_GPIO2, 0xFF);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write KP_GPIO2");
        return ret;
    }
    
    // Set cols 8-9 as keypad
    ret = tca8418_write_reg(TCA8418_REG_KP_GPIO3, 0x03);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write KP_GPIO3");
        return ret;
    }
    
    // Enable key event interrupt, auto-increment
    ret = tca8418_write_reg(TCA8418_REG_CFG, TCA8418_CFG_AI | TCA8418_CFG_KE_IEN | TCA8418_CFG_INT_CFG);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write CFG");
        return ret;
    }
    
    // Clear any pending interrupts
    uint8_t int_stat;
    ret = tca8418_read_reg(TCA8418_REG_INT_STAT, &int_stat);
    if (ret == ESP_OK && int_stat) {
        tca8418_write_reg(TCA8418_REG_INT_STAT, int_stat);  // Clear by writing 1s
    }
    
    // Read and discard any pending key events
    uint8_t key_event;
    for (int i = 0; i < 10; i++) {
        ret = tca8418_read_reg(TCA8418_REG_KEY_EVENT_A, &key_event);
        if (ret != ESP_OK || key_event == 0) break;
    }
    
    ESP_LOGI(TAG, "TCA8418 initialized successfully");
    return ESP_OK;
}

/**
 * @brief Keyboard layout map from M5Stack official code
 * Layout: 4 rows × 14 columns after remap
 * 
 * Row 0: `  1  2  3  4  5  6  7  8  9  0  -  =  del
 * Row 1: tab q  w  e  r  t  y  u  i  o  p  [  ]  \
 * Row 2: shift caps a  s  d  f  g  h  j  k  l  ;  '  enter  
 * Row 3: ctrl opt alt z  x  c  v  b  n  m  ,  .  /  space
 */
static const key_code_t _key_value_map[4][14] = {
    // Row 0: ` 1 2 3 4 5 6 7 8 9 0 - = del
    {KEY_ESC, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0, KEY_NONE, KEY_NONE, KEY_BACKSPACE},
    // Row 1: tab q w e r t y u i o p [ ] backslash
    {KEY_TAB, KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P, KEY_NONE, KEY_NONE, KEY_NONE},
    // Row 2: fn shift a s d f g h j k l ; ' enter (code 3=fn, code 7=shift)
    {KEY_FN, KEY_SHIFT, KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L, KEY_NONE, KEY_NONE, KEY_ENTER},
    // Row 3: ctrl opt alt z x c v b n m , . / space
    {KEY_NONE, KEY_OPT, KEY_ALT, KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M, KEY_NONE, KEY_NONE, KEY_NONE, KEY_SPACE}
};

// Shift key state
static bool shift_held = false;

/**
 * @brief Remap TCA8418 raw coordinates to Cardputer layout
 * Based on M5Stack official keyboard.cpp
 */
static void remap_key(uint8_t *row, uint8_t *col)
{
    uint8_t raw_row = *row;
    uint8_t raw_col = *col;
    
    // Col: raw_row * 2, +1 if raw_col > 3
    uint8_t new_col = raw_row * 2;
    if (raw_col > 3) new_col++;
    
    // Row: (raw_col + 4) % 4
    uint8_t new_row = (raw_col + 4) % 4;
    
    *row = new_row;
    *col = new_col;
}

/**
 * @brief Convert TCA8418 key code to our key_code_t
 * Uses M5Stack official remap algorithm + special keys
 */
static key_code_t tca8418_to_keycode(uint8_t key_code)
{
    if (key_code == 0) return KEY_NONE;
    
    // Special keys that are outside the standard 4x14 matrix
    // These were confirmed by user testing
    switch (key_code) {
        case 57: return KEY_UP;
        case 58: return KEY_DOWN;
        case 56: return KEY_LEFT;
        case 59: return KEY_RIGHT;
    }
    
    // Parse raw code to row/col
    // Formula: key_code = (row * 10) + col + 1
    uint16_t buffer = key_code;
    buffer--;
    uint8_t raw_row = buffer / 10;
    uint8_t raw_col = buffer % 10;
    
    // Apply remap (from M5Stack official code)
    uint8_t row = raw_row;
    uint8_t col = raw_col;
    remap_key(&row, &col);
    
    // Bounds check
    if (row >= 4 || col >= 14) {
        ESP_LOGW(TAG, "Key out of bounds: code=%d raw(%d,%d) -> (%d,%d)", 
                 key_code, raw_row, raw_col, row, col);
        return KEY_NONE;
    }
    
    key_code_t result = _key_value_map[row][col];
    
    if (result == KEY_NONE) {
        ESP_LOGD(TAG, "Unmapped key: code=%d raw(%d,%d) -> (%d,%d)", 
                 key_code, raw_row, raw_col, row, col);
    }
    
    return result;
}

esp_err_t keyboard_init(void)
{
    ESP_LOGI(TAG, "Initializing Cardputer ADV keyboard (TCA8418)...");
    ESP_LOGI(TAG, "I2C pins: SDA=%d, SCL=%d", KEYBOARD_I2C_SDA, KEYBOARD_I2C_SCL);

    // Initialize I2C
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = KEYBOARD_I2C_SDA,
        .scl_io_num = KEYBOARD_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = KEYBOARD_I2C_FREQ,
    };
    
    esp_err_t ret = i2c_param_config(KEYBOARD_I2C_PORT, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C param config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2c_driver_install(KEYBOARD_I2C_PORT, conf.mode, 0, 0, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Test TCA8418 presence
    uint8_t test_val;
    ret = tca8418_read_reg(TCA8418_REG_CFG, &test_val);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TCA8418 not found at address 0x%02X", KEYBOARD_I2C_ADDR);
        return ret;
    }
    ESP_LOGI(TAG, "TCA8418 found, CFG register: 0x%02X", test_val);

    // Initialize TCA8418
    ret = tca8418_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TCA8418 initialization failed");
        return ret;
    }

    // Create key event queue
    key_queue = xQueueCreate(16, sizeof(key_code_t));
    if (key_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create key queue");
        return ESP_FAIL;
    }

    keyboard_initialized = true;
    ESP_LOGI(TAG, "Keyboard initialized successfully");
    return ESP_OK;
}

static void scan_keyboard(void)
{
    if (!keyboard_initialized) return;

    // Read key event count
    uint8_t key_lck_ec;
    esp_err_t ret = tca8418_read_reg(TCA8418_REG_KEY_LCK_EC, &key_lck_ec);
    if (ret != ESP_OK) return;
    
    uint8_t event_count = key_lck_ec & 0x0F;  // Lower 4 bits = event count
    
    // Process all pending key events
    for (int i = 0; i < event_count && i < 10; i++) {
        uint8_t key_event;
        ret = tca8418_read_reg(TCA8418_REG_KEY_EVENT_A, &key_event);
        if (ret != ESP_OK || key_event == 0) break;
        
        uint8_t key_code = key_event & TCA8418_KEY_EVENT_CODE_MASK;
        bool pressed = (key_event & TCA8418_KEY_EVENT_PRESSED) != 0;
        
        ESP_LOGI(TAG, "Key event: code=%d, %s", key_code, pressed ? "PRESSED" : "released");
        
        key_code_t key = tca8418_to_keycode(key_code);
        
        // Track shift state (press and release)
        if (key == KEY_SHIFT) {
            shift_held = pressed;
            ESP_LOGI(TAG, "Shift %s", pressed ? "pressed" : "released");
        }
        
        if (pressed && key != KEY_NONE && key != KEY_SHIFT) {
            last_key = key;
            xQueueSend(key_queue, &key, 0);
            
            if (key_callback) {
                key_callback(key, true);
            }
        }
    }
    
    // Clear interrupt if any events were processed
    if (event_count > 0) {
        uint8_t int_stat;
        tca8418_read_reg(TCA8418_REG_INT_STAT, &int_stat);
        if (int_stat) {
            tca8418_write_reg(TCA8418_REG_INT_STAT, int_stat);
        }
    }
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
