/**
 * @file html_select_screen.h
 * @brief HTML portal selection screen for Evil Twin attack
 */

#ifndef HTML_SELECT_SCREEN_H
#define HTML_SELECT_SCREEN_H

#include "screen_manager.h"
#include "uart_handler.h"

// Parameters for creating the HTML select screen
typedef struct {
    wifi_network_t *networks;  // Networks to pass to evil_twin_screen (ownership transferred)
    int network_count;         // Number of networks
} html_select_screen_params_t;

/**
 * @brief Create the HTML portal selection screen
 * @param params Pointer to html_select_screen_params_t
 * @return Created screen or NULL on failure
 */
screen_t* html_select_screen_create(void *params);

#endif // HTML_SELECT_SCREEN_H




