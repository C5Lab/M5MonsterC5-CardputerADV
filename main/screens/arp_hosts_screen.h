/**
 * @file arp_hosts_screen.h
 * @brief ARP hosts list screen (from list_hosts_vendor command)
 */

#ifndef ARP_HOSTS_SCREEN_H
#define ARP_HOSTS_SCREEN_H

#include "screen_manager.h"

/**
 * @brief Create the ARP hosts list screen
 * @param params Not used
 * @return Screen instance
 */
screen_t* arp_hosts_screen_create(void *params);

#endif // ARP_HOSTS_SCREEN_H
