#pragma once
#include "collector_core.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum { COL_GET_CONFIG, COL_POST_HEARTBEAT, COL_POST_EVENTS, COL_SET_CREDENTIALS, COL_CLEAR_CREDENTIALS } col_job_kind;
typedef struct {
    col_job_kind kind;
    /* 配置任务与 HTTP 任务串行，凭据和请求正文不会同时使用。 */
    union { char body[COL_BODY]; col_credentials credentials; };
    char response[COL_RESPONSE], batch_id[64];
    int status;
    esp_err_t transport;
    const char *diagnostic;
} col_job;
typedef struct {
    char node[81], state[40], error[81], ip[24], last_upload[25];
    char device_name[129], device_suffix[9];
    int device_rssi, device_age, device_index, device_total;
    unsigned pending, addresses, events, dropped, raw_dropped;
    uint32_t heap_free, heap_min, heap_largest, stack_min;
    int64_t revision;
    int battery, page, menu;
    bool wifi, clock, configured, local_pause, provisioning, clearing;
} col_view;
typedef struct { int button, event; } col_key;
typedef struct { col_credentials credentials; uint32_t generation; } col_provisioning;
int64_t col_mono(void);
int64_t col_utc(void);
bool col_clock_ready(void);
void col_wipe(void *p,size_t n);
esp_err_t col_scan_init(QueueHandle_t samples);
esp_err_t col_scan_set(bool enabled);
bool col_scan_active(void);
bool col_scan_ready(void);
unsigned col_scan_lost(void);
uint32_t col_scan_epoch(void);
esp_err_t col_scan_shutdown(void);
esp_err_t col_network_init(QueueHandle_t jobs,QueueHandle_t done);
bool col_wifi_ready(void);
uint32_t col_wifi_ip(void);
const char *col_network_diagnostic(void);
uint32_t col_network_stack(void);
uint32_t col_scan_stack(void);
esp_err_t col_usb_init(QueueHandle_t credentials);
void col_usb_open(bool open);
bool col_usb_is_open(void);
uint32_t col_usb_generation(void);
uint32_t col_usb_stack(void);
esp_err_t col_ui_init(QueueHandle_t views,QueueHandle_t keys);
void col_controller_start(void);
