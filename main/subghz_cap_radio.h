/**
 * @file subghz_cap_radio.h
 * @brief Cap-mode SubGHz: local CC1101 + Monster subghz_ext_*.
 */

#ifndef SUBGHZ_CAP_RADIO_H
#define SUBGHZ_CAP_RADIO_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

esp_err_t subghz_cap_radio_enable(void);
void subghz_cap_radio_disable(void);
bool subghz_cap_radio_ready(void);

esp_err_t subghz_cap_listen_start(float freq_mhz, bool raw_mode, int min_rssi_dbm);
void subghz_cap_listen_stop(void);
bool subghz_cap_listen_running(void);

esp_err_t subghz_cap_save(int local_idx);
esp_err_t subghz_cap_tx_capture(int local_idx);
esp_err_t subghz_cap_tx_sd(int sd_idx, const char *name);

esp_err_t subghz_cap_jam_start(float freq_mhz);
void subghz_cap_jam_stop(void);

esp_err_t subghz_cap_tx_tesla(void);

#endif /* SUBGHZ_CAP_RADIO_H */
