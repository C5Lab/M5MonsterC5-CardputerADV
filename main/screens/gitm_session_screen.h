/**
 * @file gitm_session_screen.h
 * @brief Running GITM / Rogue GITM session (status poll + stop)
 */

#ifndef GITM_SESSION_SCREEN_H
#define GITM_SESSION_SCREEN_H

#include "screen_manager.h"

typedef enum {
    GITM_MODE_CLEAN = 0,
    GITM_MODE_ROGUE = 1,
} gitm_mode_t;

typedef struct {
    gitm_mode_t mode;
    char ssid[33];
    char password[64];
    int victim_index;  /* firmware 1-based index; Rogue only */
} gitm_session_params_t;

/**
 * @brief Create the GITM session screen
 * @param params gitm_session_params_t* (takes ownership)
 * @return Screen instance
 */
screen_t *gitm_session_screen_create(void *params);

#endif /* GITM_SESSION_SCREEN_H */
