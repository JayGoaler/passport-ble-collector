#include "collector_platform.h"
#include "demo_radio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_log.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static col_store store;
static col_config config={.window_ms=2000,.heartbeat_sec=30,.max_batch=100,.active_sec=45,.stale_sec=120};
static col_schedule schedule;
static col_job job;
static col_provisioning incoming;
static QueueHandle_t sample_q,key_q,view_q,config_q,job_q,done_q;
static bool credentials_set,config_set,busy,local_pause,clear_requested,capacity_probe;
static char node[81],last_upload[25];
static const char *local_error;
static uint64_t batch_seq;
static int page,menu,device_index;
static int64_t clear_until;
static bool network_ready;
static uint32_t last_sample_epoch;

static void reset_store(void)
{
    uint8_t random[16]; esp_fill_random(random,sizeof(random)); char boot[33];
    for (int i=0;i<16;i++) snprintf(boot+i*2,3,"%02x",random[i]);
    uint64_t dropped=store.dropped+store.count, invalid=store.raw_invalid;
    uint64_t overflow=store.raw_overflow, partial=store.partial_windows;
    col_store_init(&store,boot);
    store.dropped=dropped; store.raw_invalid=invalid;
    store.raw_overflow=overflow; store.partial_windows=partial;
}
static bool dispatch(col_job_kind kind,int64_t now)
{
    job.kind=kind; col_job *p=&job;
    if (xQueueSend(job_q,&p,0)!=pdTRUE) return false;
    busy=true;
    if (kind==COL_POST_EVENTS || kind==COL_POST_HEARTBEAT) schedule.write_at=now+600;
    return true;
}
static bool same_config(const col_config *a,const col_config *b)
{
    return a->scan_enabled==b->scan_enabled && a->window_ms==b->window_ms &&
        a->heartbeat_sec==b->heartbeat_sec && a->max_batch==b->max_batch &&
        a->strong==b->strong && a->weak==b->weak && a->active_sec==b->active_sec &&
        a->stale_sec==b->stale_sec && a->retention_days==b->retention_days;
}
static void apply_remote(const col_config *next,int64_t now)
{
    if (config_set && next->revision<config.revision) { local_error="CONFIG_REVISION_ROLLBACK"; return; }
    if (config_set && next->revision==config.revision) {
        if (!same_config(next,&config)) local_error="CONFIG_REVISION_CONFLICT";
        else if (local_error && (!strcmp(local_error,"INVALID_CONFIG") || !strcmp(local_error,"CONFIG_REVISION_CONFLICT"))) local_error=NULL;
        return;
    }
    if (config_set && next->scan_enabled==config.scan_enabled && next->window_ms==config.window_ms) {
        config=*next; local_error=NULL; schedule.heartbeat_at=now; return;
    }
    if (col_scan_set(false)!=ESP_OK) { local_error="SCAN_STOP_FAILED"; return; }
    col_window_reset(&store);
    bool wanted=col_scan_allowed(true,local_pause,col_usb_is_open(),col_clock_ready(),next,&schedule);
    if (wanted && col_scan_set(true)!=ESP_OK) { local_error="SCAN_START_FAILED"; return; }
    config=*next; config_set=true; local_error=NULL; schedule.heartbeat_at=now;
    printf("COLLECTOR {\"status\":\"AUTH_CONFIG_APPLIED\",\"revision\":%" PRId64 "}\n",config.revision);
}
static void response(int64_t now)
{
    col_job *p=NULL; if (xQueueReceive(done_q,&p,0)!=pdTRUE) return;
    busy=false;
    if (p->kind==COL_SET_CREDENTIALS || p->kind==COL_CLEAR_CREDENTIALS) {
        credentials_set=p->kind==COL_SET_CREDENTIALS && p->transport==ESP_OK;
        local_error=p->transport==ESP_OK?NULL:"CREDENTIAL_APPLY_FAILED";
        printf("COLLECTOR {\"status\":\"%s\"}\n",credentials_set?"WIFI_CONNECTING":local_error?local_error:"CLEARED");
        return;
    }
    col_result r=p->status==401?COL_AUTH:p->transport==ESP_OK?col_classify(p->status,p->response):COL_RETRY;
    if (r==COL_ACK) {
        if (p->kind==COL_GET_CONFIG) {
            col_config next;
            if (col_parse_config(p->response,node,&next)) apply_remote(&next,now);
            else { local_error="INVALID_CONFIG"; r=COL_RETRY; }
        } else if (p->kind==COL_POST_HEARTBEAT) {
            int64_t revision=0;
            if (!col_heartbeat_ack(p->response,&revision)) r=COL_RETRY;
            else if (revision>config.revision) schedule.config_at=now;
        } else if (col_ack(p->response,p->batch_id,store.pinned)) {
            col_confirm(&store,true); col_time_format(col_utc(),last_upload);
            schedule.capacity_blocked=false; capacity_probe=false;
        } else r=COL_RETRY;
    }
    if (p->kind==COL_POST_EVENTS && store.pinned) {
        if (r==COL_BAD_INPUT || (r==COL_SPLIT && store.pinned==1)) col_discard_pinned(&store);
        else col_confirm(&store,false);
    }
    if (r==COL_BAD_INPUT && p->kind!=COL_POST_EVENTS) r=COL_CONFIG_ERROR;
    col_schedule_result(&schedule,r,now,esp_random());
    if (r==COL_RETRY && p->diagnostic && p->transport!=ESP_OK) schedule.error=p->diagnostic;
    if (p->kind==COL_GET_CONFIG && r!=COL_ACK) schedule.config_at=now+10000;
#ifdef CONFIG_COLLECTOR_WOKWI
    printf("WOKWI_HTTP kind=%d ack=%d status=%d\n",(int)p->kind,r==COL_ACK,p->status);
#endif
    col_wipe(p->response,sizeof(p->response));
}
static void key(const col_key *k,int64_t now)
{
    if (k->event==BSP_BTN_LONG && k->button==BSP_BTN_OK) { page=page==3?0:3; clear_until=0; return; }
    if (k->event!=BSP_BTN_CLICK) return;
    if (k->button==BSP_BTN_UP || k->button==BSP_BTN_DOWN) {
        int direction=k->button==BSP_BTN_UP?-1:1;
        if (page==3) menu=(menu+direction+5)%5; else page=(page+direction+3)%3;
        clear_until=0; return;
    }
    if (k->button!=BSP_BTN_OK) return;
    if (page==1) { device_index++; return; }
    if (page!=3) return;
    if (menu==0) local_pause=!local_pause;
    else if (menu==1) col_usb_open(!col_usb_is_open());
    else if (menu==2) { col_schedule_init(&schedule); local_error=NULL; }
    else if (menu==3) {
        if (now<clear_until) { clear_requested=true; col_usb_open(false); clear_until=0; }
        else clear_until=now+5000;
    } else page=0;
}
static void publish(void)
{
    col_view v={0}; snprintf(v.node,sizeof(v.node),"%s",node); v.page=page; v.menu=menu;
    v.wifi=col_wifi_ready(); v.clock=col_clock_ready(); v.configured=credentials_set;
    v.local_pause=local_pause; v.provisioning=col_usb_is_open(); v.clearing=col_mono()<clear_until;
    v.pending=(unsigned)store.count; v.dropped=(unsigned)store.dropped;
    v.raw_dropped=col_scan_lost()+(unsigned)(store.raw_invalid+store.raw_overflow);
    v.revision=config_set?config.revision:0;
    const char *error=local_error?local_error:schedule.error?schedule.error:col_network_diagnostic();
    snprintf(v.error,sizeof(v.error),"%s",error?error:"");
    const char *state=v.provisioning?"USB 配置中":!credentials_set?"请先配置":!v.wifi?"等待网络":!v.clock?"等待校时":
        !config_set?"等待服务配置":schedule.auth_blocked?"认证失败":schedule.capacity_blocked?"容量已满 / 未上传":
        col_scan_active()?"正在扫描":local_pause?"本地暂停":!config.scan_enabled?"远程暂停":"扫描未就绪";
    snprintf(v.state,sizeof(v.state),"%s",state);
    uint32_t ip=col_wifi_ip(); snprintf(v.ip,sizeof(v.ip),"%u.%u.%u.%u",(unsigned)(ip&255),
        (unsigned)((ip>>8)&255),(unsigned)((ip>>16)&255),(unsigned)(ip>>24));
    snprintf(v.last_upload,sizeof(v.last_upload),"%s",last_upload);
    size_t indices[COL_ADDRS];
    for (size_t i=0;i<COL_ADDRS;i++) if (store.buckets[i].used) {
        indices[v.device_total++]=i; if (store.buckets[i].count) v.addresses++;
    }
    v.events=(unsigned)store.sequence;
    if (v.device_total) {
        device_index%=v.device_total; v.device_index=device_index;
        col_bucket *b=&store.buckets[indices[device_index]];
        snprintf(v.device_name,sizeof(v.device_name),"%s",b->meta.name);
        snprintf(v.device_suffix,sizeof(v.device_suffix),"%s",b->meta.address+9);
        int age=(int)((col_mono()-b->last_ms)/1000);
        v.device_age=age>config.active_sec?-1:age; v.device_rssi=b->last_rssi;
    } else v.device_age=-1;
    v.heap_free=esp_get_free_heap_size(); v.heap_min=esp_get_minimum_free_heap_size();
    v.heap_largest=heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    v.stack_min=uxTaskGetStackHighWaterMark(NULL); v.battery=bsp_battery_soc();
#ifdef CONFIG_COLLECTOR_WOKWI
    /* 仿真自动化证据；不打印凭据、地址或广播内容。 */
    printf("WOKWI_VIEW page=%d menu=%d usb=%d wifi=%d clock=%d revision=%lld pending=%u devices=%d pause=%d\n",
        v.page,v.menu,v.provisioning,v.wifi,v.clock,(long long)v.revision,v.pending,v.device_total,v.local_pause);
#endif
    /* 诊断不包含地址、SSID、Token；用于笔记本长稳采样。 */
    static unsigned diagnostics;
    if (++diagnostics%30==0) printf("COLLECTOR {\"status\":\"METRICS\",\"heap\":%lu,\"minimum\":%lu,\"largest\":%lu,\"controller_stack\":%lu,\"network_stack\":%lu,\"ble_stack\":%lu,\"usb_stack\":%lu,\"pending\":%u,\"dropped\":%u}\n",
        (unsigned long)v.heap_free,(unsigned long)v.heap_min,(unsigned long)v.heap_largest,
        (unsigned long)v.stack_min,(unsigned long)col_network_stack(),(unsigned long)col_scan_stack(),(unsigned long)col_usb_stack(),v.pending,v.dropped);
    xQueueOverwrite(view_q,&v);
}
static int64_t ui_at,scan_retry;
static bool was_scanning;
static void step(void)
{
        int64_t now=col_mono(); response(now);
        col_key k; while (xQueueReceive(key_q,&k,0)==pdTRUE) key(&k,now);
        if (network_ready && !busy && (clear_requested || uxQueueMessagesWaiting(config_q))) {
            if (col_scan_set(false)!=ESP_OK) local_error="SCAN_STOP_FAILED";
            else if (clear_requested || xQueueReceive(config_q,&incoming,0)==pdTRUE) {
                bool accepted=clear_requested || incoming.generation==col_usb_generation();
                if (accepted) {
                    col_window_reset(&store); col_schedule_init(&schedule); reset_store();
                    config_set=false; credentials_set=false; capacity_probe=false; last_upload[0]=0;
                    if (clear_requested) {
                        clear_requested=false; col_usb_open(false); col_wipe(node,sizeof(node));
                        while (xQueueReceive(config_q,&incoming,0)==pdTRUE) col_wipe(&incoming,sizeof(incoming));
                        dispatch(COL_CLEAR_CREDENTIALS,now);
                    } else {
                        job.credentials=incoming.credentials;
                        snprintf(node,sizeof(node),"%s",job.credentials.node_id);
                        dispatch(COL_SET_CREDENTIALS,now);
                    }
                }
                col_wipe(&incoming,sizeof(incoming));
            }
        }
        if (schedule.capacity_blocked && !store.count && now>=schedule.capacity_at && !capacity_probe) capacity_probe=true;
        col_schedule scan_schedule=schedule;
        if (capacity_probe) scan_schedule.capacity_blocked=false;
        bool want=col_scan_allowed(config_set,local_pause,col_usb_is_open() || clear_requested,
            col_clock_ready(),&config,&scan_schedule);
        if (capacity_probe && store.count) want=false;
        if (want!=col_scan_active() && now>=scan_retry) {
            esp_err_t err=col_scan_set(want);
            if (err!=ESP_OK) { local_error=want?"SCAN_START_FAILED":"SCAN_STOP_FAILED"; scan_retry=now+1000; }
            else if (local_error && (!strcmp(local_error,"SCAN_START_FAILED") || !strcmp(local_error,"SCAN_STOP_FAILED"))) local_error=NULL;
        }
        bool scanning=col_scan_active();
        bool admitting=want && scanning;
        if ((was_scanning && !admitting) || last_sample_epoch!=col_scan_epoch()) {
            col_window_reset(&store); last_sample_epoch=col_scan_epoch();
        }
        int64_t offset=col_utc()-now;
        if (store.offset_set && llabs(offset-store.utc_offset)>2000) {
            col_window_reset(&store); store.utc_offset=offset;
            if (!col_time_allowed(store.last_observation,col_utc(),true)) store.last_observation=0;
        }
        was_scanning=admitting;
        col_sample sample;
        unsigned drained=0;
        while (drained++<64 && xQueueReceive(sample_q,&sample,0)==pdTRUE) {
            if (admitting && sample.epoch==col_scan_epoch() && col_clock_ready()) col_ingest(&store,&sample,config.window_ms);
            else store.raw_invalid++;
        }
        if (admitting && uxQueueMessagesWaiting(sample_q)==0) col_window_close(&store,now,config.window_ms);
        col_expire(&store,now);
        if (!busy && credentials_set && col_wifi_ready() && col_clock_ready() &&
            !schedule.auth_blocked && !schedule.protocol_blocked && now>=schedule.retry_at) {
            if (now>=schedule.config_at) {
                job.body[0]=0; if (dispatch(COL_GET_CONFIG,now)) schedule.config_at=now+10000;
            } else if (config_set && now>=schedule.write_at) {
                if (now>=schedule.heartbeat_at) {
                    const char *state=scanning?"scanning":!col_scan_ready()?"unavailable":
                        local_error || schedule.auth_blocked?"error":"paused";
                    if (col_heartbeat(node,col_utc(),state,col_scan_ready(),&store,
                        local_error?local_error:schedule.error,config.revision,job.body,sizeof(job.body)) &&
                        dispatch(COL_POST_HEARTBEAT,now)) schedule.heartbeat_at=now+config.heartbeat_sec*1000;
                } else if (store.count && now>=schedule.upload_at && (!schedule.capacity_blocked || now>=schedule.capacity_at)) {
                    snprintf(job.batch_id,sizeof(job.batch_id),"%s-b%" PRIu64,store.boot,++batch_seq);
                    size_t limit=schedule.batch_limit<(unsigned)config.max_batch?schedule.batch_limit:(unsigned)config.max_batch;
                    if (col_batch(&store,node,job.batch_id,col_utc(),limit,job.body,sizeof(job.body))) {
                        if (dispatch(COL_POST_EVENTS,now)) schedule.upload_at=now+config.window_ms;
                        else col_confirm(&store,false);
                    }
                }
            }
        }
        if (now>=ui_at) { publish(); ui_at=now+1000; }
}
static void run(void *unused)
{
    (void)unused;
    for (;;) { step(); vTaskDelay(pdMS_TO_TICKS(20)); }
}
void col_controller_start(void)
{
    reset_store(); col_schedule_init(&schedule);
    sample_q=xQueueCreate(16,sizeof(col_sample)); key_q=xQueueCreate(8,sizeof(col_key));
    view_q=xQueueCreate(1,sizeof(col_view)); config_q=xQueueCreate(1,sizeof(col_provisioning));
    job_q=xQueueCreate(1,sizeof(col_job *)); done_q=xQueueCreate(1,sizeof(col_job *));
    if (!sample_q || !key_q || !view_q || !config_q || !job_q || !done_q) { ESP_LOGE("collector","QUEUE_ALLOCATION"); return; }
    esp_err_t err=demo_radio_nvs_prepare();
    if (err==ESP_OK) err=col_network_init(job_q,done_q);
    network_ready=err==ESP_OK;
    if (!network_ready) local_error="NETWORK_INIT_FAILED";
    err=col_scan_init(sample_q); if (err!=ESP_OK) local_error="BLE_INIT_FAILED";
    (void)bsp_battery_init();
    err=col_ui_init(view_q,key_q); if (err!=ESP_OK) { ESP_LOGE("collector","UI_INIT_FAILED"); return; }
    err=col_usb_init(config_q); if (err!=ESP_OK) local_error="USB_INIT_FAILED";
    if (xTaskCreate(run,"collector",6144,NULL,4,NULL)!=pdPASS) ESP_LOGE("collector","TASK_ALLOCATION");
#ifdef CONFIG_COLLECTOR_WOKWI
    else printf("WOKWI_READY synthetic_ble=1 display=ILI9341 buttons=GPIO\n");
#endif
}
