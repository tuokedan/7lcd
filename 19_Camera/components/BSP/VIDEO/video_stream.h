#ifndef VIDEO_STREAM_H
#define VIDEO_STREAM_H

#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the ESP32 Wi-Fi AP and the HTTP MJPEG video server. */
void video_stream_init(void);

/*
 * Publish the current camera frame for the PC receiver.
 * The function is non-blocking with respect to the HTTP client.
 * Frames are throttled internally to keep the camera/LCD/AI responsive.
 */
void video_stream_publish_frame(const camera_fb_t *fb);

/* Return true while recent PC reverse-video frames are arriving. */
bool video_stream_pc_video_active(void);

#ifdef __cplusplus
}
#endif

#endif
