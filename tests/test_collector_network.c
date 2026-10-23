/* 真实网络适配器 + 可控ESP边界。测试不建立网络连接，不证明TLS/设备已经可用。 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
static int fake_gettimeofday(struct timeval *tv,void *tz);
#define gettimeofday fake_gettimeofday
#include "../main/collector_network.c"
#undef gettimeofday

struct fake_semaphore { unsigned count; } semaphore;
struct fake_http {
    esp_http_client_config_t config;
    char url[384],name[257],host_header[257],auth_header[80],content_type[80];
    const char *body;
    int body_len,method,status;
    bool closed;
} http;
static int64_t tick_ms;
static unsigned checks,close_calls,cleanup_calls,init_calls,perform_calls,dns_calls;
static unsigned disconnect_calls,connect_calls,sntp_inits,sntp_deinits;
static bool dns_timeout,dns_missing,dns_ipv6,complete_response;
static int parse_ca_error,header_error,tls_flags,tls_error;
static esp_err_t perform_error;
static dns_found_callback late_dns;
static void *late_arg;
static enum { MODE_NORMAL,MODE_STREAM,MODE_SLOW,MODE_EAGAIN } mode;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"network check failed at %s:%d: %s\n",__FILE__,__LINE__,#x); abort(); } } while (0)

static int fake_gettimeofday(struct timeval *tv,void *tz)
{ (void)tz; tv->tv_sec=1792713600+tick_ms/1000; tv->tv_usec=tick_ms%1000*1000; return 0; }
int64_t esp_timer_get_time(void) { return tick_ms*1000; }
uint32_t esp_random(void) { return 0; }
void vTaskDelay(unsigned t) { tick_ms+=t; }
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t t) { (void)t; return 2048; }
int xTaskCreate(void (*fn)(void *),const char *n,unsigned sz,void *arg,unsigned prio,TaskHandle_t *task)
{ (void)fn;(void)n;(void)sz;(void)arg;(void)prio; if (task) *task=&http; return pdPASS; }
int xQueueReceive(QueueHandle_t q,void *out,unsigned wait) { (void)q;(void)out;(void)wait; return 0; }
int xQueueSend(QueueHandle_t q,const void *out,unsigned wait) { (void)q;(void)out;(void)wait; return 1; }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { semaphore.count=0; return &semaphore; }
int xSemaphoreGive(SemaphoreHandle_t s) { s->count=1; return pdTRUE; }
int xSemaphoreTake(SemaphoreHandle_t s,unsigned wait)
{ if (s->count) { s->count=0; return pdTRUE; } tick_ms+=wait; return 0; }
esp_err_t demo_radio_network_prepare(void) { return ESP_OK; }
void *esp_netif_create_default_wifi_sta(void) { return &http; }
esp_err_t esp_event_handler_instance_register(esp_event_base_t b,int32_t id,void (*f)(void *,esp_event_base_t,int32_t,void *),void *a,esp_event_handler_instance_t *h)
{ (void)b;(void)id;(void)f;(void)a; *h=1; return ESP_OK; }
esp_err_t esp_wifi_init(const wifi_init_config_t *c) { (void)c; return ESP_OK; }
esp_err_t esp_wifi_set_storage(int storage) { CHECK(storage==WIFI_STORAGE_RAM); return ESP_OK; }
esp_err_t esp_wifi_set_mode(int m) { CHECK(m==WIFI_MODE_STA); return ESP_OK; }
esp_err_t esp_wifi_start(void) { return ESP_OK; }
esp_err_t esp_wifi_disconnect(void) { disconnect_calls++; return ESP_OK; }
esp_err_t esp_wifi_connect(void) { connect_calls++; return ESP_OK; }
esp_err_t esp_wifi_set_config(int interface,const wifi_config_t *c)
{ (void)c; CHECK(interface==WIFI_IF_STA); return ESP_OK; }
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *c) { CHECK(c->sync_cb!=NULL); sntp_inits++; return ESP_OK; }
void esp_netif_sntp_deinit(void) { sntp_deinits++; }
void esp_log_level_set(const char *tag,int level) { (void)tag;(void)level; }
void mbedtls_x509_crt_init(mbedtls_x509_crt *c) { c->unused=0; }
void mbedtls_x509_crt_free(mbedtls_x509_crt *c) { (void)c; }
int mbedtls_x509_crt_parse(mbedtls_x509_crt *c,const unsigned char *data,size_t n)
{ (void)c; CHECK(n>0 && data[n-1]==0); return parse_ca_error; }
err_t tcpip_try_callback(void (*fn)(void *),void *arg) { fn(arg); return ERR_OK; }
err_t dns_gethostbyname_addrtype(const char *name,ip_addr_t *address,dns_found_callback fn,void *arg,int kind)
{
    CHECK(name[0]!=0 && kind==LWIP_DNS_ADDRTYPE_IPV4_IPV6); dns_calls++;
    if (dns_timeout) { late_dns=fn; late_arg=arg; return ERR_INPROGRESS; }
    if (dns_missing) return -1;
    address->v6=dns_ipv6;
    snprintf(address->text,sizeof(address->text),"%s",dns_ipv6?"2001:db8::5":"192.0.2.5"); return ERR_OK;
}
char *ipaddr_ntoa_r(const ip_addr_t *a,char *dst,int size) { snprintf(dst,(size_t)size,"%s",a->text); return dst; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c)
{
    init_calls++; memset(&http,0,sizeof(http)); http.config=*c;
    snprintf(http.url,sizeof(http.url),"%s",c->url); snprintf(http.name,sizeof(http.name),"%s",c->common_name);
    return &http;
}
esp_err_t esp_http_client_close(esp_http_client_handle_t h)
{
    CHECK(++close_calls<10); h->closed=true;
    /* IDF close同步触发DISCONNECTED，检验回调不会递归关闭。 */
    esp_http_client_event_t e={.event_id=HTTP_EVENT_DISCONNECTED,.client=h};
    h->config.event_handler(&e); return ESP_OK;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t h) { (void)h; cleanup_calls++; return ESP_OK; }
