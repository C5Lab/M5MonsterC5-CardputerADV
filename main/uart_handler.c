/**
 * @file uart_handler.c
 * @brief UART handler implementation with background task and callbacks
 */

#include "uart_handler.h"
#include "settings.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "UART";

// UART task handle
static TaskHandle_t uart_task_handle = NULL;

// Line callback
static uart_response_callback_t line_callback = NULL;
static void *line_callback_user_data = NULL;

// Scan state
static bool is_scanning = false;
static uart_scan_complete_callback_t scan_callback = NULL;
static void *scan_callback_user_data = NULL;
static wifi_network_t networks[MAX_NETWORKS];
static int network_count = 0;
static char scan_status[64] = "Ready";

// Mutex for thread safety
static SemaphoreHandle_t uart_mutex = NULL;

// WiFi client connection state
static bool wifi_connected = false;

// Line buffer
static char line_buffer[1024];
static int line_pos = 0;

/**
 * @brief Parse a CSV network line
 * Format: "1","SSID","","BSSID","channel","security","rssi","band"
 */
static bool parse_network_line(const char *line, wifi_network_t *network)
{
    // Check if line starts with a quote (CSV format)
    if (line[0] != '"') {
        return false;
    }

    // Parse using simple state machine
    char *str = strdup(line);
    if (!str) return false;

    char *fields[8] = {0};
    int field_count = 0;
    char *ptr = str;
    
    while (*ptr && field_count < 8) {
        // Skip leading quote
        if (*ptr == '"') ptr++;
        
        // Find start of field
        fields[field_count] = ptr;
        
        // Find end of field (closing quote)
        while (*ptr && *ptr != '"') ptr++;
        if (*ptr == '"') {
            *ptr = '\0';
            ptr++;
        }
        
        // Skip comma
        if (*ptr == ',') ptr++;
        
        field_count++;
    }

    if (field_count >= 8) {
        network->id = atoi(fields[0]);
        strncpy(network->ssid, fields[1], MAX_SSID_LEN - 1);
        network->ssid[MAX_SSID_LEN - 1] = '\0';
        // fields[2] is empty
        strncpy(network->bssid, fields[3], MAX_BSSID_LEN - 1);
        network->bssid[MAX_BSSID_LEN - 1] = '\0';
        network->channel = atoi(fields[4]);
        strncpy(network->security, fields[5], MAX_SECURITY_LEN - 1);
        network->security[MAX_SECURITY_LEN - 1] = '\0';
        network->rssi = atoi(fields[6]);
        strncpy(network->band, fields[7], MAX_BAND_LEN - 1);
        network->band[MAX_BAND_LEN - 1] = '\0';
        network->selected = false;
        
        free(str);
        return true;
    }

    free(str);
    return false;
}

/**
 * @brief Process a complete line from UART
 */
static void process_line(const char *line)
{
    ESP_LOGD(TAG, "RX: %s", line);

    // Call line callback if registered
    if (line_callback) {
        line_callback(line, line_callback_user_data);
    }

    // Handle scan mode
    if (is_scanning) {
        // Check for scan completion
        if (strstr(line, "Scan results printed.") != NULL) {
            ESP_LOGI(TAG, "Scan complete, found %d networks", network_count);
            snprintf(scan_status, sizeof(scan_status), "Found %d networks", network_count);
            is_scanning = false;
            
            if (scan_callback) {
                scan_callback(networks, network_count, scan_callback_user_data);
                scan_callback = NULL;
                scan_callback_user_data = NULL;
            }
            return;
        }

        // Try to parse as network entry
        if (line[0] == '"' && network_count < MAX_NETWORKS) {
            wifi_network_t network = {0};
            if (parse_network_line(line, &network)) {
                networks[network_count++] = network;
                snprintf(scan_status, sizeof(scan_status), "Scanning... %d networks", network_count);
            }
        }

        // Update status based on known messages
        if (strstr(line, "Starting background WiFi scan") != NULL) {
            snprintf(scan_status, sizeof(scan_status), "Scanning...");
        } else if (strstr(line, "WiFi scan completed") != NULL) {
            snprintf(scan_status, sizeof(scan_status), "Processing results...");
        }
    }
}

