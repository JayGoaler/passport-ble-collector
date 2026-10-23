#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../main/collector_controller.c"

struct fake_queue { unsigned capacity,size,count; unsigned char data[]; };
static QueueHandle_t allocated[8]; static unsigned allocations;
static int64_t fake_now=10000;
static bool fake_online=true,fake_clock=true,fake_scan,fake_ready=true,fake_usb;
static unsigned starts,stops,fake_epoch=1,usb_generation;
static esp_err_t scan_error;
QueueHandle_t xQueueCreate(unsigned n,unsigned size)
{
    QueueHandle_t q=calloc(1,sizeof(*q)+n*size); assert(q);
    q->capacity=n; q->size=size; allocated[allocations++]=q; return q;
}
int xQueueSend(QueueHandle_t q,const void *p,unsigned ticks)
{ (void)ticks; if (q->count==q->capacity) return 0; memcpy(q->data+q->count++*q->size,p,q->size); return 1; }
int xQueueReceive(QueueHandle_t q,void *p,unsigned ticks)
{ (void)ticks; if (!q->count) return 0; memcpy(p,q->data,q->size); q->count--; memmove(q->data,q->data+q->size,q->count*q->size); return 1; }
int xQueueOverwrite(QueueHandle_t q,const void *p) { q->count=0; return xQueueSend(q,p,0); }
unsigned uxQueueMessagesWaiting(QueueHandle_t q) { return q->count; }
int xTaskCreate(void (*fn)(void *),const char *n,unsigned s,void *a,unsigned p,TaskHandle_t *t)
{ (void)fn;(void)n;(void)s;(void)a;(void)p;(void)t; return 1; }
void vTaskDelay(unsigned t) { fake_now+=t; }
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t t) { (void)t; return 1000; }
unsigned esp_get_free_heap_size(void) { return 100000; }
unsigned esp_get_minimum_free_heap_size(void) { return 90000; }
unsigned heap_caps_get_largest_free_block(unsigned f) { (void)f; return 50000; }
uint32_t esp_random(void) { return 0; }
void esp_fill_random(void *p,size_t n) { memset(p,0x45,n); }
esp_err_t bsp_battery_init(void) { return ESP_OK; }
int bsp_battery_soc(void) { return 55; }
esp_err_t demo_radio_nvs_prepare(void) { return ESP_OK; }
esp_err_t demo_radio_network_prepare(void) { return ESP_OK; }
int64_t col_mono(void) { return fake_now; }
int64_t col_utc(void) { return 1792713600000LL+fake_now; }
bool col_clock_ready(void) { return fake_clock; }
void col_wipe(void *p,size_t n) { memset(p,0,n); }
esp_err_t col_scan_init(QueueHandle_t q) { (void)q; return ESP_OK; }
esp_err_t col_scan_set(bool enabled)
{
    if (scan_error!=ESP_OK) return scan_error;
    if (enabled!=fake_scan) { if (enabled) starts++; else stops++; fake_epoch++; }
    fake_scan=enabled; return ESP_OK;
}
bool col_scan_active(void) { return fake_scan; }
bool col_scan_ready(void) { return fake_ready; }
unsigned col_scan_lost(void) { return 0; }
uint32_t col_scan_epoch(void) { return fake_epoch; }
uint32_t col_scan_stack(void) { return 1000; }
esp_err_t col_scan_shutdown(void) { return col_scan_set(false); }
esp_err_t col_network_init(QueueHandle_t a,QueueHandle_t b) { (void)a;(void)b; return ESP_OK; }
bool col_wifi_ready(void) { return fake_online; }
uint32_t col_wifi_ip(void) { return 0; }
uint32_t col_network_stack(void) { return 1000; }
const char *col_network_diagnostic(void) { return fake_online?NULL:"WIFI_CONNECTING"; }
esp_err_t col_usb_init(QueueHandle_t q) { (void)q; return ESP_OK; }
void col_usb_open(bool on) { fake_usb=on; usb_generation++; }
uint32_t col_usb_generation(void) { return usb_generation; }
uint32_t col_usb_stack(void) { return 1000; }
bool col_usb_is_open(void) { return fake_usb; }
esp_err_t col_ui_init(QueueHandle_t a,QueueHandle_t b) { (void)a;(void)b; return ESP_OK; }
static void clean(void)
{
    while (allocations) free(allocated[--allocations]);
    config=(col_config){.window_ms=2000,.heartbeat_sec=30,.max_batch=100,.active_sec=45,.stale_sec=120};
    fake_scan=false; fake_online=true; fake_clock=true; fake_ready=true; fake_usb=false;
    fake_now=10000; starts=stops=0; scan_error=ESP_OK;
    credentials_set=config_set=busy=local_pause=clear_requested=capacity_probe=false;
    ui_at=scan_retry=clear_until=0; was_scanning=false; local_error=NULL; last_sample_epoch=0;
    node[0]=last_upload[0]=0; page=menu=device_index=0; batch_seq=0;
    memset(&store,0,sizeof(store));
    col_controller_start();
}
static col_config remote(void)
{ return (col_config){true,2000,30,100,-60,-85,45,120,7,1}; }
static void complete(int status,const char *body)
{
    col_job *p=NULL; assert(xQueueReceive(job_q,&p,0));
    p->transport=ESP_OK; p->status=status; p->diagnostic=NULL;
    snprintf(p->response,sizeof(p->response),"%s",body); assert(xQueueSend(done_q,&p,0));
}
int main(void)
{
    clean(); col_config c=remote(); apply_remote(&c,fake_now);
    assert(config_set && config.revision==1 && fake_scan && starts==1);
    apply_remote(&c,fake_now+10000); assert(stops==0 && starts==1);
    c.window_ms=3000; apply_remote(&c,fake_now); assert(config.window_ms==2000 && !strcmp(local_error,"CONFIG_REVISION_CONFLICT"));
    c.revision=2; scan_error=ESP_FAIL; apply_remote(&c,fake_now);
    assert(config.revision==1 && config.window_ms==2000 && fake_scan);
    scan_error=ESP_OK; local_pause=true; apply_remote(&c,fake_now);
    assert(config.revision==2 && !fake_scan && local_pause);
    c.scan_enabled=false; c.revision=3; local_pause=false; apply_remote(&c,fake_now);
    assert(!fake_scan && config.revision==3);
    puts("controller: 配置先执行、失败保留版本、同版本不重启、双暂停开关 PASS");

    clean(); c=remote(); apply_remote(&c,fake_now); strcpy(node,"firmware-host-test"); credentials_set=true;
    schedule.config_at=fake_now+10000; schedule.heartbeat_at=fake_now+30000;
    col_schedule_result(&schedule,COL_RETRY,fake_now,0);
    col_sample p={0}; uint8_t address[6]={1,0,0,0,0,0xc0};
    assert(col_ad(&p,address,1,-55,NULL,0)); p.mono_ms=fake_now; p.utc_ms=col_utc(); p.epoch=fake_epoch;
    assert(xQueueSend(sample_q,&p,0)); step(); assert(!busy && schedule.retry_at==11000);
    fake_online=false; fake_now=12000; step(); assert(fake_scan && !busy && store.count==1);
    fake_online=true; schedule.write_at=fake_now+60000; schedule.upload_at=0; schedule.heartbeat_at=0;
    step(); assert(!busy);
    fake_now=20001; step(); assert(busy && job.kind==COL_GET_CONFIG);
    complete(401,"{\"error\":{\"code\":\"INVALID_NODE_TOKEN\"}}"); step();
    assert(schedule.auth_blocked && !fake_scan && !busy);
    puts("controller: 新观测不取消退避、断网有界采集、429写冷却允许GET、401停止请求 PASS");

    clean(); c=remote(); apply_remote(&c,fake_now); strcpy(node,"firmware-host-test"); credentials_set=true;
    schedule.config_at=999999; schedule.heartbeat_at=0;
    step(); assert(busy && job.kind==COL_POST_HEARTBEAT);
    complete(200,"{\"schema_version\":\"1.0\",\"config_revision\":2,\"server_time\":\"2026-10-23T00:00:00.000Z\"}");
    step(); assert(busy && job.kind==COL_GET_CONFIG);
    puts("controller: 心跳优先、服务端新revision触发配置拉取 PASS");

    clean(); c=remote(); apply_remote(&c,fake_now); strcpy(node,"firmware-host-test"); credentials_set=true;
    schedule.config_at=999999; schedule.heartbeat_at=999999;
    col_event e={0}; strcpy(e.id,"immutable-1"); strcpy(e.meta.address,"C0:00:00:00:00:01");
    e.utc_ms=col_utc(); e.created_ms=fake_now; e.count=1; e.window_ms=2000; e.min=e.max=-55; e.median_x2=-110;
    assert(col_enqueue(&store,&e)); step(); assert(busy && job.kind==COL_POST_EVENTS && store.pinned==1);
    complete(200,"{\"schema_version\":\"1.0\",\"batch_id\":\"wrong\",\"accepted\":1,\"duplicates\":0,\"rejected\":0,\"server_time\":\"2026-10-23T00:00:00.000Z\"}");
    step(); assert(store.count==1 && store.pinned==0 && schedule.retry_at>fake_now);
    assert(!strcmp(store.queue[0].id,"immutable-1"));
    col_provisioning queued={.generation=usb_generation};
    strcpy(queued.credentials.node_id,"must-not-apply"); assert(xQueueSend(config_q,&queued,0));
    clear_requested=true; step(); assert(busy && job.kind==COL_CLEAR_CREDENTIALS && !fake_scan && !config_set);
    assert(uxQueueMessagesWaiting(config_q)==0 && store.dropped==1);
    complete(0,""); step(); assert(!credentials_set && !node[0] && store.count==0);
    assert(xQueueSend(config_q,&queued,0)); step();
    assert(!busy && !credentials_set && !node[0]);
    puts("controller: 未确认不出队、原事件不变、清除撤销排队凭据、保留开机丢弃计数 PASS");

    clean(); c=remote(); apply_remote(&c,fake_now); strcpy(node,"firmware-host-test"); credentials_set=true;
    schedule.config_at=0; step(); assert(busy);
    complete(401,""); job.transport=ESP_FAIL; step();
    assert(schedule.auth_blocked && !fake_scan);
    puts("controller: SDK认证处理返回失败仍识别401并停止 PASS");

    clean(); c=remote(); apply_remote(&c,fake_now); strcpy(node,"firmware-host-test"); credentials_set=true;
    schedule.config_at=999999; schedule.heartbeat_at=0;
    store.offset_set=true; store.utc_offset=col_utc()-fake_now+3600000;
    store.last_observation=col_utc()+3600000;
    step(); assert(store.last_observation==0 && busy && job.kind==COL_POST_HEARTBEAT);
    puts("controller: 校时倒跳且无新样本仍发送真实空历史心跳 PASS");

    clean(); c=remote(); apply_remote(&c,fake_now); schedule.auth_blocked=true; scan_error=ESP_FAIL;
    p.mono_ms=fake_now; p.utc_ms=col_utc(); p.epoch=fake_epoch;
    assert(xQueueSend(sample_q,&p,0)); step();
    assert(fake_scan && !store.window_open && store.count==0 && store.last_observation==0);
    puts("controller: 底层停止失败后仍禁止新上传采样 PASS");
    clean(); while (allocations) free(allocated[--allocations]);
    puts("Collector controller runtime tests: PASS");
}
