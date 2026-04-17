/**
 * @file uart_handler.c
 * @brief UART handler implementation with background task and callbacks
 */

#include "uart_handler.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>

static const char *TAG = "UART";

// UART task handle
static TaskHandle_t uart_task_handle = NULL;

// Line callback
static uart_response_callback_t line_callback = NULL;
static void *line_callback_user_data = NULL;
// Monitor callback (always active)
static uart_response_callback_t monitor_callback = NULL;
static void *monitor_callback_user_data = NULL;

// Scan state
static bool is_scanning = false;
static uart_scan_complete_callback_t scan_callback = NULL;
static void *scan_callback_user_data = NULL;
static wifi_network_t networks[MAX_NETWORKS];
static int network_count = 0;
static char scan_status[64] = "Ready";
static int64_t scan_last_poll_us = 0;

// Mutex for thread safety
static SemaphoreHandle_t uart_mutex = NULL;

// WiFi client connection state
static bool wifi_connected = false;
static bool wardrive_active = false;

// Board ping detection state
static volatile bool pong_received = false;

// Line buffer
static char line_buffer[1024];
static int line_pos = 0;

/**
 * @brief Log current memory info
 */
static void log_memory_info(const char *context)
{
    ESP_LOGI(TAG, "[MEM %s] Internal: %luKB, DMA: %luKB, Total: %luKB",
             context,
             (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_DMA) / 1024),
             (unsigned long)(esp_get_free_heap_size() / 1024));
}

/**
 * @brief Parse quoted CSV fields in-place.
 */
static int parse_quoted_csv_fields(char *line, char **fields, int max_fields)
{
    if (!line || !fields || max_fields <= 0) return 0;
    int field_count = 0;
    char *p = line;

    while (*p && field_count < max_fields) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (*p != '"') break;
        p++;
        fields[field_count++] = p;
        while (*p) {
            if (*p == '"') {
                *p = '\0';
                p++;
                break;
            }
            p++;
        }
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ',') p++;
    }
    return field_count;
}

