#include "platform.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ESP_ERR_INVALID_ARG 0x102
void vTaskDelete(TaskHandle_t task);
#include "../sim/wokwi/collector_scan.c"

struct fake_queue { unsigned capacity,count; col_sample data[16]; };
static struct fake_queue queue;
static void (*task_function)(void *);
static void *task_argument;
static int task_marker;
static bool create_failure,interrupt_on_clock,pause_after_send;
static unsigned creates,deletes,delays;
static int64_t now=10000;
static jmp_buf tick_finished;

int xQueueSend(QueueHandle_t q,const void *p,unsigned ticks)
{
    assert(q==&queue && ticks==0);
    if (q->count==q->capacity) return pdFALSE;
    q->data[q->count++]=*(const col_sample *)p;
    if (pause_after_send) { pause_after_send=false; assert(col_scan_set(false)==ESP_OK); }
    return pdTRUE;
}
int xTaskCreate(void (*fn)(void *),const char *name,unsigned stack,void *arg,
                unsigned priority,TaskHandle_t *out)
{
    creates++;
    assert(!strcmp(name,"wokwi_scan") && stack>=2048 && stack<=4096 && priority==5);
    if (create_failure) return pdFALSE;
    task_function=fn; task_argument=arg; *out=&task_marker; return pdPASS;
}
void vTaskDelete(TaskHandle_t task)
{ assert(task==&task_marker); deletes++; task_function=NULL; }
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task)
{ assert(task==&task_marker); return 1234; }
void vTaskDelay(unsigned ticks)
{ assert(ticks==250); delays++; now+=ticks; longjmp(tick_finished,1); }
int64_t col_mono(void)
{
    if (interrupt_on_clock) {
        interrupt_on_clock=false;
        assert(col_scan_set(false)==ESP_OK && col_scan_set(true)==ESP_OK);
    }
    return now;
}
int64_t col_utc(void) { return 1792713600000LL+now; }
static void tick(void)
{
    assert(task_function);
    if (!setjmp(tick_finished)) task_function(task_argument);
}
static void check_sample(const col_sample *p,unsigned device,unsigned sequence,
                         uint32_t expected_epoch,int64_t expected_mono)
{
    static const char *addresses[]={"C0:FF:EE:F0:00:01","C0:FF:EE:F0:00:02","C0:FF:EE:F0:00:03"};
    static const char *names[]={"Wokwi-A","Wokwi-B","Wokwi-C"};
    static const char *uuids[]={"180F","12345678","12345678-1234-5678-9ABC-DEF012345678"};
    static const int expected_rssi[][4]={{-63,-60,-57,-54},{-80,-82,-79,-81},{-45,-47,-42,-44}};
    assert(!strcmp(p->meta.address,addresses[device]) && !strcmp(p->meta.name,names[device]));
    assert(p->meta.address_type==2 && p->meta.uuid_count==1 && !strcmp(p->meta.uuids[0],uuids[device]));
    assert(p->meta.has_manufacturer && p->meta.manufacturer==0xffff-device);
    assert(p->meta.has_tx==(device!=2));
    if (device!=2) assert(p->meta.tx==(device==0?-8:-4));
    assert(p->rssi==expected_rssi[device][sequence%4]);
    assert(p->epoch==expected_epoch && p->mono_ms==expected_mono);
    assert(p->utc_ms==1792713600000LL+expected_mono);
}
int main(void)
{
    queue.capacity=16;
    assert(!col_scan_ready() && !col_scan_active() && !col_scan_lost() && !col_scan_stack());
    assert(col_scan_set(true)==ESP_ERR_INVALID_STATE && col_scan_set(false)==ESP_OK);
    assert(col_scan_shutdown()==ESP_OK && col_scan_init(NULL)==ESP_ERR_INVALID_ARG);
    create_failure=true;
    assert(col_scan_init(&queue)==ESP_ERR_NO_MEM && !col_scan_ready() && !col_scan_active());
    assert(col_scan_set(true)==ESP_ERR_INVALID_STATE && col_scan_shutdown()==ESP_OK && deletes==0);
    create_failure=false;
    assert(col_scan_init(&queue)==ESP_OK && col_scan_ready() && !col_scan_active());
    assert(col_scan_stack()==1234 && col_scan_init(&queue)==ESP_ERR_INVALID_STATE && creates==2);
    tick(); assert(queue.count==0 && col_scan_epoch()==0);
    puts("wokwi scan: readiness, creation failure recovery and idle silence PASS");

    assert(col_scan_set(true)==ESP_OK && col_scan_active());
    uint32_t first_epoch=col_scan_epoch(); assert(first_epoch==1);
    assert(col_scan_set(true)==ESP_OK && col_scan_epoch()==first_epoch);
    int64_t first_time=now;
    for (unsigned i=0;i<4;i++) tick();
    assert(queue.count==12 && delays==5 && col_scan_lost()==0);
    for (unsigned i=0;i<12;i++) check_sample(&queue.data[i],i%3,i/3,first_epoch,first_time+250*(i/3));
    puts("wokwi scan: real AD parser, three identities, 16/32/128-bit UUIDs, four RSSI frames and timestamps PASS");

    assert(col_scan_set(false)==ESP_OK && !col_scan_active() && col_scan_epoch()==first_epoch+1);
    uint32_t stopped_epoch=col_scan_epoch();
    assert(col_scan_set(false)==ESP_OK && col_scan_epoch()==stopped_epoch);
    tick(); assert(queue.count==12); /* 暂停不清除控制器拥有的队列。 */
    assert(col_scan_set(true)==ESP_OK && col_scan_epoch()==stopped_epoch+1);
    for (unsigned i=0;i<12;i++) assert(queue.data[i].epoch!=col_scan_epoch());
    queue.count=0; tick(); assert(queue.count==3);
    for (unsigned i=0;i<3;i++) check_sample(&queue.data[i],i,4,col_scan_epoch(),now-250);
    puts("wokwi scan: idempotent start/stop, epoch isolation and deterministic resume PASS");

    queue.count=0; queue.capacity=2;
    tick(); assert(queue.count==2 && col_scan_lost()==1);
    col_sample retained[2]; memcpy(retained,queue.data,sizeof(retained));
    tick(); assert(queue.count==2 && col_scan_lost()==4 && !memcmp(retained,queue.data,sizeof(retained)));
    queue.count=0; queue.capacity=16;
    tick(); assert(queue.count==3 && col_scan_lost()==4);
    puts("wokwi scan: nonblocking queue overflow, exact loss count, retained FIFO data and recovery PASS");

    queue.count=0; uint32_t before_race=col_scan_epoch(); interrupt_on_clock=true;
    tick(); assert(queue.count==0 && col_scan_active() && col_scan_epoch()==before_race+2);
    tick(); assert(queue.count==3);
    for (unsigned i=0;i<3;i++) assert(queue.data[i].epoch==col_scan_epoch());
    queue.count=0; uint32_t queued_epoch=col_scan_epoch(); pause_after_send=true;
    tick(); assert(queue.count==1 && !col_scan_active() && queue.data[0].epoch==queued_epoch);
    assert(queue.data[0].epoch!=col_scan_epoch() && col_scan_lost()==4);
    tick(); assert(queue.count==1);
    puts("wokwi scan: pause/restart during generation rejects stale samples; queued old epoch remains identifiable PASS");

    assert(col_scan_set(true)==ESP_OK);
    uint32_t before_shutdown=col_scan_epoch();
    assert(col_scan_shutdown()==ESP_OK && !col_scan_active() && !col_scan_ready());
    assert(col_scan_epoch()==before_shutdown+1 && col_scan_stack()==0 && deletes==1 && queue.count==1);
    assert(col_scan_shutdown()==ESP_OK && deletes==1 && col_scan_set(true)==ESP_ERR_INVALID_STATE);
    assert(col_scan_init(&queue)==ESP_OK && col_scan_set(true)==ESP_OK);
    assert(col_scan_epoch()>before_shutdown && col_scan_lost()==4);
    queue.count=0; tick(); assert(queue.count==3);
    for (unsigned i=0;i<3;i++) check_sample(&queue.data[i],i,0,col_scan_epoch(),now-250);
    assert(col_scan_shutdown()==ESP_OK && creates==3 && deletes==2);
    puts("wokwi scan: shutdown/reinitialization preserves boot loss and epoch history; no worker leak PASS");
    return 0;
}
