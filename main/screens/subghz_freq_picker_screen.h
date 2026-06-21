/**
 * @file subghz_freq_picker_screen.h
 * @brief Frequency picker (preset list + Custom via text input) for Sub-GHz screens
 */

#ifndef SUBGHZ_FREQ_PICKER_SCREEN_H
#define SUBGHZ_FREQ_PICKER_SCREEN_H

#include "screen_manager.h"

typedef void (*subghz_freq_picker_cb_t)(float freq_mhz, void *user_data);

typedef struct {
    float initial_freq;
    subghz_freq_picker_cb_t on_pick;
    void *user_data;
} subghz_freq_picker_params_t;

/**
 * @brief Create the SubGHz frequency picker screen.
 * Takes ownership of params (frees on create).
 */
screen_t* subghz_freq_picker_screen_create(void *params);

#endif
