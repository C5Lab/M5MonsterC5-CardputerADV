/**
 * @file sniffer_probes_screen.h
 * @brief Sniffer probes screen showing probe requests
 */

#ifndef SNIFFER_PROBES_SCREEN_H
#define SNIFFER_PROBES_SCREEN_H

#include "screen_manager.h"

/**
 * @brief Create the sniffer probes screen
 * @param params Not used, can be NULL
 * @return Created screen or NULL on failure
 */
screen_t* sniffer_probes_screen_create(void *params);

#endif // SNIFFER_PROBES_SCREEN_H



