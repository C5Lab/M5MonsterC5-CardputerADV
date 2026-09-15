/**
 * @file subghz_ext_client.h
 * @brief UART client for Monster subghz_ext_* (decode/encode/save, no radio).
 */

#ifndef SUBGHZ_EXT_CLIENT_H
#define SUBGHZ_EXT_CLIENT_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define SUBGHZ_EXT_MAX_TIMINGS  2560
#define SUBGHZ_EXT_TOKEN_LEN    40

typedef enum {
    SUBGHZ_EXT_OK = 0,
    SUBGHZ_EXT_ERR_TIMEOUT,
    SUBGHZ_EXT_ERR_LICENSE,
    SUBGHZ_EXT_ERR_NO_DECODE,
    SUBGHZ_EXT_ERR_SAVE,
    SUBGHZ_EXT_ERR_ENCODE,
    SUBGHZ_EXT_ERR_INFO,
} subghz_ext_status_t;

typedef struct {
    bool ok;
    bool rolling;
    char type[32];
    char proto[32];
    char id[32];
    char serial[32];
    char mf[32];
    char learn[24];
    int bits;
    int te;
    int btn;
    int cnt;
    uint32_t seed;
    float freq;
} subghz_ext_decode_t;

typedef struct {
    char type[32];
    char serial[32];
    char id[32];
    char mf[32];
    char learn[24];
    char name[40];
    int bits;
    int te;
    int btn;
    int cnt;
    int edges;
    float freq;
    bool is_raw;
    bool rolling;
} subghz_ext_info_t;

subghz_ext_status_t subghz_ext_reset(void);

subghz_ext_status_t subghz_ext_decode(const int32_t *timings, int count,
                                      float freq_mhz, subghz_ext_decode_t *out);

subghz_ext_status_t subghz_ext_save(int *mem_idx_out);

/** Encode protocol fields. timings_out must hold SUBGHZ_EXT_MAX_TIMINGS. */
subghz_ext_status_t subghz_ext_encode(const char *cmd, int32_t *timings_out, int *count_out);

/** `subghz_info N sd`. Optional raw_out collects [SUBGHZ_INFO_RAW] timings. */
subghz_ext_status_t subghz_ext_info_sd(int idx, subghz_ext_info_t *out,
                                       int32_t *raw_out, int raw_cap, int *raw_count);

/**
 * `subghz_ext_tx sd <idx|name>`. Monster encodes cnt+1, persists, returns
 * timings. Prefer name when non-empty (SD index shifts after delete).
 * timings_out must hold SUBGHZ_EXT_MAX_TIMINGS.
 */
subghz_ext_status_t subghz_ext_tx_sd(int idx, const char *name,
                                     int32_t *timings_out, int *count_out,
                                     float *freq_out, int *reps_out);

#endif /* SUBGHZ_EXT_CLIENT_H */
