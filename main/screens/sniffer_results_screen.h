/**
 * @file sniffer_results_screen.h
 * @brief Sniffer results screen showing SSIDs and clients
 */

#ifndef SNIFFER_RESULTS_SCREEN_H
#define SNIFFER_RESULTS_SCREEN_H

#include "screen_manager.h"

/**
 * @brief Create the sniffer results screen
 * @param params Not used, can be NULL
 * @return Created screen or NULL on failure
 */
screen_t* sniffer_results_screen_create(void *params);

#endif // SNIFFER_RESULTS_SCREEN_H



