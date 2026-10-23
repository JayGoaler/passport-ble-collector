#include "collector_platform.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "freertos/semphr.h"
#include <stdatomic.h>

static QueueHandle_t samples;
static SemaphoreHandle_t stopped;
static TaskHandle_t host;
static atomic_bool ready, active;
static atomic_uint lost, epoch;
static uint8_t own_type;
static bool initialized, stop_requested, host_done;
static int gap(struct ble_gap_event *e,void *arg)
{
    (void)arg;
    if (e->type==BLE_GAP_EVENT_DISC && atomic_load(&active)) {
        col_sample p;
        if (col_ad(&p,e->disc.addr.val,e->disc.addr.type,e->disc.rssi,e->disc.data,e->disc.length_data)) {
            p.mono_ms=col_mono(); p.utc_ms=col_utc(); p.epoch=atomic_load(&epoch);
            if (xQueueSend(samples,&p,0)!=pdTRUE) atomic_fetch_add(&lost,1);
        } else atomic_fetch_add(&lost,1);
    } else if (e->type==BLE_GAP_EVENT_DISC_COMPLETE) atomic_store(&active,false);
    return 0;
}
static void reset(int reason)
{ (void)reason; atomic_store(&ready,false); atomic_store(&active,false); atomic_fetch_add(&epoch,1); }
static void sync_host(void)
{
    int rc=ble_hs_util_ensure_addr(0);
    if (!rc) rc=ble_hs_id_infer_auto(0,&own_type);
    atomic_store(&ready,rc==0);
}
static void run(void *unused)
{
    (void)unused; nimble_port_run(); xSemaphoreGive(stopped);
    for (;;) vTaskSuspend(NULL);
}
esp_err_t col_scan_init(QueueHandle_t q)
{
    if (initialized) return ESP_ERR_INVALID_STATE;
    samples=q; stopped=xSemaphoreCreateBinary(); if (!stopped) return ESP_ERR_NO_MEM;
    esp_err_t err=nimble_port_init();
    if (err!=ESP_OK) { vSemaphoreDelete(stopped); stopped=NULL; return err; }
    initialized=true; stop_requested=false; host_done=false;
    ble_hs_cfg.sync_cb=sync_host; ble_hs_cfg.reset_cb=reset;
    if (xTaskCreate(run,"collector_ble",NIMBLE_HS_STACK_SIZE,NULL,5,&host)!=pdPASS) {
        nimble_port_deinit(); initialized=false; vSemaphoreDelete(stopped); stopped=NULL; return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
esp_err_t col_scan_set(bool enabled)
{
    if (!enabled) {
        if (!atomic_load(&active)) return ESP_OK;
        int rc=ble_gap_disc_cancel();
        if (rc!=0 && rc!=BLE_HS_EALREADY) return ESP_FAIL;
        atomic_store(&active,false); atomic_fetch_add(&epoch,1); return ESP_OK;
    }
    if (atomic_load(&active)) return ESP_OK;
    if (!atomic_load(&ready)) return ESP_ERR_INVALID_STATE;
    struct ble_gap_disc_params p={0};
    p.passive=1; p.filter_duplicates=0; p.itvl=160; p.window=80;
    atomic_fetch_add(&epoch,1);
    int rc=ble_gap_disc(own_type,BLE_HS_FOREVER,&p,gap,NULL);
    if (rc) return ESP_FAIL;
    atomic_store(&active,true); return ESP_OK;
}
bool col_scan_active(void) { return atomic_load(&active); }
bool col_scan_ready(void) { return atomic_load(&ready); }
unsigned col_scan_lost(void) { return atomic_load(&lost); }
uint32_t col_scan_epoch(void) { return atomic_load(&epoch); }
uint32_t col_scan_stack(void) { return host ? uxTaskGetStackHighWaterMark(host) : 0; }
esp_err_t col_scan_shutdown(void)
{
    if (!initialized) return ESP_OK;
    esp_err_t err=col_scan_set(false); if (err!=ESP_OK) return err;
    if (host) {
        if (!stop_requested) {
            if (nimble_port_stop()!=0) return ESP_FAIL;
            stop_requested=true;
        }
        if (!host_done) {
            if (xSemaphoreTake(stopped,pdMS_TO_TICKS(2000))!=pdTRUE) return ESP_ERR_TIMEOUT;
            host_done=true;
        }
        vTaskDelete(host); host=NULL;
    }
    err=nimble_port_deinit();
    if (err==ESP_OK) { initialized=false; atomic_store(&ready,false); vSemaphoreDelete(stopped); stopped=NULL; }
    return err;
}
