#include "audio.h"

#include <string.h>
#include <errno.h>

#include "driver/i2c.h"
#include "driver/i2s.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "audio";

#define AUDIO_I2C_PORT       I2C_NUM_0
#define AUDIO_I2C_SDA        GPIO_NUM_12
#define AUDIO_I2C_SCL        GPIO_NUM_13
#define AUDIO_ES8388_ADDR    0x10

#define AUDIO_I2S_TX_PORT    I2S_NUM_0
#define AUDIO_I2S_RX_PORT    I2S_NUM_0
#define AUDIO_MCLK           GPIO_NUM_0
#define AUDIO_BCLK           GPIO_NUM_14
#define AUDIO_LRCK           GPIO_NUM_19
#define AUDIO_DOUT           GPIO_NUM_21
#define AUDIO_DIN            GPIO_NUM_20

#define AUDIO_SAMPLE_RATE    16000
#define AUDIO_CHANNELS       2
#define AUDIO_FRAME_SAMPLES  320
#define AUDIO_FRAME_BYTES    (AUDIO_FRAME_SAMPLES * AUDIO_CHANNELS * sizeof(int16_t))
#define AUDIO_UDP_PORT       5005

#define ES8388_CONTROL1       0x00
#define ES8388_CONTROL2       0x01
#define ES8388_CHIPPOWER      0x02
#define ES8388_ADCPOWER       0x03
#define ES8388_DACPOWER       0x04
#define ES8388_MASTERMODE     0x08
#define ES8388_ADCCONTROL1    0x09
#define ES8388_ADCCONTROL2    0x0A
#define ES8388_ADCCONTROL3    0x0B
#define ES8388_ADCCONTROL4    0x0C
#define ES8388_ADCCONTROL5    0x0D
#define ES8388_ADCCONTROL8    0x10
#define ES8388_ADCCONTROL9    0x11
#define ES8388_ADCCONTROL10   0x12
#define ES8388_ADCCONTROL11   0x13
#define ES8388_ADCCONTROL12   0x14
#define ES8388_ADCCONTROL13   0x15
#define ES8388_ADCCONTROL14   0x16
#define ES8388_DACCONTROL1    0x17
#define ES8388_DACCONTROL2    0x18
#define ES8388_DACCONTROL3    0x19
#define ES8388_DACCONTROL4    0x1A
#define ES8388_DACCONTROL5    0x1B
#define ES8388_DACCONTROL16   0x26
#define ES8388_DACCONTROL17   0x27
#define ES8388_DACCONTROL20   0x2A
#define ES8388_DACCONTROL21   0x2B
#define ES8388_DACCONTROL23   0x2D
#define ES8388_DACCONTROL24   0x2E
#define ES8388_DACCONTROL25   0x2F
#define ES8388_DACCONTROL26   0x30
#define ES8388_DACCONTROL27   0x31

static int s_audio_socket = -1;
static struct sockaddr_storage s_peer_addr;
static socklen_t s_peer_addr_len = 0;
static volatile bool s_peer_valid = false;
static portMUX_TYPE s_peer_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t s_rx_packets = 0;
static uint32_t s_rx_nonzero = 0;
static uint32_t s_tx_packets = 0;
static int32_t s_rx_peak = 0;
static int32_t s_tx_peak = 0;
static uint32_t s_rx_sum_abs = 0;
static uint32_t s_tx_sum_abs = 0;
static uint32_t s_tx_left_peak = 0;
static uint32_t s_tx_right_peak = 0;
static uint32_t s_tx_left_avg = 0;
static uint32_t s_tx_right_avg = 0;
static uint32_t s_tx_raw_frames = 0;

static esp_err_t es8388_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    return i2c_master_write_to_device(AUDIO_I2C_PORT, AUDIO_ES8388_ADDR,
                                       data, sizeof(data), pdMS_TO_TICKS(100));
}

static esp_err_t es8388_read_reg(uint8_t reg, uint8_t *value)
{
    return i2c_master_write_read_device(AUDIO_I2C_PORT, AUDIO_ES8388_ADDR,
                                        &reg, 1, value, 1, pdMS_TO_TICKS(100));
}

