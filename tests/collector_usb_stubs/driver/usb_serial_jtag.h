#pragma once
#include <stddef.h>
#include "esp_err.h"
typedef struct { size_t tx_buffer_size,rx_buffer_size; } usb_serial_jtag_driver_config_t;
esp_err_t usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t *config);
int usb_serial_jtag_write_bytes(const void *data,size_t size,unsigned timeout);
int usb_serial_jtag_read_bytes(void *data,size_t size,unsigned timeout);
