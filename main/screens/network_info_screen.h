/**
 * @file network_info_screen.h
 * @brief Network information detail screen
 */

#ifndef NETWORK_INFO_SCREEN_H
#define NETWORK_INFO_SCREEN_H

#include "screen_manager.h"
#include "uart_handler.h"

// Parameters for creating the network info screen
typedef struct {
    wifi_network_t *network;  // Pointer to network to display (not owned)
} network_info_params_t;

/**
 * @brief Create the network info screen
 * @param params Pointer to network_info_params_t
 * @return Created screen or NULL on failure
 */
screen_t* network_info_screen_create(void *params);

#endif // NETWORK_INFO_SCREEN_H


