/* 验证实际仿真应答模块及核心序列化；不执行真实网络，也不启动 Wokwi。 */
#include <stdlib.h>
#include "../sim/wokwi/collector_network.c"
static unsigned checks;
static int64_t tick_ms=1000;
#define CHECK(x) do { checks++; if(!(x)) { fprintf(stderr,"fixture check failed line %d: %s\n",__LINE__,#x); abort(); } } while(0)
int64_t esp_timer_get_time(void) { return tick_ms*1000; }
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t t) { (void)t; return 2048; }
int xTaskCreate(void (*fn)(void *),const char *n,unsigned sz,void *arg,unsigned prio,TaskHandle_t *out)
{ (void)fn;(void)n;(void)sz;(void)arg;(void)prio; *out=(void *)1; return pdPASS; }
int xQueueReceive(QueueHandle_t q,void *out,unsigned wait) { (void)q;(void)out;(void)wait; return 0; }
int xQueueSend(QueueHandle_t q,const void *out,unsigned wait) { (void)q;(void)out;(void)wait; return pdTRUE; }
static col_job job;
static col_store store;
static void credentials(void)
{
    memset(&job,0,sizeof(job)); job.kind=COL_SET_CREDENTIALS;
    strcpy(job.credentials.ssid,"Wokwi-GUEST"); strcpy(job.credentials.base_url,"https://fixture.wokwi.invalid");
    strcpy(job.credentials.node_id,"Wokwi"); strcpy(job.credentials.token,fixture_token); strcpy(job.credentials.ca,fixture_ca);
}
static void make_batch(unsigned serial)
{
    col_store_init(&store,"0123456789abcdef0123456789abcdef");
    for(unsigned i=1;i<=3;i++) {
        col_event event={.utc_ms=col_utc(),.created_ms=col_mono(),.count=4,.window_ms=2000,.median_x2=-117,.min=-65,.max=-55};
        snprintf(event.id,sizeof(event.id),"fixture-%u-%u",serial,i);
        snprintf(event.meta.address,sizeof(event.meta.address),"C0:FF:EE:F0:00:%02u",i);
        event.meta.address_type=1;
        CHECK(col_enqueue(&store,&event));
    }
    memset(&job,0,sizeof(job)); job.kind=COL_POST_EVENTS; strcpy(job.batch_id,"fixture-batch");
    CHECK(col_batch(&store,"Wokwi",job.batch_id,col_utc(),3,job.body,sizeof(job.body)));
}
int main(void)
{
    CHECK(col_network_init(NULL,(void *)2)==ESP_ERR_INVALID_ARG);
    CHECK(col_network_init((void *)1,(void *)2)==ESP_OK);
    CHECK(col_network_init((void *)1,(void *)2)==ESP_ERR_INVALID_STATE);
    CHECK(!col_wifi_ready() && !col_clock_ready() && !col_wifi_ip());
    CHECK(col_utc()==1792713601000LL);
    for(unsigned field_index=0;field_index<6;field_index++) {
        credentials();
        char *fields[]={job.credentials.ssid,job.credentials.password,job.credentials.base_url,
            job.credentials.node_id,job.credentials.token,job.credentials.ca};
        fields[field_index][0]='!'; process_job(&job);
        CHECK(job.transport==ESP_ERR_INVALID_ARG && !col_wifi_ready());
        const unsigned char *p=(const unsigned char *)&job.credentials;
        bool wiped=true; for(size_t i=0;i<sizeof(job.credentials);i++) if(p[i]) wiped=false;
        CHECK(wiped);
    }
    job.kind=COL_GET_CONFIG; process_job(&job); CHECK(job.status==401);
    credentials(); process_job(&job);
    CHECK(job.transport==ESP_OK && col_wifi_ready() && col_clock_ready());
    CHECK(col_wifi_ip()==0x010200c0U);
    job.kind=COL_GET_CONFIG; process_job(&job); col_config config;
    CHECK(job.status==200 && col_parse_config(job.response,"Wokwi",&config));
    CHECK(config.scan_enabled && config.revision==1 && config.window_ms==2000);
    col_store_init(&store,"0123456789abcdef0123456789abcdef");
    job.kind=COL_POST_HEARTBEAT;
    CHECK(col_heartbeat("Wokwi",col_utc(),"scanning",true,&store,NULL,1,job.body,sizeof(job.body)));
    process_job(&job); int64_t revision;
    CHECK(job.status==200 && col_heartbeat_ack(job.response,&revision) && revision==1);
    strcpy(job.body,"{\"node_id\":\"Wokwi\"}"); process_job(&job); CHECK(job.status==400);
    make_batch(1); process_job(&job);
    CHECK(job.status==200 && col_ack(job.response,job.batch_id,3));
    CHECK(seen_count==3 && device_mask==7);
    process_job(&job);
    CHECK(job.status==200 && col_ack(job.response,job.batch_id,3));
    CHECK(strstr(job.response,"\"accepted\":0") && strstr(job.response,"\"duplicates\":3") && seen_count==3);
    make_batch(2);
    cJSON *json=cJSON_Parse(job.body);
    cJSON *events=field(json,"observations");
    cJSON_SetNumberValue(field(cJSON_GetArrayItem(events,1),"rssi_dbm"),0);
    CHECK(cJSON_PrintPreallocated(json,job.body,sizeof(job.body),false)); cJSON_Delete(json);
    process_job(&job); CHECK(job.status==400 && seen_count==3);
    strcpy(job.body,"{\"nested\":[[[[[]]]]]}"); process_job(&job); CHECK(job.status==400);
    make_batch(3); seen_count=255; process_job(&job); CHECK(job.status==409 && seen_count==255);
    job.kind=COL_CLEAR_CREDENTIALS; process_job(&job);
    CHECK(job.transport==ESP_OK && !col_wifi_ready() && !col_clock_ready() && !col_wifi_ip());
    job.kind=COL_GET_CONFIG; process_job(&job); CHECK(job.status==401);
    printf("Wokwi fixture host checks: PASS (%u); Wokwi/device NOT RUN\n",checks);
    return 0;
}
