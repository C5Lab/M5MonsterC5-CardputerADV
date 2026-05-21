#ifndef SUBGHZ_PARSER_H
#define SUBGHZ_PARSER_H

#include <stdbool.h>

typedef enum {
    SUBGHZ_SIGNAL_KIND_NONE = 0,
    SUBGHZ_SIGNAL_KIND_RX,
    SUBGHZ_SIGNAL_KIND_RX_DUP,
    SUBGHZ_SIGNAL_KIND_RAW,
    SUBGHZ_SIGNAL_KIND_LIST,
} subghz_signal_kind_t;

typedef struct {
    subghz_signal_kind_t kind;
    int idx;
    char type[32];
    float freq;
    int bits;
    char serial[32];
    int btn;
    int cnt;
    char mf[32];
    char proto[32];
    char learn[24];
    char name[40];
    int te;
    int edges;
    bool is_raw;
    bool is_duplicate;
} subghz_signal_info_t;

const char *subghz_normalize_line(const char *line);
bool subghz_parse_rssi_line(const char *line, int *out_rssi);
bool subghz_parse_signal_line(const char *line, subghz_signal_info_t *out);

#endif