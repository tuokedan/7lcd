#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camera.h"
#include "lcd.h"

void app_main(void)
{ 
    lcd_init();
    camera_init();

    while (1)
    {
        camera_show(0,0);              /* 显示图像 */
        vTaskDelay(5);
    }
}
