#include "collector_core.h"
#include "cJSON.h"
#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(col_store) <= 65536, "聚合和队列超过 64 KiB");
static const char *address_names[] = {"unknown", "public", "random_static", "random_private"};
static size_t utf8_width(const uint8_t *s, size_t n)
{
    if (!n) return 0;
    if (s[0] < 0x80) return 1;
    size_t w = s[0] >= 0xc2 && s[0] <= 0xdf ? 2 :
               s[0] >= 0xe0 && s[0] <= 0xef ? 3 :
               s[0] >= 0xf0 && s[0] <= 0xf4 ? 4 : 0;
    if (!w || w > n) return 0;
    for (size_t i = 1; i < w; i++) if ((s[i] & 0xc0) != 0x80) return 0;
    if ((s[0] == 0xe0 && s[1] < 0xa0) || (s[0] == 0xed && s[1] >= 0xa0) ||
        (s[0] == 0xf0 && s[1] < 0x90) || (s[0] == 0xf4 && s[1] >= 0x90)) return 0;
    return w;
}
bool col_utf8(const char *s)
{
    if (!s) return false;
    size_t n = strlen(s);
    for (size_t i = 0; i < n;) {
        size_t w = utf8_width((const uint8_t *)s + i, n - i);
        if (!w || (unsigned char)s[i] < 32) return false;
        i += w;
    }
    return true;
}
size_t col_clean_utf8(char *out, size_t cap, const uint8_t *in, size_t n)
{
    size_t j = 0;
    if (!cap) return 0;
    for (size_t i = 0; i < n;) {
        size_t w = utf8_width(in + i, n - i);
        if (!w || in[i] < 32 || in[i] == 127) { i++; continue; }
        if (j + w >= cap) break;
        memcpy(out + j, in + i, w); j += w; i += w;
    }
    out[j] = 0;
    return j;
}
bool col_id(const char *s, size_t max)
{
    if (!s || !*s || strlen(s) > max) return false;
    for (; *s; s++) if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') ||
                         (*s >= '0' && *s <= '9') || *s == '_' || *s == '-')) return false;
    return true;
}
static bool leap(int y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }
static int month_days(int y, int m)
{
    static const int d[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return d[m - 1] + (m == 2 && leap(y));
}
bool col_time_parse(const char *s, int64_t *ms)
{
    if (!s || !ms) return false;
    size_t n = strlen(s);
    if (n != 20 && n != 24) return false;
    for (size_t i = 0; i < n; i++) {
        char expected = i == 4 || i == 7 ? '-' : i == 10 ? 'T' :
                        i == 13 || i == 16 ? ':' : i == n-1 ? 'Z' : i == 19 ? '.' : 0;
        if (expected ? s[i] != expected : s[i] < '0' || s[i] > '9') return false;
    }
    int y = 0,m = 0,d = 0,h = 0,mi = 0,se = 0,sub = 0;
    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &y,&m,&d,&h,&mi,&se) != 6 ||
        m < 1 || m > 12 || d < 1 || d > month_days(y,m) || h > 23 || mi > 59 || se > 59) return false;
    if (n == 24) sub = (s[20]-'0')*100 + (s[21]-'0')*10 + s[22]-'0';
    int64_t days = 0;
    for (int a = 1970; a < y; a++) days += 365 + leap(a);
    for (int a = y; a < 1970; a++) days -= 365 + leap(a);
    for (int a = 1; a < m; a++) days += month_days(y,a);
    *ms = ((days + d - 1)*86400 + h*3600 + mi*60 + se)*1000 + sub;
    return true;
}
bool col_time_format(int64_t ms, char out[25])
{
    if (ms < -62167219200000LL || ms > 253402300799999LL) return false;
    int64_t days = ms / 86400000, rem = ms % 86400000;
    if (rem < 0) { rem += 86400000; days--; }
    int y = 1970, m = 1;
    while (days < 0) { y--; days += 365 + leap(y); }
    while (days >= 365 + leap(y)) { days -= 365 + leap(y); y++; }
    while (days >= month_days(y,m)) { days -= month_days(y,m); m++; }
    return snprintf(out,25,"%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",y,m,(int)days+1,
        (int)(rem/3600000),(int)(rem/60000%60),(int)(rem/1000%60),(int)(rem%1000)) == 24;
}
bool col_time_allowed(int64_t ms, int64_t now, bool historical)
{
    /* 差值用无符号数比较，避免边界输入发生有符号溢出。 */
    if (ms > now) return (uint64_t)ms - (uint64_t)now <= 60000;
    return historical || (uint64_t)now - (uint64_t)ms <= 604800000;
}

