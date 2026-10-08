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
#include "pin_config.h"
#include "lcd.h"
#include "zk.h"

static const char *TAG = "AUDIO";

struct wavinfo
{
    char *name;       // wav file path
    bool status;      // playing is 1, stop is 0
    int current_time;    // last stop time

    int bit;          // bit
    int channel;
    int sample_rate;
    int while_time;   // sample num
    int format;
    struct wavinfo * next;
};



struct wavinfo current_wav_msg;


#define I2S_NUM         (0)

#define NUM_CHANNELS        (2)
#define CONFIG_EXAMPLE_BIT_SAMPLE (16)
#define SAMPLE_RATE         (16000)
#define SAMPLE_SIZE         (CONFIG_EXAMPLE_BIT_SAMPLE * 1024)
#define BYTE_RATE           (SAMPLE_RATE * (CONFIG_EXAMPLE_BIT_SAMPLE / 8)) * NUM_CHANNELS

const int WAVE_HEADER_SIZE = 44;


uint8_t i2s_read_buff[1153 * 10]={0};
uint16_t bytes_read=0;
uint16_t bytes_write=0;

#define RECORD_FILE "/sdcard/record.wav"  //录音文件
#define RECORD_TIME 5      //录音时间



void generate_wav_header(char* wav_header, uint32_t wav_size, uint32_t sample_rate){

    // See this for reference: http://soundfile.sapp.org/doc/WaveFormat/
    uint32_t file_size = wav_size + WAVE_HEADER_SIZE - 8;
    uint32_t byte_rate = BYTE_RATE;

    const char set_wav_header[] = {
        'R','I','F','F', // ChunkID
        (char)file_size, (char)(file_size >> 8), (char)(file_size >> 16), (char)(file_size >> 24), // ChunkSize
        'W','A','V','E', // Format
        'f','m','t',' ', // Subchunk1ID
        0x10, 0x00, 0x00, 0x00, // Subchunk1Size (16 for PCM)
        0x01, 0x00, // AudioFormat (1 for PCM)
        (char)(NUM_CHANNELS), (char)(NUM_CHANNELS >> 8), // NumChannels
        (char)(sample_rate), (char)(sample_rate >> 8), (char)(sample_rate >> 16), (char)(sample_rate >> 24), // SampleRate
        (char)(byte_rate), (char)(byte_rate >> 8), (char)(byte_rate >> 16), (char)(byte_rate >> 24), // ByteRate
        0x02, 0x00, // BlockAlign
        0x10, 0x00, // BitsPerSample (16 bits)
        'd','a','t','a', // Subchunk2ID
        (char)(wav_size), (char)(wav_size >> 8), (char)(wav_size >> 16), (char)(wav_size >> 24), // Subchunk2Size
    };

    memcpy(wav_header, set_wav_header, sizeof(set_wav_header));
}


void record_wav(uint32_t rec_time)
{
    // Use POSIX and C standard library functions to work with files.
    int flash_wr_size = 0;
    ESP_LOGI(TAG, "Opening file");

    char wav_header_fmt[WAVE_HEADER_SIZE];

    printf("SAMPLE_RATE=%d.\r\n", SAMPLE_RATE);
    uint32_t flash_rec_time = BYTE_RATE * rec_time;
    //产生wav头数据
    generate_wav_header(wav_header_fmt, flash_rec_time, SAMPLE_RATE);

    //创建SD卡文件前，先删除SD卡上同名的文件
    struct stat st;
    if (stat(RECORD_FILE, &st) == 0) {
        // 删除存在的文件
        unlink(RECORD_FILE);
    }

    //创建WAV文件
    FILE* f = fopen(RECORD_FILE, "a");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing");
        return;
    }

    //写入产生wav头数据，
    fwrite(wav_header_fmt, 1, WAVE_HEADER_SIZE, f);
    i2s_zero_dma_buffer(I2S_NUM);

    //开始录音
    while (flash_wr_size < flash_rec_time) {
        //读I2S数据
        i2s_read(I2S_NUM, (char *)i2s_read_buff, 1153 * 10, &bytes_read, 100);
        //写入读取的数据到SD卡
        //printf("bytes_read=%d\n",bytes_read);
        fwrite(i2s_read_buff, 1, bytes_read, f);
        flash_wr_size += bytes_read;

        printf("flash_wr_size = %d\n",flash_wr_size);
    }

    ESP_LOGI(TAG, "Recording done!");
    fclose(f);
    ESP_LOGI(TAG, "File written on SDCard");
}

