#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../main/collector_usb.c"

static int64_t now;
static unsigned critical_depth,parse_count,queue_count,installed,vfs_calls,task_calls;
static int race_parse,race_queue;
static bool queue_full,reopen_in_reply;
static esp_err_t install_result;
static col_provisioning queued;
static char output[4096];
static const char credentials[]= "{\"ssid\":\"ssid-secret\",\"password\":\"password-secret\","
    "\"base_url\":\"https://collector.invalid:443\",\"node_id\":\"passport-host\","
    "\"token\":\"TTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTT\","
    "\"ca\":\"-----BEGIN CERTIFICATE-----\\nTEST_ONLY\\n-----END CERTIFICATE-----\"}";

bool col_parse_credentials_real(const char *json,col_credentials *value);
bool col_parse_credentials(const char *json,col_credentials *value)
{
    assert(critical_depth==0); parse_count++;
    bool ok=col_parse_credentials_real(json,value);
    if (race_parse==1) now+=120001;
    if (race_parse==2) { col_usb_open(false); col_usb_open(true); }
    if (race_parse==3) col_usb_open(false);
    return ok;
}
int64_t col_mono(void) { return now; }
void col_wipe(void *data,size_t size) { memset(data,0,size); }
void fake_critical_enter(portMUX_TYPE *lock) { assert(lock==&window_lock && !critical_depth); critical_depth++; }
void fake_critical_exit(portMUX_TYPE *lock) { assert(lock==&window_lock && critical_depth==1); critical_depth--; }
int xQueueSend(QueueHandle_t queue,const void *item,unsigned timeout)
{
    assert(queue==configs && critical_depth==0 && timeout==0);
    if (queue_full) return 0;
    queued=*(const col_provisioning *)item; queue_count++;
    if (race_queue==1) col_usb_open(false);
    if (race_queue==2) { col_usb_open(false); col_usb_open(true); }
    return pdTRUE;
}
esp_err_t usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t *config)
{
    assert(config->tx_buffer_size==1024 && config->rx_buffer_size==1024);
    installed++; return install_result;
}
void usb_serial_jtag_vfs_use_driver(void) { vfs_calls++; }
int xTaskCreate(void (*entry)(void *),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *handle)
{
    assert(entry==worker && !strcmp(name,"collector_usb") && stack>=4096 && !arg && priority>0 && handle==&usb_task);
    *handle=(void *)4; task_calls++; return pdPASS;
}
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t handle) { assert(handle==(void *)4); return 1024; }
int usb_serial_jtag_write_bytes(const void *data,size_t size,unsigned timeout)
{
    assert(critical_depth==0 && timeout==500 && strlen(output)+size<sizeof(output));
    memcpy(output+strlen(output),data,size);
    if (reopen_in_reply) { reopen_in_reply=false; col_usb_open(true); }
    return (int)size;
}
int usb_serial_jtag_read_bytes(void *data,size_t size,unsigned timeout)
{ (void)data; (void)size; (void)timeout; abort(); }
static void clean(void)
{
    now=1000; critical_depth=parse_count=queue_count=installed=vfs_calls=task_calls=0;
    race_parse=race_queue=0; queue_full=reopen_in_reply=false; install_result=ESP_OK;
    memset(line,0,sizeof(line)); memset(&parsed,0,sizeof(parsed)); memset(&queued,0,sizeof(queued));
    memset(output,0,sizeof(output)); used=0; too_long=false; line_generation=0; usb_task=NULL;
    atomic_store(&generation,0); atomic_store(&open_until,0); configs=(void *)1;
}
static void byte(char ch) { receive_step(1,ch,col_usb_generation()); }
static void feed(const char *text) { while (*text) byte(*text++); }
static void submit(void) { feed(credentials); byte('\n'); }
static void wiped(void)
{
    for (size_t i=0;i<sizeof(line);i++) assert(line[i]==0);
    for (size_t i=0;i<sizeof(parsed);i++) assert(((unsigned char *)&parsed)[i]==0);
    assert(!used && !too_long);
}
static void status(const char *name)
{
    char expected[160]; snprintf(expected,sizeof(expected),"COLLECTOR {\"status\":\"%s\"}\n",name);
    assert(!strcmp(output,expected));
    assert(!strstr(output,"ssid-secret") && !strstr(output,"password-secret") && !strstr(output,"TTTT") && !strstr(output,"TEST_ONLY"));
}
int main(void)
{
    clean(); assert(col_usb_stack()==0); assert(col_usb_init((void *)1)==ESP_OK);
    assert(installed==1 && vfs_calls==1 && task_calls==1 && col_usb_stack()==1024);
    clean(); install_result=ESP_FAIL; assert(col_usb_init((void *)1)==ESP_FAIL); assert(!vfs_calls && !task_calls);
    puts("USB: 驱动初始化和任务接口 PASS");

    clean(); submit(); status("CONFIG_LOCKED"); assert(!queue_count && !parse_count); wiped();
    clean(); col_usb_open(true); byte('\n'); status("INPUT_SIZE"); assert(!parse_count); wiped();
    clean(); col_usb_open(true); feed("{\"ssid\":\"ssid-secret\"}\n"); status("INVALID_FIELDS"); assert(parse_count==1 && !queue_count); wiped();
    clean(); col_usb_open(true); feed(credentials); byte('\r'); assert(!output[0]); byte('\n');
    status("RECEIVED_RAM_ONLY"); assert(queue_count==1 && queued.generation==col_usb_generation() && !col_usb_is_open());
    assert(!strcmp(queued.credentials.password,"password-secret")); wiped();
    puts("USB: 锁定、空行、坏字段、CRLF、禁止回显、RAM_ONLY后关闭 PASS");

    clean(); col_usb_open(true); feed("{\"ssid\":\"old"); assert(used);
    now+=120001; receive_step(0,0,col_usb_generation()); wiped(); assert(!col_usb_is_open() && !output[0]);
    col_usb_open(true); submit(); status("RECEIVED_RAM_ONLY"); assert(queue_count==1); wiped();
    clean(); col_usb_open(true); feed("{\"ssid\":\"old"); uint32_t old=col_usb_generation();
    col_usb_open(false); col_usb_open(true); receive_step(1,'x',old); wiped();
    submit(); status("RECEIVED_RAM_ONLY"); assert(queue_count==1 && queued.generation!=old); wiped();
    clean(); col_usb_open(true); feed("{\"ssid\":\"old"); now+=120001; col_usb_open(true);
    submit(); status("RECEIVED_RAM_ONLY"); assert(queue_count==1); wiped();
    puts("USB: 超时及close/open未被worker观察时丢弃旧半行和跨窗read字节 PASS");

    clean(); col_usb_open(true); for (size_t i=0;i<sizeof(line)+10;i++) byte('x');
    assert(too_long && used==sizeof(line)-1 && !output[0]); byte('\n');
    status("INPUT_SIZE"); assert(!parse_count && !queue_count); wiped();
    clean(); col_usb_open(true); for (size_t i=0;i<sizeof(line)+1;i++) byte('x');
    col_usb_open(false); col_usb_open(true); submit(); status("RECEIVED_RAM_ONLY"); assert(queue_count==1); wiped();
    clean(); col_usb_open(true); queue_full=true; submit(); status("BUSY"); assert(!queue_count && col_usb_is_open()); wiped();
    memset(output,0,sizeof(output)); queue_full=false; submit(); status("RECEIVED_RAM_ONLY"); assert(queue_count==1); wiped();
    puts("USB: 超长行及overflow跨窗恢复、队列满有界失败和重试 PASS");

    for (int race=1;race<=3;race++) {
        clean(); col_usb_open(true); race_parse=race; submit();
        status("CONFIG_LOCKED"); assert(!queue_count); wiped();
    }
    for (int race=1;race<=2;race++) {
        clean(); col_usb_open(true); old=col_usb_generation(); race_queue=race; submit();
        status("CONFIG_LOCKED"); assert(queue_count==1 && queued.generation==old && queued.generation!=col_usb_generation());
        assert(col_usb_is_open()==(race==2)); wiped();
    }
    clean(); col_usb_open(true); old=col_usb_generation(); reopen_in_reply=true; submit();
    status("RECEIVED_RAM_ONLY"); assert(queue_count==1 && queued.generation==old && col_usb_is_open() && col_usb_generation()!=old); wiped();
    puts("USB: parse/queue/回执期间clear及reopen竞争保留窗口代际隔离 PASS");
    puts("Collector USB source tests: PASS (设备USB串口和物理按键 NOT RUN)");
    return 0;
}
