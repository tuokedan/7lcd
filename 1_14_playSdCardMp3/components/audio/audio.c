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
#include <string.h>
#include <math.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "driver/i2s.h"
#include "audio.h"
#include "esp_log.h"
#include "es8388.h"
#include "app_key.h"
#include "mp3dec.h"
#include "driver/touch_pad.h"
#include "pin_config.h"
#include "sdcard_list.h"
#include "sdcard_scan.h"
#include "lcd.h"
#include "zk.h"

static const char *TAG = "AUDIO";

#define SAMPLE_RATE     (44100)
#define I2S_NUM         (1)
extern playlist_operator_handle_t sdcard_list_handle;


enum {
    AUDIO_STOP = 0,
    AUDIO_PLAY,
    AUDIO_NEXT,
    AUDIO_LAST
};

int play_flag = AUDIO_STOP;
int audio_play_index = 0;
int cur_volume = 50;
int set_volume = 50;

void aplay_mp3(const char *path)
{
    ESP_LOGI(TAG, "start to decode %s", path);
    HMP3Decoder hMP3Decoder;
    MP3FrameInfo mp3FrameInfo;
    unsigned char *readBuf = malloc(MAINBUF_SIZE);
    char *url = NULL;

    if (readBuf == NULL) {
        ESP_LOGE(TAG, "readBuf malloc failed");
        return;
    }

    short *output = malloc(1153 * 4);

    if (output == NULL) {
        free(readBuf);
        ESP_LOGE(TAG, "outBuf malloc failed");
        return;
    }

    hMP3Decoder = MP3InitDecoder();

    if (hMP3Decoder == 0) {
        free(readBuf);
        free(output);
        ESP_LOGE(TAG, "memory is not enough..");
        return;
    }

    int samplerate = 0;
    i2s_zero_dma_buffer(I2S_NUM);
    FILE *mp3File = fopen(path, "rb");

    if (mp3File == NULL) {
        MP3FreeDecoder(hMP3Decoder);
        free(readBuf);
        free(output);
        ESP_LOGE(TAG, "open file failed");
        sdcard_list_next(sdcard_list_handle, 1, &url);
        return;
    }

    char tag[10];
    int tag_len = 0;
    int read_bytes = fread(tag, 1, 10, mp3File);

    if (read_bytes == 10) {
        if (memcmp(tag, "ID3", 3) == 0) {
            tag_len = ((tag[6] & 0x7F) << 21) | ((tag[7] & 0x7F) << 14) | ((tag[8] & 0x7F) << 7) | (tag[9] & 0x7F);
            // ESP_LOGI(TAG,"tag_len: %d %x %x %x %x", tag_len,tag[6],tag[7],tag[8],tag[9]);
            fseek(mp3File, tag_len - 10, SEEK_SET);
        } else {
            fseek(mp3File, 0, SEEK_SET);
        }
    }

    int bytesLeft = 0;
    unsigned char *readPtr = readBuf;
    play_flag = AUDIO_PLAY;

    while (1) {
        switch (play_flag) {
            case AUDIO_STOP: {
                while (!play_flag) {
                    i2s_zero_dma_buffer(I2S_NUM);
                    vTaskDelay(100 / portTICK_RATE_MS);
                }
            }
            break;

            case AUDIO_PLAY: {
            }
            break;

            case AUDIO_NEXT: {
                sdcard_list_next(sdcard_list_handle, 1, &url);
                goto stop;
            }
            break;

            case AUDIO_LAST: {
                sdcard_list_prev(sdcard_list_handle, 1, &url);

                goto stop;
            }
            break;
        }

        if (bytesLeft < MAINBUF_SIZE) {
            memmove(readBuf, readPtr, bytesLeft);
            int br = fread(readBuf + bytesLeft, 1, MAINBUF_SIZE - bytesLeft, mp3File);

            if ((br == 0) && (bytesLeft == 0)) {
                ESP_LOGI(TAG, "mp3file end");
                sdcard_list_next(sdcard_list_handle, 1, &url);
                break;
            }

            bytesLeft = bytesLeft + br;
            readPtr = readBuf;
        }

        int offset = MP3FindSyncWord(readPtr, bytesLeft);

        if (offset < 0) {
            ESP_LOGE(TAG, "MP3FindSyncWord not find");
            bytesLeft = 0;
            continue;
        } else {
            readPtr += offset;                    /*!< data start point */
            bytesLeft -= offset;                 /*!< in buffer */
            int errs = MP3Decode(hMP3Decoder, &readPtr, &bytesLeft, output, 0);

            if (errs != 0) {
                ESP_LOGE(TAG, "MP3Decode failed ,code is %d ", errs);
                sdcard_list_next(sdcard_list_handle, 1, &url);
                break;
            }

            MP3GetLastFrameInfo(hMP3Decoder, &mp3FrameInfo);

            if (samplerate != mp3FrameInfo.samprate) {
                samplerate = mp3FrameInfo.samprate;
                i2s_set_clk(I2S_NUM, samplerate, 16, mp3FrameInfo.nChans);
                ESP_LOGI(TAG, "mp3file info---bitrate=%d,layer=%d,nChans=%d,samprate=%d,outputSamps=%d", mp3FrameInfo.bitrate, mp3FrameInfo.layer, mp3FrameInfo.nChans, mp3FrameInfo.samprate, mp3FrameInfo.outputSamps);

                //显示
                {
                  char buff[200]={0};
                  sprintf(buff, "名字：%s",path);
                  Gui_DrawFont_GBK16(0,60,BLUE,WHITE, 0, (u8*)buff);

                  sprintf(buff, "采样率：%d",mp3FrameInfo.samprate);
                  Gui_DrawFont_GBK16(0,80,BLUE,WHITE, 0, (u8*)buff);

                  sprintf(buff, "通道数：%d",mp3FrameInfo.nChans);
                  Gui_DrawFont_GBK16(0,100,BLUE,WHITE, 0, (u8*)buff);

                  sprintf(buff, "bitrate：%d",mp3FrameInfo.bitrate);
                  Gui_DrawFont_GBK16(0,120,BLUE,WHITE, 0, (u8*)buff);

                  sprintf(buff, "layer:%d",mp3FrameInfo.layer);
                  Gui_DrawFont_GBK16(0,140,BLUE,WHITE, 0, (u8*)buff);

                  lcd_update();//刷新显示
                }
            }

            size_t bytes_write = 0;
            i2s_write(I2S_NUM, (const char *) output, mp3FrameInfo.outputSamps * 2, &bytes_write, 100 / portTICK_RATE_MS);
        }
    }

stop:
    i2s_zero_dma_buffer(I2S_NUM);
    MP3FreeDecoder(hMP3Decoder);
    free(readBuf);
    free(output);
    fclose(mp3File);

    ESP_LOGI(TAG, "end mp3 decode ..");
}

