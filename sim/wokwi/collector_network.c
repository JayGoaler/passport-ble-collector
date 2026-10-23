#include "sdkconfig.h"
#if !defined(CONFIG_COLLECTOR_WOKWI) || !CONFIG_COLLECTOR_WOKWI
#error "Wokwi fixture networking is simulation-only"
#endif
#include "collector_platform.h"
#include "cJSON.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* 固定合成凭据；占位 CA 不是证书，本模块绝不执行 Wi-Fi、DNS、SNTP 或 TLS。 */
static const char fixture_token[]="WWWWWWWWWW" "WWWWWWWWWW" "WWWWWWWWWW" "WWWWWWWWWW" "WWW";
static const char fixture_ca[]="-----BEGIN CERTIFICATE-----\nSYNTHETIC-NOT-A-CERT\n-----END CERTIFICATE-----\n";
static const char fixture_config[]=
    "{\"schema_version\":\"1.0\",\"node_id\":\"Wokwi\",\"scan_enabled\":true,"
    "\"aggregation_window_ms\":2000,\"heartbeat_interval_sec\":30,\"max_batch_size\":100,"
    "\"config_revision\":1,\"strong_signal_dbm\":-60,\"weak_signal_dbm\":-85,"
    "\"active_threshold_sec\":45,\"stale_threshold_sec\":120,\"retention_days\":7}";
_Static_assert(sizeof(fixture_token)==44,"synthetic token must have 43 characters");
static QueueHandle_t jobs,results;
static TaskHandle_t task;
static atomic_bool configured;
/* 有界会话去重表；满后拒绝新事件，不静默淘汰而破坏重试幂等。 */
static char seen[256][64];
static unsigned seen_count,device_mask,heartbeats,config_reads;

