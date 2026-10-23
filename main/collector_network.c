#include "collector_platform.h"
#include "demo_radio.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_netif_sntp.h"
#include "freertos/semphr.h"
#include "http_parser.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_tls_errors.h"
#include "mbedtls/x509_crt.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

static atomic_bool online, configured, time_synced;
static atomic_int wifi_reason;
static TaskHandle_t network_task;
static atomic_uint ip_addr;
static QueueHandle_t jobs,results;
static col_credentials credentials;
static esp_event_handler_instance_t wifi_handler,ip_handler;
static bool sntp_started;
static esp_http_client_handle_t client;
static size_t received;
static int64_t deadline;
static bool overflow,aborted;
static SemaphoreHandle_t dns_done;
static atomic_bool dns_busy;
static bool dns_ok;
static char dns_name[257],tls_name[257],resolved_host[64];
static ip_addr_t dns_address;
static col_job *current;

int64_t col_mono(void) { return esp_timer_get_time()/1000; }
int64_t col_utc(void) { struct timeval t; gettimeofday(&t,NULL); return (int64_t)t.tv_sec*1000+t.tv_usec/1000; }
bool col_clock_ready(void) { return atomic_load(&time_synced) && col_utc()>=1767225600000LL; }
static void time_sync(struct timeval *t)
{ atomic_store(&time_synced,t->tv_sec>=1767225600); }
uint32_t col_network_stack(void)
{ return network_task ? uxTaskGetStackHighWaterMark(network_task) : 0; }
const char *col_network_diagnostic(void)
{
    if (!atomic_load(&configured)) return NULL;
    if (!atomic_load(&online)) {
        int reason=atomic_load(&wifi_reason);
        return reason==WIFI_REASON_AUTH_FAIL || reason==WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ?
            "WIFI_AUTH_FAILED" : reason==WIFI_REASON_NO_AP_FOUND ? "WIFI_AP_NOT_FOUND" : "WIFI_CONNECTING";
    }
    if (!col_clock_ready()) return "WAITING_SNTP";
    return NULL;
}
void col_wipe(void *p,size_t n) { volatile unsigned char *v=p; while (n--) *v++=0; }
bool col_wifi_ready(void) { return atomic_load(&online); }
uint32_t col_wifi_ip(void) { return atomic_load(&ip_addr); }
static void wifi_event(void *arg,esp_event_base_t base,int32_t id,void *data)
{
    (void)arg;
    if (base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) {
        atomic_store(&ip_addr,((ip_event_got_ip_t *)data)->ip_info.ip.addr);
        atomic_store(&online,true); atomic_store(&wifi_reason,0);
        printf("COLLECTOR {\"status\":\"WIFI_CONNECTED\"}\n");
    } else if (base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) {
        atomic_store(&online,false); atomic_store(&ip_addr,0);
        atomic_store(&wifi_reason,((wifi_event_sta_disconnected_t *)data)->reason);
    }
}
static esp_err_t http_event(esp_http_client_event_t *e)
{
    if (e->event_id==HTTP_EVENT_ON_DATA &&
        (e->data_len<0 || received+(size_t)e->data_len>=COL_RESPONSE)) overflow=true;
    if (overflow || col_mono()>=deadline) {
        /* IDF 的 parser 忽略事件返回值；显式关闭transport才会中止持续响应。 */
        if (!aborted && e->event_id!=HTTP_EVENT_DISCONNECTED) {
            aborted=true; esp_http_client_close(e->client);
        }
        return ESP_FAIL;
    }
    if (e->event_id==HTTP_EVENT_ON_DATA) {
        memcpy(current->response+received,e->data,(size_t)e->data_len); received+=(size_t)e->data_len;
        current->response[received]=0;
    }
    return ESP_OK;
}
static void close_client(void)
{ if (client) { esp_http_client_cleanup(client); client=NULL; } }
static esp_err_t apply_credentials(col_credentials *c)
{
    mbedtls_x509_crt ca; mbedtls_x509_crt_init(&ca);
    int rc=mbedtls_x509_crt_parse(&ca,(const unsigned char *)c->ca,strlen(c->ca)+1);
    mbedtls_x509_crt_free(&ca); if (rc!=0) return ESP_ERR_INVALID_ARG;
    close_client(); atomic_store(&configured,false); atomic_store(&online,false);
    (void)esp_wifi_disconnect();
    if (sntp_started) { esp_netif_sntp_deinit(); sntp_started=false; }
    atomic_store(&time_synced,false);
    wifi_config_t wifi={0};
    memcpy(wifi.sta.ssid,c->ssid,strlen(c->ssid)); memcpy(wifi.sta.password,c->password,strlen(c->password));
    esp_err_t err=esp_wifi_set_config(WIFI_IF_STA,&wifi); col_wipe(&wifi,sizeof(wifi));
    if (err!=ESP_OK) return err;
    col_wipe(&credentials,sizeof(credentials)); credentials=*c;
    atomic_store(&configured,true);
    esp_sntp_config_t time_config=ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2,
        ESP_SNTP_SERVER_LIST("pool.ntp.org","time.cloudflare.com"));
    time_config.sync_cb=time_sync;
    err=esp_netif_sntp_init(&time_config);
    if (err!=ESP_OK) { atomic_store(&configured,false); return err; }
    sntp_started=true;
    return esp_wifi_connect();
}
/* DNS 在 TCP/IP 线程异步执行。超时后的回调只持有静态数据，不引用调用栈。 */
static void dns_found(const char *name,const ip_addr_t *address,void *arg)
{
    (void)name; (void)arg; dns_ok=address!=NULL;
    if (address) dns_address=*address;
    xSemaphoreGive(dns_done); atomic_store(&dns_busy,false);
}
static void dns_start(void *unused)
{
    (void)unused; ip_addr_t address;
    err_t err=dns_gethostbyname_addrtype(dns_name,&address,dns_found,NULL,LWIP_DNS_ADDRTYPE_IPV4_IPV6);
    if (err==ERR_OK) dns_found(dns_name,&address,NULL);
    else if (err!=ERR_INPROGRESS) dns_found(dns_name,NULL,NULL);
}
static esp_err_t resolve_endpoint(const char *name)
{
    if (atomic_load(&dns_busy)) return ESP_ERR_TIMEOUT;
    (void)xSemaphoreTake(dns_done,0);
    snprintf(dns_name,sizeof(dns_name),"%s",name); dns_ok=false; atomic_store(&dns_busy,true);
    if (tcpip_try_callback(dns_start,NULL)!=ERR_OK) { atomic_store(&dns_busy,false); return ESP_FAIL; }
    int64_t remaining=deadline-col_mono();
    if (remaining<=0 || xSemaphoreTake(dns_done,pdMS_TO_TICKS(remaining))!=pdTRUE) return ESP_ERR_TIMEOUT;
    if (!dns_ok) return ESP_ERR_NOT_FOUND;
    char ip[48];
    if (!ipaddr_ntoa_r(&dns_address,ip,sizeof(ip))) return ESP_FAIL;
    if (IP_IS_V6(&dns_address)) snprintf(resolved_host,sizeof(resolved_host),"[%s]",ip);
    else snprintf(resolved_host,sizeof(resolved_host),"%s",ip);
    return ESP_OK;
}
static void request(col_job *j)
{
    j->status=0; j->response[0]=0; j->transport=ESP_ERR_INVALID_STATE; j->diagnostic=NULL;
    if (!atomic_load(&online) || !col_clock_ready()) return;
    deadline=col_mono()+10000; current=j; received=0; overflow=false; aborted=false;
    struct http_parser_url parsed;
    if (http_parser_parse_url(credentials.base_url,strlen(credentials.base_url),0,&parsed)!=0 ||
        !(parsed.field_set & (1U<<UF_HOST))) { j->diagnostic="ENDPOINT_CONFIG"; return; }
    size_t host_len=parsed.field_data[UF_HOST].len;
    if (host_len>=sizeof(tls_name)) { j->diagnostic="ENDPOINT_CONFIG"; return; }
    memcpy(tls_name,credentials.base_url+parsed.field_data[UF_HOST].off,host_len); tls_name[host_len]=0;
    esp_err_t resolved=resolve_endpoint(tls_name);
    if (resolved!=ESP_OK) { j->transport=resolved; j->diagnostic="DNS_FAILED"; return; }
    char url[384];
    unsigned port=parsed.field_set & (1U<<UF_PORT)?parsed.port:443;
    snprintf(url,sizeof(url),"https://%s:%u/api/tools/bluetooth-explorer/ingest/v1/%s",resolved_host,port,
        j->kind==COL_GET_CONFIG?"config":j->kind==COL_POST_HEARTBEAT?"heartbeat":"observations");
    if (!client) {
        esp_http_client_config_t cfg={.url=url,.cert_pem=credentials.ca,.event_handler=http_event,
            .timeout_ms=2000,.buffer_size=1024,.buffer_size_tx=1024,.disable_auto_redirect=true,
            .keep_alive_enable=true,.is_async=true,.common_name=tls_name,.max_authorization_retries=-1};
        client=esp_http_client_init(&cfg); if (!client) { j->transport=ESP_ERR_NO_MEM; return; }
    }
    char auth[52]; snprintf(auth,sizeof(auth),"Bearer %s",credentials.token);
    esp_err_t err=esp_http_client_set_url(client,url);
    if (err==ESP_OK) err=esp_http_client_set_header(client,"Host",credentials.base_url+8);
    if (err==ESP_OK) err=esp_http_client_set_header(client,"Authorization",auth);
    col_wipe(auth,sizeof(auth));
    if (err==ESP_OK) err=esp_http_client_set_header(client,"Content-Type","application/json");
    if (err==ESP_OK) err=esp_http_client_set_method(client,j->kind==COL_GET_CONFIG?HTTP_METHOD_GET:HTTP_METHOD_POST);
    if (err==ESP_OK) err=esp_http_client_set_post_field(client,j->kind==COL_GET_CONFIG?NULL:j->body,
        j->kind==COL_GET_CONFIG?0:(int)strlen(j->body));
    bool performed=false;
    if (err==ESP_OK) do {
        performed=true; err=esp_http_client_perform(client);
        if (err!=ESP_ERR_HTTP_EAGAIN) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    } while (col_mono()<deadline);
    if (err==ESP_ERR_HTTP_EAGAIN || col_mono()>=deadline) err=ESP_ERR_TIMEOUT;
    if (overflow) err=ESP_ERR_INVALID_SIZE;
    if (err==ESP_OK && !esp_http_client_is_complete_data_received(client)) err=ESP_FAIL;
    j->transport=err;
    if (performed) j->status=esp_http_client_get_status_code(client);
    /* 401 的 SDK Basic/Digest 处理可能返回错误；仍保留已解析的 HTTP 状态。 */
    if (err==ESP_OK) { /* 连接可复用 */ }
    else {
        int tls_code=0,flags=0;
        esp_err_t tls=esp_http_client_get_and_clear_last_tls_error(client,&tls_code,&flags);
        j->diagnostic=tls==ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME?"DNS_FAILED":
            flags & MBEDTLS_X509_BADCERT_CN_MISMATCH?"TLS_HOSTNAME":
            flags & (MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE)?"TLS_CERT_TIME":
            flags?"TLS_CERT_VERIFY":err==ESP_ERR_TIMEOUT?"HTTP_TIMEOUT":
            overflow?"RESPONSE_TOO_LARGE":tls_code?"TLS_HANDSHAKE":"HTTP_TRANSPORT";
        close_client();
    }
}
static void worker(void *unused)
{
    (void)unused; int64_t reconnect_at=0; unsigned delay=1000;
    for (;;) {
        col_job *j=NULL;
        if (xQueueReceive(jobs,&j,pdMS_TO_TICKS(100))==pdTRUE) {
            if (j->kind==COL_SET_CREDENTIALS) {
                j->transport=apply_credentials(&j->credentials); j->status=0;
                col_wipe(&j->credentials,sizeof(j->credentials)); reconnect_at=col_mono()+10000;
            } else if (j->kind==COL_CLEAR_CREDENTIALS) {
                close_client(); atomic_store(&configured,false); atomic_store(&online,false);
                (void)esp_wifi_disconnect(); wifi_config_t empty={0};
                j->transport=esp_wifi_set_config(WIFI_IF_STA,&empty);
                col_wipe(&credentials,sizeof(credentials)); j->status=0;
                atomic_store(&time_synced,false); atomic_store(&ip_addr,0);
                if (sntp_started) { esp_netif_sntp_deinit(); sntp_started=false; }
            } else request(j);
            xQueueSend(results,&j,portMAX_DELAY);
        }
        if (atomic_load(&configured) && !atomic_load(&online) && col_mono()>=reconnect_at) {
            (void)esp_wifi_connect(); reconnect_at=col_mono()+delay+(esp_random()%251);
            if (delay<60000) delay=delay>30000?60000:delay*2;
        } else if (atomic_load(&online)) delay=1000;
    }
}
esp_err_t col_network_init(QueueHandle_t in,QueueHandle_t out)
{
    jobs=in; results=out;
    dns_done=xSemaphoreCreateBinary(); if (!dns_done) return ESP_ERR_NO_MEM;
    esp_err_t err=demo_radio_network_prepare(); if (err!=ESP_OK) return err;
    if (!esp_netif_create_default_wifi_sta()) return ESP_ERR_NO_MEM;
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();
    err=esp_wifi_init(&init); if (err!=ESP_OK) return err;
    err=esp_wifi_set_storage(WIFI_STORAGE_RAM); if (err!=ESP_OK) return err;
    err=esp_event_handler_instance_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,NULL,&wifi_handler);
    if (err!=ESP_OK) return err;
    err=esp_event_handler_instance_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event,NULL,&ip_handler);
    if (err!=ESP_OK) return err;
    err=esp_wifi_set_mode(WIFI_MODE_STA); if (err!=ESP_OK) return err;
    err=esp_wifi_start(); if (err!=ESP_OK) return err;
    /* SDK 日志不能泄漏 SSID、地址、请求头或原始响应。 */
    esp_log_level_set("wifi",ESP_LOG_NONE); esp_log_level_set("HTTP_CLIENT",ESP_LOG_NONE);
    esp_log_level_set("esp-tls",ESP_LOG_NONE); esp_log_level_set("mbedtls",ESP_LOG_NONE);
    return xTaskCreate(worker,"collector_net",8192,NULL,4,&network_task)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
}
