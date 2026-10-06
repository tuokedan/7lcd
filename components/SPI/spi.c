#include "spi.h"
#include <string.h>
#include "driver/gpio.h"
#include "esp_err.h"

spi_device_handle_t spi2_handle = NULL;

esp_err_t spi2_init(void)
{
    spi_bus_config_t bus_cfg = {
        .flags = SPICOMMON_BUSFLAG_MASTER,
        .isr_cpu_id = INTR_CPU_ID_AUTO,
        .max_transfer_sz = 240 * 240 * 2,
        .miso_io_num = GPIO_NUM_47,
        .mosi_io_num = GPIO_NUM_48,
        .sclk_io_num = GPIO_NUM_42,
        .quadhd_io_num = -1,
        .quadwp_io_num = -1,
    };

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    return ESP_OK;
}

uint8_t spi2_transfer_byte(uint8_t data)
{
    spi_transaction_t t = {0};

    t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    t.length = 8;
    t.tx_data[0] = data;

    if (spi_device_polling_transmit(spi2_handle, &t) != ESP_OK) {
        return 0;
    }

    return t.rx_data[0];
}

esp_err_t spi2_write_data(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_OK;
    }

    spi_transaction_t t = {0};
    t.length = len * 8;
    t.tx_buffer = data;

    return spi_device_transmit(spi2_handle, &t);
}
