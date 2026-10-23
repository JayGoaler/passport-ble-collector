#include "collector_platform.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static atomic_llong open_until;
static QueueHandle_t configs;
static char line[8192];
static col_provisioning parsed;
static atomic_uint generation;
static TaskHandle_t usb_task;
static portMUX_TYPE window_lock=portMUX_INITIALIZER_UNLOCKED;
static size_t used;
static bool too_long;
static uint32_t line_generation;
void col_usb_open(bool open)
{
    portENTER_CRITICAL(&window_lock);
    atomic_fetch_add(&generation,1); atomic_store(&open_until,open?col_mono()+120000:0);
    portEXIT_CRITICAL(&window_lock);
}
uint32_t col_usb_generation(void) { return atomic_load(&generation); }
uint32_t col_usb_stack(void) { return usb_task ? uxTaskGetStackHighWaterMark(usb_task) : 0; }
bool col_usb_is_open(void) { return col_mono()<atomic_load(&open_until); }
static void reply(const char *s)
{ usb_serial_jtag_write_bytes(s,strlen(s),pdMS_TO_TICKS(500)); }
/* 只关闭本行所属窗口；控制线程期间新开的窗口不受旧回执影响。 */
static bool close_received_window(uint32_t expected)
{
    portENTER_CRITICAL(&window_lock);
    bool same=expected==atomic_load(&generation);
    if (same) atomic_store(&open_until,0);
    portEXIT_CRITICAL(&window_lock);
    return same;
}
static void receive_step(int n,char ch,uint32_t read_generation)
{
    portENTER_CRITICAL(&window_lock);
    uint32_t current=atomic_load(&generation);
    bool open=col_mono()<atomic_load(&open_until);
    portEXIT_CRITICAL(&window_lock);
    if (line_generation!=current || !open) {
        if (used || too_long) col_wipe(line,sizeof(line));
        used=0; too_long=false; line_generation=current;
    }
    if (n!=1 || read_generation!=current) return;
    if (ch=='\r') return;
    if (ch=='\n') {
        if (!open) reply("COLLECTOR {\"status\":\"CONFIG_LOCKED\"}\n");
        else if (too_long || !used) reply("COLLECTOR {\"status\":\"INPUT_SIZE\"}\n");
        else {
            line[used]=0; parsed.generation=line_generation;
            if (!col_parse_credentials(line,&parsed.credentials)) reply("COLLECTOR {\"status\":\"INVALID_FIELDS\"}\n");
            else if (!col_usb_is_open() || parsed.generation!=atomic_load(&generation)) reply("COLLECTOR {\"status\":\"CONFIG_LOCKED\"}\n");
            else if (xQueueSend(configs,&parsed,0)!=pdTRUE) reply("COLLECTOR {\"status\":\"BUSY\"}\n");
            else if (!close_received_window(parsed.generation)) reply("COLLECTOR {\"status\":\"CONFIG_LOCKED\"}\n");
            else reply("COLLECTOR {\"status\":\"RECEIVED_RAM_ONLY\"}\n");
            col_wipe(&parsed,sizeof(parsed));
        }
        col_wipe(line,sizeof(line)); used=0; too_long=false;
    } else if (open) {
        if (used+1<sizeof(line)) line[used++]=ch; else too_long=true;
    }
}
static void worker(void *unused)
{
    (void)unused;
    for (;;) {
        char ch=0; uint32_t read_generation=col_usb_generation();
        int n=usb_serial_jtag_read_bytes(&ch,1,pdMS_TO_TICKS(100));
        receive_step(n,ch,read_generation);
    }
}
esp_err_t col_usb_init(QueueHandle_t q)
{
    configs=q;
    usb_serial_jtag_driver_config_t c={.tx_buffer_size=1024,.rx_buffer_size=1024};
    esp_err_t err=usb_serial_jtag_driver_install(&c); if (err!=ESP_OK) return err;
    usb_serial_jtag_vfs_use_driver();
    return xTaskCreate(worker,"collector_usb",8192,NULL,3,&usb_task)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
}
