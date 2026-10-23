set(collector_scan "collector_scan.c")
set(collector_network "collector_network.c")
if(CONFIG_COLLECTOR_WOKWI)
    set(collector_scan "../sim/wokwi/collector_scan.c")
    set(collector_network "../sim/wokwi/collector_network.c")
    if(CONFIG_BT_ENABLED)
        message(FATAL_ERROR "Wokwi configuration must disable real Bluetooth")
    endif()
endif()
idf_component_register(
    SRCS "main.c" "demo_radio.c" "collector_core.c" "${collector_scan}"
         "${collector_network}" "collector_usb.c" "collector_controller.c" "collector_ui.c" "../assets/fonts/collector_font_16.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash
             json http_parser esp_http_client esp-tls mbedtls lwip esp_driver_usb_serial_jtag
)
