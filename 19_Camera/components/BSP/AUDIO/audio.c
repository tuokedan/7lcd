#include "audio.h"

#include <string.h>
#include <errno.h>

#include "driver/i2c.h"
#include "driver/i2s.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "audio";

/* Board schematic: ES8388 I2C and I2S */
#define AUDIO_I2C_PORT       I2C_NUM_0
#define AUDIO_I2C_SDA       GPIO_NUM_12
#define AUDIO_I2C_SCL       GPIO_NUM_13
#define AUDIO_ES8388_ADDR   0x10

#define AUDIO_I2S_PORT      I2S_NUM_0
#define AUDIO_MCLK          GPIO_NUM_0
#define AUDIO_BCLK          GPIO_NUM_14
#define AUDIO_LRCK          GPIO_NUM_19
#define AUDIO_DOUT         GPIO_NUM_21 /* ESP32 -> ES8388 DSD IN */
#define AUDIO_DIN          GPIO_NUM_20 /* ES8388 ASD OUT -> ESP32 */

#define AUDIO_SAMPLE_RATE   16000
#define AUDIO_BITS          16
#define AUDIO_CHANNELS      2
#define AUDIO_FRAME_SAMPLES 320 /* 20 ms */
#define AUDIO_FRAME_BYTES   (AUDIO_FRAME_SAMPLES * AUDIO_CHANNELS * sizeof(int16_t))

#define AUDIO_UDP_PORT      5005

#define ES8388_CONTROL1         0x00
#define ES8388_CONTROL2         0x01
#define ES8388_CHIPPOWER        0x02
#define ES8388_ADCPOWER         0x03
#define ES8388_DACPOWER         0x04
#define ES8388_MASTERMODE       0x08
#define ES8388_ADCCONTROL1      0x09
#define ES8388_ADCCONTROL2      0x0A
#define ES8388_ADCCONTROL3      0x0B
#define ES8388_ADCCONTROL4      0x0C
#define ES8388_ADCCONTROL5      0x0D
#define ES8388_ADCCONTROL8      0x10
#define ES8388_ADCCONTROL9      0x11
#define ES8388_ADCCONTROL10     0x12
#define ES8388_ADCCONTROL11     0x13
#define ES8388_ADCCONTROL12     0x14
#define ES8388_ADCCONTROL13     0x15
#define ES8388_ADCCONTROL14     0x16
#define ES8388_DACCONTROL1      0x17
#define ES8388_DACCONTROL2      0x18
#define ES8388_DACCONTROL3      0x19
#define ES8388_DACCONTROL4      0x1A
#define ES8388_DACCONTROL5      0x1B
#define ES8388_DACCONTROL16     0x26
#define ES8388_DACCONTROL17     0x27
#define ES8388_DACCONTROL20     0x2A
#define ES8388_DACCONTROL21     0x2B
#define ES8388_DACCONTROL23     0x2D
#define ES8388_DACCONTROL24     0x2E
#define ES8388_DACCONTROL25     0x2F
#define ES8388_DACCONTROL26     0x30
#define ES8388_DACCONTROL27     0x31

