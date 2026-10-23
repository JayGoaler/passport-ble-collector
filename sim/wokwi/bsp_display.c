// Wokwi 专用 ILI9341 适配：真实 Passport 仍使用 ST7789P3 及其厂商寄存器。
// https://docs.wokwi.com/parts/wokwi-ili9341 ：RST/LED 不参与仿真。
// 使用软件复位、不接 MISO；GPIO21 PWM 保留 API，但不验证背光亮度/休眠功耗。
#include "sdkconfig.h"
#if !defined(CONFIG_COLLECTOR_WOKWI) || !CONFIG_COLLECTOR_WOKWI
#error "Wokwi display must only be compiled for CONFIG_COLLECTOR_WOKWI"
#endif
#include "bsp_display.h"
#include "bsp_pins.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>

static const char *TAG = "wokwi_disp";
typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    uint8_t madctl;
    int x_gap;
    int y_gap;
} sim_panel_t;

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static bool s_bus_ready;
static bool s_bl_ready;
static bool s_ready;

static esp_err_t panel_reset(esp_lcd_panel_t *base)
{
    sim_panel_t *p = (sim_panel_t *)base;
    esp_err_t err = esp_lcd_panel_io_tx_param(p->io, LCD_CMD_SWRESET, NULL, 0);
    if (err == ESP_OK) {
        // Wokwi ILI9341 的竖屏原生基准为 MX|BGR。flow-01 实际截图验证：
        // MADCTL=0 会整屏左右镜像并互换红蓝；此处仍对外提供正常 RGB565 坐标。
        p->madctl = LCD_CMD_MX_BIT | LCD_CMD_BGR_BIT;
        p->x_gap = p->y_gap = 0;
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    return err;
}

static esp_err_t panel_init(esp_lcd_panel_t *base)
{
    sim_panel_t *p = (sim_panel_t *)base;
    esp_err_t err = esp_lcd_panel_io_tx_param(p->io, LCD_CMD_SLPOUT, NULL, 0);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(120));
    const uint8_t rgb565 = 0x55;
    err = esp_lcd_panel_io_tx_param(p->io, LCD_CMD_COLMOD, &rgb565, 1);
    if (err != ESP_OK) return err;
    // 数字模拟器只需标准命令；真实 ILI9341 面板电源/gamma 序列不在本适配范围。
    return esp_lcd_panel_io_tx_param(p->io, LCD_CMD_MADCTL, &p->madctl, 1);
}

static esp_err_t panel_del(esp_lcd_panel_t *base)
{
    free(base); // IO 和 SPI 总线由 BSP 持有。
    return ESP_OK;
}

static esp_err_t panel_draw(esp_lcd_panel_t *base, int xs, int ys, int xe, int ye,
                            const void *pixels)
{
    sim_panel_t *p = (sim_panel_t *)base;
    const int width = (p->madctl & LCD_CMD_MV_BIT) ? BSP_LCD_H : BSP_LCD_W;
    const int height = (p->madctl & LCD_CMD_MV_BIT) ? BSP_LCD_W : BSP_LCD_H;
    if (!pixels || xs < 0 || ys < 0 || xe <= xs || ye <= ys ||
        xe > width - p->x_gap || ye > height - p->y_gap) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t bytes = (size_t)(xe - xs) * (size_t)(ye - ys) * 2;
    xs += p->x_gap; xe += p->x_gap;
    ys += p->y_gap; ye += p->y_gap;
    const uint8_t columns[] = {xs >> 8, xs & 255, (xe - 1) >> 8, (xe - 1) & 255};
    const uint8_t rows[] = {ys >> 8, ys & 255, (ye - 1) >> 8, (ye - 1) & 255};
    esp_err_t err = esp_lcd_panel_io_tx_param(p->io, LCD_CMD_CASET, columns, 4);
    if (err != ESP_OK) return err;
    err = esp_lcd_panel_io_tx_param(p->io, LCD_CMD_RASET, rows, 4);
    if (err != ESP_OK) return err;
    // 原 LVGL port 已交换 RGB565 字节；保留异步 DMA 和 color-done 回调。
    return esp_lcd_panel_io_tx_color(p->io, LCD_CMD_RAMWR, pixels, bytes);
}

