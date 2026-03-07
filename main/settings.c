/**
 * @file settings.c
 * @brief Application settings stored in NVS
 */

#include "settings.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "SETTINGS";

// NVS namespace and keys
#define NVS_NAMESPACE       "settings"
#define NVS_KEY_UART_TX     "uart_tx"
#define NVS_KEY_UART_RX     "uart_rx"
#define NVS_KEY_RED_TEAM    "red_team"
#define NVS_KEY_SCR_TIMEOUT "scr_tmout"
#define NVS_KEY_SCR_BRIGHT  "scr_bright"
#define NVS_KEY_GPS_TYPE    "gps_type"

// Cached values
static int uart_tx_pin = DEFAULT_UART_TX_PIN;
static int uart_rx_pin = DEFAULT_UART_RX_PIN;
static bool red_team_enabled = false;  // Default: disabled
static uint32_t screen_timeout_ms = DEFAULT_SCREEN_TIMEOUT_MS;
static uint8_t screen_brightness = DEFAULT_SCREEN_BRIGHTNESS;
static gps_type_t gps_type = GPS_TYPE_ATGM;  // Default: ATGM

// Reserved GPIO pins that should not be used (ESP32-S3 specific)
// These include strapping pins, flash/PSRAM pins, USB pins, etc.
static const int reserved_pins[] = {
    // Strapping pins
    0,   // Boot mode
    3,   // JTAG
    45,  // VDD_SPI voltage
    46,  // Boot mode / ROM log
    // USB pins
    19,  // USB_D-
    20,  // USB_D+
    // Flash/PSRAM pins (QSPI mode)
    26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37,
};

#define RESERVED_PINS_COUNT (sizeof(reserved_pins) / sizeof(reserved_pins[0]))

static bool is_reserved_pin(int pin)
{
    for (int i = 0; i < RESERVED_PINS_COUNT; i++) {
        if (reserved_pins[i] == pin) {
            return true;
        }
    }
    return false;
}

bool settings_is_valid_gpio_pin(int pin)
{
    // Check range
    if (pin < MIN_GPIO_PIN || pin > MAX_GPIO_PIN) {
        return false;
    }
    
    // Check if reserved
    if (is_reserved_pin(pin)) {
        return false;
    }
    
    return true;
}

esp_err_t settings_init(void)
{
    ESP_LOGI(TAG, "Initializing settings...");
    
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Open NVS namespace
    nvs_handle_t handle;
    ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    
    if (ret == ESP_OK) {
        // Read stored values
        int32_t tx = DEFAULT_UART_TX_PIN;
        int32_t rx = DEFAULT_UART_RX_PIN;
        
        if (nvs_get_i32(handle, NVS_KEY_UART_TX, &tx) == ESP_OK) {
            uart_tx_pin = (int)tx;
            ESP_LOGI(TAG, "Loaded UART TX pin: %d", uart_tx_pin);
        }
        
        if (nvs_get_i32(handle, NVS_KEY_UART_RX, &rx) == ESP_OK) {
            uart_rx_pin = (int)rx;
            ESP_LOGI(TAG, "Loaded UART RX pin: %d", uart_rx_pin);
        }
        
        uint8_t red_team_val = 0;
        if (nvs_get_u8(handle, NVS_KEY_RED_TEAM, &red_team_val) == ESP_OK) {
            red_team_enabled = (red_team_val != 0);
            ESP_LOGI(TAG, "Loaded Red Team enabled: %s", red_team_enabled ? "true" : "false");
        }
        
        uint32_t timeout_val = 0;
        if (nvs_get_u32(handle, NVS_KEY_SCR_TIMEOUT, &timeout_val) == ESP_OK) {
            screen_timeout_ms = timeout_val;
            ESP_LOGI(TAG, "Loaded screen timeout: %lu ms", (unsigned long)screen_timeout_ms);
        }
        
        uint8_t bright_val = 0;
        if (nvs_get_u8(handle, NVS_KEY_SCR_BRIGHT, &bright_val) == ESP_OK) {
            if (bright_val >= 1 && bright_val <= 100) {
                screen_brightness = bright_val;
            }
            ESP_LOGI(TAG, "Loaded screen brightness: %d%%", screen_brightness);
        }
        
        uint8_t gps_val = 0;
        if (nvs_get_u8(handle, NVS_KEY_GPS_TYPE, &gps_val) == ESP_OK) {
            // Migration: old GPS_TYPE_EXTERNAL(2) removed, old GPS_TYPE_CAP(3) -> new GPS_TYPE_CAP(2)
            if (gps_val == 3) gps_val = GPS_TYPE_CAP;
            if (gps_val <= GPS_TYPE_CAP) {
                gps_type = (gps_type_t)gps_val;
            } else {
                gps_type = GPS_TYPE_ATGM;
            }
            ESP_LOGI(TAG, "Loaded GPS type: %d", gps_type);
        }
        
        nvs_close(handle);
    } else if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No settings found, using defaults (TX=%d, RX=%d)", 
                 uart_tx_pin, uart_rx_pin);
    } else {
        ESP_LOGW(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(ret));
    }
    
    ESP_LOGI(TAG, "Settings initialized (TX=%d, RX=%d)", uart_tx_pin, uart_rx_pin);
    return ESP_OK;
}

