/**
 * @file nfc_emulate_screen.h
 * @brief NFC card emulation status screen
 */

#ifndef NFC_EMULATE_SCREEN_H
#define NFC_EMULATE_SCREEN_H

#include "screen_manager.h"
#include <stdbool.h>

typedef struct {
    int idx;
    bool have_data;
    char type[32];
} nfc_emulate_params_t;

screen_t *nfc_emulate_screen_create(void *params);

#endif