static void es8388_log_reg(uint8_t reg, const char *name)
{
    uint8_t value = 0;
    esp_err_t ret = es8388_read_reg(reg, &value);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ES8388 %s (0x%02X) = 0x%02X", name, reg, value);
    } else {
        ESP_LOGE(TAG, "ES8388 read %s (0x%02X) failed: %s",
                 name, reg, esp_err_to_name(ret));
    }
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
    if (ret != ESP_OK) return ret;

    ret = i2c_driver_install(AUDIO_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    return ret == ESP_ERR_INVALID_STATE ? ESP_OK : ret;
}

static esp_err_t es8388_init(void)
{
    static const struct { uint8_t reg; uint8_t value; } init_regs[] = {
        {ES8388_DACCONTROL3, 0x04},
        {ES8388_CONTROL2,    0x50},
        {ES8388_CHIPPOWER,   0x00},
        {ES8388_MASTERMODE,  0x00},

        {ES8388_DACPOWER,    0xC0},
        {ES8388_CONTROL1,    0x12},
        {ES8388_DACCONTROL1, 0x18},
        {ES8388_DACCONTROL2, 0x02},
        {ES8388_DACCONTROL16,0x00},
        {ES8388_DACCONTROL17,0x90},
        {ES8388_DACCONTROL20,0x90},
        {ES8388_DACCONTROL21,0x80},
        {ES8388_DACCONTROL23,0x00},
        {ES8388_DACCONTROL24,0x1E},
        {ES8388_DACCONTROL25,0x1E},
        {ES8388_DACCONTROL26,0x1E},
        {ES8388_DACCONTROL27,0x1E},
        {ES8388_DACCONTROL4, 0x00},
        {ES8388_DACCONTROL5, 0x00},

        /* ADC configuration: copied from the board's proven 1_15_recorder. */
        {ES8388_ADCPOWER,    0xFF},
        /* LIN1/RIN1 are the ADC inputs on this board. */
        {ES8388_ADCCONTROL2, 0x00},
        {ES8388_ADCCONTROL3, 0x02},
        {ES8388_ADCCONTROL4, 0x0D},
        {ES8388_ADCCONTROL5, 0x02},
        /* ADC digital volume = 0 dB. */
        {ES8388_ADCCONTROL8, 0x00},
        {ES8388_ADCCONTROL9, 0x00},
        /* MIC PGA: 24 dB on both L/R channels (8 << 4 | 8 = 0x88), exactly as es8388_set_mic_gain(24dB). */
        {ES8388_ADCCONTROL1, 0xBB},
        {ES8388_ADCCONTROL10,0xEA},
        {ES8388_ADCCONTROL11,0xC0},
        {ES8388_ADCCONTROL12,0x12},
        {ES8388_ADCCONTROL13,0x06},
        {ES8388_ADCCONTROL14,0xC3},

        {ES8388_DACPOWER,    0x3C},
        {ES8388_DACCONTROL3, 0x00},

        /*
         * IMPORTANT: reproduce the board's proven 1_15_recorder
         * es8388_start(ES_MODULE_ADC_DAC) state-machine kick.
         *
         * The recorder writes DACCONTROL21=0x80 and, when that value
         * changes, pulses CHIPPOWER 0xF0 -> 0x00 before powering the
         * ADC/DAC. Our previous code wrote CHIPPOWER=0x00 only once,
         * before DACCONTROL21 was changed, so the ES8388 internal
         * state machine was never explicitly restarted at this point.
         */
        /* The original recorder leaves DACCONTROL21 at 0x80; no extra state-machine pulse. */
        {ES8388_ADCPOWER,    0x09},
        {ES8388_ADCCONTROL4, 0x0C},
    };

    for (size_t i = 0; i < sizeof(init_regs) / sizeof(init_regs[0]); ++i) {
        esp_err_t ret = es8388_write_reg(init_regs[i].reg, init_regs[i].value);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ES8388 write reg 0x%02X=0x%02X failed: %s",
                     init_regs[i].reg, init_regs[i].value, esp_err_to_name(ret));
            return ret;
        }
    }

    /* Read back the codec state so we stop guessing about I2C writes. */
    es8388_log_reg(ES8388_CONTROL1, "CONTROL1");
    es8388_log_reg(ES8388_CONTROL2, "CONTROL2");
    es8388_log_reg(ES8388_CHIPPOWER, "CHIPPOWER");
    es8388_log_reg(ES8388_ADCPOWER, "ADCPOWER");
    es8388_log_reg(ES8388_ADCCONTROL1, "ADCCONTROL1");
    es8388_log_reg(ES8388_ADCCONTROL2, "ADCCONTROL2");
    es8388_log_reg(ES8388_ADCCONTROL3, "ADCCONTROL3");
    es8388_log_reg(ES8388_ADCCONTROL4, "ADCCONTROL4");
    es8388_log_reg(ES8388_ADCCONTROL4, "ADCCONTROL4");
    es8388_log_reg(ES8388_ADCCONTROL5, "ADCCONTROL5");
    es8388_log_reg(ES8388_ADCCONTROL8, "ADCCONTROL8");
    es8388_log_reg(ES8388_ADCCONTROL9, "ADCCONTROL9");
    es8388_log_reg(ES8388_ADCCONTROL10, "ADCCONTROL10");
    es8388_log_reg(ES8388_ADCCONTROL11, "ADCCONTROL11");
    es8388_log_reg(ES8388_ADCCONTROL12, "ADCCONTROL12");
    es8388_log_reg(ES8388_ADCCONTROL13, "ADCCONTROL13");
    es8388_log_reg(ES8388_ADCCONTROL14, "ADCCONTROL14");

    /* Match es8388_ctrl_state(BOTH, START): ADCPOWER must be 0x00. */
    esp_err_t adc_power_ret = es8388_write_reg(ES8388_ADCPOWER, 0x00);
    if (adc_power_ret != ESP_OK) {
        ESP_LOGE(TAG, "ES8388 ADCPOWER final write failed: %s", esp_err_to_name(adc_power_ret));
        return adc_power_ret;
    }
    es8388_log_reg(ES8388_ADCPOWER, "ADCPOWER(final)");

    /* The original board driver calls es8388_set_mic_gain(24dB) last; it writes 0x08. */
    esp_err_t gain_ret = es8388_write_reg(ES8388_ADCCONTROL1, 0x08);
    if (gain_ret != ESP_OK) return gain_ret;
    es8388_log_reg(ES8388_ADCCONTROL1, "ADCCONTROL1(final)");
    ESP_LOGI(TAG, "ES8388 initialized to board-recorder state: ADC I2S normal/16-bit, MIC_GAIN_24DB=0x08, ADCPOWER=0x00");
    return ESP_OK;
}

