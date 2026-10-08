//=============
//头文件包含
//=============
#include "driver/gpio.h"
#include <driver/adc.h>
#include "freertos/FreeRTOS.h"
#include <hal/adc_types.h>
#include "app_key.h"
///////////////////////////////////////0.424//////////////////////1.017////////////////////////1.65/////////////////////2.48////////////
key_config_t key_configs[4]={{BUTTON_MENU, 400, 600}, {BUTTON_PLAY, 1100, 1300}, {BUTTON_UP, 1900, 2150}, {BUTTON_DOWN, 2950, 3200}};
button_name_t pressed=BUTTON_IDLE;

//ADC按键的通道
#define ADC1_EXAMPLE_CHAN0 ADC1_CHANNEL_0

int64_t backup_time = 0;
int64_t last_time = 0;



//返回按键1~4
int GetKey(void)
{
  {
    uint32_t adc = adc1_get_raw(ADC1_EXAMPLE_CHAN0);
    int i=0;

 //   printf("adc=%d.\r\n", adc);
    if (adc<key_configs[0].min || adc>key_configs[3].max) return 0;

    backup_time = esp_timer_get_time();
    for(i=0; i<4; i++)
    {
      if (adc>key_configs[i].min && adc<key_configs[i].max)
      {
        if (((backup_time - last_time) > 500000))//连续两次按键时间不能大于0.5秒
        {
          printf("Button[%d] is clicked", key_configs[i].key);
          last_time = backup_time;
          return key_configs[i].key;
        }
      }
    }
  }

  return 0;
}


//ADC初始化
//针对S3芯片
//0DB:表示支持的电压为0~750mv
//2.5DB:表示支持的电压为0~1050mv
//6DB:表示支持的电压为0~1300mv
//11DB:表示支持的电压为0~2500mv

//按键初始化
void initKey()
{
    //ADC初始化
    adc1_config_width((adc_bits_width_t)ADC_WIDTH_BIT_12);//12位ADC，最大4096
    adc1_config_channel_atten(ADC1_EXAMPLE_CHAN0, ADC_ATTEN_DB_11);
}

