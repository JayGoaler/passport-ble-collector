#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
esp_err_t bsp_display_init(void);
bool bsp_lvgl_init(void);
bool bsp_lvgl_lock(uint32_t timeout);
void bsp_lvgl_unlock(void);
void bsp_display_backlight(int value);
