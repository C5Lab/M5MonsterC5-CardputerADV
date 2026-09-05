// JanOS Capture Gateway (GITM) status parser.
//
// Implements the controller-side contract from
// projectZero/ESP32C5/docs/janos-capture-gateway.md section 8.4/8.5:
// line-oriented parsing of the `[CGW]` / `[CGW_CLIENT]` block that
// `capture_gateway status` emits, plus the `[PCAP_FINAL]` stop summary.
//
// Deliberately free of LVGL and transport dependencies so it can be driven
// from a UART task and unit-tested on the host.

#ifndef CGW_PARSER_H
#define CGW_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// JanOS Phase 1 caps the SoftAP well below this; the extra room means a burst
// of joins is truncated in the list rather than corrupting the snapshot.
#define CGW_MAX_CLIENTS 12

typedef struct {
    char mac[18];
    char ip[16];
} cgw_client_t;

// One complete status response. Fields absent from an idle block keep their
// zero value; per section 8.3 "absent" means not applicable, not zero, so read
// them only when `active` is true.
typedef struct {
    bool complete;          // a full block terminated by "[CGW] END" was seen
    bool active;
    bool upstream;
    bool napt;
    bool capture_active;
    bool dns_proxy;
    bool adaptive;

    unsigned channel;
    unsigned reported_clients;   // "clients=" count as reported on the status line

    char ssid[33];
    char security[8];            // "open" | "wpa2"
    char upstream_ssid[33];
    char ap_ip[16];
    char sta_ip[16];
    char dns[16];
    char upstream_dns[16];
    char file[128];              // full remote path on the JanOS SD card
    char client_isolation[8];    // capability value, currently "off"
    char pcap_scope[24];

    uint32_t packets;
    uint32_t drops;              // recorder total
    uint32_t drop_alloc;
    uint32_t drop_queue;
    uint32_t drop_write;
    uint32_t queue_depth;
    uint32_t queue_capacity;
    uint32_t queue_high_water;
    uint32_t filter_drops;

    uint32_t rate_limit_kbps;
    uint32_t rate_effective_kbps;
    uint32_t throttle_events;
    uint32_t pause_events;
    uint32_t rate_queue_depth;
    uint32_t rate_queue_capacity;
    uint32_t rate_queue_drops;   // shaper, NOT recorder loss
    uint32_t rate_queue_high_water;

    uint64_t file_bytes;         // u64: overflows 32 bits on long captures

    cgw_client_t clients[CGW_MAX_CLIENTS];
    int client_count;            // rows actually stored (may be < reported_clients)
    bool clients_truncated;
} cgw_snapshot_t;

// The `[PCAP_FINAL]` line emitted by universal `stop` after the limiter and
// writer have drained.
typedef struct {
    bool valid;
    char file[128];
    uint32_t frames;
    uint32_t drops;
    uint32_t drop_alloc;
    uint32_t drop_queue;
    uint32_t drop_write;
    uint32_t rate_queue_drops;
    uint32_t throttle_events;
    uint32_t pause_events;
} cgw_final_t;

// Reset a snapshot before feeding it the first line of a new response.
void cgw_snapshot_reset(cgw_snapshot_t *s);

// Feed one received line (without its terminator). Unrelated log output is
// ignored. Returns true when the line was the exact "[CGW] END" terminator,
// i.e. `s` now holds a complete snapshot ready to publish.
bool cgw_parse_line(const char *line, cgw_snapshot_t *s);

// Parse a "[PCAP_FINAL] ..." line. Returns true when `out` was filled.
// Also accepts the legacy "PCAP saved: /sdcard/...pcap" marker, which yields
// only the file path (frames/drops stay zero and `valid` is still true).
bool cgw_parse_final_line(const char *line, cgw_final_t *out);

// True when the recorder lost packets, i.e. the capture is missing evidence.
bool cgw_recorder_degraded(const cgw_snapshot_t *s);

// True when the adaptive shaper overflowed. Separate from recorder loss:
// forwarded traffic was dropped, but what reached the recorder is intact.
bool cgw_shaper_degraded(const cgw_snapshot_t *s);

// Basename of `file`, or "" when unset.
const char *cgw_file_basename(const cgw_snapshot_t *s);

#ifdef __cplusplus
}
#endif

#endif  // CGW_PARSER_H