esp_err_t esp_http_client_set_url(esp_http_client_handle_t h,const char *u)
{ snprintf(h->url,sizeof(h->url),"%s",u); return ESP_OK; }
esp_err_t esp_http_client_set_header(esp_http_client_handle_t h,const char *k,const char *v)
{
    if (header_error) return header_error;
    if (!strcmp(k,"Host")) snprintf(h->host_header,sizeof(h->host_header),"%s",v);
    else if (!strcmp(k,"Authorization")) snprintf(h->auth_header,sizeof(h->auth_header),"%s",v);
    else if (!strcmp(k,"Content-Type")) snprintf(h->content_type,sizeof(h->content_type),"%s",v);
    return ESP_OK;
}
esp_err_t esp_http_client_set_method(esp_http_client_handle_t h,int m) { h->method=m; return ESP_OK; }
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t h,const char *s,int n) { h->body=s; h->body_len=n; return ESP_OK; }
esp_err_t esp_http_client_perform(esp_http_client_handle_t h)
{
    perform_calls++;
    if (mode==MODE_EAGAIN) return ESP_ERR_HTTP_EAGAIN;
    if (perform_error) return perform_error;
    h->status=200;
    if (mode==MODE_STREAM || mode==MODE_SLOW) {
        char data[1024]; memset(data,'x',sizeof(data));
        for (unsigned i=0;i<32 && !h->closed;i++) {
            if (mode==MODE_SLOW) tick_ms+=1000;
            esp_http_client_event_t e={.event_id=HTTP_EVENT_ON_DATA,.client=h,.data=data,
                .data_len=mode==MODE_SLOW?1:(int)sizeof(data)};
            /* 真实IDF忽略事件返回值；只有close能中止模拟持续响应。 */
            h->config.event_handler(&e);
        }
        CHECK(h->closed); return ESP_FAIL;
    }
    esp_http_client_event_t e={.event_id=HTTP_EVENT_ON_DATA,.client=h,.data="{}",.data_len=2};
    h->config.event_handler(&e); return ESP_OK;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t h) { (void)h; return complete_response; }
int esp_http_client_get_status_code(esp_http_client_handle_t h) { return h->status; }
esp_err_t esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t h,int *code,int *flags)
{ (void)h; *code=tls_error; *flags=tls_flags; return ESP_OK; }