void playback_wav(char *wav_name)
{
    printf("wav name is %s\n",wav_name);
    //打开播放文件
    FILE* f_wav_play = fopen(wav_name, "r");
    if (f_wav_play == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing");
        return;
    }

    char wav_header_fmt_play[WAVE_HEADER_SIZE];
    //读取wav头数据
    fread(wav_header_fmt_play, 1, WAVE_HEADER_SIZE, f_wav_play);

    
    char temp_cpr[8];
    strncpy(temp_cpr,wav_header_fmt_play,4);
    if(strstr(temp_cpr , "RIFF"))
    {
        // printf("this is wav file\n");
        strncpy(temp_cpr,wav_header_fmt_play+8,8);
        if(strstr(temp_cpr , "WAVEfmt "))
        {
            //上面二个if，确定打开的文件格式是wav       
#if 0
            current_wav_msg = get_wav_msg(wav_name);

            // wav size
            printf("wav size = %02x, %02x, %02x, %02x\n",wav_header_fmt_play[40],wav_header_fmt_play[41],wav_header_fmt_play[42],wav_header_fmt_play[43]);

            current_wav_msg.while_time = wav_header_fmt_play[40] + wav_header_fmt_play[41]* (1<<8) + wav_header_fmt_play[42]* (1<<16) + wav_header_fmt_play[43] * (1<<24);
            printf("while_time = %d\n",current_wav_msg.while_time);

            // wav sample rate
            current_wav_msg.sample_rate = wav_header_fmt_play[24] + wav_header_fmt_play[25]* (1<<8) + wav_header_fmt_play[26]* (1<<16) + wav_header_fmt_play[27] * (1<<24);
            printf("sample_rate = %d\n",current_wav_msg.sample_rate);
#endif
            int while_time = wav_header_fmt_play[40] + wav_header_fmt_play[41]* (1<<8) + wav_header_fmt_play[42]* (1<<16) + wav_header_fmt_play[43] * (1<<24);
            int temp_current_time = 0;
            bytes_read = 5000;

            // 开始播放
            while (temp_current_time < while_time) {
                //从文件读出wav数据
                fseek(f_wav_play, WAVE_HEADER_SIZE+temp_current_time,SEEK_SET);
                bytes_read=fread(i2s_read_buff, 1, 5000, f_wav_play);
                //写入读取的I2S数据
                i2s_write(I2S_NUM, i2s_read_buff, bytes_read, &bytes_write, 100);

                temp_current_time += bytes_write;
                printf("temp_current_time = %d\n",temp_current_time);
            }
        }
    }
    ESP_LOGI(TAG, "play done!");
    i2s_zero_dma_buffer(I2S_NUM);
    fclose(f_wav_play);
}

int mic_task_run=0;
static void mic_task(void *arg)
{
    esp_err_t ret = 0;
    /*!<  for 36Khz sample rates, we create 100Hz sine wave, every cycle need 36000/100 = 360 samples (4-bytes or 8-bytes each sample) */
    /*!<  depend on bits_per_sample */
    /*!<  using 6 buffers, we need 60-samples per buffer */
    /*!<  if 2-channels, 16-bit each channel, total buffer is 360*4 = 1440 bytes */
    /*!<  if 2-channels, 24/32-bit each channel, total buffer is 360*8 = 2880 bytes */
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX,   //配置I2S既可以录音，也可以放音
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = 16,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,                           /*!< 1-channels */
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .dma_buf_count = 6,
        .dma_buf_len = 256,
        .use_apll = true,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL2 | ESP_INTR_FLAG_IRAM,
    };
    //硬件引脚定义
    i2s_pin_config_t pin_config = {
        .mck_io_num=I2S_MCLK,
        .bck_io_num = I2S_SCLK,
        .ws_io_num = I2S_LCLK,
        .data_out_num = I2S_DOUT,
        .data_in_num = I2S_DSIN                                          /*!< Not used */
    };

    //I2S初始化
    ret = i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL);
    ESP_LOGI(TAG, "i2s_driver_install ret=%d.\r\n", ret);
    //设置I2S对应硬件IO口
    ret = i2s_set_pin(I2S_NUM, &pin_config);
    ESP_LOGI(TAG, "i2s_set_pin ret=%d.\r\n", ret);

    while(1)
    {
      if(1==mic_task_run)
      {
        //LCD提示
        Gui_DrawFont_GBK24(0,120,RED,WHITE, 0, (u8*)"录音中......");
        lcd_update();//刷新显示
        vTaskDelay(500 / portTICK_RATE_MS);

        //录音中
        record_wav(RECORD_TIME);
        mic_task_run=2;
      }

      if(2==mic_task_run)
      {
        //LCD提示
        Gui_DrawFont_GBK24(0,120,RED,WHITE, 0, (u8*)"播放中......");
        lcd_update();//刷新显示

        //播放中
        playback_wav(RECORD_FILE);

        //LCD提示
        Gui_DrawFont_GBK24(0,120,RED,WHITE, 0, (u8*)" ");
        lcd_update();//刷新显示
        mic_task_run=0;
      }

      vTaskDelay(50 / portTICK_RATE_MS);
    }
}

static void audio_control_task(void *arg)
{
    while (1) {
        switch (GetKey()) {
            case 1: {//KEY2录音
                mic_task_run=1;//设置录音标志
            }
            break;

            case 2: {//KEY3播放
                mic_task_run=2;//设置播放标志
            }
            break;
        }
        vTaskDelay(50 / portTICK_RATE_MS);
    }
}

//录音文件入口
int key_task()
{
    es8388_init();//(1)
    es8388_config_i2s(AUDIO_HAL_I2S_NORMAL, AUDIO_HAL_BIT_LENGTH_16BITS);//(2)
    es8388_set_voice_volume(100);//(3)
    es8388_ctrl_state(AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);
    es8388_set_mic_gain(MIC_GAIN_24DB);
    xTaskCreate(mic_task, "mic_task", 4096, NULL, 6, NULL);
    xTaskCreate(audio_control_task, "audio_control_task", 1024*10, NULL, 5, NULL);
    return 0;
}



