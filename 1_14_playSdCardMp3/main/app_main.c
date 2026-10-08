// Copyright 2019 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/apps/sntp.h"
#include "i2c_bus.h"
#include "driver/rmt.h"
#include "audio.h"
#include "es8388.h"
#include "pin_config.h"

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"


#include "sdcard_list.h"
#include "sdcard_scan.h"
#include "app_key.h"
#include "lcd.h"
#include "zk.h"



static const char *TAG = "main";

#define MOUNT_POINT "/sdcard"
static sdmmc_card_t *mount_card = NULL;
playlist_operator_handle_t sdcard_list_handle = NULL;


static esp_err_t sdcard_fat_init(sdmmc_card_t **card_handle)
{
    esp_err_t ret;

    // Options for mounting the filesystem.
    // If format_if_mount_failed is set to true, SD card will be partitioned and
    // formatted in case when mounting fails.
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,//加载失败，是否格式化
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    sdmmc_card_t *card;
    const char mount_point[] = MOUNT_POINT;
    ESP_LOGI(TAG, "Initializing SD card");

    // Use settings defined above to initialize SD card and mount FAT filesystem.
    // Note: esp_vfs_fat_sdmmc/sdspi_mount is all-in-one convenience functions.
    // Please check its source code and implement error recovery when developing
    // production applications.

    ESP_LOGI(TAG, "Using SDMMC peripheral");
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    // This initializes the slot without card detect (CD) and write protect (WP) signals.
    // Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

    slot_config.width = 1;//1：使用单总线方式  4：使用4线方式

    //数据线定义
    slot_config.clk = SDCARD_PIN_CLK;
    slot_config.cmd = SDCARD_PIN_CMD;
    slot_config.d0 = SDCARD_PIN_D0;

    // Enable internal pullups on enabled pins. The internal pullups
    // are insufficient however, please make sure 10k external pullups are
    // connected on the bus. This is for debug / example purpose only.
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "Mounting filesystem");
    //加载
    ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. "
                     "If you want the card to be formatted, set the EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). "
                     "Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
        }
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Filesystem mounted");

    // Card has been initialized, print its properties
    sdmmc_card_print_info(stdout, card);
    if(card_handle) *card_handle = card;
    return ESP_OK;

}

void sdcard_url_save_cb(void *user_data, char *url)
{
    playlist_operator_handle_t sdcard_handle = (playlist_operator_handle_t)user_data;
    esp_err_t ret = sdcard_list_save(sdcard_handle, url);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Fail to save sdcard url to sdcard playlist");
    }
}

void app_main()
{
    /*!< Print basic information */
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_TCP", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_SSL", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);

    /*!< Initialize NVS */
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    //LCD初始化
    lcd_init();

    //SD卡初始化
    sdcard_fat_init(&mount_card);
    if (mount_card == NULL) {

        Gui_DrawFont_GBK24(0,160,RED,WHITE, 0, (u8*)"请检查SD卡是否插入?");   //24x24字符
        lcd_update();//刷新显示

        ESP_LOGE(TAG, "init SD Card failed !");
        while(1) vTaskDelay(100);
    }

    //显示屏提示
    lcd_clear();
    Gui_DrawFont_GBK32(40,10,BLUE,WHITE, 0, (u8*)"音乐播放器");   //32x32汉字
    lcd_update();//刷新显示

    //枚举SD卡里的MP3文件
    sdcard_list_create(&sdcard_list_handle);
    sdcard_scan(sdcard_url_save_cb, MOUNT_POINT, 0, (const char *[]) {"mp3"}, 1, sdcard_list_handle);
    sdcard_list_show(sdcard_list_handle);

    //I2C初始化
    ESP_ERROR_CHECK(i2c_bus_init());
    //初始化按键
    initKey();
    //进入MP3播放
    audio_mp3_init();
}