static int find_network_by_bssid(const char *bssid)
{
    if (!bssid || !bssid[0]) return -1;
    for (int i = 0; i < network_count; i++) {
        if (strcasecmp(networks[i].bssid, bssid) == 0) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief Parse a CSV network line
 * Format: "1","SSID","","BSSID","channel","security","rssi","band","vendor"
 */
static bool parse_network_line(const char *line, wifi_network_t *network)
{
    if (!line || !network || line[0] != '"') {
        return false;
    }

    char *str = strdup(line);
    if (!str) return false;

    char *fields[12] = {0};
    int field_count = parse_quoted_csv_fields(str, fields, 12);
    if (field_count >= 8) {
        network->id = atoi(fields[0]);
        strncpy(network->ssid, fields[1], MAX_SSID_LEN - 1);
        network->ssid[MAX_SSID_LEN - 1] = '\0';
        strncpy(network->bssid, fields[3], MAX_BSSID_LEN - 1);
        network->bssid[MAX_BSSID_LEN - 1] = '\0';
        network->channel = atoi(fields[4]);
        strncpy(network->security, fields[5], MAX_SECURITY_LEN - 1);
        network->security[MAX_SECURITY_LEN - 1] = '\0';
        network->rssi = atoi(fields[6]);
        strncpy(network->band, fields[7], MAX_BAND_LEN - 1);
        network->band[MAX_BAND_LEN - 1] = '\0';
        network->selected = false;
    }
    bool ok = (field_count >= 8);
    free(str);
    return ok;
}

/**
 * @brief Process a complete line from UART
 */
static void process_line(const char *line)
{
    ESP_LOGI(TAG, "RX: %s", line);

    // Call monitor callback if registered (does not interfere with line callback)
    if (monitor_callback) {
        monitor_callback(line, monitor_callback_user_data);
    }

    // Call line callback if registered
    if (line_callback) {
        line_callback(line, line_callback_user_data);
    }

    // Handle scan mode
    if (is_scanning) {
        // Check for scan completion
        if (strstr(line, "Scan results printed") != NULL) {
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
                int existing = find_network_by_bssid(network.bssid);
                if (existing >= 0) {
                    networks[existing] = network;
                } else if (network_count < MAX_NETWORKS) {
                    networks[network_count++] = network;
                }
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

        if (is_scanning) {
            int64_t now = esp_timer_get_time();
            if (now - scan_last_poll_us >= 1000000) {
                scan_last_poll_us = now;
                uart_send_command("show_scan_results");
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
    log_memory_info("TX Command");
    
    int len = strlen(cmd);
    int written = uart_write_bytes(UART_PORT_NUM, cmd, len);
    
    // Commands must end with CRLF.
    if (len < 2 || cmd[len - 2] != '\r' || cmd[len - 1] != '\n') {
        uart_write_bytes(UART_PORT_NUM, "\r\n", 2);
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

void uart_register_monitor_callback(uart_response_callback_t callback, void *user_data)
{
    xSemaphoreTake(uart_mutex, portMAX_DELAY);
    monitor_callback = callback;
    monitor_callback_user_data = user_data;
    xSemaphoreGive(uart_mutex);
}

void uart_clear_line_callback(void)
{
    xSemaphoreTake(uart_mutex, portMAX_DELAY);
    line_callback = NULL;
    line_callback_user_data = NULL;
    xSemaphoreGive(uart_mutex);
}

void uart_clear_monitor_callback(void)
{
    xSemaphoreTake(uart_mutex, portMAX_DELAY);
    monitor_callback = NULL;
    monitor_callback_user_data = NULL;
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
    scan_last_poll_us = esp_timer_get_time();
    snprintf(scan_status, sizeof(scan_status), "Starting scan...");
    
    xSemaphoreGive(uart_mutex);

    // Send scan command
    uart_flush_rx();
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

void uart_flush_rx(void)
{
    // Non-blocking drain of UART RX buffer to avoid UI/task stalls.
    size_t buffered = 0;
    if (uart_get_buffered_data_len(UART_PORT_NUM, &buffered) == ESP_OK && buffered > 0) {
        uint8_t tmp[64];
        while (buffered > 0) {
            int to_read = (buffered > sizeof(tmp)) ? (int)sizeof(tmp) : (int)buffered;
            int rd = uart_read_bytes(UART_PORT_NUM, tmp, to_read, 0);
            if (rd <= 0) break;
            buffered -= (size_t)rd;
        }
    }
    line_pos = 0;
    memset(line_buffer, 0, sizeof(line_buffer));
}

bool uart_is_wardrive_active(void)
{
    return wardrive_active;
}

void uart_set_wardrive_active(bool active)
{
    wardrive_active = active;
}

/**
 * @brief Callback to detect pong response
 */
static void ping_response_callback(const char *line, void *user_data)
{
    (void)user_data;
    if (strcmp(line, "pong") == 0) {
        pong_received = true;
        ESP_LOGI(TAG, "Pong received - board detected");
    }
}

bool uart_check_board_ping(int timeout_ms)
{
    ESP_LOGI(TAG, "Checking board connection (ping)...");
    
    // Reset pong flag
    pong_received = false;
    
    // Register our callback temporarily
    uart_response_callback_t old_callback = line_callback;
    void *old_user_data = line_callback_user_data;
    
    uart_register_line_callback(ping_response_callback, NULL);
    
    // Send ping command
    uart_send_command("ping");
    
    // Wait for pong with timeout
    int elapsed = 0;
    const int check_interval = 10;  // Check every 10ms
    
    while (elapsed < timeout_ms && !pong_received) {
        vTaskDelay(pdMS_TO_TICKS(check_interval));
        elapsed += check_interval;
    }
    
    // Restore previous callback
    uart_register_line_callback(old_callback, old_user_data);
    
    if (pong_received) {
        ESP_LOGI(TAG, "Board detected successfully");
    } else {
        ESP_LOGW(TAG, "Board not detected (timeout after %dms)", timeout_ms);
    }
    
    return pong_received;
}
