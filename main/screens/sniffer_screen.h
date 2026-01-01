/**
 * @file sniffer_screen.h
 * @brief Sniffer attack running screen
 */

#ifndef SNIFFER_SCREEN_H
#define SNIFFER_SCREEN_H

#include "screen_manager.h"
#include "uart_handler.h"

// Parameters for creating the sniffer screen
typedef struct {
    wifi_network_t *networks;  // Networks being sniffed (ownership transferred)
    int count;                 // Number of networks
} sniffer_screen_params_t;

/**
 * @brief Create the sniffer running screen
 * @param params Pointer to sniffer_screen_params_t
 * @return Created screen or NULL on failure
 */
screen_t* sniffer_screen_create(void *params);

#endif // SNIFFER_SCREEN_H



