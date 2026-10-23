#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define COLLECTOR_VERSION "passport-ble/0.1.0"
#define COL_ADDRS 16
#define COL_QUEUE 32
#define COL_UUIDS 4
/* 协议允许至多 32 KiB；本机 8 KiB 上限按完整事件自动拆批。 */
#define COL_BODY 8192
#define COL_RESPONSE 4096
#define COL_TTL_MS 300000
#define COL_SAFE_INT 9007199254740991LL

typedef struct {
    char address[18], name[129], uuids[COL_UUIDS][37];
    uint8_t address_type, uuid_count;
    bool has_tx, has_manufacturer;
    int8_t tx;
    uint16_t manufacturer;
} col_metadata;
typedef struct {
    col_metadata meta;
    int8_t rssi;
    int64_t mono_ms, utc_ms;
    uint32_t epoch;
} col_sample;
typedef struct {
    col_metadata meta;
    char id[64];
    int64_t utc_ms, created_ms;
    uint32_t count;
    uint16_t window_ms;
    int16_t median_x2;
    int8_t min, max;
} col_event;
typedef struct {
    col_metadata meta;
    uint32_t hist[127], count;
    int64_t utc_ms, last_ms;
    int8_t last_rssi;
    bool used;
} col_bucket;
typedef struct {
    col_bucket buckets[COL_ADDRS];
    col_event queue[COL_QUEUE];
    size_t count, pinned, cursor;
    uint64_t dropped, raw_invalid, raw_overflow, partial_windows;
    uint64_t sequence;
    char boot[33];
    /* 满表后的首个未采地址获下一窗口优先准入，最多等待 250 ms。 */
    char next_address[18];
    uint8_t next_address_type;
    int64_t window_start, last_observation, utc_offset;
    bool window_open, offset_set, admission_wait;
} col_store;
typedef struct {
    bool scan_enabled;
    int window_ms, heartbeat_sec, max_batch, strong, weak, active_sec, stale_sec, retention_days;
    int64_t revision;
} col_config;
typedef struct {
    char ssid[33], password[65], base_url[257], node_id[81], token[44], ca[4097];
} col_credentials;
typedef enum { COL_RETRY, COL_ACK, COL_AUTH, COL_IDENTITY, COL_CONFIG_ERROR,
    COL_BAD_INPUT, COL_SCHEMA, COL_REVISION, COL_SPLIT, COL_RATE,
    COL_HISTORY_FULL, COL_DEVICES_FULL } col_result;
typedef struct {
    int64_t retry_at, write_at, config_at, heartbeat_at, upload_at, capacity_at;
    unsigned failures, batch_limit;
    bool auth_blocked, protocol_blocked, capacity_blocked;
    const char *error;
} col_schedule;

bool col_id(const char *s, size_t max);
bool col_utf8(const char *s);
size_t col_clean_utf8(char *out, size_t cap, const uint8_t *in, size_t n);
bool col_time_parse(const char *s, int64_t *ms);
bool col_time_format(int64_t ms, char out[25]);
bool col_time_allowed(int64_t ms, int64_t now, bool historical);
bool col_ad(col_sample *out, const uint8_t address[6], unsigned address_type,
            int rssi, const uint8_t *ad, size_t len);
void col_store_init(col_store *s, const char *boot);
void col_window_reset(col_store *s);
void col_ingest(col_store *s, const col_sample *sample, int window_ms);
void col_window_close(col_store *s, int64_t now, int window_ms);
void col_expire(col_store *s, int64_t now);
bool col_enqueue(col_store *s, const col_event *e);
void col_confirm(col_store *s, bool accepted);
void col_discard_pinned(col_store *s);
bool col_parse_config(const char *json, const char *node, col_config *out);
bool col_parse_credentials(const char *json, col_credentials *out);
bool col_event_valid(const col_event *e, int64_t now);
bool col_batch(col_store *s, const char *node, const char *batch, int64_t now,
               size_t limit, char *out, size_t cap);
bool col_heartbeat(const char *node, int64_t now, const char *state, bool ble_ready,
                   const col_store *store, const char *error, int64_t revision,
                   char *out, size_t cap);
bool col_ack(const char *json, const char *batch, size_t n);
bool col_heartbeat_ack(const char *json, int64_t *revision);
col_result col_classify(int status, const char *json);
void col_schedule_init(col_schedule *s);
void col_schedule_result(col_schedule *s, col_result r, int64_t now, unsigned jitter);
bool col_scan_allowed(bool configured, bool local_pause, bool provisioning,
                      bool clock_ready, const col_config *c, const col_schedule *s);
