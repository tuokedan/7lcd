#ifndef __MYSPI_H_
#define __MYSPI_H_

#include <stddef.h>
#include <stdint.h>
#include "driver/spi_master.h"
#include "esp_err.h"

extern spi_device_handle_t spi2_handle;

esp_err_t spi2_init(void);
uint8_t spi2_transfer_byte(uint8_t data);
esp_err_t spi2_write_data(const uint8_t *data, size_t len);

#endif
