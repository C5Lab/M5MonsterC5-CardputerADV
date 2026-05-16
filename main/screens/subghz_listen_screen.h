/**
 * @file subghz_listen_screen.h
 * @brief Sub-GHz Listen (RX) screen with signal list
 */

#ifndef SUBGHZ_LISTEN_SCREEN_H
#define SUBGHZ_LISTEN_SCREEN_H

#include "screen_manager.h"
#include <stdbool.h>

typedef struct {
    float freq_mhz;
    bool  autostart;
} subghz_listen_params_t;

screen_t* subghz_listen_screen_create(void *params);

/**
 * @brief Set a pending freq + autostart consumed by the next Listen create.
 *        Used by Scanner to jump straight into Listen on a detected freq.
 */
void subghz_listen_screen_set_pending(float freq_mhz, bool autostart);

#endif
