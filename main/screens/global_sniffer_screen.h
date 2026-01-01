/**
 * @file global_sniffer_screen.h
 * @brief Global sniffer screen header (no network selection required)
 */

#ifndef GLOBAL_SNIFFER_SCREEN_H
#define GLOBAL_SNIFFER_SCREEN_H

#include "screen_manager.h"

/**
 * @brief Create the global sniffer screen
 * @param params Unused (pass NULL)
 * @return Pointer to the created screen, or NULL on failure
 */
screen_t* global_sniffer_screen_create(void *params);

#endif // GLOBAL_SNIFFER_SCREEN_H


