if(CONFIG_COLLECTOR_WOKWI)
    # 仅模拟器替换面板和按键；真实 LVGL、圆角、I2C、电量等继续复用。
    set(display_source "../../sim/wokwi/bsp_display.c")
    set(button_source "../../sim/wokwi/bsp_button.c")
else()
    set(display_source "src/bsp_display.c")
    set(button_source "src/bsp_button.c")
endif()

idf_component_register(
    SRCS "src/bsp_i2c.c"
         "${display_source}"
         "src/bsp_display_lvgl.c"
         "src/bsp_display_rounding.c"
         "${button_source}"
         "src/bsp_audio.c"
         "src/bsp_es8311_sleep_check.c"
         "src/bsp_battery.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_i2c esp_driver_i2s esp_lcd esp_lvgl_port button esp_adc esp_codec_dev
    PRIV_REQUIRES esp_timer
)