/**
 * @brief UART receive task
 */
static void uart_rx_task(void *arg)
{
    uint8_t data[128];
    
    while (1) {
        int len = uart_read_bytes(UART_PORT_NUM, data, sizeof(data) - 1, pdMS_TO_TICKS(100));
        
        if (len > 0) {
            data[len] = '\0';
            
            // Process byte by byte to find complete lines
            for (int i = 0; i < len; i++) {
                char c = data[i];
                
                if (c == '\n' || c == '\r') {
                    if (line_pos > 0) {
                        line_buffer[line_pos] = '\0';
                        process_line(line_buffer);
                        line_pos = 0;
                    }
                } else if (line_pos < sizeof(line_buffer) - 1) {
                    line_buffer[line_pos++] = c;
                }
            }
        }
    }
}

esp_err_t uart_handler_init(void)
{
    int tx_pin = settings_get_uart_tx_pin();
    int rx_pin = settings_get_uart_rx_pin();
    
    ESP_LOGI(TAG, "Initializing UART handler (TX=%d, RX=%d)...", tx_pin, rx_pin);

    // Create mutex
    uart_mutex = xSemaphoreCreateMutex();
    if (!uart_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_FAIL;
    }

    // Configure UART
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_param_config(UART_PORT_NUM, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART param config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_set_pin(UART_PORT_NUM, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART set pin failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_driver_install(UART_PORT_NUM, UART_BUF_SIZE, UART_BUF_SIZE, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Create RX task
    BaseType_t task_ret = xTaskCreate(uart_rx_task, "uart_rx", 4096, NULL, 10, &uart_task_handle);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UART RX task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "UART handler initialized successfully");
    return ESP_OK;
}

esp_err_t uart_send_command(const char *cmd)
{
    if (!cmd) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(uart_mutex, portMAX_DELAY);
    
    ESP_LOGI(TAG, "TX: %s", cmd);
    
    int len = strlen(cmd);
    int written = uart_write_bytes(UART_PORT_NUM, cmd, len);
    
    // Send newline if not present
    if (len > 0 && cmd[len - 1] != '\n') {
        uart_write_bytes(UART_PORT_NUM, "\n", 1);
    }
    
    xSemaphoreGive(uart_mutex);
    
    return (written == len) ? ESP_OK : ESP_FAIL;
}

void uart_register_line_callback(uart_response_callback_t callback, void *user_data)
{
    xSemaphoreTake(uart_mutex, portMAX_DELAY);
    line_callback = callback;
    line_callback_user_data = user_data;
    xSemaphoreGive(uart_mutex);
}

void uart_clear_line_callback(void)
{
    xSemaphoreTake(uart_mutex, portMAX_DELAY);
    line_callback = NULL;
    line_callback_user_data = NULL;
    xSemaphoreGive(uart_mutex);
}

esp_err_t uart_start_wifi_scan(uart_scan_complete_callback_t callback, void *user_data)
{
    if (is_scanning) {
        ESP_LOGW(TAG, "Scan already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(uart_mutex, portMAX_DELAY);
    
    // Reset state
    network_count = 0;
    memset(networks, 0, sizeof(networks));
    is_scanning = true;
    scan_callback = callback;
    scan_callback_user_data = user_data;
    snprintf(scan_status, sizeof(scan_status), "Starting scan...");
    
    xSemaphoreGive(uart_mutex);

    // Send scan command
    return uart_send_command("scan_networks");
}

bool uart_is_scanning(void)
{
    return is_scanning;
}

const char* uart_get_scan_status(void)
{
    return scan_status;
}

bool uart_is_wifi_connected(void)
{
    return wifi_connected;
}

void uart_set_wifi_connected(bool connected)
{
    wifi_connected = connected;
}