static esp_err_t audio_i2s_init(void)
{
    /*
     * Use the same legacy full-duplex I2S0 arrangement as the board's proven
     * 1_15_recorder example. TX and RX must use the same I2S0 handle in
     * legacy full-duplex mode; using I2S_NUM_1 here leaves the RX handle
     * uninstalled and causes i2s_read() to dereference a null handle.
     *
     * ES8388 is clocked by ESP32-S3:
     *   MCLK=GPIO0, BCLK=GPIO14, LRCK=GPIO19
     *   DOUT=GPIO21 (speaker), DIN=GPIO20 (microphone)
     *
     * The previous I2S0-master/I2S1-slave experiment produced zero DMA
     * bytes, proving that the slave receiver was not seeing a usable clock.
     * Go back to the known-good recorder topology before changing any
     * codec settings again.
     */
    i2s_config_t cfg = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX,
        .sample_rate = AUDIO_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 6,
        .dma_buf_len = 256,
        .use_apll = true,
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

    esp_err_t ret = i2s_driver_install(AUDIO_I2S_TX_PORT, &cfg, 0, NULL);
    if (ret != ESP_OK) return ret;

    ret = i2s_set_pin(AUDIO_I2S_TX_PORT, &pins);
    if (ret != ESP_OK) {
        i2s_driver_uninstall(AUDIO_I2S_TX_PORT);
        return ret;
    }

    ret = i2s_zero_dma_buffer(AUDIO_I2S_TX_PORT);
    if (ret != ESP_OK) {
        i2s_driver_uninstall(AUDIO_I2S_TX_PORT);
        return ret;
    }

    ret = i2s_start(AUDIO_I2S_TX_PORT);
    if (ret != ESP_OK) {
        i2s_driver_uninstall(AUDIO_I2S_TX_PORT);
        return ret;
    }

    ESP_LOGI(TAG,
             "I2S recorder-compatible full-duplex: I2S0 master TX+RX; "
             "MCLK=%d BCLK=%d LRCK=%d DOUT=%d DIN=%d",
             AUDIO_MCLK, AUDIO_BCLK, AUDIO_LRCK, AUDIO_DOUT, AUDIO_DIN);

    const gpio_num_t diag_pins[] = {AUDIO_MCLK, AUDIO_BCLK, AUDIO_LRCK, AUDIO_DIN};
    const char *diag_names[] = {"GPIO0 MCLK", "GPIO14 BCLK", "GPIO19 LRCK", "GPIO20 ASDOUT"};
    for (size_t p = 0; p < 4; ++p) {
        int last = gpio_get_level(diag_pins[p]);
        int ones = last ? 1 : 0;
        int zeros = last ? 0 : 1;
        int transitions = 0;
        for (int i = 0; i < 20000; ++i) {
            int level = gpio_get_level(diag_pins[p]);
            if (level) ++ones; else ++zeros;
            if (level != last) ++transitions;
            last = level;
        }
        ESP_LOGI(TAG, "%s diagnostic: 0=%d 1=%d transitions=%d",
                 diag_names[p], zeros, ones, transitions);
    }
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

    while (true) {
        socklen_t from_len = sizeof(from);
        int len = recvfrom(s_audio_socket, buffer, sizeof(buffer), 0,
                           (struct sockaddr *)&from, &from_len);
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (len == 0) continue;

        audio_set_peer(&from, from_len);

        if (len == AUDIO_FRAME_BYTES) {
            const int16_t *samples = (const int16_t *)buffer;
            int32_t peak = 0;
            uint32_t sum_abs = 0;
            for (size_t i = 0; i < AUDIO_FRAME_BYTES / sizeof(int16_t); ++i) {
                int32_t v = samples[i];
                int32_t a = v < 0 ? -v : v;
                if (a > peak) peak = a;
                sum_abs += (uint32_t)a;
            }
            s_rx_packets++;
            if (peak > 32) s_rx_nonzero++;
            s_rx_peak = peak;
            s_rx_sum_abs = sum_abs / (AUDIO_FRAME_BYTES / sizeof(int16_t));

            size_t written = 0;
            i2s_write(AUDIO_I2S_TX_PORT, buffer, AUDIO_FRAME_BYTES,
                      &written, pdMS_TO_TICKS(30));
        }
    }
}