static esp_err_t set_madctl(sim_panel_t *p, uint8_t value)
{
    esp_err_t err = esp_lcd_panel_io_tx_param(p->io, LCD_CMD_MADCTL, &value, 1);
    if (err == ESP_OK) p->madctl = value;
    return err;
}

static esp_err_t panel_mirror(esp_lcd_panel_t *base, bool x, bool y)
{
    sim_panel_t *p = (sim_panel_t *)base;
    uint8_t value = p->madctl & ~(LCD_CMD_MX_BIT | LCD_CMD_MY_BIT);
    // 相对面板原生扫描方向应用逻辑镜像；LVGL 注册时会再次调用本函数。
    if (!x) value |= LCD_CMD_MX_BIT;
    if (y) value |= LCD_CMD_MY_BIT;
    return set_madctl(p, value);
}

static esp_err_t panel_swap(esp_lcd_panel_t *base, bool swap)
{
    sim_panel_t *p = (sim_panel_t *)base;
    uint8_t value = p->madctl & ~LCD_CMD_MV_BIT;
    if (swap) value |= LCD_CMD_MV_BIT;
    return set_madctl(p, value);
}

static esp_err_t panel_gap(esp_lcd_panel_t *base, int x, int y)
{
    sim_panel_t *p = (sim_panel_t *)base;
    const int width = (p->madctl & LCD_CMD_MV_BIT) ? BSP_LCD_H : BSP_LCD_W;
    const int height = (p->madctl & LCD_CMD_MV_BIT) ? BSP_LCD_W : BSP_LCD_H;
    if (x < 0 || y < 0 || x >= width || y >= height) return ESP_ERR_INVALID_ARG;
    p->x_gap = x;
    p->y_gap = y;
    return ESP_OK;
}

static esp_err_t panel_invert(esp_lcd_panel_t *base, bool invert)
{
    sim_panel_t *p = (sim_panel_t *)base;
    return esp_lcd_panel_io_tx_param(p->io, invert ? LCD_CMD_INVON : LCD_CMD_INVOFF, NULL, 0);
}

static esp_err_t panel_on(esp_lcd_panel_t *base, bool on)
{
    sim_panel_t *p = (sim_panel_t *)base;
    return esp_lcd_panel_io_tx_param(p->io, on ? LCD_CMD_DISPON : LCD_CMD_DISPOFF, NULL, 0);
}

static esp_err_t panel_sleep(esp_lcd_panel_t *base, bool sleep)
{
    sim_panel_t *p = (sim_panel_t *)base;
    esp_err_t err = esp_lcd_panel_io_tx_param(p->io, sleep ? LCD_CMD_SLPIN : LCD_CMD_SLPOUT, NULL, 0);
    if (err == ESP_OK) vTaskDelay(pdMS_TO_TICKS(120));
    return err;
}

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = BSP_BL_LEDC_MODE, .timer_num = BSP_BL_LEDC_TIMER,
        .duty_resolution = BSP_BL_LEDC_RES, .freq_hz = BSP_BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) return err;
    const ledc_channel_config_t channel = {
        .gpio_num = BSP_LCD_BL, .speed_mode = BSP_BL_LEDC_MODE,
        .channel = BSP_BL_LEDC_CHANNEL, .timer_sel = BSP_BL_LEDC_TIMER,
        .duty = 0, .hpoint = 0,
    };
    err = ledc_channel_config(&channel);
    if (err == ESP_OK) s_bl_ready = true;
    else ledc_timer_rst(BSP_BL_LEDC_MODE, BSP_BL_LEDC_TIMER);
    return err;
}