static void audio_task(void *arg)
{
    esp_err_t ret = 0;

    //I2S参数设置
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,// | I2S_MODE_RX,   //                                /*!<  Only TX */
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = 16,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,                           /*!< 1-channels */
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .dma_buf_count = 6,
        .dma_buf_len = 256,
        .use_apll = true,
        .tx_desc_auto_clear = true,     /*!< I2S auto clear tx descriptor if there is underflow condition (helps in avoiding noise in case of data unavailability) */
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,//ESP_INTR_FLAG_LEVEL2 | ESP_INTR_FLAG_IRAM,
    };

    //I2S硬件引脚设置
    i2s_pin_config_t pin_config = {
        .mck_io_num=I2S_MCLK,
        .bck_io_num = I2S_SCLK,
        .ws_io_num = I2S_LCLK,
        .data_out_num = I2S_DOUT,
        .data_in_num = I2S_DSIN
    };

    //I2S驱动加载
    ret = i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL);

    //I2S引脚配置
    ret = i2s_set_pin(I2S_NUM, &pin_config);

    while (1) {
        char *url = NULL;
        sdcard_list_current(sdcard_list_handle, &url);

        if(url==NULL) return;
        ESP_LOGW(TAG, "URL: %s", url);

       // vTaskDelay(100);

        aplay_mp3(url);
    }
}



static void audio_control_task(void *arg)
{
    uint32_t volume = 80;
    es8388_set_voice_volume(volume);

    while (1) {
        switch (GetKey()) {
            case 1: {//KEY2
                ESP_LOGI(TAG, "PLAY / STOP");
                play_flag = play_flag ? AUDIO_STOP : AUDIO_PLAY;
            }
            break;

            case 2: {//KEY3
                ESP_LOGI(TAG, "AUDIO_NEXT");
                play_flag = AUDIO_NEXT;
            }
            break;

            case 3: {//KEY4

                ESP_LOGI(TAG, "AUDIO_LAST");
                play_flag = AUDIO_LAST;
            }
            break;

            case 4: {//KEY5
            }
            break;

            default: {

            }
            break;
        }
        vTaskDelay(50 / portTICK_RATE_MS);
    }

}


int audio_mp3_init()
{
    es8388_init();//(1)
    es8388_config_i2s(AUDIO_HAL_I2S_NORMAL, AUDIO_HAL_BIT_LENGTH_16BITS);//(2)
    es8388_set_voice_volume(80);//(3)
    es8388_ctrl_state(AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

    xTaskCreate(audio_control_task, "audio_control_task", 1024*20, NULL, 5, NULL);
    xTaskCreate(audio_task, "audio_task", 1024*20, NULL, 5, NULL);

    return 0;
}