bool col_ad(col_sample *out, const uint8_t a[6], unsigned type, int rssi,
            const uint8_t *ad, size_t len)
{
    if (!out || !a || (!ad && len)) return false;
    memset(out,0,sizeof(*out));
    if (rssi < -127 || rssi >= 0) return false;
    out->rssi = (int8_t)rssi;
    snprintf(out->meta.address,18,"%02X:%02X:%02X:%02X:%02X:%02X",a[5],a[4],a[3],a[2],a[1],a[0]);
    out->meta.address_type = type == 0 ? 1 : type == 1 ?
        ((a[5] & 0xc0) == 0xc0 ? 2 : (a[5] & 0xc0) == 0x80 ? 0 : 3) : 0;
    for (size_t i = 0; i < len;) {
        size_t field = ad[i++];
        if (!field) break;
        if (field > len-i) return false;
        uint8_t t = ad[i++]; size_t n = field-1; const uint8_t *p = ad+i;
        if (t == 8 || t == 9) col_clean_utf8(out->meta.name,sizeof(out->meta.name),p,n);
        else if (t == 0x0a && n == 1 && p[0] != 127 && p[0] != 128) {
            out->meta.tx = (int8_t)p[0]; out->meta.has_tx = true;
        } else if (t == 0xff && n >= 2) {
            out->meta.manufacturer = (uint16_t)p[0] | ((uint16_t)p[1]<<8); out->meta.has_manufacturer = true;
        } else if (t >= 2 && t <= 7) {
            size_t w = t <= 3 ? 2 : t <= 5 ? 4 : 16;
            if (n % w) return false;
            for (size_t j = 0; j < n && out->meta.uuid_count < COL_UUIDS; j += w) {
                char uuid[37]; size_t k = 0;
                for (size_t b = 0; b < w; b++) {
                    if (w == 16 && (b == 4 || b == 6 || b == 8 || b == 10)) uuid[k++] = '-';
                    snprintf(uuid+k,3,"%02X",p[j+w-b-1]); k+=2;
                }
                bool duplicate = false;
                for (unsigned b = 0; b < out->meta.uuid_count; b++)
                    if (!strcmp(uuid,out->meta.uuids[b])) duplicate = true;
                if (!duplicate) strcpy(out->meta.uuids[out->meta.uuid_count++],uuid);
            }
        }
        i += n;
    }
    return true;
}
void col_store_init(col_store *s, const char *boot)
{ memset(s,0,sizeof(*s)); snprintf(s->boot,sizeof(s->boot),"%.32s",boot); }
void col_window_reset(col_store *s)
{
    bool had = false;
    for (size_t i=0;i<COL_ADDRS;i++) {
        had |= s->buckets[i].count != 0;
        s->buckets[i].count = 0;
        memset(s->buckets[i].hist,0,sizeof(s->buckets[i].hist));
    }
    if (had) s->partial_windows++;
    s->window_open = false;
    s->admission_wait=false; s->next_address[0]=0;
}
static void remove_at(col_store *s, size_t i)
{ memmove(s->queue+i,s->queue+i+1,(s->count-i-1)*sizeof(col_event)); s->count--; }
void col_expire(col_store *s, int64_t now)
{
    for (size_t i=s->pinned;i<s->count;)
        if (now-s->queue[i].created_ms >= COL_TTL_MS) { remove_at(s,i); s->dropped++; } else i++;
}
bool col_enqueue(col_store *s, const col_event *e)
{
    if (s->count == COL_QUEUE) {
        if (s->pinned == s->count) { s->dropped++; return false; }
        remove_at(s,s->pinned); s->dropped++;
    }
    s->queue[s->count++] = *e; return true;
}
void col_confirm(col_store *s, bool accepted)
{
    if (accepted) {
        size_t n = s->pinned;
        memmove(s->queue,s->queue+n,(s->count-n)*sizeof(col_event)); s->count-=n;
    }
    s->pinned=0;
}
void col_discard_pinned(col_store *s)
{ s->dropped += s->pinned; col_confirm(s,true); }
void col_window_close(col_store *s, int64_t now, int window_ms)
{
    if (!s->window_open || now-s->window_start < window_ms) return;
    for (size_t i=0;i<COL_ADDRS;i++) {
        col_bucket *b=&s->buckets[i]; if (!b->count) continue;
        col_event e={0}; e.meta=b->meta; e.utc_ms=b->utc_ms; e.created_ms=now;
        e.count=b->count; e.window_ms=(uint16_t)window_ms;
        uint32_t a=(b->count-1)/2, z=b->count/2, total=0;
        int low=0,high=0; bool first=true;
        for (int h=0;h<127;h++) if (b->hist[h]) {
            if (first) { e.min=(int8_t)(h-127); first=false; }
            e.max=(int8_t)(h-127);
            if (total<=a && total+b->hist[h]>a) low=h-127;
            if (total<=z && total+b->hist[h]>z) high=h-127;
            total+=b->hist[h];
        }
        e.median_x2=(int16_t)(low+high);
        if (s->sequence == UINT64_MAX) s->dropped++;
        else {
            snprintf(e.id,sizeof(e.id),"%s-%" PRIu64,s->boot,++s->sequence);
            col_enqueue(s,&e);
        }
        b->count=0; memset(b->hist,0,sizeof(b->hist));
    }
    s->window_open=false;
    s->admission_wait=s->next_address[0]!=0;
}
void col_ingest(col_store *s, const col_sample *p, int window_ms)
{
    if (p->rssi < -127 || p->rssi >= 0 || p->utc_ms <= 0) { s->raw_invalid++; return; }
    int64_t offset=p->utc_ms-p->mono_ms;
    if (s->offset_set && llabs(offset-s->utc_offset)>2000) col_window_reset(s);
    s->utc_offset=offset; s->offset_set=true;
    col_window_close(s,p->mono_ms,window_ms);
    if (!s->window_open) { s->window_start=p->mono_ms; s->window_open=true; }
    if (s->admission_wait) {
        if ((strcmp(p->meta.address,s->next_address) || p->meta.address_type!=s->next_address_type) &&
            p->mono_ms-s->window_start<250) { s->raw_overflow++; return; }
        s->admission_wait=false; s->next_address[0]=0;
    }
    col_bucket *b=NULL;
    for (size_t i=0;i<COL_ADDRS;i++) if (s->buckets[i].used &&
        !strcmp(s->buckets[i].meta.address,p->meta.address) &&
        s->buckets[i].meta.address_type==p->meta.address_type) { b=&s->buckets[i]; break; }
    if (!b) for (size_t i=0;i<COL_ADDRS;i++) {
        size_t j=(s->cursor+i)%COL_ADDRS;
        if (!s->buckets[j].count) {
            b=&s->buckets[j]; memset(b,0,sizeof(*b)); b->meta=p->meta;
            b->used=true; s->cursor=(j+1)%COL_ADDRS; break;
        }
    }
    if (!b || b->count==100000) {
        if (!b && !s->next_address[0]) {
            strcpy(s->next_address,p->meta.address); s->next_address_type=p->meta.address_type;
        }
        s->raw_overflow++; return;
    }
    if (p->meta.name[0]) strcpy(b->meta.name,p->meta.name);
    if (p->meta.uuid_count) {
        b->meta.uuid_count=p->meta.uuid_count; memcpy(b->meta.uuids,p->meta.uuids,sizeof(b->meta.uuids));
    }
    if (p->meta.has_tx) { b->meta.has_tx=true; b->meta.tx=p->meta.tx; }
    if (p->meta.has_manufacturer) { b->meta.has_manufacturer=true; b->meta.manufacturer=p->meta.manufacturer; }
    b->hist[p->rssi+127]++; b->count++; b->utc_ms=p->utc_ms;
    b->last_ms=p->mono_ms; b->last_rssi=p->rssi; s->last_observation=p->utc_ms;
}

