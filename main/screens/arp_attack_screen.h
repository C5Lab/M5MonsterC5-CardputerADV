/**
 * @file arp_attack_screen.h
 * @brief ARP attack screen (shows attack in progress)
 */

#ifndef ARP_ATTACK_SCREEN_H
#define ARP_ATTACK_SCREEN_H

#include "screen_manager.h"

// Parameters for ARP attack screen
typedef struct {
    char ip[16];
    char mac[18];
    char vendor[32];
} arp_attack_params_t;

/**
 * @brief Create the ARP attack screen
 * @param params arp_attack_params_t pointer (takes ownership)
 * @return Screen instance
 */
screen_t* arp_attack_screen_create(void *params);

#endif // ARP_ATTACK_SCREEN_H