static void reset_fixture(void)
{
    memset(&http,0,sizeof(http)); memset(&credentials,0,sizeof(credentials));
    snprintf(credentials.base_url,sizeof(credentials.base_url),"https://collector.example.invalid:8443");
    snprintf(credentials.ssid,sizeof(credentials.ssid),"synthetic");
    snprintf(credentials.ca,sizeof(credentials.ca),"synthetic-public-CA");
    memset(credentials.token,'t',43); credentials.token[43]=0;
    client=NULL; current=NULL; received=0; overflow=aborted=false; tick_ms=1000;
    close_calls=cleanup_calls=init_calls=perform_calls=dns_calls=0;
    disconnect_calls=connect_calls=sntp_inits=sntp_deinits=0;
    dns_timeout=dns_missing=dns_ipv6=false; complete_response=true;
    parse_ca_error=header_error=tls_flags=tls_error=0; perform_error=ESP_OK; mode=MODE_NORMAL;
    atomic_store(&online,true); atomic_store(&configured,true); atomic_store(&time_synced,true);
    atomic_store(&dns_busy,false); dns_done=xSemaphoreCreateBinary(); dns_ok=false;
    sntp_started=false; late_dns=NULL; network_task=NULL;
}
static col_job test_job(void)
{ col_job j={.kind=COL_GET_CONFIG}; snprintf(j.body,sizeof(j.body),"{\"synthetic\":true}"); return j; }
static void test_tls_and_reuse(void)
{
    reset_fixture(); col_job j=test_job(); request(&j);
    CHECK(j.transport==ESP_OK && j.status==200 && !strcmp(j.response,"{}"));
    CHECK(!strcmp(http.url,"https://192.0.2.5:8443/api/tools/bluetooth-explorer/ingest/v1/config"));
    CHECK(!strcmp(http.name,"collector.example.invalid"));
    CHECK(!strcmp(http.host_header,"collector.example.invalid:8443"));
    CHECK(http.config.cert_pem==credentials.ca && !http.config.skip_cert_common_name_check);
    CHECK(!http.config.use_global_ca_store && http.config.crt_bundle_attach==NULL);
    CHECK(http.config.disable_auto_redirect && http.config.is_async && http.config.keep_alive_enable);
    CHECK(http.config.max_authorization_retries==-1);
    CHECK(!strncmp(http.auth_header,"Bearer ",7) && strlen(http.auth_header)==50);
    CHECK(http.method==HTTP_METHOD_GET && http.body==NULL && http.body_len==0);
    j.kind=COL_POST_HEARTBEAT; request(&j);
    CHECK(init_calls==1 && cleanup_calls==0 && perform_calls==2);
    CHECK(strstr(http.url,"/heartbeat")!=NULL && http.method==HTTP_METHOD_POST);
    CHECK(http.body==j.body && http.body_len==(int)strlen(j.body));
    CHECK(!strcmp(http.content_type,"application/json") && !strcmp(j.response,"{}"));
    dns_ipv6=true; j.kind=COL_POST_EVENTS; request(&j);
    CHECK(strstr(http.url,"https://[2001:db8::5]:8443/")==http.url && strstr(http.url,"/observations"));
    CHECK(!strcmp(http.name,"collector.example.invalid"));
}
static void test_callback_abort(void)
{
    reset_fixture(); col_job j=test_job(); mode=MODE_STREAM; request(&j);
    CHECK(j.transport==ESP_ERR_INVALID_SIZE && !strcmp(j.diagnostic,"RESPONSE_TOO_LARGE"));
    CHECK(close_calls==1 && cleanup_calls==1 && client==NULL && received<COL_RESPONSE);
    CHECK(j.response[received]==0);
    reset_fixture(); j=test_job(); mode=MODE_SLOW; request(&j);
    CHECK(close_calls==1 && cleanup_calls==1 && client==NULL && tick_ms==11000);
    CHECK(j.transport!=ESP_OK && received==9);
    reset_fixture(); j=test_job(); request(&j);
    current=&j; received=COL_RESPONSE-2; deadline=tick_ms+10000;
    char payload[2]={'a','b'};
    esp_http_client_event_t e={.event_id=HTTP_EVENT_ON_DATA,.client=client,.data=payload,.data_len=1};
    CHECK(http_event(&e)==ESP_OK && received==COL_RESPONSE-1);
    CHECK(http_event(&e)==ESP_FAIL && close_calls==1);
    CHECK(http_event(&e)==ESP_FAIL && close_calls==1); /* 同一请求只close一次。 */
}
static void test_auth_and_failures(void)
{
    reset_fixture(); col_job j=test_job(); request(&j); /* 先建立client，使401真实头状态可控。 */
    http.status=401; perform_error=ESP_ERR_NOT_SUPPORTED; request(&j);
    CHECK(j.status==401 && j.transport==ESP_ERR_NOT_SUPPORTED && cleanup_calls==1 && client==NULL);
    reset_fixture(); j=test_job(); complete_response=false; request(&j);
    CHECK(j.transport!=ESP_OK && cleanup_calls==1);
    reset_fixture(); j=test_job(); perform_error=ESP_FAIL; tls_flags=MBEDTLS_X509_BADCERT_CN_MISMATCH; request(&j);
    CHECK(!strcmp(j.diagnostic,"TLS_HOSTNAME") && cleanup_calls==1);
    reset_fixture(); j=test_job(); perform_error=ESP_FAIL; tls_flags=MBEDTLS_X509_BADCERT_EXPIRED; request(&j);
    CHECK(!strcmp(j.diagnostic,"TLS_CERT_TIME"));
    reset_fixture(); j=test_job(); header_error=ESP_ERR_NO_MEM; request(&j);
    CHECK(perform_calls==0 && j.status==0 && j.transport==ESP_ERR_NO_MEM);
    reset_fixture(); j=test_job(); mode=MODE_EAGAIN; request(&j);
    CHECK(j.transport==ESP_ERR_TIMEOUT && tick_ms==11000 && cleanup_calls==1);
}
static void test_dns_lifetime(void)
{
    reset_fixture(); col_job j=test_job(); dns_timeout=true; request(&j);
    CHECK(j.transport==ESP_ERR_TIMEOUT && !strcmp(j.diagnostic,"DNS_FAILED"));
    CHECK(tick_ms==11000 && !init_calls && atomic_load(&dns_busy) && late_dns);
    request(&j); CHECK(dns_calls==1 && !init_calls); /* 超时旧查询未结束时不重用静态参数。 */
    ip_addr_t reply={.v6=false}; strcpy(reply.text,"192.0.2.8");
    late_dns("ignored",&reply,late_arg); CHECK(!atomic_load(&dns_busy));
    dns_timeout=false; request(&j); CHECK(j.transport==ESP_OK && dns_calls==2 && init_calls==1);
    reset_fixture(); j=test_job(); dns_missing=true; request(&j);
    CHECK(j.transport==ESP_ERR_NOT_FOUND && !init_calls && !atomic_load(&dns_busy));
    reset_fixture(); j=test_job(); atomic_store(&online,false); request(&j);
    CHECK(j.transport==ESP_ERR_INVALID_STATE && !dns_calls && !init_calls);
    reset_fixture(); j=test_job(); atomic_store(&time_synced,false); request(&j);
    CHECK(j.transport==ESP_ERR_INVALID_STATE && !dns_calls && !init_calls);
}
static void test_credentials_sntp(void)
{
    reset_fixture(); col_credentials copy=credentials;
    CHECK(apply_credentials(&copy)==ESP_OK && sntp_inits==1 && sntp_started);
    CHECK(!col_clock_ready() && !col_wifi_ready() && disconnect_calls==1 && connect_calls==1);
    CHECK(apply_credentials(&copy)==ESP_OK && sntp_inits==2 && sntp_deinits==1);
    parse_ca_error=-1; CHECK(apply_credentials(&copy)==ESP_ERR_INVALID_ARG && sntp_inits==2);
    struct timeval t={.tv_sec=1792713600}; time_sync(&t); CHECK(col_clock_ready());
    t.tv_sec=1; time_sync(&t); CHECK(!col_clock_ready());
}
int main(void)
{
    test_tls_and_reuse(); test_callback_abort(); test_auth_and_failures(); test_dns_lifetime(); test_credentials_sntp();
    printf("collector network boundary: %u checks PASS (stubbed ESP transport; no device/TLS handshake)\n",checks);
    return 0;
}
