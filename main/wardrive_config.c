/**
 * @file wardrive_config.c
 * @brief Wardrive 2.0 config + blacklist parsing (see wardrive_config.h).
 */

#include "wardrive_config.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

void wardrive_config_set_defaults(wardrive_config_t *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->bands = WARDRIVE_BAND_WIFI24 | WARDRIVE_BAND_WIFI5 | WARDRIVE_BAND_BLE;
    cfg->channel_mode = WARDRIVE_CH_POPULAR;
    cfg->custom_channels[0] = '\0';
    cfg->wifi_rssi_delta = 5;
    cfg->ble_rssi_delta = 5;
    cfg->mem_cap = 1000;
    cfg->startup_cooldown = 0;
    cfg->antisurv_sensitivity = WARDRIVE_ANTISURV_MED;
    cfg->loaded = false;
}

// Parse an integer that follows the first '=' on the line.
static bool wdcfg_int_after_eq(const char *p, int *out)
{
    const char *eq = strchr(p, '=');
    if (!eq) return false;
    return sscanf(eq + 1, "%d", out) == 1;
}

// Extract the colon-separated channel list that follows the "custom" token.
static void extract_channel_list(const char *p, char *out, size_t len)
{
    out[0] = '\0';
    const char *c = strstr(p, "custom");
    if (!c) return;
    c += 6;
    while (*c && !isdigit((unsigned char)*c)) c++;
    size_t i = 0;
    while (*c && i < len - 1 && (isdigit((unsigned char)*c) || *c == ':')) {
        out[i++] = *c++;
    }
    out[i] = '\0';
}

bool wardrive_config_parse_line(wardrive_config_t *cfg, const char *line)
{
    if (!cfg || !line) return false;

    const char *p = strstr(line, "[WDCFG]");
    if (!p) return false;               // not a config line; ignore
    p += 7;                             // skip "[WDCFG]"
    while (*p == ' ') p++;

    if (strncmp(p, "END", 3) == 0) {
        cfg->loaded = true;
        return true;
    }

    if (strstr(p, "bands")) {
        cfg->bands = 0;
        if (strstr(p, "wifi24")) cfg->bands |= WARDRIVE_BAND_WIFI24;
        if (strstr(p, "wifi5"))  cfg->bands |= WARDRIVE_BAND_WIFI5;
        if (strstr(p, "ble"))    cfg->bands |= WARDRIVE_BAND_BLE;
    } else if (strstr(p, "channels")) {
        if (strstr(p, "custom")) {
            cfg->channel_mode = WARDRIVE_CH_CUSTOM;
            extract_channel_list(p, cfg->custom_channels, sizeof(cfg->custom_channels));
        } else if (strstr(p, "popular")) {
            cfg->channel_mode = WARDRIVE_CH_POPULAR;
        } else if (strstr(p, "all")) {
            cfg->channel_mode = WARDRIVE_CH_ALL;
        }
    } else if (strstr(p, "custom")) {
        // Standalone "custom=1:6:11:36" line (the list, sent separately from
        // the channels= mode line). Empty when the mode is not custom.
        const char *eq = strchr(p, '=');
        if (eq) {
            eq++;
            size_t i = 0;
            while (*eq && i < sizeof(cfg->custom_channels) - 1 &&
                   (isdigit((unsigned char)*eq) || *eq == ':')) {
                cfg->custom_channels[i++] = *eq++;
            }
            cfg->custom_channels[i] = '\0';
        }
    } else if (strstr(p, "antisurv")) {
        if (strstr(p, "high"))     cfg->antisurv_sensitivity = WARDRIVE_ANTISURV_HIGH;
        else if (strstr(p, "med")) cfg->antisurv_sensitivity = WARDRIVE_ANTISURV_MED;
        else if (strstr(p, "low")) cfg->antisurv_sensitivity = WARDRIVE_ANTISURV_LOW;
    } else if (strstr(p, "rssi")) {
        int v;
        if (wdcfg_int_after_eq(p, &v)) {
            if (strstr(p, "ble")) cfg->ble_rssi_delta = v;
            else                  cfg->wifi_rssi_delta = v;
        }
    } else if (strstr(p, "mem_cap") || strstr(p, "memcap")) {
        int v;
        if (wdcfg_int_after_eq(p, &v)) cfg->mem_cap = v;
    } else if (strstr(p, "cooldown")) {
        int v;
        if (wdcfg_int_after_eq(p, &v)) cfg->startup_cooldown = v;
    }

    return false;
}

// ---- Blacklist ------------------------------------------------------------

void wardrive_blacklist_reset(wardrive_blacklist_t *bl)
{
    if (!bl) return;
    bl->count = 0;
    memset(bl->macs, 0, sizeof(bl->macs));
}

bool wardrive_blacklist_add(wardrive_blacklist_t *bl, const char *mac)
{
    if (!bl || !mac || !mac[0]) return false;
    if (bl->count >= WARDRIVE_BLACKLIST_MAX) return false;
    snprintf(bl->macs[bl->count], WARDRIVE_MAC_LEN, "%s", mac);
    bl->count++;
    return true;
}
