#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "video_stream.h"
#include "audio.h"
#include "esp_log.h"

/*
 * Audio-only diagnostic:
 * Wi-Fi AP + UDP + ES8388 + I2S RX/TX are active.
 * Camera/LCD/YOLO/HTTP video are completely disabled to remove
 * camera DMA/VSYNC contention from the microphone test.
 */
#define AUDIO_DIAG_MODE 1

void app_main(void)
{
#if AUDIO_DIAG_MODE
    ESP_LOGI("main", "AUDIO ONLY DIAG: camera/LCD/YOLO/HTTP video disabled");
    video_stream_init_wifi_only();
#else
    /* Full application is restored here when audio diagnostics are finished. */
    ESP_LOGI("main", "Full application mode is not enabled in this diagnostic build");
#endif

    audio_init();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
