#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camera.h"
#include "lcd.h"
#include "yolo_person.h"
#include "video_stream.h"
#include "audio.h"

/*
 * Temporary stability/diagnostic switch.
 *
 * 1 = isolate MIC -> ES8388 ADC -> I2S RX -> UDP -> PC.
 *     Camera/LCD remain active, but YOLO, JPEG encoding and HTTP video
 *     are disabled so camera VSYNC/WDT load does not interfere with audio.
 *
 * 0 = restore the full bidirectional video + YOLO application.
 */
#define AUDIO_DIAG_MODE 1

void app_main()
{
    lcd_init();
    camera_init();

#if AUDIO_DIAG_MODE
    ESP_LOGI("main", "AUDIO DIAG MODE: YOLO + HTTP video disabled");
    video_stream_init_wifi_only();
#else
    yolo_person_init();
    video_stream_init();
#endif

    audio_init();

    while (1) {
        camera_show(0, 0);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
