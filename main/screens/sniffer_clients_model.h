#ifndef SNIFFER_CLIENTS_MODEL_H
#define SNIFFER_CLIENTS_MODEL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define SC_MAX_APS 100
#define SC_MAX_CLIENTS 5000

typedef struct {
    uint8_t bssid[6];
    uint8_t channel;
    uint8_t ssid_len;
    uint8_t ssid[32];
} sc_ap_t;
typedef struct { uint8_t mac[6]; uint8_t ap; } sc_client_t;
typedef struct {
    sc_ap_t *aps;
    sc_client_t *clients;
    unsigned ap_count, client_count, expected_aps, expected_clients;
    bool begun, complete, failed;
    char error[48];
} sc_model_t;
typedef enum { SC_IGNORED, SC_UPDATED, SC_STARTED, SC_REFUSED } sc_event_t;
void sc_model_free(sc_model_t *model);
sc_event_t sc_model_line(sc_model_t *model, const char *line,
                         const uint8_t pending_bssid[6], const uint8_t pending_station[6]);
bool sc_parse_mac(const char *text, uint8_t mac[6]);
void sc_mac_text(char out[18], const uint8_t mac[6]);
void sc_ap_label(const sc_ap_t *ap, char *out, size_t size);
#endif
