#include "sdkconfig.h"
#if !defined(CONFIG_COLLECTOR_WOKWI) || !CONFIG_COLLECTOR_WOKWI
#error "Wokwi synthetic scanner must only be compiled for CONFIG_COLLECTOR_WOKWI"
#endif
#include "collector_platform.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdio.h>

/* 人工广播只服务于模拟测试，不连接 NimBLE，也不接收周边蓝牙设备。 */
static const uint8_t advert_a[]={
    2,0x01,0x06, 8,0x09,'W','o','k','w','i','-','A',
    3,0x03,0x0f,0x18, 3,0xff,0xff,0xff, 2,0x0a,0xf8
};
static const uint8_t advert_b[]={
    2,0x01,0x06, 8,0x09,'W','o','k','w','i','-','B',
    5,0x05,0x78,0x56,0x34,0x12, 3,0xff,0xfe,0xff, 2,0x0a,0xfc
};
static const uint8_t advert_c[]={
    8,0x09,'W','o','k','w','i','-','C',
    17,0x07,0x78,0x56,0x34,0x12,0xf0,0xde,0xbc,0x9a,
              0x78,0x56,0x34,0x12,0x78,0x56,0x34,0x12,
    3,0xff,0xfd,0xff
};
static const struct {
    uint8_t address[6];
    const uint8_t *advert;
    size_t length;
    int8_t rssi[4];
} devices[]={
    {{1,0,0xf0,0xee,0xff,0xc0},advert_a,sizeof(advert_a),{-63,-60,-57,-54}},
    {{2,0,0xf0,0xee,0xff,0xc0},advert_b,sizeof(advert_b),{-80,-82,-79,-81}},
    {{3,0,0xf0,0xee,0xff,0xc0},advert_c,sizeof(advert_c),{-45,-47,-42,-44}}
};
static QueueHandle_t samples;
static TaskHandle_t worker;
static atomic_bool ready, active;
static atomic_uint lost, epoch;
static bool initialized;
static uint32_t frame;

static void emit_frame(void)
{
    uint32_t current_epoch=atomic_load(&epoch);
    if (!atomic_load(&active)) return;
    unsigned accepted=0;
    for (size_t i=0;i<sizeof(devices)/sizeof(devices[0]);i++) {
        col_sample sample;
        if (!col_ad(&sample,devices[i].address,1,devices[i].rssi[frame%4],
                    devices[i].advert,devices[i].length)) {
            atomic_fetch_add(&lost,1);
            continue;
        }
        sample.mono_ms=col_mono(); sample.utc_ms=col_utc(); sample.epoch=current_epoch;
        /* 控制器可在生成期间暂停/重启；旧 epoch 不得被标成新一轮扫描。 */
        if (!atomic_load(&active) || atomic_load(&epoch)!=current_epoch) return;
        if (xQueueSend(samples,&sample,0)==pdTRUE) accepted++;
        else atomic_fetch_add(&lost,1);
    }
    if (frame%4==0) {
        printf("WOKWI_SCAN_TICK epoch=%lu frame=%lu accepted=%u lost=%u\n",
               (unsigned long)current_epoch,(unsigned long)frame,accepted,atomic_load(&lost));
    }
    frame++;
}
static void run(void *unused)
{
    (void)unused;
    for (;;) { emit_frame(); vTaskDelay(pdMS_TO_TICKS(250)); }
}
esp_err_t col_scan_init(QueueHandle_t q)
{
    if (initialized) return ESP_ERR_INVALID_STATE;
    if (!q) return ESP_ERR_INVALID_ARG;
    samples=q; frame=0;
    if (xTaskCreate(run,"wokwi_scan",3072,NULL,5,&worker)!=pdPASS) {
        samples=NULL; worker=NULL; return ESP_ERR_NO_MEM;
    }
    initialized=true; atomic_store(&ready,true);
    printf("WOKWI_SCAN_READY source=synthetic devices=3 period_ms=250\n");
    return ESP_OK;
}
esp_err_t col_scan_set(bool enabled)
{
    if (!enabled) {
        if (!atomic_exchange(&active,false)) return ESP_OK;
        atomic_fetch_add(&epoch,1);
        printf("WOKWI_SCAN_STOP epoch=%u\n",atomic_load(&epoch));
        return ESP_OK;
    }
    if (atomic_load(&active)) return ESP_OK;
    if (!atomic_load(&ready)) return ESP_ERR_INVALID_STATE;
    atomic_fetch_add(&epoch,1); atomic_store(&active,true);
    printf("WOKWI_SCAN_START epoch=%u\n",atomic_load(&epoch));
    return ESP_OK;
}
bool col_scan_active(void) { return atomic_load(&active); }
bool col_scan_ready(void) { return atomic_load(&ready); }
unsigned col_scan_lost(void) { return atomic_load(&lost); }
uint32_t col_scan_epoch(void) { return atomic_load(&epoch); }
uint32_t col_scan_stack(void) { return worker ? uxTaskGetStackHighWaterMark(worker) : 0; }
esp_err_t col_scan_shutdown(void)
{
    if (!initialized) return ESP_OK;
    col_scan_set(false); atomic_store(&ready,false);
    if (worker) { vTaskDelete(worker); worker=NULL; }
    samples=NULL; initialized=false;
    printf("WOKWI_SCAN_SHUTDOWN\n");
    return ESP_OK;
}
