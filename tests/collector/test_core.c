#include "collector_core.h"
#include "cJSON.h"
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 使用任务书中的合成广播和日期，不连接任何真实节点。 */
static unsigned assertions, groups;
#define CHECK(x) do { assertions++; if (!(x)) { \
    fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
#define RUN(fn) do { fn(); groups++; puts("PASS " #fn); } while (0)
static const int64_t NOW=1792717202000LL; /* 2026-10-23T01:00:02.000Z */
static const char *CONFIG="{\"schema_version\":\"1.0\",\"node_id\":\"firmware-host-test\","
    "\"scan_enabled\":true,\"aggregation_window_ms\":2000,\"heartbeat_interval_sec\":30,"
    "\"max_batch_size\":100,\"config_revision\":1,\"strong_signal_dbm\":-60,"
    "\"weak_signal_dbm\":-85,\"active_threshold_sec\":45,\"stale_threshold_sec\":120,\"retention_days\":7}";
static const char *NODE="firmware-host-test";
static col_store store;
static char body[COL_BODY];
static int fail_after=-1;
static unsigned allocations, live_allocations;
static void *json_alloc(size_t n)
{
    if (fail_after>=0 && allocations++==(unsigned)fail_after) return NULL;
    void *p=malloc(n); if (p) live_allocations++; return p;
}
static void json_free(void *p) { if (p) { CHECK(live_allocations>0); live_allocations--; free(p); } }
static const cJSON *get(const cJSON *j,const char *key)
{ const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,key); CHECK(v!=NULL); return v; }
static void equal_string(const cJSON *j,const char *key,const char *expected)
{ const cJSON *v=get(j,key); CHECK(cJSON_IsString(v)); CHECK(!strcmp(v->valuestring,expected)); }
static void equal_number(const cJSON *j,const char *key,double expected)
{ const cJSON *v=get(j,key); CHECK(cJSON_IsNumber(v)); CHECK(v->valuedouble==expected); }
static col_sample sample(unsigned address,int rssi,int64_t mono)
{
    const uint8_t a[]={address,0,0,0xee,0xff,0xc0}; col_sample p;
    CHECK(col_ad(&p,a,1,rssi,NULL,0)); p.mono_ms=mono; p.utc_ms=NOW+mono;
    return p;
}
static col_event event(unsigned id,int64_t created)
{
    col_event e={0}; snprintf(e.id,sizeof(e.id),"boot-test-%u",id);
    e.meta=sample(id,-60,0).meta; e.utc_ms=NOW; e.created_ms=created;
    e.count=4; e.window_ms=2000; e.min=-63; e.max=-54; e.median_x2=-117;
    return e;
}
static void test_time(void)
{
    struct { const char *text; int64_t value; } cases[]={
        {"1970-01-01T00:00:00Z",0},{"1969-12-31T23:59:59.999Z",-1},
        {"2000-02-29T12:34:56.789Z",951827696789LL},{"2026-10-23T01:00:02.000Z",1792717202000LL},
        {"0000-01-01T00:00:00.000Z",-62167219200000LL},{"9999-12-31T23:59:59.999Z",253402300799999LL}};
    for (size_t i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        int64_t t=42,roundtrip=42; char text[25];
        CHECK(col_time_parse(cases[i].text,&t)); CHECK(t==cases[i].value);
        CHECK(col_time_format(t,text)); CHECK(col_time_parse(text,&roundtrip)); CHECK(t==roundtrip);
    }
    const char *bad[]={NULL,"","2026-02-29T00:00:00Z","1900-02-29T00:00:00Z",
        "2026-04-31T00:00:00Z","2026-00-01T00:00:00Z","2026-13-01T00:00:00Z",
        "2026-01-00T00:00:00Z","2026-01-01T24:00:00Z","2026-01-01T00:60:00Z",
        "2026-01-01T00:00:60Z","2026-01-01T00:00:00+00:00","2026-01-01t00:00:00Z",
        "2026-01-01T00:00:00.1Z","2026-01-01T00:00:00.0000Z","2026-01-01T00:00:00Zjunk"};
    for (size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++) { int64_t t=42; CHECK(!col_time_parse(bad[i],&t)); CHECK(t==42); }
    char text[25]; CHECK(!col_time_format(INT64_MIN,text)); CHECK(!col_time_format(INT64_MAX,text));
    CHECK(col_time_allowed(NOW+60000,NOW,false)); CHECK(!col_time_allowed(NOW+60001,NOW,false));
    CHECK(col_time_allowed(NOW-604800000,NOW,false)); CHECK(!col_time_allowed(NOW-604800001,NOW,false));
    CHECK(col_time_allowed(0,NOW,true)); CHECK(!col_time_allowed(NOW+60001,NOW,true));
    CHECK(!col_time_allowed(INT64_MAX,INT64_MIN,true)); CHECK(!col_time_allowed(INT64_MIN,INT64_MAX,false));
    CHECK(col_time_allowed(INT64_MIN,INT64_MIN,false));
}
static void test_utf8_and_ids(void)
{
    CHECK(col_id("boot_A-1",80)); CHECK(!col_id("",80)); CHECK(!col_id("abc:",80)); CHECK(!col_id("abc",2));
    CHECK(col_utf8("广播测试")); CHECK(!col_utf8("a\n")); CHECK(!col_utf8("\xed\xa0\x80"));
    const uint8_t bytes[]={'A',0,1,31,127,0xff,0xc0,0xaf,0xe4,0xb8,0xad,0xf0,0x9f,0x98,0x80};
    char clean[20]; CHECK(col_clean_utf8(clean,sizeof(clean),bytes,sizeof(bytes))==8);
    CHECK(!strcmp(clean,"A中😀")); CHECK(col_utf8(clean));
    CHECK(col_clean_utf8(clean,4,bytes,sizeof(bytes))==1); CHECK(!strcmp(clean,"A"));
    CHECK(col_clean_utf8(clean,5,bytes,sizeof(bytes))==4); CHECK(!strcmp(clean,"A中"));
    CHECK(col_clean_utf8(clean,0,bytes,sizeof(bytes))==0);
}
static void test_ad(void)
{
    const uint8_t address[]={1,0,0,0xee,0xff,0xc0}; col_sample p;
    const uint8_t ad[]={
        7,9,0xe8,0x93,0x9d,0xe7,0x89,0x99, /* 蓝牙 */
        5,3,0x0d,0x18,0x0f,0x18,
        5,5,0x78,0x56,0x34,0x12,
        17,7,0xff,0xee,0xdd,0xcc,0xbb,0xaa,0x99,0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11,0x00,
        3,3,0x0d,0x18, 2,10,0xf6, 5,0xff,0x4c,0x00,0xab,0xcd};
    CHECK(col_ad(&p,address,1,-60,ad,sizeof(ad))); CHECK(!strcmp(p.meta.address,"C0:FF:EE:00:00:01"));
    CHECK(p.meta.address_type==2); CHECK(!strcmp(p.meta.name,"蓝牙")); CHECK(p.meta.uuid_count==4);
    CHECK(!strcmp(p.meta.uuids[0],"180D")); CHECK(!strcmp(p.meta.uuids[1],"180F"));
    CHECK(!strcmp(p.meta.uuids[2],"12345678")); CHECK(!strcmp(p.meta.uuids[3],"00112233-4455-6677-8899-AABBCCDDEEFF"));
    CHECK(p.meta.has_tx && p.meta.tx==-10); CHECK(p.meta.has_manufacturer && p.meta.manufacturer==76);
    CHECK(col_ad(&p,address,0,-127,NULL,0)); CHECK(p.meta.address_type==1 && !p.meta.name[0]);
    CHECK(!p.meta.has_tx && !p.meta.has_manufacturer && !p.meta.uuid_count);
    const int invalid_rssi[]={-128,0,127,128};
    for (size_t i=0;i<sizeof(invalid_rssi)/sizeof(invalid_rssi[0]);i++) CHECK(!col_ad(&p,address,1,invalid_rssi[i],NULL,0));
    uint8_t a[6]={0};
    for (unsigned bits=0;bits<4;bits++) { a[5]=(uint8_t)(bits<<6); CHECK(col_ad(&p,a,1,-1,NULL,0)); CHECK(p.meta.address_type==(bits==3?2:bits==2?0:3)); }
    const uint8_t short_ad[]={4,9,'A'}; CHECK(!col_ad(&p,address,1,-60,short_ad,sizeof(short_ad)));
    const uint8_t bad_uuid[]={2,3,0x18}; CHECK(!col_ad(&p,address,1,-60,bad_uuid,sizeof(bad_uuid)));
    const uint8_t unknown[]={2,10,127,2,10,128}; CHECK(col_ad(&p,address,1,-60,unknown,sizeof(unknown))); CHECK(!p.meta.has_tx);
    const uint8_t duplicates[]={2,9,'A',2,9,'B',5,3,0x0d,0x18,0x0d,0x18};
    CHECK(col_ad(&p,address,1,-60,duplicates,sizeof(duplicates))); CHECK(!strcmp(p.meta.name,"B")); CHECK(p.meta.uuid_count==1);
    uint8_t huge[132]={131,9}; memset(huge+2,'x',130); CHECK(col_ad(&p,address,1,-60,huge,sizeof(huge))); CHECK(strlen(p.meta.name)==128);
    CHECK(!col_ad(&p,address,1,-60,NULL,1));
}
static uint32_t random_state=0x8badf00d;
static unsigned deterministic_random(void)
{ random_state=random_state*1664525U+1013904223U; return random_state; }
static int compare_rssi(const void *a,const void *b) { return *(const int *)a-*(const int *)b; }
static void test_median_oracle_and_ad_boundaries(void)
{
    for (unsigned count=1;count<=257;count++) {
        int values[257]; col_store_init(&store,"oracle");
        for (unsigned i=0;i<count;i++) {
            values[i]=(int)(deterministic_random()%127)-127;
            col_sample p=sample(1,values[i],i); col_ingest(&store,&p,2000);
        }
        qsort(values,count,sizeof(values[0]),compare_rssi); col_window_close(&store,2000,2000);
        CHECK(store.count==1 && store.queue[0].count==count);
        CHECK(store.queue[0].min==values[0] && store.queue[0].max==values[count-1]);
        CHECK(store.queue[0].median_x2==values[(count-1)/2]+values[count/2]);
    }
    const uint8_t address[]={1,2,3,4,5,6};
    for (size_t length=0;length<=255;length++) for (unsigned iteration=0;iteration<16;iteration++) {
        uint8_t *ad=length?malloc(length):NULL; CHECK(!length || ad);
        for (size_t i=0;i<length;i++) ad[i]=(uint8_t)deterministic_random();
        col_sample p;
        if (col_ad(&p,address,0,-60,ad,length)) {
            CHECK(col_utf8(p.meta.name)); CHECK(p.meta.uuid_count<=COL_UUIDS);
            col_event e=event(1,0); e.meta=p.meta; CHECK(col_event_valid(&e,NOW));
        }
        free(ad);
    }
}
static void test_aggregate(void)
{
    col_store_init(&store,"boot-one");
    int values[]={-63,-60,-57,-54};
    for (unsigned i=0;i<4;i++) { col_sample p=sample(1,values[i],i*100); col_ingest(&store,&p,2000); }
    col_window_close(&store,1999,2000); CHECK(store.count==0);
    col_window_close(&store,2000,2000); CHECK(store.count==1);
    col_event *e=&store.queue[0]; CHECK(e->count==4 && e->min==-63 && e->max==-54 && e->median_x2==-117);
    CHECK(e->utc_ms==NOW+300 && e->window_ms==2000); CHECK(!strcmp(e->id,"boot-one-1"));
    col_window_close(&store,5000,2000); CHECK(store.count==1);
    int odd[]={-70,-50,-60};
    for (unsigned i=0;i<3;i++) { col_sample p=sample(1,odd[i],5000+i*100); col_ingest(&store,&p,2000); }
    col_window_close(&store,7000,2000); CHECK(store.queue[1].median_x2==-120 && store.queue[1].count==3);
    col_sample p=sample(2,-127,8000); col_ingest(&store,&p,2000); p.rssi=0; col_ingest(&store,&p,2000);
    p.rssi=127; col_ingest(&store,&p,2000); CHECK(store.raw_invalid==2);
    col_window_close(&store,10000,2000); CHECK(store.queue[2].median_x2==-254 && store.queue[2].count==1);
    col_store_init(&store,"boot-two"); p=sample(1,-60,0); col_ingest(&store,&p,2000); col_window_close(&store,2000,2000);
    CHECK(!strcmp(store.queue[0].id,"boot-two-1"));
}
static void test_window_resets_and_capacity(void)
{
    col_store_init(&store,"reset"); col_sample p=sample(1,-60,0); col_ingest(&store,&p,2000);
    col_window_reset(&store); CHECK(store.partial_windows==1 && store.count==0 && !store.window_open);
    p=sample(1,-40,500); col_ingest(&store,&p,3000); col_window_close(&store,3499,3000); CHECK(store.count==0);
    col_window_close(&store,3500,3000); CHECK(store.count==1 && store.queue[0].count==1 && store.queue[0].window_ms==3000);
    col_store_init(&store,"clock"); p=sample(1,-60,0); col_ingest(&store,&p,2000);
    p=sample(1,-50,100); p.utc_ms+=5000; col_ingest(&store,&p,2000); CHECK(store.partial_windows==1);
    col_window_close(&store,2100,2000); CHECK(store.count==1 && store.queue[0].count==1 && store.queue[0].median_x2==-100);
    col_store_init(&store,"bounded");
    for (unsigned i=0;i<COL_ADDRS+1;i++) { p=sample(i+1,-60,0); col_ingest(&store,&p,2000); }
    CHECK(store.raw_overflow==1); col_window_close(&store,2000,2000); CHECK(store.count==COL_ADDRS);
    p=sample(99,-60,2000); col_ingest(&store,&p,2000); CHECK(store.admission_wait);
    p=sample(99,-60,2250); col_ingest(&store,&p,2000); CHECK(!store.admission_wait);
    col_window_close(&store,4000,2000);
    CHECK(store.count==COL_ADDRS+1 && !strcmp(store.queue[COL_ADDRS].meta.address,"C0:FF:EE:00:00:63"));
    col_store_init(&store,"max-count"); p=sample(1,-60,0);
    for (unsigned i=0;i<100001;i++) col_ingest(&store,&p,2000);
    CHECK(store.raw_overflow==1); col_window_close(&store,2000,2000); CHECK(store.queue[0].count==100000);
    CHECK(sizeof(col_store)<=65536);
}
static void test_fair_admission(void)
{
    bool seen[65]={0}; col_store_init(&store,"fair");
    for (unsigned window=0;window<(64+COL_ADDRS-1)/COL_ADDRS;window++) {
        for (unsigned round=0;round<2;round++) for (unsigned i=1;i<=64;i++) {
            col_sample p=sample(i,-60,window*2000+round*100); col_ingest(&store,&p,2000);
        }
        col_window_close(&store,(window+1)*2000,2000);
        for (size_t i=0;i<store.count;i++) {
            unsigned tail=0; CHECK(sscanf(store.queue[i].meta.address+15,"%2x",&tail)==1);
            CHECK(tail>=1 && tail<=64); seen[tail]=true;
        }
        store.pinned=store.count; col_confirm(&store,true);
    }
    for (unsigned i=1;i<=64;i++) CHECK(seen[i]);
    CHECK(store.raw_overflow>0 && store.dropped==0);
    col_store_init(&store,"overflow-sequence"); store.sequence=UINT64_MAX;
    col_sample p=sample(1,-60,0); col_ingest(&store,&p,2000); col_window_close(&store,2000,2000);
    CHECK(store.count==0 && store.dropped==1);
    for (size_t i=0;i<COL_ADDRS;i++) CHECK(store.buckets[i].count==0);
}
static void test_queue_and_inflight(void)
{
    col_store_init(&store,"queue");
    for (unsigned i=0;i<COL_QUEUE;i++) { col_event e=event(i,100); CHECK(col_enqueue(&store,&e)); }
    CHECK(col_batch(&store,NODE,"batch-frozen",NOW,32,body,sizeof(body)));
    size_t in_flight=store.pinned; CHECK(in_flight>0 && in_flight<=32);
    col_event frozen[32]; size_t frozen_bytes=in_flight*sizeof(frozen[0]);
    memcpy(frozen,store.queue,frozen_bytes);
    for (unsigned i=COL_QUEUE;i<COL_QUEUE+100;i++) { col_event e=event(i,200); CHECK(col_enqueue(&store,&e)); }
    CHECK(store.count==COL_QUEUE && store.dropped==100); CHECK(!memcmp(frozen,store.queue,frozen_bytes));
    char oldest[64]; snprintf(oldest,sizeof(oldest),"boot-test-%zu",100+in_flight);
    CHECK(!strcmp(store.queue[in_flight].id,oldest));
    col_expire(&store,COL_TTL_MS+200);
    CHECK(store.count==in_flight && store.pinned==in_flight && store.dropped==COL_QUEUE+100-in_flight);
    CHECK(!memcmp(frozen,store.queue,frozen_bytes));
    col_confirm(&store,false); CHECK(store.count==in_flight && store.pinned==0);
    CHECK(col_batch(&store,NODE,"batch-retry",NOW,32,body,sizeof(body)));
    CHECK(store.pinned==in_flight && !memcmp(frozen,store.queue,frozen_bytes));
    col_confirm(&store,true); CHECK(store.count==0);
    col_event e=event(1,1000); CHECK(col_enqueue(&store,&e));
    col_expire(&store,1000+COL_TTL_MS-1); CHECK(store.count==1);
    col_expire(&store,1000+COL_TTL_MS); CHECK(store.count==0);
    for (unsigned i=0;i<COL_QUEUE;i++) { e=event(i,0); CHECK(col_enqueue(&store,&e)); }
    store.pinned=store.count; uint64_t dropped=store.dropped; CHECK(!col_enqueue(&store,&e));
    CHECK(store.count==COL_QUEUE && store.dropped==dropped+1);
    col_discard_pinned(&store); CHECK(store.count==0 && store.dropped==dropped+COL_QUEUE+1);
}
static void test_small_body_full_field_batches(void)
{
    col_store_init(&store,"full-fields"); col_event originals[64];
    char node[81],batch_id[81],large_buffer[32768];
    memset(node,'n',80); node[80]=0; memset(batch_id,'b',80); batch_id[80]=0;
    for (unsigned i=0;i<64;i++) {
        col_event e=event(i,0); snprintf(e.id,sizeof(e.id),"%060u-%02u",0,i);
        for (size_t k=0;k<128;k++) e.meta.name[k]=k%2?'\\':'"'; e.meta.name[128]=0;
        e.meta.has_tx=true; e.meta.tx=-127; e.meta.has_manufacturer=true; e.meta.manufacturer=65535;
        e.meta.uuid_count=COL_UUIDS;
        for (unsigned k=0;k<COL_UUIDS;k++)
            snprintf(e.meta.uuids[k],sizeof(e.meta.uuids[k]),"FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFF%X",k);
        e.count=100000; e.window_ms=3000; e.min=-127; e.max=-126; e.median_x2=-253;
        CHECK(col_event_valid(&e,NOW)); originals[i]=e;
    }
    size_t sent=0,produced=0,batches=0;
    while (sent<64) {
        if (!store.count) {
            /* 64 个事件分轮生产，不要求全部同时装入受限队列。 */
            size_t round_count=64-produced;
            if (round_count>COL_QUEUE) round_count=COL_QUEUE;
            for (size_t i=0;i<round_count;i++) CHECK(col_enqueue(&store,&originals[produced++]));
            CHECK(store.count==round_count && store.dropped==0);
        }
        /* 调用者提供更大缓冲也不能绕过本机上限。 */
        memset(large_buffer,0xa5,sizeof(large_buffer));
        CHECK(col_batch(&store,node,batch_id,NOW,100,large_buffer,sizeof(large_buffer)));
        size_t n=store.pinned; CHECK(n>0 && n<=32 && n<=store.count);
        CHECK(strlen(large_buffer)<COL_BODY && strlen(large_buffer)<=32768);
        for (size_t i=COL_BODY;i<sizeof(large_buffer);i++) CHECK((unsigned char)large_buffer[i]==0xa5);
        if (!batches) CHECK(n<COL_QUEUE && n<32); /* 最大字段会触发字节上限拆批。 */
        CHECK(!memcmp(store.queue,originals+sent,n*sizeof(originals[0])));
        cJSON *j=cJSON_Parse(large_buffer); CHECK(j); const cJSON *array=get(j,"observations");
        CHECK(cJSON_GetArraySize(array)==(int)n);
        for (size_t i=0;i<n;i++) {
            const col_event *e=&originals[sent+i]; const cJSON *o=cJSON_GetArrayItem(array,(int)i);
            CHECK(cJSON_GetArraySize(o)==14);
            equal_string(o,"event_id",e->id); equal_string(o,"local_device_id",e->meta.address);
            equal_string(o,"local_name",e->meta.name); equal_string(o,"observed_at","2026-10-23T01:00:02.000Z");
            equal_number(o,"sample_count",100000); equal_number(o,"window_ms",3000);
            equal_number(o,"rssi_dbm",-126.5); equal_number(o,"rssi_min_dbm",-127); equal_number(o,"rssi_max_dbm",-126);
            equal_number(o,"tx_power_dbm",-127); equal_number(o,"manufacturer_id",65535);
            const cJSON *uuids=get(o,"service_uuids"); CHECK(cJSON_GetArraySize(uuids)==COL_UUIDS);
            for (unsigned k=0;k<COL_UUIDS;k++) {
                const cJSON *uuid=cJSON_GetArrayItem(uuids,(int)k); CHECK(cJSON_IsString(uuid));
                CHECK(!strcmp(uuid->valuestring,e->meta.uuids[k]));
            }
        }
        if (!batches) {
            char *before=cJSON_PrintUnformatted(array); CHECK(before);
            col_confirm(&store,false); CHECK(store.count==produced && store.pinned==0);
            CHECK(col_batch(&store,node,batch_id,NOW,100,large_buffer,sizeof(large_buffer)));
            CHECK(store.pinned==n && !memcmp(store.queue,originals,n*sizeof(originals[0])));
            cJSON *retry=cJSON_Parse(large_buffer); CHECK(retry);
            char *after=cJSON_PrintUnformatted(get(retry,"observations")); CHECK(after);
            CHECK(!strcmp(before,after)); cJSON_free(before); cJSON_free(after); cJSON_Delete(retry);
        }
        cJSON_Delete(j); col_confirm(&store,true); sent+=n; batches++;
        CHECK(store.count==produced-sent && store.count<=COL_QUEUE && store.pinned==0 && store.dropped==0);
    }
    CHECK(sent==64 && produced==64 && batches>1);
    /* 配置给出更小条数时，字节空间足够也必须遵守该上限。 */
    CHECK(col_enqueue(&store,&originals[0])); CHECK(col_enqueue(&store,&originals[1]));
    CHECK(col_batch(&store,node,batch_id,NOW,1,body,sizeof(body))); CHECK(store.pinned==1);
    col_confirm(&store,true); CHECK(store.count==1 && !strcmp(store.queue[0].id,originals[1].id));
}
static void test_protocol_and_bounds(void)
{
    col_store_init(&store,"protocol"); col_event e=event(1,0); CHECK(col_enqueue(&store,&e));
    CHECK(col_batch(&store,NODE,"fixture-batch",NOW,32,body,sizeof(body)));
    cJSON *j=cJSON_Parse(body); CHECK(j); CHECK(cJSON_GetArraySize(j)==5);
    equal_string(j,"schema_version","1.0"); equal_string(j,"node_id",NODE);
    equal_string(j,"sent_at","2026-10-23T01:00:02.000Z");
    const cJSON *a=get(j,"observations"); CHECK(cJSON_GetArraySize(a)==1);
    const cJSON *o=cJSON_GetArrayItem(a,0); CHECK(cJSON_GetArraySize(o)==14);
    equal_number(o,"rssi_dbm",-58.5); equal_number(o,"sample_count",4);
    equal_string(o,"identifier_type","ble_address"); equal_string(o,"address_type","random_static");
    CHECK(cJSON_IsNull(get(o,"local_name"))); CHECK(cJSON_IsNull(get(o,"tx_power_dbm")));
    CHECK(cJSON_IsNull(get(o,"manufacturer_id"))); CHECK(cJSON_GetArraySize(get(o,"service_uuids"))==0);
    cJSON_Delete(j); col_confirm(&store,false);
    CHECK(!col_batch(&store,NODE,"small",NOW,32,body,8)); CHECK(store.pinned==0 && store.count==1);
    CHECK(!col_batch(&store,NODE,"small",NOW,32,body,200)); CHECK(store.pinned==0 && store.count==1);
    e=event(2,0); CHECK(col_enqueue(&store,&e));
    CHECK(col_batch(&store,NODE,"split",NOW,32,body,700)); CHECK(store.pinned==1);
    j=cJSON_Parse(body); CHECK(j); CHECK(cJSON_GetArraySize(get(j,"observations"))==1); cJSON_Delete(j);
    col_confirm(&store,true); CHECK(store.count==1 && !strcmp(store.queue[0].id,"boot-test-2"));
    e=event(3,0); e.meta.has_tx=true; e.meta.tx=127; CHECK(!col_event_valid(&e,NOW));
    e.meta.tx=-128; CHECK(!col_event_valid(&e,NOW)); e.meta.tx=126; CHECK(col_event_valid(&e,NOW));
    e.meta.uuid_count=1; strcpy(e.meta.uuids[0],"0x180D"); CHECK(!col_event_valid(&e,NOW));
    strcpy(e.meta.uuids[0],"180D"); CHECK(col_event_valid(&e,NOW));
    memset(e.meta.uuids[0],'F',37); CHECK(!col_event_valid(&e,NOW));
    e=event(3,0); e.min=-40; CHECK(!col_event_valid(&e,NOW));
    e=event(3,0); e.utc_ms=NOW+60001; CHECK(!col_event_valid(&e,NOW));
    e=event(3,0); memset(e.meta.name,'x',sizeof(e.meta.name)); CHECK(!col_event_valid(&e,NOW));
    store.queue[0].count=0; CHECK(!col_batch(&store,NODE,"bad",NOW,32,body,sizeof(body)));
    CHECK(store.count==0 && store.pinned==0 && store.dropped==1);
}
static void test_heartbeat(void)
{
    col_store_init(&store,"heartbeat");
    CHECK(col_heartbeat(NODE,NOW,"paused",true,&store,NULL,0,body,sizeof(body)));
    cJSON *j=cJSON_Parse(body); CHECK(j); CHECK(cJSON_GetArraySize(j)==12);
    equal_string(j,"scan_state","paused"); equal_number(j,"applied_config_revision",0);
    CHECK(cJSON_IsNull(get(j,"last_observation_at"))); CHECK(cJSON_IsNull(get(j,"error_code"))); cJSON_Delete(j);
    store.last_observation=1; CHECK(col_heartbeat(NODE,NOW,"scanning",true,&store,NULL,1,body,sizeof(body)));
    store.last_observation=NOW+60000; CHECK(col_heartbeat(NODE,NOW,"scanning",true,&store,NULL,1,body,sizeof(body)));
    store.last_observation++; CHECK(!col_heartbeat(NODE,NOW,"scanning",true,&store,NULL,1,body,sizeof(body)));
    store.last_observation=0; store.dropped=COL_SAFE_INT;
    CHECK(col_heartbeat(NODE,NOW,"error",false,&store,"DEVICE_LIMIT",COL_SAFE_INT,body,sizeof(body)));
    j=cJSON_Parse(body); CHECK(j); equal_number(j,"dropped_count",9007199254740991.0);
    equal_number(j,"applied_config_revision",9007199254740991.0); cJSON_Delete(j);
    store.dropped++; CHECK(!col_heartbeat(NODE,NOW,"error",false,&store,NULL,1,body,sizeof(body)));
    store.dropped=0; CHECK(!col_heartbeat(NODE,NOW,"offline",false,&store,NULL,1,body,sizeof(body)));
    CHECK(!col_heartbeat(NODE,NOW,"paused",false,&store,"bad\nerror",1,body,sizeof(body)));
    CHECK(!col_heartbeat(NODE,NOW,"paused",false,&store,NULL,COL_SAFE_INT+1,body,sizeof(body)));
}
static void test_config(void)
{
    col_config c={0}; CHECK(col_parse_config(CONFIG,NODE,&c));
    CHECK(c.scan_enabled && c.window_ms==2000 && c.max_batch==100 && c.revision==1);
    const char *invalid[]={"[]","[1]","null","{}","true","{\"schema_version\":\"1.0\",\"schema_version\":\"1.0\"}","{}junk"};
    col_config previous=c;
    for (size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) { CHECK(!col_parse_config(invalid[i],NODE,&c)); CHECK(!memcmp(&c,&previous,sizeof(c))); }
    CHECK(!col_parse_config(CONFIG,"other-node",&c));
    struct { const char *key; double value; } bad[]={
        {"aggregation_window_ms",1999},{"aggregation_window_ms",3001},{"aggregation_window_ms",2000.5},
        {"heartbeat_interval_sec",29},{"max_batch_size",32},{"config_revision",0},
        {"weak_signal_dbm",-60},{"stale_threshold_sec",45},{"retention_days",31}};
    for (size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        cJSON *j=cJSON_Parse(CONFIG); CHECK(cJSON_ReplaceItemInObjectCaseSensitive(j,bad[i].key,cJSON_CreateNumber(bad[i].value)));
        char *text=cJSON_PrintUnformatted(j); CHECK(text); CHECK(!col_parse_config(text,NODE,&c));
        CHECK(!memcmp(&c,&previous,sizeof(c))); cJSON_free(text); cJSON_Delete(j);
    }
    cJSON *j=cJSON_Parse(CONFIG); CHECK(cJSON_AddBoolToObject(j,"enabled",true));
    char *text=cJSON_PrintUnformatted(j); CHECK(!col_parse_config(text,NODE,&c)); cJSON_free(text); cJSON_Delete(j);
    j=cJSON_Parse(CONFIG); CHECK(cJSON_ReplaceItemInObjectCaseSensitive(j,"schema_version",cJSON_CreateString("1.1")));
    text=cJSON_PrintUnformatted(j); CHECK(!col_parse_config(text,NODE,&c)); cJSON_free(text); cJSON_Delete(j);
    j=cJSON_Parse(CONFIG); CHECK(cJSON_ReplaceItemInObjectCaseSensitive(j,"config_revision",cJSON_CreateRaw("9007199254740992")));
    text=cJSON_PrintUnformatted(j); CHECK(!col_parse_config(text,NODE,&c)); cJSON_free(text); cJSON_Delete(j);
}
static cJSON *credentials_json(void)
{
    cJSON *j=cJSON_CreateObject(); CHECK(j);
    CHECK(cJSON_AddStringToObject(j,"ssid","synthetic-test-network"));
    CHECK(cJSON_AddStringToObject(j,"password","test-only"));
    CHECK(cJSON_AddStringToObject(j,"base_url","https://collector.invalid:8443"));
    CHECK(cJSON_AddStringToObject(j,"node_id",NODE));
    CHECK(cJSON_AddStringToObject(j,"token","AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
    CHECK(cJSON_AddStringToObject(j,"ca","-----BEGIN CERTIFICATE-----\nTEST-NOT-A-CERT\n-----END CERTIFICATE-----"));
    return j;
}
static void test_credentials(void)
{
    col_credentials c={0}; cJSON *j=credentials_json(); char *text=cJSON_PrintUnformatted(j);
    CHECK(col_parse_credentials(text,&c)); CHECK(!strcmp(c.node_id,NODE)); cJSON_free(text);
    col_credentials before=c;
    const char *invalid[]={"[]","[1]","[{}]","null","true","{\"ssid\":\"x\\u0000y\"}","{\"ssid\":\"a\",\"ssid\":\"b\"}"};
    for (size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) { CHECK(!col_parse_credentials(invalid[i],&c)); CHECK(!memcmp(&c,&before,sizeof(c))); }
    const char *urls[]={"http://collector.invalid","https://","https://user@host","https://host/path","https://host?query","https://host#fragment","https://host\\path"};
    for (size_t i=0;i<sizeof(urls)/sizeof(urls[0]);i++) {
        CHECK(cJSON_ReplaceItemInObjectCaseSensitive(j,"base_url",cJSON_CreateString(urls[i])));
        text=cJSON_PrintUnformatted(j); CHECK(!col_parse_credentials(text,&c)); cJSON_free(text);
    }
    cJSON_Delete(j); j=credentials_json();
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(j,"password",cJSON_CreateString("literal\\u0000text")));
    text=cJSON_PrintUnformatted(j); CHECK(col_parse_credentials(text,&c)); CHECK(!strcmp(c.password,"literal\\u0000text")); cJSON_free(text);
    CHECK(cJSON_AddStringToObject(j,"unknown","x")); text=cJSON_PrintUnformatted(j);
    CHECK(!col_parse_credentials(text,&c)); cJSON_free(text); cJSON_Delete(j);
}
static void test_json_depth(void)
{
    col_credentials credentials={0};
    /* 任意 USB 输入不能先进入 cJSON 的默认千层递归后再拒绝。 */
    const char *deep[]={"[[[[[[[[[0]]]]]]]]]","{\"x\":{\"x\":{\"x\":{\"x\":{\"x\":{\"x\":{\"x\":{\"x\":{}}}}}}}}}",
        "{\"ignored\":\"\\\"[\\\\\",\"deep\":[[[[[[[[0]]]]]]]]}"};
    for (size_t i=0;i<sizeof(deep)/sizeof(deep[0]);i++) {
        allocations=0; fail_after=INT_MAX;
        CHECK(!col_parse_credentials(deep[i],&credentials));
        fail_after=-1; CHECK(allocations==0 && live_allocations==0);
    }
    /* 八层仍交由实际解析器处理，不把深度限制误设为七层。 */
    allocations=0; fail_after=INT_MAX;
    CHECK(!col_parse_credentials("[[[[[[[[0]]]]]]]]",&credentials));
    fail_after=-1; CHECK(allocations>0 && live_allocations==0);
    const char *passwords[]={"[[[[[[[[[[[{{{{{{{{{{{", "]]]]]]]]]]]}}}}}}}}}}}",
        "quote\" [ { slash\\ [ {", "\\\"[[[[[[[[[[[[[[[[[[[["};
    for (size_t i=0;i<sizeof(passwords)/sizeof(passwords[0]);i++) {
        cJSON *j=credentials_json();
        CHECK(cJSON_ReplaceItemInObjectCaseSensitive(j,"password",cJSON_CreateString(passwords[i])));
        char *text=cJSON_PrintUnformatted(j); CHECK(text);
        CHECK(col_parse_credentials(text,&credentials)); CHECK(!strcmp(credentials.password,passwords[i]));
        cJSON_free(text); cJSON_Delete(j);
    }
    char huge[8192]; memset(huge,'[',4000); memset(huge+4000,']',4000); huge[8000]=0;
    allocations=0; fail_after=INT_MAX;
    CHECK(!col_parse_credentials(huge,&credentials)); fail_after=-1;
    CHECK(allocations==0 && live_allocations==0);
}
static void test_acknowledgments(void)
{
    const char *ok="{\"schema_version\":\"1.0\",\"batch_id\":\"batch-1\",\"accepted\":2,\"duplicates\":1,\"rejected\":0,\"server_time\":\"2026-10-23T01:00:02.100Z\"}";
    CHECK(col_ack(ok,"batch-1",3)); CHECK(!col_ack(ok,"batch-2",3)); CHECK(!col_ack(ok,"batch-1",2));
    CHECK(!col_ack("<html>error</html>","batch-1",3)); CHECK(!col_ack("{\"accepted\":2","batch-1",3));
    const char *keys[]={"accepted","duplicates","rejected","schema_version","server_time"};
    for (size_t i=0;i<5;i++) {
        cJSON *j=cJSON_Parse(ok); cJSON_DeleteItemFromObjectCaseSensitive(j,keys[i]);
        char *text=cJSON_PrintUnformatted(j); CHECK(!col_ack(text,"batch-1",3)); cJSON_free(text); cJSON_Delete(j);
    }
    cJSON *j=cJSON_Parse(ok); CHECK(cJSON_ReplaceItemInObjectCaseSensitive(j,"accepted",cJSON_CreateNumber(1.5)));
    char *text=cJSON_PrintUnformatted(j); CHECK(!col_ack(text,"batch-1",3)); cJSON_free(text); cJSON_Delete(j);
    int64_t revision=0;
    CHECK(col_heartbeat_ack("{\"schema_version\":\"1.0\",\"config_revision\":7,\"server_time\":\"2026-10-23T01:00:02Z\"}",&revision));
    CHECK(revision==7); CHECK(!col_heartbeat_ack("{}",&revision));
    CHECK(!col_heartbeat_ack("{\"schema_version\":\"1.0\",\"config_revision\":9,\"server_time\":\"truncated\"}",&revision));
    CHECK(revision==7);
}
static void test_scheduler(void)
{
    struct { int status; const char *code; col_result result; } cases[]={
        {200,"",COL_ACK},{401,"INVALID_NODE_TOKEN",COL_AUTH},{403,"NODE_MISMATCH",COL_IDENTITY},
        {404,"NOT_FOUND",COL_CONFIG_ERROR},{405,"",COL_CONFIG_ERROR},{415,"CONTENT_TYPE",COL_CONFIG_ERROR},
        {413,"BODY_TOO_LARGE",COL_SPLIT},{429,"RATE_LIMITED",COL_RATE},{400,"INVALID_INPUT",COL_BAD_INPUT},
        {400,"UNSUPPORTED_SCHEMA",COL_SCHEMA},{400,"INVALID_REVISION",COL_REVISION},
        {409,"OBSERVATION_LIMIT",COL_HISTORY_FULL},{409,"DEVICE_LIMIT",COL_DEVICES_FULL},
        {409,"unexpected",COL_CONFIG_ERROR},{500,"INTERNAL_ERROR",COL_RETRY},{503,"",COL_RETRY},{0,"",COL_RETRY}};
    for (size_t i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        char response[128]; snprintf(response,sizeof(response),"{\"error\":{\"code\":\"%s\"}}",cases[i].code);
        CHECK(col_classify(cases[i].status,response)==cases[i].result);
    }
    CHECK(col_classify(502,"not JSON")==COL_RETRY); CHECK(col_classify(401,"not JSON")==COL_AUTH);
    col_schedule s; col_schedule_init(&s); int delays[]={1000,2000,4000,8000,16000,32000,60000,60000};
    for (size_t i=0;i<sizeof(delays)/sizeof(delays[0]);i++) { col_schedule_result(&s,COL_RETRY,100,0); CHECK(s.retry_at==100+delays[i]); }
    col_schedule_result(&s,COL_RETRY,100,250); CHECK(s.retry_at==60350);
    col_schedule_result(&s,COL_ACK,200,0); CHECK(s.failures==0 && s.retry_at==0);
    col_schedule_result(&s,COL_RATE,200,0); CHECK(s.write_at==60200);
    col_schedule_result(&s,COL_ACK,300,0); CHECK(s.write_at==60200);
    col_schedule_result(&s,COL_SPLIT,0,0); CHECK(s.batch_limit==16);
    for (unsigned i=0;i<10;i++) col_schedule_result(&s,COL_SPLIT,0,0);
    CHECK(s.batch_limit==1);
    col_schedule_result(&s,COL_HISTORY_FULL,500,0); CHECK(s.capacity_blocked && s.capacity_at==60500);
    col_schedule_result(&s,COL_ACK,600,0); CHECK(s.capacity_blocked && !strcmp(s.error,"OBSERVATION_LIMIT"));
    col_schedule_init(&s); col_config c; CHECK(col_parse_config(CONFIG,NODE,&c));
    CHECK(col_scan_allowed(true,false,false,true,&c,&s)); CHECK(!col_scan_allowed(true,true,false,true,&c,&s));
    c.scan_enabled=false; CHECK(!col_scan_allowed(true,false,false,true,&c,&s)); c.scan_enabled=true;
    CHECK(!col_scan_allowed(false,false,false,true,&c,&s)); CHECK(!col_scan_allowed(true,false,true,true,&c,&s));
    CHECK(!col_scan_allowed(true,false,false,false,&c,&s));
    col_schedule_result(&s,COL_AUTH,0,0); CHECK(s.auth_blocked && !col_scan_allowed(true,false,false,true,&c,&s));
    col_schedule_result(&s,COL_ACK,0,0); CHECK(s.auth_blocked);
    col_schedule_init(&s); col_schedule_result(&s,COL_SCHEMA,0,0); CHECK(s.protocol_blocked);
}
static void test_json_allocation_failures(void)
{
    bool saw_failure_after_pin=false,saw_success=false;
    for (int failure=0;failure<130;failure++) {
        col_store_init(&store,"oom"); col_event a=event(1,0),b=event(2,0);
        CHECK(col_enqueue(&store,&a)); CHECK(col_enqueue(&store,&b));
        allocations=0; fail_after=failure;
        bool ok=col_batch(&store,NODE,"oom-batch",NOW,2,body,sizeof(body));
        fail_after=-1;
        if (!ok) { CHECK(store.pinned==0); if (failure>40) saw_failure_after_pin=true; }
        else { CHECK(store.pinned==2); cJSON *j=cJSON_Parse(body); CHECK(j); cJSON_Delete(j); saw_success=true; }
        CHECK(store.count==2 && !memcmp(&store.queue[0],&a,sizeof(a)) && !memcmp(&store.queue[1],&b,sizeof(b)));
        CHECK(live_allocations==0);
    }
    CHECK(saw_failure_after_pin && saw_success);
}
static void fixture_write(const char *directory,const char *name,const char *text)
{
    char path[2048]; CHECK(snprintf(path,sizeof(path),"%s/%s",directory,name)<(int)sizeof(path));
    FILE *f=fopen(path,"wb"); CHECK(f); CHECK(fputs(text,f)>=0); CHECK(fputc('\n',f)!=EOF); CHECK(fclose(f)==0);
}
static void emit_fixtures(const char *directory,int64_t now)
{
    col_store_init(&store,"fixture-boot"); int rssi[]={-63,-60,-57,-54};
    for (unsigned i=0;i<4;i++) { col_sample p=sample(1,rssi[i],i*100); p.utc_ms=now-1000+i*100; col_ingest(&store,&p,2000); }
    col_window_close(&store,2000,2000);
    col_event e=event(2,0); e.utc_ms=now-500; strcpy(e.meta.name,"广播测试");
    e.meta.uuid_count=3; strcpy(e.meta.uuids[0],"180D"); strcpy(e.meta.uuids[1],"12345678");
    strcpy(e.meta.uuids[2],"00112233-4455-6677-8899-AABBCCDDEEFF");
    e.meta.has_tx=true; e.meta.tx=-10; e.meta.has_manufacturer=true; e.meta.manufacturer=76;
    CHECK(col_enqueue(&store,&e));
    CHECK(col_batch(&store,NODE,"firmware-c-fixture",now,32,body,sizeof(body)));
    fixture_write(directory,"batch.json",body);
    CHECK(col_heartbeat(NODE,now,"scanning",true,&store,NULL,1,body,sizeof(body)));
    fixture_write(directory,"heartbeat.json",body);
    store.last_observation=1;
    CHECK(col_heartbeat(NODE,now,"paused",true,&store,NULL,1,body,sizeof(body)));
    fixture_write(directory,"heartbeat-historical.json",body);
    puts("PASS emitted C fixtures");
}
int main(int argc,char **argv)
{
    cJSON_Hooks hooks={json_alloc,json_free}; cJSON_InitHooks(&hooks);
    RUN(test_time); RUN(test_utf8_and_ids); RUN(test_ad); RUN(test_median_oracle_and_ad_boundaries); RUN(test_aggregate);
    RUN(test_window_resets_and_capacity); RUN(test_fair_admission); RUN(test_queue_and_inflight);
    RUN(test_small_body_full_field_batches); RUN(test_protocol_and_bounds);
    RUN(test_heartbeat); RUN(test_config); RUN(test_credentials); RUN(test_json_depth); RUN(test_acknowledgments);
    RUN(test_scheduler); RUN(test_json_allocation_failures);
    if (argc==3) { char *end=NULL; int64_t now=strtoll(argv[2],&end,10); CHECK(end && !*end); emit_fixtures(argv[1],now); }
    else CHECK(argc==1);
    CHECK(live_allocations==0);
    printf("collector core: %u groups PASS, %u checks; store=%zu bytes; cJSON=%s\n",groups,assertions,sizeof(col_store),cJSON_Version());
    return 0;
}
