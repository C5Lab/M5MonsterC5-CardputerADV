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

/**
 * @brief Flush pending RX data from UART driver
 */
void uart_flush_rx(void);

/**
 * @brief Get wardrive activity flag
 * @return true when wardrive is running
 */
bool uart_is_wardrive_active(void);

/**
 * @brief Set wardrive activity flag
 * @param active true when wardrive is running
 */
void uart_set_wardrive_active(bool active);

/**
 * @brief Probe for a Sub-GHz module on the connected JanOS device.
 *        Sends `subghz_status` and waits for a line that starts with
 *        `[SUBGHZ_STATUS]`. Caches the result for uart_is_subghz_available().
 * @param timeout_ms Timeout in milliseconds to wait for response
 * @return true if a [SUBGHZ_STATUS] line was seen within the timeout
 */
bool uart_check_subghz_available(int timeout_ms);

/**
 * @brief Return the cached Sub-GHz availability flag set by the boot probe.
 * @return true if uart_check_subghz_available() previously succeeded
 */
bool uart_is_subghz_available(void);

#endif // UART_HANDLER_H



