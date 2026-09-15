#include "recorder.h"
#include "hardware_config.h"
#include <WiFi.h>
#include "esp_camera.h"

static volatile bool s_running = false;

// 录制任务: 抓帧 -> 写 socket, 松开/停止时关闭连接收尾
static void recordTask(void *)
{
    WiFiClient client;
    client.setTimeout(5000);
    bool connected = client.connect(REC_SERVER_HOST, REC_SERVER_PORT);
    if (!connected) {
        Serial.printf("[Rec] 连接录制服务器失败 %s:%d\n", REC_SERVER_HOST, REC_SERVER_PORT);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }
    Serial.printf("[Rec] 开始录制 -> %s:%d%s (MJPEG)\n", REC_SERVER_HOST, REC_SERVER_PORT, REC_SERVER_PATH);

    // HTTP/1.0 无 Content-Length: 关闭连接即结束, 接收端读到 EOF 为止
    char req[192];
    snprintf(req, sizeof(req),
             "POST %s HTTP/1.0\r\nHost: %s:%d\r\n"
             "Content-Type: application/octet-stream\r\nConnection: close\r\n\r\n",
             REC_SERVER_PATH, REC_SERVER_HOST, REC_SERVER_PORT);
    client.print(req);

    uint32_t frames = 0, bytes = 0;
    while (s_running) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {          // 帧未就绪, 稍后重试
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (client.connected()) {
            client.write((const uint8_t *)fb->buf, fb->len);
            frames++;
            bytes += fb->len;
        } else {
            Serial.println("[Rec] 服务器断开, 停止录制");
            s_running = false;
        }
        esp_camera_fb_return(fb);
    }

    client.stop();
    Serial.printf("[Rec] 录制结束: %u 帧, %.1f MB\n", frames, bytes / 1048576.0f);
    vTaskDelete(NULL);
}

namespace recorder {

void begin() {}

bool start()
{
    if (s_running || !configured()) return false;
    s_running = true;
    xTaskCreatePinnedToCore(recordTask, "rec", 4096, NULL, 3, NULL, 1);
    return true;
}

void stop() { s_running = false; }

bool active() { return s_running; }

bool configured() { return REC_SERVER_HOST[0] != 0; }

} // namespace recorder