/* 严格解析: 拒绝尾部垃圾、重复键、JSON 字符串中的 U+0000。 */
static bool unique_keys(const cJSON *j)
{
    for (const cJSON *p=j->child;p;p=p->next) {
        if (cJSON_IsObject(j)) for (const cJSON *q=p->next;q;q=q->next)
            if (p->string && q->string && !strcmp(p->string,q->string)) return false;
        if (!unique_keys(p)) return false;
    }
    return true;
}
static bool bounded_json_depth(const char *json)
{
    unsigned depth=0; bool quoted=false,escaped=false;
    /* 在 cJSON 递归和重复键遍历前限制栈深；协议最大只需四层。 */
    for (const char *p=json;*p;p++) {
        if (quoted) {
            if (escaped) escaped=false;
            else if (*p=='\\') escaped=true;
            else if (*p=='"') quoted=false;
        } else if (*p=='"') quoted=true;
        else if (*p=='{' || *p=='[') { if (++depth>8) return false; }
        else if (*p=='}' || *p==']') { if (!depth) return false; depth--; }
    }
    return !quoted && depth==0;
}
static cJSON *parse(const char *json)
{
    if (!json || !bounded_json_depth(json)) return NULL;
    /* 只拒绝实际 NUL 转义；字面量反斜线后的 u0000 是正常字符串。 */
    for (const char *p=json; *p; p++) if (*p=='\\') {
        if (!strncmp(p,"\\u0000",6)) return NULL;
        if (p[1]) p++;
    }
    cJSON *j=cJSON_ParseWithOpts(json,NULL,true);
    if (j && !unique_keys(j)) { cJSON_Delete(j); return NULL; }
    return j;
}
static const cJSON *item(const cJSON *j,const char *k) { return cJSON_GetObjectItemCaseSensitive(j,k); }
static const char *string(const cJSON *j,const char *k)
{ const cJSON *v=item(j,k); return cJSON_IsString(v) ? v->valuestring : NULL; }
static bool eq(const cJSON *j,const char *k,const char *s)
{ const char *v=string(j,k); return v && !strcmp(v,s); }
static bool number(const cJSON *j,const char *k,int64_t lo,int64_t hi,int64_t *v)
{
    const cJSON *x=item(j,k);
    if (!cJSON_IsNumber(x) || !isfinite(x->valuedouble) || x->valuedouble<lo || x->valuedouble>hi ||
        floor(x->valuedouble)!=x->valuedouble) return false;
    *v=(int64_t)x->valuedouble; return true;
}
static bool known_keys(const cJSON *j,const char *const *keys,size_t n)
{
    if (!cJSON_IsObject(j)) return false;
    for (const cJSON *p=j->child;p;p=p->next) {
        bool known=false;
        for (size_t i=0;i<n;i++) if (p->string && !strcmp(p->string,keys[i])) known=true;
        if (!known) return false;
    }
    return true;
}
bool col_parse_config(const char *json,const char *node,col_config *out)
{
    cJSON *j=parse(json); col_config c={0}; int64_t n=0;
    static const char *const keys[]={"schema_version","node_id","scan_enabled","aggregation_window_ms",
        "heartbeat_interval_sec","max_batch_size","config_revision","strong_signal_dbm","weak_signal_dbm",
        "active_threshold_sec","stale_threshold_sec","retention_days"};
    bool ok=out && node && known_keys(j,keys,sizeof(keys)/sizeof(keys[0])) &&
        eq(j,"schema_version","1.0") && eq(j,"node_id",node) && cJSON_IsBool(item(j,"scan_enabled"));
#define READ(key,field,lo,hi) do { if (!number(j,key,lo,hi,&n)) ok=false; else c.field=(int)n; } while(0)
    if (ok) {
        c.scan_enabled=cJSON_IsTrue(item(j,"scan_enabled"));
        READ("aggregation_window_ms",window_ms,2000,3000);
        READ("heartbeat_interval_sec",heartbeat_sec,30,30);
        READ("max_batch_size",max_batch,100,100);
        READ("strong_signal_dbm",strong,-126,-1); READ("weak_signal_dbm",weak,-127,-2);
        READ("active_threshold_sec",active_sec,5,120); READ("stale_threshold_sec",stale_sec,10,3600);
        READ("retention_days",retention_days,1,30);
        if (!number(j,"config_revision",1,COL_SAFE_INT,&c.revision)) ok=false;
        if (c.weak>=c.strong || c.stale_sec<=c.active_sec) ok=false;
    }
#undef READ
    if (ok) *out=c;
    cJSON_Delete(j); return ok;
}
bool col_parse_credentials(const char *json,col_credentials *out)
{
    cJSON *j=parse(json); bool ok=cJSON_IsObject(j); col_credentials c={0};
    const char *const keys[]={"ssid","password","base_url","node_id","token","ca"};
    char *dst[]={c.ssid,c.password,c.base_url,c.node_id,c.token,c.ca};
    size_t caps[]={sizeof(c.ssid),sizeof(c.password),sizeof(c.base_url),sizeof(c.node_id),sizeof(c.token),sizeof(c.ca)};
    ok=ok && out && known_keys(j,keys,6);
    for (size_t i=0;i<6;i++) {
        const char *s=string(j,keys[i]);
        if (!s || strlen(s)>=caps[i] || (i!=1 && !*s) || (i!=5 && !col_utf8(s))) ok=false;
        else strcpy(dst[i],s);
    }
    if (ok) {
        const char *host=c.base_url+8;
        ok=!strncmp(c.base_url,"https://",8) && *host && !strpbrk(host,"/?#@ \\") &&
           col_id(c.node_id,80) && strlen(c.token)==43 && col_id(c.token,43) &&
           strstr(c.ca,"-----BEGIN CERTIFICATE-----") && strstr(c.ca,"-----END CERTIFICATE-----");
    }
    if (ok) *out=c;
    volatile unsigned char *wipe=(volatile unsigned char *)&c;
    for (size_t i=0;i<sizeof(c);i++) wipe[i]=0;
    cJSON_Delete(j); return ok;
}
static bool uuid_valid(const char *s)
{
    size_t n=0;
    while (n<37 && s[n]) n++;
    if (n!=4 && n!=8 && n!=36) return false;
    for (size_t i=0;i<n;i++) {
        bool dash=n==36 && (i==8 || i==13 || i==18 || i==23);
        if (dash ? s[i]!='-' : !isxdigit((unsigned char)s[i])) return false;
    }
    return true;
}
bool col_event_valid(const col_event *e,int64_t now)
{
    if (!e || !memchr(e->id,0,sizeof(e->id)) || !memchr(e->meta.name,0,sizeof(e->meta.name)) ||
        !memchr(e->meta.address,0,sizeof(e->meta.address))) return false;
    if (!col_id(e->id,63) || !col_time_allowed(e->utc_ms,now,false) ||
        e->count<1 || e->count>100000 || e->window_ms<2000 || e->window_ms>3000 ||
        e->min < -127 || e->max>=0 || e->min*2>e->median_x2 || e->max*2<e->median_x2 ||
        e->meta.address_type>3 || e->meta.uuid_count>COL_UUIDS || !col_utf8(e->meta.name) ||
        (e->meta.has_tx && (e->meta.tx < -127 || e->meta.tx > 126))) return false;
    if (strlen(e->meta.address)!=17) return false;
    for (int i=0;i<17;i++) if (i%3==2 ? e->meta.address[i]!=':' : !isxdigit((unsigned char)e->meta.address[i])) return false;
    for (unsigned i=0;i<e->meta.uuid_count;i++) if (!uuid_valid(e->meta.uuids[i])) return false;
    return true;
}
static bool addstr(cJSON *j,const char *k,const char *s)
{ return cJSON_AddStringToObject(j,k,s)!=NULL; }
static bool addnum(cJSON *j,const char *k,double n)
{ return cJSON_AddNumberToObject(j,k,n)!=NULL; }
static bool addint64(cJSON *j,const char *k,int64_t n)
{
    /* cJSON 的 15 位近似输出会改动大整数；这些已验证的计数必须精确。 */
    char decimal[24]; snprintf(decimal,sizeof(decimal),"%" PRId64,n);
    return cJSON_AddRawToObject(j,k,decimal)!=NULL;
}
static bool addnullable(cJSON *j,const char *k,const char *s)
{ return s && *s ? addstr(j,k,s) : cJSON_AddNullToObject(j,k)!=NULL; }
static cJSON *event_json(const col_event *e)
{
    char t[25]; if (!col_time_format(e->utc_ms,t)) return NULL;
    cJSON *j=cJSON_CreateObject(); if (!j) return NULL;
    bool ok=addstr(j,"event_id",e->id) && addstr(j,"observed_at",t) && addstr(j,"local_device_id",e->meta.address) &&
        addstr(j,"identifier_type","ble_address") && addstr(j,"address_type",address_names[e->meta.address_type]) &&
        addnullable(j,"local_name",e->meta.name) && addnum(j,"rssi_dbm",e->median_x2/2.0) &&
        addnum(j,"rssi_min_dbm",e->min) && addnum(j,"rssi_max_dbm",e->max) &&
        addnum(j,"sample_count",e->count) && addnum(j,"window_ms",e->window_ms);
    ok &= e->meta.has_tx ? addnum(j,"tx_power_dbm",e->meta.tx) : cJSON_AddNullToObject(j,"tx_power_dbm")!=NULL;
    ok &= e->meta.has_manufacturer ? addnum(j,"manufacturer_id",e->meta.manufacturer) : cJSON_AddNullToObject(j,"manufacturer_id")!=NULL;
    cJSON *a=cJSON_AddArrayToObject(j,"service_uuids"); ok &= a!=NULL;
    for (unsigned i=0;a && i<e->meta.uuid_count;i++) {
        cJSON *v=cJSON_CreateString(e->meta.uuids[i]);
        if (!v || !cJSON_AddItemToArray(a,v)) { cJSON_Delete(v); ok=false; }
    }
    if (!ok) { cJSON_Delete(j); return NULL; } return j;
}
bool col_batch(col_store *s,const char *node,const char *batch,int64_t now,size_t limit,char *out,size_t cap)
{
    if (!out || cap<8) return false;
    if (cap>COL_BODY) cap=COL_BODY;
    out[0]=0;
    if (s->pinned || !s->count || !limit || !col_id(node,80) || !col_id(batch,80)) return false;
    size_t count=s->count<limit?s->count:limit; if (count>32) count=32;
    char t[25]; if (!col_time_format(now,t)) return false;
    for (size_t i=0;i<count;i++) if (!col_event_valid(&s->queue[i],now)) {
        remove_at(s,i); s->dropped++; return false;
    }
    /* 逐条序列化，避免为整个批次同时分配 JSON 树。 */
    cJSON *head=cJSON_CreateObject();
    if (!head) return false;
    bool ok=addstr(head,"schema_version","1.0") && addstr(head,"node_id",node) &&
        addstr(head,"batch_id",batch) && addstr(head,"sent_at",t) &&
        cJSON_PrintPreallocated(head,out,(int)cap,false);
    cJSON_Delete(head); if (!ok) return false;
    size_t used=strlen(out)-1; const char *prefix=",\"observations\":[";
    if (used+strlen(prefix)+3>=cap) return false;
    strcpy(out+used,prefix); used+=strlen(prefix);
    for (size_t i=0;i<count;i++) {
        cJSON *j=event_json(&s->queue[i]);
        if (!j) { s->pinned=0; out[0]=0; return false; }
        size_t start=used;
        if (i) out[used++]=',';
        ok=cJSON_PrintPreallocated(j,out+used,(int)(cap-used-3),false);
        cJSON_Delete(j);
        if (!ok) { used=start; break; }
        used+=strlen(out+used); s->pinned++;
    }
    if (!s->pinned) { out[0]=0; return false; }
    strcpy(out+used,"]}"); return true;
}
bool col_heartbeat(const char *node,int64_t now,const char *state,bool ble_ready,const col_store *s,
    const char *error,int64_t revision,char *out,size_t cap)
{
    char t[25],last[25]; if (!col_time_format(now,t)) return false;
    const char *states[]={"scanning","paused","permission_denied","bluetooth_off","unavailable","error"};
    bool valid=false; for (size_t i=0;state && i<6;i++) if (!strcmp(state,states[i])) valid=true;
    if (!valid || !out || cap<8 || cap>INT32_MAX || !col_id(node,80) || revision<0 ||
        revision>COL_SAFE_INT || s->count>COL_QUEUE || s->dropped>COL_SAFE_INT ||
        (error && *error && (strlen(error)>80 || !col_utf8(error)))) return false;
    if (s->last_observation && (!col_time_format(s->last_observation,last) ||
        !col_time_allowed(s->last_observation,now,true))) return false;
    cJSON *j=cJSON_CreateObject(); if (!j) return false;
    bool ok=addstr(j,"schema_version","1.0") && addstr(j,"node_id",node) && addstr(j,"observed_at",t) &&
        addstr(j,"collector_version",COLLECTOR_VERSION) && addstr(j,"scan_state",state) &&
        addstr(j,"bluetooth_state",ble_ready?"powered_on":"unavailable") && addstr(j,"permission_state","not_applicable") &&
        addnullable(j,"last_observation_at",s->last_observation?last:NULL) &&
        addnum(j,"pending_count",s->count) && addint64(j,"dropped_count",(int64_t)s->dropped) &&
        addnullable(j,"error_code",error) && addint64(j,"applied_config_revision",revision) &&
        cJSON_PrintPreallocated(j,out,(int)cap,false);
    cJSON_Delete(j); return ok;
}
bool col_ack(const char *json,const char *batch,size_t n)
{
    if (!col_id(batch,80) || !n || n>100) return false;
    cJSON *j=parse(json); int64_t a=0,d=0,r=0,t=0;
    bool ok=j && eq(j,"schema_version","1.0") && eq(j,"batch_id",batch) &&
        number(j,"accepted",0,100,&a) && number(j,"duplicates",0,100,&d) && number(j,"rejected",0,0,&r) &&
        a+d==(int64_t)n && col_time_parse(string(j,"server_time"),&t);
    cJSON_Delete(j); return ok;
}
bool col_heartbeat_ack(const char *json,int64_t *revision)
{
    cJSON *j=parse(json); int64_t t=0,next=0;
    bool ok=revision && j && eq(j,"schema_version","1.0") && number(j,"config_revision",1,COL_SAFE_INT,&next) &&
        col_time_parse(string(j,"server_time"),&t);
    if (ok) *revision=next;
    cJSON_Delete(j); return ok;
}
col_result col_classify(int status,const char *json)
{
    if (status==200) return COL_ACK;
    cJSON *j=parse(json); const cJSON *e=item(j,"error"); col_result r=COL_RETRY;
    if (status==401) r=COL_AUTH;
    else if (status==403) r=COL_IDENTITY;
    else if (status==404 || status==405 || status==415) r=COL_CONFIG_ERROR;
    else if (status==413) r=COL_SPLIT;
    else if (status==429) r=COL_RATE;
    else if (status==400) r=eq(e,"code","UNSUPPORTED_SCHEMA")?COL_SCHEMA:
        eq(e,"code","INVALID_REVISION")?COL_REVISION:COL_BAD_INPUT;
    else if (status==409) r=eq(e,"code","OBSERVATION_LIMIT")?COL_HISTORY_FULL:
        eq(e,"code","DEVICE_LIMIT")?COL_DEVICES_FULL:COL_CONFIG_ERROR;
    cJSON_Delete(j); return r;
}
void col_schedule_init(col_schedule *s)
{ memset(s,0,sizeof(*s)); s->batch_limit=32; }
void col_schedule_result(col_schedule *s,col_result r,int64_t now,unsigned jitter)
{
    if (r==COL_ACK) { s->failures=0; s->retry_at=0; if (!s->capacity_blocked) s->error=NULL; return; }
    if (r==COL_RATE) { s->write_at=now+60000; s->error="RATE_LIMITED"; return; }
    if (r==COL_AUTH) { s->auth_blocked=true; s->error="INVALID_NODE_TOKEN"; return; }
    if (r==COL_IDENTITY || r==COL_CONFIG_ERROR || r==COL_SCHEMA) {
        s->protocol_blocked=true;
        s->error=r==COL_IDENTITY?"NODE_MISMATCH":r==COL_SCHEMA?"UNSUPPORTED_SCHEMA":"ENDPOINT_CONFIG"; return;
    }
    if (r==COL_HISTORY_FULL || r==COL_DEVICES_FULL) {
        s->capacity_blocked=true; s->capacity_at=now+60000;
        s->error=r==COL_HISTORY_FULL?"OBSERVATION_LIMIT":"DEVICE_LIMIT"; return;
    }
    if (r==COL_REVISION) { s->config_at=now; s->error="INVALID_REVISION"; s->retry_at=now+10000; return; }
    if (r==COL_SPLIT) { if (s->batch_limit>1) s->batch_limit=(s->batch_limit+1)/2; s->error="BODY_TOO_LARGE"; return; }
    if (r==COL_BAD_INPUT) { s->error="INVALID_INPUT"; s->retry_at=now+10000; return; }
    unsigned shift=s->failures<6?s->failures:6;
    int64_t delay=(int64_t)(1U<<shift)*1000; if (delay>60000) delay=60000;
    s->retry_at=now+delay+(jitter%251); if (s->failures<7) s->failures++;
    s->error="TRANSPORT_OR_ACK";
}
bool col_scan_allowed(bool configured,bool pause,bool provisioning,bool clock_ready,
    const col_config *c,const col_schedule *s)
{ return configured && c->scan_enabled && !pause && !provisioning && clock_ready &&
    !s->auth_blocked && !s->protocol_blocked && !s->capacity_blocked; }
