#include "cgw_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Field helpers
//
// Section 8.5 requires key=value fields to be accepted in any order, unknown
// fields to be ignored, and the last duplicate to win. All lookups therefore
// scan the line for a key that starts either at the beginning of the field
// area or right after a space, so "drops=" never matches inside
// "rate_queue_drops=".
// ---------------------------------------------------------------------------

// Locate the value of `key` within `line`. `key` must include the '='.
static const char *field(const char *line, const char *key)
{
    size_t klen = strlen(key);
    const char *best = NULL;
    for (const char *p = strstr(line, key); p; p = strstr(p + 1, key)) {
        if (p != line && p[-1] != ' ') continue;  // not at a field boundary
        best = p + klen;                          // last duplicate wins
    }
    return best;
}

static bool field_u32(const char *line, const char *key, uint32_t *out)
{
    const char *v = field(line, key);
    if (!v || *v < '0' || *v > '9') return false;
    *out = (uint32_t)strtoul(v, NULL, 10);
    return true;
}

static bool field_u64(const char *line, const char *key, uint64_t *out)
{
    const char *v = field(line, key);
    if (!v || *v < '0' || *v > '9') return false;
    *out = (uint64_t)strtoull(v, NULL, 10);
    return true;
}

static bool field_uint(const char *line, const char *key, unsigned *out)
{
    uint32_t tmp;
    if (!field_u32(line, key, &tmp)) return false;
    *out = (unsigned)tmp;
    return true;
}

// Copy a whitespace-delimited value. Used for tokens the contract guarantees
// contain no spaces (addresses, the PCAP path, enum-like values).
static bool field_str(const char *line, const char *key, char *out, size_t out_sz)
{
    const char *v = field(line, key);
    if (!v || out_sz == 0) return false;
    size_t n = 0;
    while (v[n] && v[n] != ' ' && v[n] != '\t' && n < out_sz - 1) {
        out[n] = v[n];
        n++;
    }
    out[n] = '\0';
    return n > 0;
}

// "on"/"off" and "active"/"inactive" style flags.
static bool field_flag(const char *line, const char *key, const char *true_word, bool *out)
{
    char buf[16];
    if (!field_str(line, key, buf, sizeof(buf))) return false;
    *out = (strcmp(buf, true_word) == 0);
    return true;
}

// Copy the text between `from` and `to`. Used for the SSID line, where the
// values may contain spaces and cannot be tokenized on whitespace.
static void copy_between(const char *from, const char *to, char *out, size_t out_sz)
{
    if (out_sz == 0) return;
    size_t n = 0;
    while (from + n < to && from[n] && n < out_sz - 1) {
        out[n] = from[n];
        n++;
    }
    out[n] = '\0';
}

// Skip leading whitespace, then require `prefix` at the start of the line.
static const char *after_prefix(const char *line, const char *prefix)
{
    while (*line == ' ' || *line == '\t') line++;
    size_t plen = strlen(prefix);
    if (strncmp(line, prefix, plen) != 0) return NULL;
    return line + plen;
}

