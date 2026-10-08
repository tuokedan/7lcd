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
#pragma once

#ifdef __cplusplus
extern "C" {
#endif


//ES8388 引脚下定义

#define I2S_LCLK          (GPIO_NUM_19)//LPCK
#define I2S_SCLK          (GPIO_NUM_14)//SCLK

#define I2S_DOUT          (GPIO_NUM_21)//DSDIN
#define I2S_DSIN          (GPIO_NUM_20)//ASDOUT

#define I2C_SDA           (GPIO_NUM_12)
#define I2C_SCL           (GPIO_NUM_13)

#define I2S_MCLK          (GPIO_NUM_0)//0



//SD卡
#define SDCARD_PIN_CMD GPIO_NUM_38
#define SDCARD_PIN_CLK GPIO_NUM_39
#define SDCARD_PIN_D0  GPIO_NUM_40



#ifdef __cplusplus
}
#endif
