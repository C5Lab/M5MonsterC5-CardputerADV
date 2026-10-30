/**
 * @file radio_sniffer_screen.h
 * @brief Radio PCAP capture screen (start_pcap radio)
 */

#ifndef RADIO_SNIFFER_SCREEN_H
#define RADIO_SNIFFER_SCREEN_H

#include "screen_manager.h"

/**
 * @brief Create the Radio Sniffer screen
 * @param params Unused (pass NULL)
 * @return Pointer to the created screen, or NULL on failure
 */
screen_t* radio_sniffer_screen_create(void *params);

#endif // RADIO_SNIFFER_SCREEN_H
