/**
 * @file cap_gps.c
 * @brief LoRa CAP GPS driver - reads NMEA from CAP device via UART2 (pins TX=15, RX=13)
 */

#include "cap_gps.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "CAP_GPS";

#define CAP_UART_NUM    UART_NUM_2
#define CAP_TX_PIN      13
#define CAP_RX_PIN      15
#define CAP_BAUD_RATE   115200
#define CAP_BUF_SIZE    1024
#define CAP_LINE_SIZE   256
#define CAP_FIX_TIMEOUT_US  2000000  // 2 seconds - no data = fix lost

// GPS state
static SemaphoreHandle_t gps_mutex = NULL;
static TaskHandle_t gps_task_handle = NULL;
static volatile bool gps_running = false;

static struct {
    double lat;
    double lon;
    double alt;
    double hdop;
    int satellites;
    bool fix;
    int64_t last_update_us;
} gps_data;

/**
 * @brief Convert NMEA coordinate (DDMM.MMMM) to decimal degrees
 */
static double nmea_to_decimal(const char *coord, char hemisphere)
{
    if (!coord || !coord[0]) return 0.0;

    double raw = strtod(coord, NULL);
    int degrees = (int)(raw / 100.0);
    double minutes = raw - (degrees * 100.0);
    double decimal = degrees + (minutes / 60.0);

    if (hemisphere == 'S' || hemisphere == 'W') {
        decimal = -decimal;
    }
    return decimal;
}

/**
 * @brief Parse $GPGGA sentence
 * Format: $GPGGA,time,lat,N/S,lon,E/W,fix,sats,hdop,alt,M,...
 */
static void parse_gga(const char *nmea)
{
    char buf[CAP_LINE_SIZE];
    strncpy(buf, nmea, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *save = NULL;
    char *tok = strtok_r(buf, ",", &save);
    int field = 0;

    char lat_str[16] = {0}, ns = 0;
    char lon_str[16] = {0}, ew = 0;
    int fix_quality = -1;
    int sats = -1;
    double hdop_val = 99.9;
    double alt_val = 0.0;

    while (tok) {
        switch (field) {
            case 2:
                strncpy(lat_str, tok, sizeof(lat_str) - 1);
                break;
            case 3:
                if (tok[0]) ns = tok[0];
                break;
            case 4:
                strncpy(lon_str, tok, sizeof(lon_str) - 1);
                break;
            case 5:
                if (tok[0]) ew = tok[0];
                break;
            case 6:
                fix_quality = atoi(tok);
                break;
            case 7:
                sats = atoi(tok);
                break;
            case 8:
                hdop_val = strtod(tok, NULL);
                break;
            case 9:
                alt_val = strtod(tok, NULL);
                break;
        }
        tok = strtok_r(NULL, ",", &save);
        field++;
    }

    xSemaphoreTake(gps_mutex, portMAX_DELAY);

    if (sats >= 0) {
        gps_data.satellites = sats;
    }

    if (fix_quality > 0 && lat_str[0] && lon_str[0] && ns && ew) {
        gps_data.fix = true;
        gps_data.lat = nmea_to_decimal(lat_str, ns);
        gps_data.lon = nmea_to_decimal(lon_str, ew);
        gps_data.alt = alt_val;
        gps_data.hdop = hdop_val;
        gps_data.last_update_us = esp_timer_get_time();
    } else if (fix_quality == 0) {
        gps_data.fix = false;
    }

    xSemaphoreGive(gps_mutex);
}

/**
 * @brief Parse $GPRMC sentence for fix status
 * Format: $GPRMC,time,A/V,lat,N/S,lon,E/W,...
 */
static void parse_rmc(const char *nmea)
{
    char buf[CAP_LINE_SIZE];
    strncpy(buf, nmea, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *save = NULL;
    char *tok = strtok_r(buf, ",", &save);
    int field = 0;
    char status = 0;
    char lat_str[16] = {0}, ns = 0;
    char lon_str[16] = {0}, ew = 0;

    while (tok) {
        switch (field) {
            case 2:
                if (tok[0]) status = tok[0];
                break;
            case 3:
                strncpy(lat_str, tok, sizeof(lat_str) - 1);
                break;
            case 4:
                if (tok[0]) ns = tok[0];
                break;
            case 5:
                strncpy(lon_str, tok, sizeof(lon_str) - 1);
                break;
            case 6:
                if (tok[0]) ew = tok[0];
                break;
        }
        tok = strtok_r(NULL, ",", &save);
        field++;
    }

    xSemaphoreTake(gps_mutex, portMAX_DELAY);

    if (status == 'A' && lat_str[0] && lon_str[0] && ns && ew) {
        gps_data.fix = true;
        gps_data.lat = nmea_to_decimal(lat_str, ns);
        gps_data.lon = nmea_to_decimal(lon_str, ew);
        gps_data.last_update_us = esp_timer_get_time();
    } else if (status == 'V') {
        gps_data.fix = false;
    }

    xSemaphoreGive(gps_mutex);
}

static void handle_nmea_line(const char *line)
{
    const char *nmea = strchr(line, '$');
    if (!nmea) return;

    if (strstr(nmea, "GGA") != NULL) {
        parse_gga(nmea);
    } else if (strstr(nmea, "RMC") != NULL) {
        parse_rmc(nmea);
    }
}

/**
 * @brief UART2 reading task
 */
static void cap_gps_task(void *arg)
{
    uint8_t rx_buf[CAP_BUF_SIZE];
    char line_buf[CAP_LINE_SIZE];
    int line_pos = 0;

    ESP_LOGI(TAG, "CAP GPS task started (UART%d, TX=%d, RX=%d, %d baud)",
             CAP_UART_NUM, CAP_TX_PIN, CAP_RX_PIN, CAP_BAUD_RATE);

    while (gps_running) {
        int len = uart_read_bytes(CAP_UART_NUM, rx_buf, sizeof(rx_buf) - 1, pdMS_TO_TICKS(100));
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                char c = (char)rx_buf[i];
                if (c == '\n' || c == '\r') {
                    if (line_pos > 0) {
                        line_buf[line_pos] = '\0';
                        handle_nmea_line(line_buf);
                        line_pos = 0;
                    }
                } else if (line_pos < CAP_LINE_SIZE - 1) {
                    line_buf[line_pos++] = c;
                }
            }
        }
    }

    ESP_LOGI(TAG, "CAP GPS task stopped");
    vTaskDelete(NULL);
}

