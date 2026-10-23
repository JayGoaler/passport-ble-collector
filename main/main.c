// 蓝牙采集器入口；不加载基线硬件测试菜单。
#include "collector_platform.h"
#include "esp_log.h"
void app_main(void)
{
    esp_log_level_set("*",ESP_LOG_NONE);
    /* 仅本应用的固定故障码可输出，启动分配失败也能通过控制台诊断。 */
    esp_log_level_set("collector",ESP_LOG_ERROR);
    col_controller_start();
}