// Drop the trailing CR/LF and any trailing blanks an implementation might add.
static void rstrip(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n' ||
                     s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void cgw_snapshot_reset(cgw_snapshot_t *s)
{
    if (s) memset(s, 0, sizeof(*s));
}

// The SSID line packs three space-capable values, so it cannot be tokenized.
// Section 8.5 mandates the anchored split used here.
static void parse_ssid_line(const char *body, cgw_snapshot_t *s)
{
    // `body` already starts at "ssid=" (checked by the caller); anchoring on
    // the leading key avoids matching the "ssid=" inside "upstream_ssid=".
    const char *ssid_at = body + strlen("ssid=");

    const char *sec_at = strstr(ssid_at, " security=");
    if (!sec_at) {
        copy_between(ssid_at, ssid_at + strlen(ssid_at), s->ssid, sizeof(s->ssid));
        return;
    }
    copy_between(ssid_at, sec_at, s->ssid, sizeof(s->ssid));

    const char *sec_val = sec_at + strlen(" security=");
    const char *up_at = strstr(sec_val, " upstream_ssid=");
    if (!up_at) {
        copy_between(sec_val, sec_val + strlen(sec_val), s->security, sizeof(s->security));
        return;
    }
    copy_between(sec_val, up_at, s->security, sizeof(s->security));

    const char *up_val = up_at + strlen(" upstream_ssid=");
    copy_between(up_val, up_val + strlen(up_val), s->upstream_ssid,
                 sizeof(s->upstream_ssid));
}

// Upsert by MAC so a repeated row updates the existing entry (section 8.5).
static void upsert_client(const char *body, cgw_snapshot_t *s)
{
    cgw_client_t c;
    memset(&c, 0, sizeof(c));
    if (!field_str(body, "mac=", c.mac, sizeof(c.mac))) return;
    field_str(body, "ip=", c.ip, sizeof(c.ip));

    for (int i = 0; i < s->client_count; i++) {
        if (strcmp(s->clients[i].mac, c.mac) == 0) {
            s->clients[i] = c;
            return;
        }
    }
    if (s->client_count >= CGW_MAX_CLIENTS) {
        s->clients_truncated = true;
        return;
    }
    s->clients[s->client_count++] = c;
}

bool cgw_parse_line(const char *raw, cgw_snapshot_t *s)
{
    if (!raw || !s) return false;

    char line[320];
    snprintf(line, sizeof(line), "%s", raw);
    rstrip(line);

    const char *body = after_prefix(line, "[CGW_CLIENT] ");
    if (body) {
        upsert_client(body, s);
        return false;
    }

    body = after_prefix(line, "[CGW] ");
    if (!body) return false;   // ESP-IDF log or human text: ignore (section 8.1)

    if (strcmp(body, "END") == 0) {
        s->complete = true;
        return true;
    }

    if (strncmp(body, "status ", 7) == 0) {
        unsigned v = 0;
        if (field_uint(body, "active=", &v)) s->active = (v != 0);
        if (field_uint(body, "upstream=", &v)) s->upstream = (v != 0);
        if (field_uint(body, "napt=", &v)) s->napt = (v != 0);
        field_uint(body, "clients=", &s->reported_clients);
        field_uint(body, "channel=", &s->channel);
        return false;
    }

    if (strncmp(body, "ssid=", 5) == 0) {
        parse_ssid_line(body, s);
        return false;
    }

    if (strncmp(body, "ap_ip=", 6) == 0) {
        field_str(body, "ap_ip=", s->ap_ip, sizeof(s->ap_ip));
        field_str(body, "sta_ip=", s->sta_ip, sizeof(s->sta_ip));
        field_str(body, "dns=", s->dns, sizeof(s->dns));
        field_flag(body, "dns_proxy=", "on", &s->dns_proxy);
        field_str(body, "upstream_dns=", s->upstream_dns, sizeof(s->upstream_dns));
        return false;
    }

    if (strncmp(body, "capture=", 8) == 0) {
        field_flag(body, "capture=", "active", &s->capture_active);
        field_str(body, "file=", s->file, sizeof(s->file));
        // Prefer `packets`; `frames` is only a compatibility alias (section 8.9).
        if (!field_u32(body, "packets=", &s->packets)) {
            field_u32(body, "frames=", &s->packets);
        }
        field_u32(body, "drops=", &s->drops);
        field_u64(body, "file_bytes=", &s->file_bytes);
        return false;
    }

    if (strncmp(body, "recorder ", 9) == 0) {
        field_u32(body, "drop_alloc=", &s->drop_alloc);
        field_u32(body, "drop_queue=", &s->drop_queue);
        field_u32(body, "drop_write=", &s->drop_write);
        field_u32(body, "queue_depth=", &s->queue_depth);
        field_u32(body, "queue_capacity=", &s->queue_capacity);
        field_u32(body, "queue_high_water=", &s->queue_high_water);
        return false;
    }

    if (strncmp(body, "rate_limit_kbps=", 16) == 0) {
        field_u32(body, "rate_limit_kbps=", &s->rate_limit_kbps);
        field_u32(body, "rate_effective_kbps=", &s->rate_effective_kbps);
        field_flag(body, "adaptive=", "on", &s->adaptive);
        field_u32(body, "throttle_events=", &s->throttle_events);
        field_u32(body, "pause_events=", &s->pause_events);
        field_u32(body, "rate_queue_depth=", &s->rate_queue_depth);
        field_u32(body, "rate_queue_capacity=", &s->rate_queue_capacity);
        field_u32(body, "rate_queue_drops=", &s->rate_queue_drops);
        field_u32(body, "rate_queue_high_water=", &s->rate_queue_high_water);
        return false;
    }

    if (strncmp(body, "pcap_scope=", 11) == 0) {
        field_str(body, "pcap_scope=", s->pcap_scope, sizeof(s->pcap_scope));
        field_u32(body, "filter_drops=", &s->filter_drops);
        return false;
    }

    if (strncmp(body, "client_isolation=", 17) == 0) {
        field_str(body, "client_isolation=", s->client_isolation,
                  sizeof(s->client_isolation));
        return false;
    }

    // Unknown [CGW] line type: ignored on purpose so new firmware fields do not
    // break this parser (section 8.9).
    return false;
}

bool cgw_parse_final_line(const char *raw, cgw_final_t *out)
{
    if (!raw || !out) return false;

    char line[320];
    snprintf(line, sizeof(line), "%s", raw);
    rstrip(line);

    const char *body = after_prefix(line, "[PCAP_FINAL] ");
    if (body) {
        memset(out, 0, sizeof(*out));
        field_str(body, "file=", out->file, sizeof(out->file));
        field_u32(body, "frames=", &out->frames);
        field_u32(body, "drops=", &out->drops);
        field_u32(body, "drop_alloc=", &out->drop_alloc);
        field_u32(body, "drop_queue=", &out->drop_queue);
        field_u32(body, "drop_write=", &out->drop_write);
        field_u32(body, "rate_queue_drops=", &out->rate_queue_drops);
        field_u32(body, "throttle_events=", &out->throttle_events);
        field_u32(body, "pause_events=", &out->pause_events);
        out->valid = true;
        return true;
    }

    // Legacy finalization marker. Section 8.7 warns that ESP-IDF log prefixes
    // can precede it, so this one is matched as a substring by design.
    const char *saved = strstr(line, "PCAP saved:");
    if (saved) {
        const char *path = strstr(saved, "/sdcard/");
        if (!path) return false;
        memset(out, 0, sizeof(*out));
        size_t n = 0;
        while (path[n] && path[n] != ' ' && n < sizeof(out->file) - 1) {
            out->file[n] = path[n];
            n++;
        }
        out->file[n] = '\0';
        out->valid = true;
        return true;
    }

    return false;
}

bool cgw_recorder_degraded(const cgw_snapshot_t *s)
{
    if (!s) return false;
    return s->drops > 0 || s->drop_alloc > 0 || s->drop_queue > 0 || s->drop_write > 0;
}

bool cgw_shaper_degraded(const cgw_snapshot_t *s)
{
    return s ? (s->rate_queue_drops > 0) : false;
}

const char *cgw_file_basename(const cgw_snapshot_t *s)
{
    if (!s || s->file[0] == '\0') return "";
    const char *slash = strrchr(s->file, '/');
    return slash ? slash + 1 : s->file;
}
