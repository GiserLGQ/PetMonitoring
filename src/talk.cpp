#include "talk.h"
#include "hardware_config.h"
#include <WiFi.h>
#include <driver/i2s.h>

// ---------- I2S 麦克风 ----------
static bool s_i2sReady = false;

// INMP441 输出 24bit 数据按 32bit 帧读取, 右移 14 位得到带增益的 16bit 采样
static inline int16_t convertSample(int32_t raw)
{
    int32_t v = raw >> 14;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

// WAV 文件头(流式: 长度字段填最大值, ffmpeg/sox 等工具可正常读取)
struct WavHeader {
    char riff[4]; uint32_t chunkSize;
    char wave[4];
    char fmt[4];  uint32_t fmtSize; uint16_t audioFormat; uint16_t channels;
    uint32_t sampleRate; uint32_t byteRate; uint16_t blockAlign; uint16_t bits;
    char data[4]; uint32_t dataSize;
};

static void fillWavHeader(WavHeader &h)
{
    memcpy(h.riff, "RIFF", 4);
    h.chunkSize = 0xFFFFFFFF;
    memcpy(h.wave, "WAVE", 4);
    memcpy(h.fmt, "fmt ", 4);
    h.fmtSize = 16;
    h.audioFormat = 1;              // PCM
    h.channels = 1;                 // 单声道
    h.sampleRate = TALK_SAMPLE_RATE;
    h.bits = 16;
    h.blockAlign = h.channels * h.bits / 8;
    h.byteRate = h.sampleRate * h.blockAlign;
    memcpy(h.data, "data", 4);
    h.dataSize = 0xFFFFFFFF;
}

// ---------- 采集+上传任务 ----------
static volatile bool s_running = false;
static TaskHandle_t s_task = nullptr;

// 流式 POST: HTTP/1.1 + chunked 编码, stop 时发送终止块
static void talkTask(void *)
{
    // 采集缓冲: 一次读 1024 个 32bit 采样(4KB), 转成 2KB 的 16bit 数据
    static int32_t raw[1024];
    static int16_t pcm[1024];

    WiFiClient client;
    bool connected = false;
    if (TALK_SERVER_HOST[0] != 0) {
        client.setTimeout(5000);
        connected = client.connect(TALK_SERVER_HOST, TALK_SERVER_PORT);
        if (connected) {
            char req[256];
            snprintf(req, sizeof(req),
                     "POST %s HTTP/1.1\r\nHost: %s:%d\r\n"
                     "Content-Type: audio/wav\r\nTransfer-Encoding: chunked\r\n"
                     "Connection: close\r\n\r\n",
                     TALK_SERVER_PATH, TALK_SERVER_HOST, TALK_SERVER_PORT);
            client.print(req);
            Serial.printf("[Talk] 开始录音并上传到 %s:%d%s\n", TALK_SERVER_HOST, TALK_SERVER_PORT, TALK_SERVER_PATH);
        } else {
            Serial.println("[Talk] 对话服务器连接失败, 仅本地录音");
        }
    } else {
        Serial.println("[Talk] 开始录音 (未配置 TALK_SERVER_HOST, 仅测试)");
    }

    WavHeader header;
    fillWavHeader(header);
    auto writeChunk = [&](const void *data, size_t len) {
        if (!connected || len == 0) return;
        char head[16];
        snprintf(head, sizeof(head), "%X\r\n", (unsigned)len);
        client.print(head);
        client.write((const uint8_t *)data, len);
        client.print("\r\n");
    };
    writeChunk(&header, sizeof(header));  // 先发 WAV 头

    uint32_t frames = 0;
    while (s_running) {
        size_t br = 0;
        i2s_read(TALK_I2S_PORT, raw, sizeof(raw), &br, pdMS_TO_TICKS(200));
        size_t n = br / sizeof(int32_t);
        for (size_t i = 0; i < n; i++) pcm[i] = convertSample(raw[i]);
        if (connected) writeChunk(pcm, n * sizeof(int16_t));
        frames += n;
    }

    if (connected) {
        client.print("0\r\n\r\n");        // chunked 终止块
        client.stop();
    }
    Serial.printf("[Talk] 录音结束: %.1f 秒\n", frames / (float)TALK_SAMPLE_RATE);
    s_task = nullptr;
    vTaskDelete(NULL);
}

namespace talk {

void begin()
{
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    cfg.sample_rate = TALK_SAMPLE_RATE;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;   // INMP441 L/R 接 GND = 左声道
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_desc_num = 8;    // 旧名 dma_buf_count
    cfg.dma_frame_num = 256; // 旧名 dma_buf_len
    cfg.use_apll = false;

    i2s_pin_config_t pins = {};
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    pins.bck_io_num = TALK_I2S_BCLK;
    pins.ws_io_num = TALK_I2S_WS;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num = TALK_I2S_DATA;

    if (i2s_driver_install(TALK_I2S_PORT, &cfg, 0, NULL) == ESP_OK &&
        i2s_set_pin(TALK_I2S_PORT, &pins) == ESP_OK) {
        i2s_zero_dma_buffer(TALK_I2S_PORT);
        s_i2sReady = true;
        Serial.printf("[Talk] 麦克风就绪: BCLK=GPIO%d WS=GPIO%d SD=GPIO%d (%dkHz)\n",
                      TALK_I2S_BCLK, TALK_I2S_WS, TALK_I2S_DATA, TALK_SAMPLE_RATE / 1000);
    } else {
        Serial.println("[Talk] I2S 初始化失败");
    }
}

bool start()
{
    if (s_running || !s_i2sReady) return false;
    s_running = true;
    xTaskCreatePinnedToCore(talkTask, "talk", 6144, NULL, 3, &s_task, 1);
    return true;
}

void stop()
{
    s_running = false;   // 任务自行收尾退出
}

bool active() { return s_running; }

} // namespace talk
