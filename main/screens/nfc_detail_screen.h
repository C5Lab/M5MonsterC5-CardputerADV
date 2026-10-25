/**
 * @file nfc_detail_screen.h
 * @brief Saved NFC card detail and actions
 */

#ifndef NFC_DETAIL_SCREEN_H
#define NFC_DETAIL_SCREEN_H

#include "screen_manager.h"

typedef struct {
    int idx;
    char name[64];
} nfc_detail_params_t;

screen_t *nfc_detail_screen_create(void *params);

#endif
