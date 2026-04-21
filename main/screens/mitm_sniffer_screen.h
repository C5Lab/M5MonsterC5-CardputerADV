/**
 * @file mitm_sniffer_screen.h
 * @brief MITM network sniffer screen
 */

#ifndef MITM_SNIFFER_SCREEN_H
#define MITM_SNIFFER_SCREEN_H

#include "screen_manager.h"

/**
 * @brief Create the MITM sniffer screen
 * @param params Not used
 * @return Screen instance
 */
screen_t* mitm_sniffer_screen_create(void *params);

#endif // MITM_SNIFFER_SCREEN_H
