/**
 * @file wardrive_config.h
 * @brief Wardrive 2.0 config + blacklist data models and [WDCFG] parse helpers.
 *
 * Pure C (no ESP/UI deps) so it can be unit-isolated. Mirrors the Tab5 model.
 * The board firmware is unchanged; we only build/parse text over UART.
 */

#ifndef WARDRIVE_CONFIG_H
#define WARDRIVE_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Band bitmask (set_wardrive_bands csv tokens: wifi24, wifi5, ble)
#define WARDRIVE_BAND_WIFI24 0x01
#define WARDRIVE_BAND_WIFI5  0x02
#define WARDRIVE_BAND_BLE    0x04

typedef enum {
    WARDRIVE_CH_POPULAR = 0,
    WARDRIVE_CH_ALL     = 1,
    WARDRIVE_CH_CUSTOM  = 2,
} wardrive_channel_mode_t;

typedef enum {
    WARDRIVE_ANTISURV_LOW  = 0,
    WARDRIVE_ANTISURV_MED  = 1,
    WARDRIVE_ANTISURV_HIGH = 2,
} wardrive_antisurv_sens_t;

typedef struct {
    uint8_t bands;                          // bitmask of WARDRIVE_BAND_*
    wardrive_channel_mode_t channel_mode;
    char custom_channels[96];               // colon-separated "1:6:11:36"
    int  wifi_rssi_delta;
    int  ble_rssi_delta;
    int  mem_cap;
    int  startup_cooldown;
    wardrive_antisurv_sens_t antisurv_sensitivity;
    bool loaded;                            // true after a full get_wardrive_config
} wardrive_config_t;

/**
 * @brief Populate sane defaults (used until a get_wardrive_config completes).
 */
void wardrive_config_set_defaults(wardrive_config_t *cfg);

/**
 * @brief Feed one received line into the config parser.
 *
 * Parses lines prefixed "[WDCFG]" defensively by key substring; tolerates
 * missing keys. Call once per line received after get_wardrive_config.
 *
 * @return true when the terminating "[WDCFG] END" line is seen (sets loaded).
 */
bool wardrive_config_parse_line(wardrive_config_t *cfg, const char *line);

// ---- Blacklist ------------------------------------------------------------

#define WARDRIVE_BLACKLIST_MAX 64
#define WARDRIVE_MAC_LEN       18           // 17 chars + NUL

typedef struct {
    char macs[WARDRIVE_BLACKLIST_MAX][WARDRIVE_MAC_LEN];
    int  count;
} wardrive_blacklist_t;

/**
 * @brief Empty the blacklist cache.
 */
void wardrive_blacklist_reset(wardrive_blacklist_t *bl);

/**
 * @brief Append a MAC (truncated to 17 chars) if there is room and it is valid.
 * @return true if added.
 */
bool wardrive_blacklist_add(wardrive_blacklist_t *bl, const char *mac);

#endif // WARDRIVE_CONFIG_H