esp_err_t cap_gps_init(void)
{
    if (gps_running) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing CAP GPS on UART%d (TX=%d, RX=%d)...",
             CAP_UART_NUM, CAP_TX_PIN, CAP_RX_PIN);

    // Init mutex
    if (!gps_mutex) {
        gps_mutex = xSemaphoreCreateMutex();
        if (!gps_mutex) {
            ESP_LOGE(TAG, "Failed to create mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    // Clear state
    memset(&gps_data, 0, sizeof(gps_data));
    gps_data.satellites = -1;

    // Reset GPIO pins (ensure clean state, no SPI bus crosstalk)
    gpio_reset_pin(CAP_TX_PIN);
    gpio_reset_pin(CAP_RX_PIN);

    // Configure UART2
    const uart_config_t uart_config = {
        .baud_rate = CAP_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_driver_install(CAP_UART_NUM, CAP_BUF_SIZE * 2, 0, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install UART driver: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_param_config(CAP_UART_NUM, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure UART: %s", esp_err_to_name(ret));
        uart_driver_delete(CAP_UART_NUM);
        return ret;
    }

    ret = uart_set_pin(CAP_UART_NUM, CAP_TX_PIN, CAP_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UART pins: %s", esp_err_to_name(ret));
        uart_driver_delete(CAP_UART_NUM);
        return ret;
    }

    // Start reading task
    gps_running = true;
    BaseType_t task_ret = xTaskCreate(cap_gps_task, "cap_gps", 4096, NULL, 5, &gps_task_handle);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create GPS task");
        gps_running = false;
        uart_driver_delete(CAP_UART_NUM);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "CAP GPS initialized");
    return ESP_OK;
}

void cap_gps_deinit(void)
{
    if (!gps_running) return;

    ESP_LOGI(TAG, "Deinitializing CAP GPS...");

    gps_running = false;

    // Wait for task to finish
    if (gps_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(200));
        gps_task_handle = NULL;
    }

    uart_driver_delete(CAP_UART_NUM);

    ESP_LOGI(TAG, "CAP GPS deinitialized");
}

bool cap_gps_has_fix(void)
{
    if (!gps_running || !gps_mutex) return false;

    xSemaphoreTake(gps_mutex, portMAX_DELAY);
    bool fix = gps_data.fix;
    int64_t last = gps_data.last_update_us;
    xSemaphoreGive(gps_mutex);

    if (!fix) return false;

    // Check timeout
    int64_t now = esp_timer_get_time();
    if (last == 0 || (now - last) > CAP_FIX_TIMEOUT_US) {
        return false;
    }

    return true;
}

bool cap_gps_get_position(double *lat, double *lon, double *alt, double *hdop)
{
    if (!gps_running || !gps_mutex) return false;

    xSemaphoreTake(gps_mutex, portMAX_DELAY);
    bool fix = gps_data.fix;
    if (fix) {
        if (lat) *lat = gps_data.lat;
        if (lon) *lon = gps_data.lon;
        if (alt) *alt = gps_data.alt;
        if (hdop) *hdop = gps_data.hdop;
    }
    xSemaphoreGive(gps_mutex);

    return fix;
}

int cap_gps_get_satellites(void)
{
    if (!gps_running || !gps_mutex) return -1;

    xSemaphoreTake(gps_mutex, portMAX_DELAY);
    int sats = gps_data.satellites;
    xSemaphoreGive(gps_mutex);

    return sats;
}
