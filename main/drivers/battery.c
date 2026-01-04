/**
 * @file battery.c
 * @brief Battery level monitoring for M5Stack Cardputer-Adv
 * 
 * Reads battery voltage through ADC on GPIO10.
 * Uses ESP-IDF v5+ ADC oneshot driver.
 */

#include "battery.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "BATTERY";

// Battery ADC configuration (GPIO10 = ADC1_CHANNEL_9 on ESP32-S3)
#define BATTERY_ADC_UNIT        ADC_UNIT_1
#define BATTERY_ADC_CHANNEL     ADC_CHANNEL_9   // GPIO10
#define BATTERY_ADC_ATTEN       ADC_ATTEN_DB_12 // Full scale ~3.3V

// Battery voltage range for Li-ion (in mV at ADC input after voltage divider)
// Cardputer-Adv uses a voltage divider, so we measure half the battery voltage
// Battery: 3.0V (empty) to 4.2V (full)
// At ADC: ~1.5V to ~2.1V (with 1:1 divider)
#define BATTERY_DIVIDER_RATIO   2.0     // Voltage divider ratio
#define BATTERY_MIN_MV          3000    // 0% - battery empty
#define BATTERY_MAX_MV          4200    // 100% - battery full

// Smoothing
#define BATTERY_SAMPLES         4       // Number of samples to average

// ADC handles
static adc_oneshot_unit_handle_t adc_handle = NULL;
static adc_cali_handle_t cali_handle = NULL;
static bool battery_initialized = false;
static bool calibration_enabled = false;

// Smoothing buffer
static int voltage_buffer[BATTERY_SAMPLES] = {0};
static int buffer_index = 0;
static bool buffer_filled = false;

/**
 * @brief Initialize ADC calibration
 */
static bool init_calibration(void)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = BATTERY_ADC_UNIT,
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t ret = adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ADC calibration: curve fitting");
        return true;
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = BATTERY_ADC_UNIT,
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t ret = adc_cali_create_scheme_line_fitting(&cali_config, &cali_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ADC calibration: line fitting");
        return true;
    }
#endif

    ESP_LOGW(TAG, "ADC calibration not available");
    return false;
}

esp_err_t battery_init(void)
{
    ESP_LOGI(TAG, "Initializing battery monitoring (GPIO10)...");
    
    // Initialize ADC unit
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = BATTERY_ADC_UNIT,
    };
    
    esp_err_t ret = adc_oneshot_new_unit(&unit_cfg, &adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init ADC unit: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Configure ADC channel
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    
    ret = adc_oneshot_config_channel(adc_handle, BATTERY_ADC_CHANNEL, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to config ADC channel: %s", esp_err_to_name(ret));
        adc_oneshot_del_unit(adc_handle);
        adc_handle = NULL;
        return ret;
    }
    
    // Initialize calibration
    calibration_enabled = init_calibration();
    
    battery_initialized = true;
    
    // Take initial readings to fill buffer
    for (int i = 0; i < BATTERY_SAMPLES; i++) {
        battery_get_voltage_mv();
    }
    
    int level = battery_get_level();
    ESP_LOGI(TAG, "Battery monitoring initialized, level: %d%%", level);
    
    return ESP_OK;
}

bool battery_is_available(void)
{
    return battery_initialized;
}

int battery_get_voltage_mv(void)
{
    if (!battery_initialized || adc_handle == NULL) {
        return -1;
    }
    
    int raw_value = 0;
    esp_err_t ret = adc_oneshot_read(adc_handle, BATTERY_ADC_CHANNEL, &raw_value);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ADC read failed: %s", esp_err_to_name(ret));
        return -1;
    }
    
    int voltage_mv = 0;
    
    if (calibration_enabled && cali_handle != NULL) {
        // Use calibrated conversion
        ret = adc_cali_raw_to_voltage(cali_handle, raw_value, &voltage_mv);
        if (ret != ESP_OK) {
            // Fallback to simple conversion
            voltage_mv = (raw_value * 3300) / 4095;
        }
    } else {
        // Simple conversion without calibration
        voltage_mv = (raw_value * 3300) / 4095;
    }
    
    // Apply voltage divider ratio to get actual battery voltage
    int battery_voltage = (int)(voltage_mv * BATTERY_DIVIDER_RATIO);
    
    // Add to smoothing buffer
    voltage_buffer[buffer_index] = battery_voltage;
    buffer_index = (buffer_index + 1) % BATTERY_SAMPLES;
    if (buffer_index == 0) {
        buffer_filled = true;
    }
    
    // Calculate average
    int sum = 0;
    int count = buffer_filled ? BATTERY_SAMPLES : buffer_index;
    if (count == 0) count = 1;
    
    for (int i = 0; i < count; i++) {
        sum += voltage_buffer[i];
    }
    
    return sum / count;
}

int battery_get_level(void)
{
    int voltage = battery_get_voltage_mv();
    if (voltage < 0) {
        return -1;
    }
    
    // Clamp to valid range
    if (voltage <= BATTERY_MIN_MV) {
        return 0;
    }
    if (voltage >= BATTERY_MAX_MV) {
        return 100;
    }
    
    // Linear interpolation
    int level = ((voltage - BATTERY_MIN_MV) * 100) / (BATTERY_MAX_MV - BATTERY_MIN_MV);
    
    return level;
}