int64_t col_mono(void) { return esp_timer_get_time()/1000; }
int64_t col_utc(void) { return 1792713600000LL+col_mono(); } /* 2026-10-23T00:00:00Z */
bool col_clock_ready(void) { return atomic_load(&configured); }
bool col_wifi_ready(void) { return atomic_load(&configured); }
uint32_t col_wifi_ip(void) { return col_wifi_ready()?0x010200c0U:0; } /* TEST-NET: 192.0.2.1 */
const char *col_network_diagnostic(void) { return NULL; }
uint32_t col_network_stack(void) { return task?uxTaskGetStackHighWaterMark(task):0; }
void col_wipe(void *p,size_t n) { volatile unsigned char *v=p; while(n--) *v++=0; }
static cJSON *field(cJSON *j,const char *key) { return cJSON_GetObjectItemCaseSensitive(j,key); }
static const char *str(cJSON *j,const char *key)
{ cJSON *v=field(j,key); return cJSON_IsString(v)?v->valuestring:NULL; }
static bool equal(cJSON *j,const char *key,const char *value)
{ const char *s=str(j,key); return s && !strcmp(s,value); }
static bool exact(cJSON *j,const char *const *keys,size_t count)
{
    if(!cJSON_IsObject(j) || (size_t)cJSON_GetArraySize(j)!=count) return false;
    for(size_t i=0;i<count;i++) {
        unsigned found=0;
        for(cJSON *v=j->child;v;v=v->next) if(v->string && !strcmp(v->string,keys[i])) found++;
        if(found!=1) return false;
    }
    return true;
}
static bool number(cJSON *j,const char *key,double lo,double hi,double step,double *out)
{
    cJSON *v=field(j,key);
    if(!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble<lo ||
       v->valuedouble>hi || floor(v->valuedouble/step)!=v->valuedouble/step) return false;
    *out=v->valuedouble; return true;
}
static bool copy(char *to,size_t cap,const char *s)
{ if(!s || strlen(s)>=cap) return false; strcpy(to,s); return true; }
static bool timestamp(cJSON *j,const char *key,bool historical,int64_t *out)
{ const char *s=str(j,key); return s && col_time_parse(s,out) && col_time_allowed(*out,col_utc(),historical); }
static bool nullable(cJSON *j,const char *key,size_t cap)
{ cJSON *v=field(j,key); return cJSON_IsNull(v) || (cJSON_IsString(v) && *v->valuestring && strlen(v->valuestring)<=cap && col_utf8(v->valuestring)); }
static bool one_of(const char *s,const char *const *values,size_t n)
{ for(size_t i=0;s && i<n;i++) if(!strcmp(s,values[i])) return true; return false; }
static bool valid_heartbeat(cJSON *j)
{
    static const char *const keys[]={"schema_version","node_id","observed_at","collector_version","scan_state",
        "bluetooth_state","permission_state","last_observation_at","pending_count","dropped_count","error_code","applied_config_revision"};
    static const char *const states[]={"scanning","paused","permission_denied","bluetooth_off","unavailable","error"};
    int64_t t; double n; const char *version=str(j,"collector_version");
    return exact(j,keys,12) && equal(j,"schema_version","1.0") && equal(j,"node_id","Wokwi") &&
        timestamp(j,"observed_at",false,&t) && version && *version && strlen(version)<=64 && col_utf8(version) &&
        one_of(str(j,"scan_state"),states,6) && equal(j,"bluetooth_state","powered_on") &&
        equal(j,"permission_state","not_applicable") &&
        (cJSON_IsNull(field(j,"last_observation_at")) || timestamp(j,"last_observation_at",true,&t)) &&
        number(j,"pending_count",0,COL_QUEUE,1,&n) && number(j,"dropped_count",0,COL_SAFE_INT,1,&n) &&
        nullable(j,"error_code",80) && number(j,"applied_config_revision",0,1,1,&n);
}
static bool valid_event(cJSON *j,unsigned *mask)
{
    static const char *const keys[]={"event_id","observed_at","local_device_id","identifier_type","address_type",
        "local_name","rssi_dbm","rssi_min_dbm","rssi_max_dbm","sample_count","window_ms","tx_power_dbm","service_uuids","manufacturer_id"};
    /* 与 collector_core.c 的 address_names 索引一致。 */
    static const char *const types[]={"unknown","public","random_static","random_private"};
    static const char *const addresses[]={"C0:FF:EE:F0:00:01","C0:FF:EE:F0:00:02","C0:FF:EE:F0:00:03"};
    col_event e={0}; double n;
    if(!exact(j,keys,14) || !equal(j,"identifier_type","ble_address") ||
       !copy(e.id,sizeof(e.id),str(j,"event_id")) || !timestamp(j,"observed_at",false,&e.utc_ms) ||
       !copy(e.meta.address,sizeof(e.meta.address),str(j,"local_device_id")) || !nullable(j,"local_name",128)) return false;
    if(!cJSON_IsNull(field(j,"local_name"))) strcpy(e.meta.name,str(j,"local_name"));
    const char *type=str(j,"address_type");
    if(!one_of(type,types,4)) return false;
    for(unsigned i=0;i<4;i++) if(!strcmp(type,types[i])) e.meta.address_type=i;
    bool synthetic=false;
    for(unsigned i=0;i<3;i++) if(!strcmp(e.meta.address,addresses[i])) { *mask|=1U<<i; synthetic=true; }
    if(!synthetic) return false;
    if(!number(j,"rssi_dbm",-127,-1,0.5,&n)) return false;
    e.median_x2=(int16_t)(n*2);
    if(!number(j,"rssi_min_dbm",-127,-1,1,&n)) return false;
    e.min=(int8_t)n;
    if(!number(j,"rssi_max_dbm",-127,-1,1,&n)) return false;
    e.max=(int8_t)n;
    if(!number(j,"sample_count",1,100000,1,&n)) return false;
    e.count=(uint32_t)n;
    if(!number(j,"window_ms",2000,3000,1,&n)) return false;
    e.window_ms=(uint16_t)n;
    if(!cJSON_IsNull(field(j,"tx_power_dbm"))) {
        if(!number(j,"tx_power_dbm",-127,126,1,&n)) return false;
        e.meta.has_tx=true; e.meta.tx=(int8_t)n;
    }
    if(!cJSON_IsNull(field(j,"manufacturer_id"))) {
        if(!number(j,"manufacturer_id",0,65535,1,&n)) return false;
        e.meta.has_manufacturer=true; e.meta.manufacturer=(uint16_t)n;
    }
    cJSON *uuids=field(j,"service_uuids");
    if(!cJSON_IsArray(uuids) || cJSON_GetArraySize(uuids)>COL_UUIDS) return false;
    for(cJSON *v=uuids->child;v;v=v->next) {
        if(!cJSON_IsString(v) || !copy(e.meta.uuids[e.meta.uuid_count],37,v->valuestring)) return false;
        e.meta.uuid_count++;
    }
    return col_event_valid(&e,col_utc());
}
static bool known_event(const char *id)
{ for(unsigned i=0;i<seen_count;i++) if(!strcmp(seen[i],id)) return true; return false; }
static cJSON *parse_body(const char *body)
{
    if(!memchr(body,0,COL_BODY)) return NULL;
    unsigned depth=0; bool quoted=false,escape=false;
    for(const unsigned char *p=(const unsigned char *)body;*p;p++) {
        if(quoted) { if(escape) escape=false; else if(*p=='\\') escape=true; else if(*p=='"') quoted=false; }
        else if(*p=='"') quoted=true;
        else if(*p=='{' || *p=='[') { if(++depth>4) return NULL; }
        else if(*p=='}' || *p==']') { if(!depth) return NULL; depth--; }
    }
    if(depth || quoted) return NULL;
    return cJSON_ParseWithOpts(body,NULL,true);
}
static void reject(col_job *j,int status,const char *code)
{
    j->status=status; j->transport=ESP_OK;
    snprintf(j->response,sizeof(j->response),"{\"error\":{\"code\":\"%s\"}}",code);
    printf("WOKWI_FIXTURE_REJECT kind=%d status=%d\n",(int)j->kind,status);
}
static void request(col_job *j)
{
    j->status=0; j->transport=ESP_ERR_INVALID_STATE; j->response[0]=0; j->diagnostic=NULL;
    if(!atomic_load(&configured)) { reject(j,401,"INVALID_NODE_TOKEN"); return; }
    char now[25]; if(!col_time_format(col_utc(),now)) return;
    if(j->kind==COL_GET_CONFIG) {
        col_config check;
        if(!col_parse_config(fixture_config,"Wokwi",&check)) return;
        strcpy(j->response,fixture_config); j->transport=ESP_OK; j->status=200;
        printf("WOKWI_FIXTURE_CONFIG count=%u NETWORK=FIXTURE\n",++config_reads); return;
    }
    cJSON *json=parse_body(j->body);
    if(j->kind==COL_POST_HEARTBEAT && valid_heartbeat(json)) {
        snprintf(j->response,sizeof(j->response),"{\"schema_version\":\"1.0\",\"config_revision\":1,\"server_time\":\"%s\"}",now);
        j->status=200; j->transport=ESP_OK;
        printf("WOKWI_FIXTURE_HEARTBEAT count=%u NETWORK=FIXTURE\n",++heartbeats);
    } else if(j->kind==COL_POST_EVENTS) {
        static const char *const keys[]={"schema_version","node_id","batch_id","sent_at","observations"};
        int64_t t; cJSON *items=field(json,"observations"); const char *batch=str(json,"batch_id");
        bool ok=exact(json,keys,5) && equal(json,"schema_version","1.0") && equal(json,"node_id","Wokwi") &&
            batch && col_id(batch,63) && !strcmp(batch,j->batch_id) && timestamp(json,"sent_at",false,&t) &&
            cJSON_IsArray(items) && cJSON_GetArraySize(items)>0 && cJSON_GetArraySize(items)<=32;
        unsigned mask=0,accepted=0,duplicates=0;
        for(cJSON *v=ok?items->child:NULL;v;v=v->next) if(!valid_event(v,&mask)) { ok=false; break; }
        if(ok) {
            for(cJSON *v=items->child;v;v=v->next) {
                const char *id=str(v,"event_id"); bool duplicate=known_event(id);
                for(cJSON *p=items->child;p!=v;p=p->next) if(!strcmp(str(p,"event_id"),id)) duplicate=true;
                if(duplicate) duplicates++; else accepted++;
            }
            if(seen_count+accepted>256) reject(j,409,"OBSERVATION_LIMIT");
            else {
                for(cJSON *v=items->child;v;v=v->next) if(!known_event(str(v,"event_id"))) strcpy(seen[seen_count++],str(v,"event_id"));
                device_mask|=mask;
                snprintf(j->response,sizeof(j->response),"{\"schema_version\":\"1.0\",\"batch_id\":\"%s\",\"accepted\":%u,\"duplicates\":%u,\"rejected\":0,\"server_time\":\"%s\"}",batch,accepted,duplicates,now);
                j->status=200; j->transport=ESP_OK;
                unsigned devices=(device_mask&1U)+((device_mask>>1)&1U)+((device_mask>>2)&1U);
                printf("WOKWI_FIXTURE_ACK accepted=%u duplicates=%u total=%u devices=%u NETWORK=FIXTURE\n",accepted,duplicates,seen_count,devices);
            }
        } else reject(j,400,"INVALID_INPUT");
    } else reject(j,400,"INVALID_INPUT");
    cJSON_Delete(json);
}
static void process_job(col_job *j)
{
    if(j->kind==COL_SET_CREDENTIALS) {
        col_credentials *c=&j->credentials;
        bool ok=!strcmp(c->ssid,"Wokwi-GUEST") && !*c->password && !strcmp(c->base_url,"https://fixture.wokwi.invalid") &&
            !strcmp(c->node_id,"Wokwi") && !strcmp(c->token,fixture_token) && !strcmp(c->ca,fixture_ca);
        atomic_store(&configured,ok); j->transport=ok?ESP_OK:ESP_ERR_INVALID_ARG; j->status=0; j->diagnostic=NULL;
        col_wipe(c,sizeof(*c));
        printf("WOKWI_FIXTURE_CREDENTIALS accepted=%u NETWORK=FIXTURE TLS=NOT_RUN\n",ok?1U:0U);
    } else if(j->kind==COL_CLEAR_CREDENTIALS) {
        atomic_store(&configured,false); j->transport=ESP_OK; j->status=0; j->diagnostic=NULL;
        printf("WOKWI_FIXTURE_CLEAR NETWORK=FIXTURE\n");
    } else request(j);
}
static void worker(void *unused)
{
    (void)unused;
    for(;;) { col_job *j=NULL; if(xQueueReceive(jobs,&j,portMAX_DELAY)==pdTRUE && j) {
        process_job(j); xQueueSend(results,&j,portMAX_DELAY);
    } }
}
esp_err_t col_network_init(QueueHandle_t in,QueueHandle_t out)
{
    if(task) return ESP_ERR_INVALID_STATE;
    if(!in || !out) return ESP_ERR_INVALID_ARG;
    jobs=in; results=out;
    if(xTaskCreate(worker,"wokwi_fixture",6144,NULL,4,&task)!=pdPASS) return ESP_ERR_NO_MEM;
    printf("WOKWI_FIXTURE_READY NETWORK=FIXTURE WIFI=NOT_RUN TLS=NOT_RUN\n");
    return ESP_OK;
}