int settings_get_uart_tx_pin(void)
{
    return uart_tx_pin;
}

int settings_get_uart_rx_pin(void)
{
    return uart_rx_pin;
}

esp_err_t settings_set_uart_pins(int tx_pin, int rx_pin)
{
    // Validate pins
    if (!settings_is_valid_gpio_pin(tx_pin)) {
        ESP_LOGE(TAG, "Invalid TX pin: %d", tx_pin);
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!settings_is_valid_gpio_pin(rx_pin)) {
        ESP_LOGE(TAG, "Invalid RX pin: %d", rx_pin);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Open NVS for writing
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Write values
    ret = nvs_set_i32(handle, NVS_KEY_UART_TX, (int32_t)tx_pin);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write TX pin: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }
    
    ret = nvs_set_i32(handle, NVS_KEY_UART_RX, (int32_t)rx_pin);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write RX pin: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }
    
    // Commit
    ret = nvs_commit(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }
    
    nvs_close(handle);
    
    // Update cached values
    uart_tx_pin = tx_pin;
    uart_rx_pin = rx_pin;
    
    ESP_LOGI(TAG, "UART pins saved (TX=%d, RX=%d) - restart required", tx_pin, rx_pin);
    return ESP_OK;
}

bool settings_get_red_team_enabled(void)
{
    return red_team_enabled;
}

esp_err_t settings_set_red_team_enabled(bool enabled)
{
    // Open NVS for writing
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Write value
    ret = nvs_set_u8(handle, NVS_KEY_RED_TEAM, enabled ? 1 : 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write Red Team setting: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }
    
    // Commit
    ret = nvs_commit(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }
    
    nvs_close(handle);
    
    // Update cached value
    red_team_enabled = enabled;
    
    ESP_LOGI(TAG, "Red Team setting saved: %s", enabled ? "enabled" : "disabled");
    return ESP_OK;
}

uint32_t settings_get_screen_timeout_ms(void)
{
    return screen_timeout_ms;
}

esp_err_t settings_set_screen_timeout_ms(uint32_t timeout_ms)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = nvs_set_u32(handle, NVS_KEY_SCR_TIMEOUT, timeout_ms);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write screen timeout: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }
    
    ret = nvs_commit(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }
    
    nvs_close(handle);
    screen_timeout_ms = timeout_ms;
    
    ESP_LOGI(TAG, "Screen timeout saved: %lu ms", (unsigned long)timeout_ms);
    return ESP_OK;
}

uint8_t settings_get_screen_brightness(void)
{
    return screen_brightness;
}

esp_err_t settings_set_screen_brightness(uint8_t brightness)
{
    if (brightness < 1) brightness = 1;
    if (brightness > 100) brightness = 100;
    
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = nvs_set_u8(handle, NVS_KEY_SCR_BRIGHT, brightness);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write screen brightness: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }
    
    ret = nvs_commit(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }
    
    nvs_close(handle);
    screen_brightness = brightness;
    
    ESP_LOGI(TAG, "Screen brightness saved: %d%%", brightness);
    return ESP_OK;
}

gps_type_t settings_get_gps_type(void)
{
    return gps_type;
}

esp_err_t settings_set_gps_type(gps_type_t type)
{
    if (type > GPS_TYPE_CAP) {
        return ESP_ERR_INVALID_ARG;
    }
    
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = nvs_set_u8(handle, NVS_KEY_GPS_TYPE, (uint8_t)type);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write GPS type: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }
    
    ret = nvs_commit(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }
    
    nvs_close(handle);
    gps_type = type;
    
    ESP_LOGI(TAG, "GPS type saved: %d", type);
    return ESP_OK;
}
