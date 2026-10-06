#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camera.h"
#include "lcd.h"
#include "yolo_person.h"

void app_main(void)
{
    lcd_init();
    camera_init();
    yolo_person_init();

    while (1) {
        camera_show(0, 0);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
