#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_HTTP_EAGAIN 0x7007
#define ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME 0x8001
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(x) ((unsigned)(x))
#define ESP_LOG_NONE 0
#define WIFI_STORAGE_RAM 1
#define WIFI_IF_STA 0
#define WIFI_MODE_STA 1
#define ESP_EVENT_ANY_ID -1
#define WIFI_REASON_AUTH_FAIL 202
#define WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT 15
#define WIFI_REASON_NO_AP_FOUND 201
#define IP_EVENT 1
#define WIFI_EVENT 2
#define IP_EVENT_STA_GOT_IP 1
#define WIFI_EVENT_STA_DISCONNECTED 2
#define HTTP_METHOD_GET 0
#define HTTP_METHOD_POST 1
#define MBEDTLS_X509_BADCERT_CN_MISMATCH 4
#define MBEDTLS_X509_BADCERT_EXPIRED 1
#define MBEDTLS_X509_BADCERT_FUTURE 512
typedef void *TaskHandle_t;
typedef void *QueueHandle_t;
typedef struct fake_semaphore *SemaphoreHandle_t;
typedef int esp_event_base_t;
typedef int esp_event_handler_instance_t;
typedef struct { struct { struct { uint32_t addr; } ip; } ip_info; } ip_event_got_ip_t;
typedef struct { int reason; } wifi_event_sta_disconnected_t;
typedef struct { struct { unsigned char ssid[32],password[64]; } sta; } wifi_config_t;
typedef struct { int unused; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
typedef struct { void (*sync_cb)(struct timeval *); } esp_sntp_config_t;
#define ESP_SNTP_SERVER_LIST(...) 0
#define ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(...) ((esp_sntp_config_t){0})
typedef struct { int unused; } mbedtls_x509_crt;
struct fake_http;
typedef struct fake_http *esp_http_client_handle_t;
typedef struct {
    int event_id;
    esp_http_client_handle_t client;
    void *data;
    int data_len;
} esp_http_client_event_t;
enum { HTTP_EVENT_ERROR,HTTP_EVENT_ON_CONNECTED,HTTP_EVENT_HEADERS_SENT,
       HTTP_EVENT_ON_HEADER,HTTP_EVENT_ON_DATA,HTTP_EVENT_ON_FINISH,HTTP_EVENT_DISCONNECTED };
typedef struct {
    const char *url,*cert_pem,*common_name;
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    int timeout_ms,buffer_size,buffer_size_tx,max_authorization_retries;
    bool disable_auto_redirect,keep_alive_enable,is_async,skip_cert_common_name_check;
    bool use_global_ca_store;
    void *crt_bundle_attach;
} esp_http_client_config_t;
typedef int err_t;
typedef struct { bool v6; char text[48]; } ip_addr_t;
#define ERR_OK 0
#define ERR_INPROGRESS -5
#define LWIP_DNS_ADDRTYPE_IPV4_IPV6 3
#define IP_IS_V6(p) ((p)->v6)
typedef void (*dns_found_callback)(const char *,const ip_addr_t *,void *);
int xQueueReceive(QueueHandle_t,void *,unsigned);
int xQueueSend(QueueHandle_t,const void *,unsigned);
int xTaskCreate(void (*)(void *),const char *,unsigned,void *,unsigned,TaskHandle_t *);
void vTaskDelay(unsigned);
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
int xSemaphoreGive(SemaphoreHandle_t);
int xSemaphoreTake(SemaphoreHandle_t,unsigned);
int64_t esp_timer_get_time(void);
uint32_t esp_random(void);
void *esp_netif_create_default_wifi_sta(void);
esp_err_t esp_event_handler_instance_register(esp_event_base_t,int32_t,void (*)(void *,esp_event_base_t,int32_t,void *),void *,esp_event_handler_instance_t *);
esp_err_t esp_wifi_init(const wifi_init_config_t *);
esp_err_t esp_wifi_set_storage(int);
esp_err_t esp_wifi_set_mode(int);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_set_config(int,const wifi_config_t *);
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *);
void esp_netif_sntp_deinit(void);
void esp_log_level_set(const char *,int);
void mbedtls_x509_crt_init(mbedtls_x509_crt *);
void mbedtls_x509_crt_free(mbedtls_x509_crt *);
int mbedtls_x509_crt_parse(mbedtls_x509_crt *,const unsigned char *,size_t);
err_t dns_gethostbyname_addrtype(const char *,ip_addr_t *,dns_found_callback,void *,int);
err_t tcpip_try_callback(void (*)(void *),void *);
char *ipaddr_ntoa_r(const ip_addr_t *,char *,int);
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
esp_err_t esp_http_client_close(esp_http_client_handle_t);
esp_err_t esp_http_client_set_url(esp_http_client_handle_t,const char *);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t,const char *,const char *);
esp_err_t esp_http_client_set_method(esp_http_client_handle_t,int);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t,const char *,int);
esp_err_t esp_http_client_perform(esp_http_client_handle_t);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
esp_err_t esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t,int *,int *);