esp_err_t bsp_display_init(void)
{
    if (s_ready) return ESP_OK;
    if (s_panel || s_io || s_bus_ready) return ESP_ERR_INVALID_STATE;
    const spi_bus_config_t bus = {
        .mosi_io_num = BSP_LCD_MOSI, .sclk_io_num = BSP_LCD_SCLK,
        .miso_io_num = -1, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = BSP_LCD_W * 80 * 2,
    };
    esp_err_t err = spi_bus_initialize(BSP_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;
    s_bus_ready = true;
    const esp_lcd_panel_io_spi_config_t io = {
        .cs_gpio_num = BSP_LCD_CS, .dc_gpio_num = BSP_LCD_DC,
        .pclk_hz = BSP_LCD_PCLK_HZ, .spi_mode = BSP_LCD_SPI_MODE,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8, .trans_queue_depth = 10,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_HOST, &io, &s_io);
    if (err != ESP_OK) goto fail;
    sim_panel_t *panel = calloc(1, sizeof(*panel));
    if (!panel) { err = ESP_ERR_NO_MEM; goto fail; }
    panel->io = s_io;
    panel->base = (esp_lcd_panel_t){
        .reset = panel_reset, .init = panel_init, .del = panel_del,
        .draw_bitmap = panel_draw, .mirror = panel_mirror, .swap_xy = panel_swap,
        .set_gap = panel_gap, .invert_color = panel_invert,
        .disp_on_off = panel_on, .disp_sleep = panel_sleep,
    };
    s_panel = &panel->base;
    err = esp_lcd_panel_reset(s_panel);
    if (err == ESP_OK) err = esp_lcd_panel_init(s_panel);
    if (err == ESP_OK) err = esp_lcd_panel_invert_color(s_panel, false);
    if (err == ESP_OK) err = esp_lcd_panel_disp_on_off(s_panel, true);
    if (err == ESP_OK) err = backlight_init();
    if (err != ESP_OK) goto fail;
    s_ready = true;
    ESP_LOGI(TAG, "SIM ILI9341 %dx%d RGB565, MOSI=%d CLK=%d CS=%d DC=%d, software reset",
             BSP_LCD_W, BSP_LCD_H, BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC);
    return ESP_OK;

fail:
    if (s_panel) { esp_lcd_panel_del(s_panel); s_panel = NULL; }
    if (s_io) {
        const esp_err_t cleanup = esp_lcd_panel_io_del(s_io);
        if (cleanup == ESP_OK) s_io = NULL;
        else ESP_LOGE(TAG, "IO cleanup: %s", esp_err_to_name(cleanup));
    }
    if (!s_io && s_bus_ready) {
        const esp_err_t cleanup = spi_bus_free(BSP_LCD_SPI_HOST);
        if (cleanup == ESP_OK) s_bus_ready = false;
        else ESP_LOGE(TAG, "SPI cleanup: %s", esp_err_to_name(cleanup));
    }
    return err;
}

esp_lcd_panel_handle_t bsp_display_panel(void) { return s_ready ? s_panel : NULL; }
esp_lcd_panel_io_handle_t bsp_display_io(void) { return s_ready ? s_io : NULL; }

void bsp_display_backlight(uint8_t percent)
{
    if (!s_bl_ready) return;
    if (percent > 100) percent = 100;
    const uint32_t duty = (uint32_t)percent * ((1U << BSP_BL_LEDC_RES) - 1) / 100;
    esp_err_t err = ledc_set_duty(BSP_BL_LEDC_MODE, BSP_BL_LEDC_CHANNEL, duty);
    if (err == ESP_OK) err = ledc_update_duty(BSP_BL_LEDC_MODE, BSP_BL_LEDC_CHANNEL);
    if (err != ESP_OK) ESP_LOGW(TAG, "backlight PWM: %s", esp_err_to_name(err));
}

esp_err_t bsp_display_prepare_deep_sleep(void)
{
    // 模拟器不能验证真实引脚保持、背光或休眠电流；采集器不调用此 API。
    return ESP_ERR_NOT_SUPPORTED;
}
