/**
 * @file uart_handler.h
 * @brief UART handler for communication with external device
 */

#ifndef UART_HANDLER_H
#define UART_HANDLER_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// UART Configuration
#define UART_PORT_NUM       UART_NUM_1
#define UART_BAUD_RATE      115200
#define UART_BUF_SIZE       4096

// Maximum networks that can be stored
#define MAX_NETWORKS        64
#define MAX_SSID_LEN        33
#define MAX_BSSID_LEN       18
#define MAX_SECURITY_LEN    24
#define MAX_BAND_LEN        8

// WiFi network structure
typedef struct {
    int id;
    char ssid[MAX_SSID_LEN];
    char bssid[MAX_BSSID_LEN];
    int channel;
    char security[MAX_SECURITY_LEN];
    int rssi;
    char band[MAX_BAND_LEN];
    bool selected;
} wifi_network_t;

// Response callback type
typedef void (*uart_response_callback_t)(const char *line, void *user_data);

// Scan complete callback type
typedef void (*uart_scan_complete_callback_t)(wifi_network_t *networks, int count, void *user_data);

/**
 * @brief Initialize UART handler
 * @return ESP_OK on success
 */
esp_err_t uart_handler_init(void);

/**
 * @brief Send a command via UART
 * @param cmd Command string to send
 * @return ESP_OK on success
 */
esp_err_t uart_send_command(const char *cmd);

/**
 * @brief Register a callback for line-by-line response
 * @param callback Function to call for each line received
 * @param user_data User data to pass to callback
 */
void uart_register_line_callback(uart_response_callback_t callback, void *user_data);

/**
 * @brief Register a monitor callback that always receives lines
 * @param callback Function to call for each line received
 * @param user_data User data to pass to callback
 */
void uart_register_monitor_callback(uart_response_callback_t callback, void *user_data);

/**
 * @brief Clear registered monitor callback
 */
void uart_clear_monitor_callback(void);

/**
 * @brief Clear registered line callback
 */
void uart_clear_line_callback(void);

/**
 * @brief Start WiFi scan and register callback for results
 * @param callback Function to call when scan completes
 * @param user_data User data to pass to callback
 * @return ESP_OK on success
 */
esp_err_t uart_start_wifi_scan(uart_scan_complete_callback_t callback, void *user_data);

/**
 * @brief Check if scan is in progress
 * @return true if scanning
 */
bool uart_is_scanning(void);

/**
 * @brief Get scan progress message
 * @return Current status message
 */
const char* uart_get_scan_status(void);

/**
 * @brief Check if connected to WiFi (client mode)
 * @return true if connected
 */
bool uart_is_wifi_connected(void);

/**
 * @brief Set WiFi connection state
 * @param connected true if connected
 */
void uart_set_wifi_connected(bool connected);

/**
 * @brief Check if board is connected by sending ping and waiting for pong
 * @param timeout_ms Timeout in milliseconds to wait for response
 * @return true if pong received within timeout, false otherwise
 */
bool uart_check_board_ping(int timeout_ms);

#endif // UART_HANDLER_H



