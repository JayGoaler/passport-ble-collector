// Wokwi 专用：独立 GPIO 低有效按钮替代真实 Passport 的 GPIO0 ADC 分压。
// GPIO4/5/6 在真实设备上属于音频；仅采集器仿真使用，不得作为实机接线说明。
#include "sdkconfig.h"
#if !defined(CONFIG_COLLECTOR_WOKWI) || !CONFIG_COLLECTOR_WOKWI
#error "Wokwi buttons must only be compiled for CONFIG_COLLECTOR_WOKWI"
#endif
#include "bsp_button.h"
#include "bsp_pins.h"
#include "button_gpio.h"
#include "iot_button.h"
#include "esp_log.h"
#include <stdint.h>

static const char *TAG = "wokwi_btn";
static const int s_gpio[BSP_BTN_COUNT] = {
    [BSP_BTN_UP] = 4, [BSP_BTN_DOWN] = 5, [BSP_BTN_OK] = 6,
};
static button_handle_t s_btn[BSP_BTN_COUNT];
static bsp_btn_cb_t s_cb;
static void *s_user;
static volatile bool s_ready;

static void dispatch(void *button, void *index, bsp_btn_ev_t event)
{
    (void)button;
    if (s_ready && s_cb) s_cb((bsp_btn_t)(intptr_t)index, event, s_user);
}
static void pressed(void *b, void *i) { dispatch(b, i, BSP_BTN_PRESS); }
static void clicked(void *b, void *i) { dispatch(b, i, BSP_BTN_CLICK); }
static void doubled(void *b, void *i) { dispatch(b, i, BSP_BTN_DOUBLE); }
static void held(void *b, void *i) { dispatch(b, i, BSP_BTN_LONG); }

static void cleanup(void)
{
    s_ready = false;
    s_cb = NULL;
    s_user = NULL;
    for (int i = BSP_BTN_COUNT - 1; i >= 0; --i) {
        if (!s_btn[i]) continue;
        esp_err_t err = iot_button_delete(s_btn[i]);
        if (err == ESP_OK) s_btn[i] = NULL;
        else ESP_LOGE(TAG, "button %d cleanup: %s", i, esp_err_to_name(err));
    }
}

esp_err_t bsp_button_init(bsp_btn_cb_t cb, void *user)
{
    if (s_ready) {
        s_cb = cb;
        s_user = user;
        return ESP_OK;
    }
    for (int i = 0; i < BSP_BTN_COUNT; ++i) {
        if (s_btn[i]) return ESP_ERR_INVALID_STATE;
    }
    s_cb = cb;
    s_user = user;
    const button_config_t timing = {
        .short_press_time = BSP_BTN_SHORT_PRESS_MS,
        .long_press_time = BSP_BTN_LONG_PRESS_MS,
    };
    const button_event_t events[] = {
        BUTTON_PRESS_DOWN, BUTTON_SINGLE_CLICK, BUTTON_DOUBLE_CLICK,
        BUTTON_LONG_PRESS_START,
    };
    const button_cb_t callbacks[] = {pressed, clicked, doubled, held};
    for (int i = 0; i < BSP_BTN_COUNT; ++i) {
        const button_gpio_config_t gpio = {
            .gpio_num = s_gpio[i],
            .active_level = 0,
            .enable_power_save = false,
            .disable_pull = false, // 内部上拉；按钮另一端接 GND。
        };
        esp_err_t err = iot_button_new_gpio_device(&timing, &gpio, &s_btn[i]);
        if (err != ESP_OK || !s_btn[i]) {
            cleanup();
            return err == ESP_OK ? ESP_FAIL : err;
        }
        for (size_t j = 0; j < sizeof(events) / sizeof(events[0]); ++j) {
            err = iot_button_register_cb(s_btn[i], events[j], NULL,
                                        callbacks[j], (void *)(intptr_t)i);
            if (err != ESP_OK) {
                cleanup();
                return err;
            }
        }
    }
    s_ready = true;
    ESP_LOGI(TAG, "SIM GPIO UP=4 DOWN=5 OK=6 active-low, click=%dms long=%dms",
             BSP_BTN_SHORT_PRESS_MS, BSP_BTN_LONG_PRESS_MS);
    return ESP_OK;
}

int bsp_button_read_mv(void)
{
    return -1; // 模拟器没有真实 ADC 分压电压，不伪造读数。
}