static void audio_diag_task(void *arg)
{
    uint32_t last_rx = 0, last_tx = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        uint32_t rx = s_rx_packets, tx = s_tx_packets;
        ESP_LOGI(TAG,
                 "Audio diag: PC->ESP packets=%lu (+%lu), peak=%ld, avgAbs=%lu, nonzero=%lu; "
                 "ESP->PC packets=%lu (+%lu), peak=%ld, avgAbs=%lu, "
                 "Lpeak=%lu Rpeak=%lu, Lavg=%lu Ravg=%lu, peer=%s",
                 (unsigned long)rx, (unsigned long)(rx - last_rx),
                 (long)s_rx_peak, (unsigned long)s_rx_sum_abs,
                 (unsigned long)s_rx_nonzero,
                 (unsigned long)tx, (unsigned long)(tx - last_tx),
                 (long)s_tx_peak, (unsigned long)s_tx_sum_abs,
                 (unsigned long)s_tx_left_peak, (unsigned long)s_tx_right_peak,
                 (unsigned long)s_tx_left_avg, (unsigned long)s_tx_right_avg,
                 s_peer_valid ? "YES" : "NO");
        last_rx = rx;
        last_tx = tx;
    }
}

static void audio_tx_task(void *arg)
{
    uint8_t buffer[AUDIO_FRAME_BYTES];

    while (true) {
        size_t read_bytes = 0;
        esp_err_t ret = i2s_read(AUDIO_I2S_RX_PORT, buffer, sizeof(buffer),
                                 &read_bytes, pdMS_TO_TICKS(500));
        if (ret != ESP_OK || read_bytes != sizeof(buffer)) {
            if (s_tx_raw_frames < 8) {
                ESP_LOGW(TAG, "I2S RX read: err=%s bytes=%u",
                         esp_err_to_name(ret), (unsigned)read_bytes);
            }
            continue;
        }

        /*
         * IMPORTANT: inspect the I2S RX data before checking whether the PC
         * has sent anything. This lets us distinguish:
         *   1) microphone/I2S RX is dead, from
         *   2) microphone data is valid but there is no UDP peer yet.
         *
         * The previous code returned here when peer=NO, so a perfectly
         * working microphone produced no raw-I2S diagnostic at all.
         */
        struct sockaddr_storage peer;
        socklen_t peer_len;
        bool peer_valid = audio_get_peer(&peer, &peer_len);

        const int16_t *samples = (const int16_t *)buffer;
        int32_t peak = 0;
        uint32_t sum_abs = 0;
        uint32_t left_peak = 0;
        uint32_t right_peak = 0;
        uint64_t left_sum = 0;
        uint64_t right_sum = 0;
        const size_t sample_count = sizeof(buffer) / sizeof(int16_t);
        for (size_t i = 0; i < sample_count; ++i) {
            int32_t v = samples[i];
            int32_t a = v < 0 ? -v : v;
            if (a > peak) peak = a;
            sum_abs += (uint32_t)a;

            if ((i & 1U) == 0) {
                if ((uint32_t)a > left_peak) left_peak = (uint32_t)a;
                left_sum += (uint32_t)a;
            } else {
                if ((uint32_t)a > right_peak) right_peak = (uint32_t)a;
                right_sum += (uint32_t)a;
            }
        }
        s_tx_peak = peak;
        s_tx_sum_abs = sum_abs / sample_count;
        s_tx_left_peak = left_peak;
        s_tx_right_peak = right_peak;
        s_tx_left_avg = (uint32_t)(left_sum / (sample_count / 2));
        s_tx_right_avg = (uint32_t)(right_sum / (sample_count / 2));

        if (s_tx_raw_frames < 8) {
            const uint32_t *raw32 = (const uint32_t *)buffer;
            ESP_LOGI(TAG,
                     "I2S RX raw #%lu: Lpeak=%lu Lavg=%lu Rpeak=%lu Ravg=%lu; "
                     "s16=%d,%d,%d,%d,%d,%d,%d,%d",
                     (unsigned long)s_tx_raw_frames,
                     (unsigned long)s_tx_left_peak, (unsigned long)s_tx_left_avg,
                     (unsigned long)s_tx_right_peak, (unsigned long)s_tx_right_avg,
                     samples[0], samples[1], samples[2], samples[3],
                     samples[4], samples[5], samples[6], samples[7]);
            ESP_LOGI(TAG,
                     "I2S RX raw32: %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX",
                     (unsigned long)raw32[0], (unsigned long)raw32[1],
                     (unsigned long)raw32[2], (unsigned long)raw32[3],
                     (unsigned long)raw32[4], (unsigned long)raw32[5],
                     (unsigned long)raw32[6], (unsigned long)raw32[7]);
            ESP_LOGI(TAG,
                     "I2S RX bytes: %02X %02X %02X %02X %02X %02X %02X %02X "
                     "%02X %02X %02X %02X %02X %02X %02X %02X",
                     buffer[0], buffer[1], buffer[2], buffer[3],
                     buffer[4], buffer[5], buffer[6], buffer[7],
                     buffer[8], buffer[9], buffer[10], buffer[11],
                     buffer[12], buffer[13], buffer[14], buffer[15]);
            s_tx_raw_frames++;
        }

        if (!peer_valid) {
            continue;
        }

        int sent = sendto(s_audio_socket, buffer, sizeof(buffer), 0,
                          (struct sockaddr *)&peer, peer_len);
        if (sent >= 0) {
            s_tx_packets++;
        }
        if (sent < 0 && errno != ENETUNREACH) {
            ESP_LOGW(TAG, "UDP audio send error: errno=%d", errno);
        }
    }
}

static esp_err_t audio_udp_init(void)
{
    s_audio_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_audio_socket < 0) return ESP_FAIL;

    struct sockaddr_in local_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(AUDIO_UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(s_audio_socket, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        close(s_audio_socket);
        s_audio_socket = -1;
        return ESP_FAIL;
    }

    struct timeval tv = {.tv_sec = 0, .tv_usec = 50000};
    setsockopt(s_audio_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
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

    BaseType_t ret1 = xTaskCreatePinnedToCore(audio_rx_task, "audio_rx", 4096, NULL, 5, NULL, 0);
    BaseType_t ret2 = xTaskCreatePinnedToCore(audio_tx_task, "audio_tx", 4096, NULL, 5, NULL, 1);
    BaseType_t ret3 = xTaskCreatePinnedToCore(audio_diag_task, "audio_diag", 3072, NULL, 4, NULL, 0);

    if (ret1 != pdPASS || ret2 != pdPASS || ret3 != pdPASS) {
        ESP_LOGE(TAG, "Audio task creation failed");
        return;
    }

    ESP_LOGI(TAG, "Audio ready: UDP port %d, PCM 16kHz/16-bit/stereo/20ms",
             AUDIO_UDP_PORT);
}