static int s_audio_socket = -1;
static struct sockaddr_storage s_peer_addr;
static socklen_t s_peer_addr_len = 0;
static volatile bool s_peer_valid = false;
static portMUX_TYPE s_peer_lock = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t es8388_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    esp_err_t ret = i2c_master_write_to_device(
        AUDIO_I2C_PORT,
        AUDIO_ES8388_ADDR,
        data,
        sizeof(data),
        pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES8388 reg 0x%02x write 0x%02x failed: %s",
                 reg, value, esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t audio_i2c_init(void)
{
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = AUDIO_I2C_SDA,
        .scl_io_num = AUDIO_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
        .clk_flags = 0,
    };

    esp_err_t ret = i2c_param_config(AUDIO_I2C_PORT, &cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = i2c_driver_install(AUDIO_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (ret == ESP_ERR_INVALID_STATE) {
        ret = ESP_OK;
    }
    return ret;
}

static esp_err_t es8388_init(void)
{
    /*
     * ES8388 is configured as I2S slave.
     * GPIO0 supplies MCLK from the ESP32-S3 I2S peripheral.
     *
     * The register sequence follows the commonly used ES8388
     * Play+Record configuration: 16-bit I2S, 256*Fs clock ratio,
     * stereo ADC/DAC and both line outputs enabled.
     */
    static const struct {
        uint8_t reg;
        uint8_t value;
    } init_regs[] = {
        {ES8388_DACCONTROL3, 0x04}, /* mute during setup */
        {ES8388_CONTROL2,    0x50},
        {ES8388_CHIPPOWER,   0x00},
        {ES8388_MASTERMODE,  0x00}, /* codec slave */

        {ES8388_DACPOWER,    0xC0},
        {ES8388_CONTROL1,    0x12},
        {ES8388_DACCONTROL1, 0x18}, /* I2S, 16-bit */
        {ES8388_DACCONTROL2, 0x02}, /* MCLK/LRCK = 256 */
        {ES8388_DACCONTROL16,0x00}, /* LIN1/RIN1 input path */
        {ES8388_DACCONTROL17,0x90},
        {ES8388_DACCONTROL20,0x90},
        {ES8388_DACCONTROL21,0x80},
        {ES8388_DACCONTROL23,0x00},
        {ES8388_DACCONTROL24,0x1E},
        {ES8388_DACCONTROL25,0x1E},
        {ES8388_DACCONTROL26,0x00},
        {ES8388_DACCONTROL27,0x00},

        {ES8388_ADCPOWER,    0xFF},
        {ES8388_ADCCONTROL1, 0x77}, /* +21 dB PGA */
        {ES8388_ADCCONTROL2, 0x00}, /* LIN1/RIN1 */
        {ES8388_ADCCONTROL3, 0x02},
        {ES8388_ADCCONTROL4, 0x0C}, /* I2S, 16-bit */
        {ES8388_ADCCONTROL5, 0x02}, /* MCLK/LRCK = 256 */
        {ES8388_ADCCONTROL8, 0x00},
        {ES8388_ADCCONTROL9, 0x00},

        /* Voice-oriented ALC */
        {ES8388_ADCCONTROL10,0xEA},
        {ES8388_ADCCONTROL11,0xC0},
        {ES8388_ADCCONTROL12,0x12},
        {ES8388_ADCCONTROL13,0x06},
        {ES8388_ADCCONTROL14,0xC3},

        {ES8388_DACPOWER,    0x3C},
        {ES8388_DACCONTROL3, 0x00},
    };

    for (size_t i = 0; i < sizeof(init_regs) / sizeof(init_regs[0]); ++i) {
        esp_err_t ret = es8388_write_reg(init_regs[i].reg, init_regs[i].value);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    ESP_LOGI(TAG, "ES8388 initialized: 16 kHz / 16-bit / stereo / I2S slave");
    return ESP_OK;
}

static esp_err_t audio_i2s_init(void)
{
    i2s_config_t cfg = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX,
        .sample_rate = AUDIO_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 6,
        .dma_buf_len = AUDIO_FRAME_SAMPLES,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
    };

    i2s_pin_config_t pins = {
        .mck_io_num = AUDIO_MCLK,
        .bck_io_num = AUDIO_BCLK,
        .ws_io_num = AUDIO_LRCK,
        .data_out_num = AUDIO_DOUT,
        .data_in_num = AUDIO_DIN,
    };

    esp_err_t ret = i2s_driver_install(AUDIO_I2S_PORT, &cfg, 0, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_set_pin(AUDIO_I2S_PORT, &pins);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S pin setup failed: %s", esp_err_to_name(ret));
        i2s_driver_uninstall(AUDIO_I2S_PORT);
        return ret;
    }

    ret = i2s_zero_dma_buffer(AUDIO_I2S_PORT);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "I2S pins: MCLK=%d BCLK=%d LRCK=%d DOUT=%d DIN=%d",
             AUDIO_MCLK, AUDIO_BCLK, AUDIO_LRCK, AUDIO_DOUT, AUDIO_DIN);
    return ESP_OK;
}

static void audio_set_peer(const struct sockaddr_storage *addr, socklen_t len)
{
    portENTER_CRITICAL(&s_peer_lock);
    memcpy(&s_peer_addr, addr, sizeof(s_peer_addr));
    s_peer_addr_len = len;
    s_peer_valid = true;
    portEXIT_CRITICAL(&s_peer_lock);
}

static bool audio_get_peer(struct sockaddr_storage *addr, socklen_t *len)
{
    bool valid;
    portENTER_CRITICAL(&s_peer_lock);
    valid = s_peer_valid;
    if (valid) {
        memcpy(addr, &s_peer_addr, sizeof(*addr));
        *len = s_peer_addr_len;
    }
    portEXIT_CRITICAL(&s_peer_lock);
    return valid;
}

static void audio_rx_task(void *arg)
{
    uint8_t buffer[AUDIO_FRAME_BYTES];
    struct sockaddr_storage from;
    socklen_t from_len;

    while (true) {
        from_len = sizeof(from);
        int len = recvfrom(
            s_audio_socket,
            buffer,
            sizeof(buffer),
            0,
            (struct sockaddr *)&from,
            &from_len);

        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            ESP_LOGW(TAG, "UDP receive error: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (len == 0) {
            continue;
        }

        audio_set_peer(&from, from_len);

        /* Only complete PCM frames are played. */
        if (len == AUDIO_FRAME_BYTES) {
            size_t written = 0;
            i2s_write(
                AUDIO_I2S_PORT,
                buffer,
                AUDIO_FRAME_BYTES,
                &written,
                pdMS_TO_TICKS(30));
        }
    }
}

static void audio_tx_task(void *arg)
{
    uint8_t buffer[AUDIO_FRAME_BYTES];

    while (true) {
        size_t read_bytes = 0;
        esp_err_t ret = i2s_read(
            AUDIO_I2S_PORT,
            buffer,
            sizeof(buffer),
            &read_bytes,
            portMAX_DELAY);

        if (ret != ESP_OK || read_bytes != sizeof(buffer)) {
            continue;
        }

        struct sockaddr_storage peer;
        socklen_t peer_len;
        if (!audio_get_peer(&peer, &peer_len)) {
            continue;
        }

        int sent = sendto(
            s_audio_socket,
            buffer,
            sizeof(buffer),
            0,
            (struct sockaddr *)&peer,
            peer_len);

        if (sent < 0 && errno != ENETUNREACH) {
            ESP_LOGW(TAG, "UDP audio send error: errno=%d", errno);
        }
    }
}

static esp_err_t audio_udp_init(void)
{
    s_audio_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_audio_socket < 0) {
        ESP_LOGE(TAG, "UDP socket create failed: errno=%d", errno);
        return ESP_FAIL;
    }

    struct sockaddr_in local_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(AUDIO_UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(
            s_audio_socket,
            (struct sockaddr *)&local_addr,
            sizeof(local_addr)) < 0) {
        ESP_LOGE(TAG, "UDP bind failed: errno=%d", errno);
        close(s_audio_socket);
        s_audio_socket = -1;
        return ESP_FAIL;
    }

    struct timeval tv = {
        .tv_sec = 0,
        .tv_usec = 50000,
    };
    setsockopt(
        s_audio_socket,
        SOL_SOCKET,
        SO_RCVTIMEO,
        &tv,
        sizeof(tv));

    return ESP_OK;
}

void audio_init(void)
{
    ESP_LOGI(TAG, "Audio init: ES8388 + I2S + UDP voice");

    if (audio_i2c_init() != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed");
        return;
    }

    if (es8388_init() != ESP_OK) {
        ESP_LOGE(TAG, "ES8388 init failed. Check I2C pins/wiring.");
        return;
    }

    if (audio_i2s_init() != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed");
        return;
    }

    if (audio_udp_init() != ESP_OK) {
        ESP_LOGE(TAG, "UDP audio init failed");
        return;
    }

    BaseType_t ret1 = xTaskCreatePinnedToCore(
        audio_rx_task, "audio_rx", 4096, NULL, 5, NULL, 0);
    BaseType_t ret2 = xTaskCreatePinnedToCore(
        audio_tx_task, "audio_tx", 4096, NULL, 5, NULL, 1);

    if (ret1 != pdPASS || ret2 != pdPASS) {
        ESP_LOGE(TAG, "Audio task creation failed");
        return;
    }

    ESP_LOGI(TAG, "Audio ready: UDP port %d, PCM 16kHz/16-bit/stereo/20ms",
             AUDIO_UDP_PORT);
}
