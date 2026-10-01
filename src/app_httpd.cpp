// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
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
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "fb_gfx.h"
#include "driver/ledc.h"
#include "sdkconfig.h"
#include <Arduino.h>
#include <LittleFS.h>
#include "camera_sync.h"
#include "gimbal.h"
#include "talk.h"
#include "recorder.h"

#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_ARDUHAL_ESP_LOG)
#include "esp32-hal-log.h"
#endif

// Face Detection will not work on boards without (or with disabled) PSRAM
// 注意: esp-dl 1.x 头文件在 arduino-esp32 3.x(esp32 目标)中已移除,
// 本项目不使用人脸检测, 这里用 __has_include 自动降级, 避免编译报错
#if defined(BOARD_HAS_PSRAM) && __has_include("human_face_detect_msr01.hpp")
#define CONFIG_ESP_FACE_DETECT_ENABLED 1
// Face Recognition takes upward from 15 seconds per frame on chips other than ESP32S3
// Makes no sense to have it enabled for them
#if CONFIG_IDF_TARGET_ESP32S3
#define CONFIG_ESP_FACE_RECOGNITION_ENABLED 1
#else
#define CONFIG_ESP_FACE_RECOGNITION_ENABLED 0
#endif
#else
#define CONFIG_ESP_FACE_DETECT_ENABLED 0
#define CONFIG_ESP_FACE_RECOGNITION_ENABLED 0
#endif

#if CONFIG_ESP_FACE_DETECT_ENABLED

#include <vector>
#include "human_face_detect_msr01.hpp"
#include "human_face_detect_mnp01.hpp"

#define TWO_STAGE 1 /*<! 1: detect by two-stage which is more accurate but slower(with keypoints). */
                    /*<! 0: detect by one-stage which is less accurate but faster(without keypoints). */

#if CONFIG_ESP_FACE_RECOGNITION_ENABLED
#include "face_recognition_tool.hpp"
#include "face_recognition_112_v1_s16.hpp"
#include "face_recognition_112_v1_s8.hpp"

#define QUANT_TYPE 0 //if set to 1 => very large firmware, very slow, reboots when streaming...

#define FACE_ID_SAVE_NUMBER 7
#endif

#define FACE_COLOR_WHITE 0x00FFFFFF
#define FACE_COLOR_BLACK 0x00000000
#define FACE_COLOR_RED 0x000000FF
#define FACE_COLOR_GREEN 0x0000FF00
#define FACE_COLOR_BLUE 0x00FF0000
#define FACE_COLOR_YELLOW (FACE_COLOR_RED | FACE_COLOR_GREEN)
#define FACE_COLOR_CYAN (FACE_COLOR_BLUE | FACE_COLOR_GREEN)
#define FACE_COLOR_PURPLE (FACE_COLOR_BLUE | FACE_COLOR_RED)
#endif

// Enable LED FLASH setting
#define CONFIG_LED_ILLUMINATOR_ENABLED 1

// LED FLASH setup
#if CONFIG_LED_ILLUMINATOR_ENABLED

#define CONFIG_LED_MAX_INTENSITY 255
#define LED_PWM_FREQ_HZ 5000

// Keep the flash on a fixed LEDC resource. The camera uses high-speed
// timer/channel 0; the two servos reserve timers 2 and 3. Using the Arduino
// auto-allocation API here could silently pick timer 0 because the camera
// driver is not registered with Arduino's LEDC bookkeeping.
#define LED_PWM_MODE     LEDC_HIGH_SPEED_MODE
#define LED_PWM_TIMER    LEDC_TIMER_1
#define LED_PWM_CHANNEL  LEDC_CHANNEL_2

int led_duty = 0;
volatile bool isStreaming = false;
static int s_led_pin = -1;
static bool s_led_pwm_ready = false;
static int s_led_applied_duty = -1;

#endif

typedef struct
{
    httpd_req_t *req;
    size_t len;
} jpg_chunking_t;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %d.%06d\r\n\r\n";

httpd_handle_t stream_httpd = NULL;
httpd_handle_t camera_httpd = NULL;

// Each new stream invalidates the previous handler. This is only a connection
// lifecycle guard; frames remain owned by the camera driver until they have
// been sent, exactly as in the official CameraWebServer example.
static volatile uint32_t stream_generation = 0;

#if CONFIG_ESP_FACE_DETECT_ENABLED

static int8_t detection_enabled = 0;

// #if TWO_STAGE
// static HumanFaceDetectMSR01 s1(0.1F, 0.5F, 10, 0.2F);
// static HumanFaceDetectMNP01 s2(0.5F, 0.3F, 5);
// #else
// static HumanFaceDetectMSR01 s1(0.3F, 0.5F, 10, 0.2F);
// #endif

#if CONFIG_ESP_FACE_RECOGNITION_ENABLED
static int8_t recognition_enabled = 0;
static int8_t is_enrolling = 0;

#if QUANT_TYPE
    // S16 model
    FaceRecognition112V1S16 recognizer;
#else
    // S8 model
    FaceRecognition112V1S8 recognizer;
#endif
#endif

#endif

typedef struct
{
    size_t size;  //number of values used for filtering
    size_t index; //current value index
    size_t count; //value count
    int sum;
    int *values; //array to be filled with values
} ra_filter_t;

static ra_filter_t ra_filter;

static ra_filter_t *ra_filter_init(ra_filter_t *filter, size_t sample_size)
{
    memset(filter, 0, sizeof(ra_filter_t));

    filter->values = (int *)malloc(sample_size * sizeof(int));
    if (!filter->values)
    {
        return NULL;
    }
    memset(filter->values, 0, sample_size * sizeof(int));

    filter->size = sample_size;
    return filter;
}

#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
static int ra_filter_run(ra_filter_t *filter, int value)
{
    if (!filter->values)
    {
        return value;
    }
    filter->sum -= filter->values[filter->index];
    filter->values[filter->index] = value;
    filter->sum += filter->values[filter->index];
    filter->index++;
    filter->index = filter->index % filter->size;
    if (filter->count < filter->size)
    {
        filter->count++;
    }
    return filter->sum / filter->count;
}
#endif

#if CONFIG_ESP_FACE_DETECT_ENABLED
#if CONFIG_ESP_FACE_RECOGNITION_ENABLED
static void rgb_print(fb_data_t *fb, uint32_t color, const char *str)
{
    fb_gfx_print(fb, (fb->width - (strlen(str) * 14)) / 2, 10, color, str);
}

static int rgb_printf(fb_data_t *fb, uint32_t color, const char *format, ...)
{
    char loc_buf[64];
    char *temp = loc_buf;
    int len;
    va_list arg;
    va_list copy;
    va_start(arg, format);
    va_copy(copy, arg);
    len = vsnprintf(loc_buf, sizeof(loc_buf), format, arg);
    va_end(copy);
    if (len >= sizeof(loc_buf))
    {
        temp = (char *)malloc(len + 1);
        if (temp == NULL)
        {
            return 0;
        }
    }
    vsnprintf(temp, len + 1, format, arg);
    va_end(arg);
    rgb_print(fb, color, temp);
    if (len > 64)
    {
        free(temp);
    }
    return len;
}
#endif
static void draw_face_boxes(fb_data_t *fb, std::list<dl::detect::result_t> *results, int face_id)
{
    int x, y, w, h;
    uint32_t color = FACE_COLOR_YELLOW;
    if (face_id < 0)
    {
        color = FACE_COLOR_RED;
    }
    else if (face_id > 0)
    {
        color = FACE_COLOR_GREEN;
    }
    if(fb->bytes_per_pixel == 2){
        //color = ((color >> 8) & 0xF800) | ((color >> 3) & 0x07E0) | (color & 0x001F);
        color = ((color >> 16) & 0x001F) | ((color >> 3) & 0x07E0) | ((color << 8) & 0xF800);
    }
    int i = 0;
    for (std::list<dl::detect::result_t>::iterator prediction = results->begin(); prediction != results->end(); prediction++, i++)
    {
        // rectangle box
        x = (int)prediction->box[0];
        y = (int)prediction->box[1];
        w = (int)prediction->box[2] - x + 1;
        h = (int)prediction->box[3] - y + 1;
        if((x + w) > fb->width){
            w = fb->width - x;
        }
        if((y + h) > fb->height){
            h = fb->height - y;
        }
        fb_gfx_drawFastHLine(fb, x, y, w, color);
        fb_gfx_drawFastHLine(fb, x, y + h - 1, w, color);
        fb_gfx_drawFastVLine(fb, x, y, h, color);
        fb_gfx_drawFastVLine(fb, x + w - 1, y, h, color);
#if TWO_STAGE
        // landmarks (left eye, mouth left, nose, right eye, mouth right)
        int x0, y0, j;
        for (j = 0; j < 10; j+=2) {
            x0 = (int)prediction->keypoint[j];
            y0 = (int)prediction->keypoint[j+1];
            fb_gfx_fillRect(fb, x0, y0, 3, 3, color);
        }
#endif
    }
}

#if CONFIG_ESP_FACE_RECOGNITION_ENABLED
static int run_face_recognition(fb_data_t *fb, std::list<dl::detect::result_t> *results)
{
    std::vector<int> landmarks = results->front().keypoint;
    int id = -1;

    Tensor<uint8_t> tensor;
    tensor.set_element((uint8_t *)fb->data).set_shape({fb->height, fb->width, 3}).set_auto_free(false);

    int enrolled_count = recognizer.get_enrolled_id_num();

    if (enrolled_count < FACE_ID_SAVE_NUMBER && is_enrolling){
        id = recognizer.enroll_id(tensor, landmarks, "", true);
        log_i("Enrolled ID: %d", id);
        rgb_printf(fb, FACE_COLOR_CYAN, "ID[%u]", id);
    }

    face_info_t recognize = recognizer.recognize(tensor, landmarks);
    if(recognize.id >= 0){
        rgb_printf(fb, FACE_COLOR_GREEN, "ID[%u]: %.2f", recognize.id, recognize.similarity);
    } else {
        rgb_print(fb, FACE_COLOR_RED, "Intruder Alert!");
    }
    return recognize.id;
}
#endif
#endif

#if CONFIG_LED_ILLUMINATOR_ENABLED
void enable_led(bool en)
{ // Turn LED On or Off
    const int duty = en ? constrain(led_duty, 0, CONFIG_LED_MAX_INTENSITY) : 0;

    if (s_led_pwm_ready) {
        // ESP32 LEDC uses one extra duty value for 100% output at 8-bit
        // resolution. This is the same full-on handling used by Arduino's
        // ledcWrite() wrapper.
        const uint32_t pwm_duty = (duty >= CONFIG_LED_MAX_INTENSITY) ? 256U : (uint32_t)duty;
        if (ledc_set_duty(LED_PWM_MODE, LED_PWM_CHANNEL, pwm_duty) == ESP_OK) {
            ledc_update_duty(LED_PWM_MODE, LED_PWM_CHANNEL);
        }
    } else {
        // A failed PWM setup must never prevent the camera from working.
        digitalWrite(s_led_pin, duty > 0 ? HIGH : LOW);
    }

    if (duty != s_led_applied_duty) {
        s_led_applied_duty = duty;
        log_i("Set LED intensity to %d", duty);
    }
}
#endif

static size_t jpg_encode_stream(void *arg, size_t index, const void *data, size_t len)
{
    jpg_chunking_t *j = (jpg_chunking_t *)arg;
    if (!index)
    {
        j->len = 0;
    }
    if (httpd_resp_send_chunk(j->req, (const char *)data, len) != ESP_OK)
    {
        return 0;
    }
    j->len += len;
    return len;
}

static esp_err_t capture_handler(httpd_req_t *req)
{
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
    int64_t fr_start = esp_timer_get_time();
#endif

    camera_sync::LockGuard camera_lock(5000);
    if (!camera_lock.locked()) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

#if CONFIG_LED_ILLUMINATOR_ENABLED
    enable_led(true);
    vTaskDelay(150 / portTICK_PERIOD_MS); // The LED needs to be turned on ~150ms before the call to esp_camera_fb_get()
    fb = esp_camera_fb_get();             // or it won't be visible in the frame. A better way to do this is needed.
    enable_led(false);
#else
    fb = esp_camera_fb_get();
#endif

    if (!fb)
    {
        log_e("Camera capture failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char ts[32];
    snprintf(ts, 32, "%ld.%06ld", fb->timestamp.tv_sec, fb->timestamp.tv_usec);
    httpd_resp_set_hdr(req, "X-Timestamp", (const char *)ts);

#if CONFIG_ESP_FACE_DETECT_ENABLED
    size_t out_len, out_width, out_height;
    uint8_t *out_buf;
    bool s;
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
    bool detected = false;
#endif
    int face_id = 0;
    if (!detection_enabled || fb->width > 400)
    {
#endif
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
        size_t fb_len = 0;
#endif
        if (fb->format == PIXFORMAT_JPEG)
        {
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
            fb_len = fb->len;
#endif
            res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
        }
        else
        {
            jpg_chunking_t jchunk = {req, 0};
            res = frame2jpg_cb(fb, 80, jpg_encode_stream, &jchunk) ? ESP_OK : ESP_FAIL;
            httpd_resp_send_chunk(req, NULL, 0);
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
            fb_len = jchunk.len;
#endif
        }
        esp_camera_fb_return(fb);
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
        int64_t fr_end = esp_timer_get_time();
#endif
        log_i("JPG: %uB %ums", (uint32_t)(fb_len), (uint32_t)((fr_end - fr_start) / 1000));
        return res;
#if CONFIG_ESP_FACE_DETECT_ENABLED
    }

    jpg_chunking_t jchunk = {req, 0};

    if (fb->format == PIXFORMAT_RGB565
#if CONFIG_ESP_FACE_RECOGNITION_ENABLED
     && !recognition_enabled
#endif
     ){
#if TWO_STAGE
        HumanFaceDetectMSR01 s1(0.1F, 0.5F, 10, 0.2F);
        HumanFaceDetectMNP01 s2(0.5F, 0.3F, 5);
        std::list<dl::detect::result_t> &candidates = s1.infer((uint16_t *)fb->buf, {(int)fb->height, (int)fb->width, 3});
        std::list<dl::detect::result_t> &results = s2.infer((uint16_t *)fb->buf, {(int)fb->height, (int)fb->width, 3}, candidates);
#else
        HumanFaceDetectMSR01 s1(0.3F, 0.5F, 10, 0.2F);
        std::list<dl::detect::result_t> &results = s1.infer((uint16_t *)fb->buf, {(int)fb->height, (int)fb->width, 3});
#endif
        if (results.size() > 0) {
            fb_data_t rfb;
            rfb.width = fb->width;
            rfb.height = fb->height;
            rfb.data = fb->buf;
            rfb.bytes_per_pixel = 2;
            rfb.format = FB_RGB565;
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
            detected = true;
#endif
            draw_face_boxes(&rfb, &results, face_id);
        }
        s = fmt2jpg_cb(fb->buf, fb->len, fb->width, fb->height, PIXFORMAT_RGB565, 90, jpg_encode_stream, &jchunk);
        esp_camera_fb_return(fb);
    } else
    {
        out_len = fb->width * fb->height * 3;
        out_width = fb->width;
        out_height = fb->height;
        out_buf = (uint8_t*)malloc(out_len);
        if (!out_buf) {
            log_e("out_buf malloc failed");
            esp_camera_fb_return(fb);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        s = fmt2rgb888(fb->buf, fb->len, fb->format, out_buf);
        esp_camera_fb_return(fb);
        if (!s) {
            free(out_buf);
            log_e("To rgb888 failed");
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        fb_data_t rfb;
        rfb.width = out_width;
        rfb.height = out_height;
        rfb.data = out_buf;
        rfb.bytes_per_pixel = 3;
        rfb.format = FB_BGR888;

#if TWO_STAGE
        HumanFaceDetectMSR01 s1(0.1F, 0.5F, 10, 0.2F);
        HumanFaceDetectMNP01 s2(0.5F, 0.3F, 5);
        std::list<dl::detect::result_t> &candidates = s1.infer((uint8_t *)out_buf, {(int)out_height, (int)out_width, 3});
        std::list<dl::detect::result_t> &results = s2.infer((uint8_t *)out_buf, {(int)out_height, (int)out_width, 3}, candidates);
#else
        HumanFaceDetectMSR01 s1(0.3F, 0.5F, 10, 0.2F);
        std::list<dl::detect::result_t> &results = s1.infer((uint8_t *)out_buf, {(int)out_height, (int)out_width, 3});
#endif

        if (results.size() > 0) {
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
            detected = true;
#endif
#if CONFIG_ESP_FACE_RECOGNITION_ENABLED
            if (recognition_enabled) {
                face_id = run_face_recognition(&rfb, &results);
            }
#endif
            draw_face_boxes(&rfb, &results, face_id);
        }

        s = fmt2jpg_cb(out_buf, out_len, out_width, out_height, PIXFORMAT_RGB888, 90, jpg_encode_stream, &jchunk);
        free(out_buf);
    }

    if (!s) {
        log_e("JPEG compression failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
    int64_t fr_end = esp_timer_get_time();
#endif
    log_i("FACE: %uB %ums %s%d", (uint32_t)(jchunk.len), (uint32_t)((fr_end - fr_start) / 1000), detected ? "DETECTED " : "", face_id);
    return res;
#endif
}

// Official CameraWebServer-style MJPEG stream.
// The camera frame remains in the driver buffer until the multipart frame has
// been sent. This is deliberately simple: no per-frame malloc, JPEG copy,
// retry loop, or frame queue is involved.
static esp_err_t stream_handler(httpd_req_t *req)
{
    camera_fb_t *fb = NULL;
    struct timeval timestamp;
    size_t jpg_len = 0;
    uint8_t *jpg_buf = NULL;
    char part_buf[128];
    esp_err_t res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    const uint32_t my_generation = ++stream_generation;

    if (res != ESP_OK) {
        return res;
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "X-Framerate", "30");

#if CONFIG_LED_ILLUMINATOR_ENABLED
    isStreaming = true;
    enable_led(true);
#endif

    while (true) {
        if (my_generation != stream_generation) {
            res = ESP_FAIL;
            break;
        }

        camera_sync::LockGuard camera_lock(5000);
        if (!camera_lock.locked()) {
            log_e("Camera lock timeout");
            res = ESP_FAIL;
            break;
        }

        fb = esp_camera_fb_get();
        if (!fb) {
            log_e("Camera capture failed");
            res = ESP_FAIL;
        } else {
            timestamp.tv_sec = fb->timestamp.tv_sec;
            timestamp.tv_usec = fb->timestamp.tv_usec;

            if (fb->format != PIXFORMAT_JPEG) {
                const bool converted = frame2jpg(fb, 80, &jpg_buf, &jpg_len);
                esp_camera_fb_return(fb);
                fb = NULL;
                if (!converted) {
                    log_e("JPEG compression failed");
                    res = ESP_FAIL;
                }
            } else {
                jpg_len = fb->len;
                jpg_buf = fb->buf;
            }
        }

        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        if (res == ESP_OK) {
            const size_t header_len = snprintf(
                part_buf, sizeof(part_buf), _STREAM_PART, (unsigned)jpg_len,
                (int)timestamp.tv_sec, (int)timestamp.tv_usec);
            res = httpd_resp_send_chunk(req, part_buf, header_len);
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char *)jpg_buf, jpg_len);
        }

        if (fb) {
            esp_camera_fb_return(fb);
            fb = NULL;
            jpg_buf = NULL;
        } else if (jpg_buf) {
            free(jpg_buf);
            jpg_buf = NULL;
        }

        if (res != ESP_OK) {
            log_e("Send frame failed");
            break;
        }
    }

#if CONFIG_LED_ILLUMINATOR_ENABLED
    if (my_generation == stream_generation) {
        isStreaming = false;
        enable_led(false);
    }
#endif

    return res;
}

static esp_err_t parse_get(httpd_req_t *req, char **obuf)
{
    char *buf = NULL;
    size_t buf_len = 0;

    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        buf = (char *)malloc(buf_len);
        if (!buf) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            *obuf = buf;
            return ESP_OK;
        }
        free(buf);
    }
    httpd_resp_send_404(req);
    return ESP_FAIL;
}

static esp_err_t cmd_handler(httpd_req_t *req)
{
    char *buf = NULL;
    char variable[32];
    char value[32];

    if (parse_get(req, &buf) != ESP_OK) {
        return ESP_FAIL;
    }
    if (httpd_query_key_value(buf, "var", variable, sizeof(variable)) != ESP_OK ||
        httpd_query_key_value(buf, "val", value, sizeof(value)) != ESP_OK) {
        free(buf);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    free(buf);

    int val = atoi(value);
    log_i("%s = %d", variable, val);
    sensor_t *s = esp_camera_sensor_get();
    int res = 0;

    camera_sync::LockGuard camera_lock(5000);
    if (!camera_lock.locked()) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (!strcmp(variable, "framesize")) {
        if (s->pixformat == PIXFORMAT_JPEG) {
            res = s->set_framesize(s, (framesize_t)val);
            log_i("framesize request=%d result=%d active=%d", val, res, s->status.framesize);
        }
    }
    else if (!strcmp(variable, "quality"))
        res = s->set_quality(s, val);
    else if (!strcmp(variable, "contrast"))
        res = s->set_contrast(s, val);
    else if (!strcmp(variable, "brightness"))
        res = s->set_brightness(s, val);
    else if (!strcmp(variable, "saturation"))
        res = s->set_saturation(s, val);
    else if (!strcmp(variable, "gainceiling"))
        res = s->set_gainceiling(s, (gainceiling_t)val);
    else if (!strcmp(variable, "colorbar"))
        res = s->set_colorbar(s, val);
    else if (!strcmp(variable, "awb"))
        res = s->set_whitebal(s, val);
    else if (!strcmp(variable, "agc"))
        res = s->set_gain_ctrl(s, val);
    else if (!strcmp(variable, "aec"))
        res = s->set_exposure_ctrl(s, val);
    else if (!strcmp(variable, "hmirror"))
        res = s->set_hmirror(s, val);
    else if (!strcmp(variable, "vflip"))
        res = s->set_vflip(s, val);
    else if (!strcmp(variable, "awb_gain"))
        res = s->set_awb_gain(s, val);
    else if (!strcmp(variable, "agc_gain"))
        res = s->set_agc_gain(s, val);
    else if (!strcmp(variable, "aec_value"))
        res = s->set_aec_value(s, val);
    else if (!strcmp(variable, "aec2"))
        res = s->set_aec2(s, val);
    else if (!strcmp(variable, "dcw"))
        res = s->set_dcw(s, val);
    else if (!strcmp(variable, "bpc"))
        res = s->set_bpc(s, val);
    else if (!strcmp(variable, "wpc"))
        res = s->set_wpc(s, val);
    else if (!strcmp(variable, "raw_gma"))
        res = s->set_raw_gma(s, val);
    else if (!strcmp(variable, "lenc"))
        res = s->set_lenc(s, val);
    else if (!strcmp(variable, "special_effect"))
        res = s->set_special_effect(s, val);
    else if (!strcmp(variable, "wb_mode"))
        res = s->set_wb_mode(s, val);
    else if (!strcmp(variable, "ae_level"))
        res = s->set_ae_level(s, val);
#if CONFIG_LED_ILLUMINATOR_ENABLED
    else if (!strcmp(variable, "led_intensity")) {
        led_duty = constrain(val, 0, CONFIG_LED_MAX_INTENSITY);
        if (isStreaming)
            enable_led(true);
    }
#endif

#if CONFIG_ESP_FACE_DETECT_ENABLED
    else if (!strcmp(variable, "face_detect")) {
        detection_enabled = val;
#if CONFIG_ESP_FACE_RECOGNITION_ENABLED
        if (!detection_enabled) {
            recognition_enabled = 0;
        }
#endif
    }
#if CONFIG_ESP_FACE_RECOGNITION_ENABLED
    else if (!strcmp(variable, "face_enroll")){
        is_enrolling = !is_enrolling;
        log_i("Enrolling: %s", is_enrolling?"true":"false");
    }
    else if (!strcmp(variable, "face_recognize")) {
        recognition_enabled = val;
        if (recognition_enabled) {
            detection_enabled = val;
        }
    }
#endif
#endif
    else {
        log_i("Unknown command: %s", variable);
        res = -1;
    }

    if (res < 0) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

static int print_reg(char * p, sensor_t * s, uint16_t reg, uint32_t mask){
    return sprintf(p, "\"0x%x\":%u,", reg, s->get_reg(s, reg, mask));
}

static esp_err_t status_handler(httpd_req_t *req)
{
    static char json_response[1024];

    sensor_t *s = esp_camera_sensor_get();
    char *p = json_response;
    *p++ = '{';

    if(s->id.PID == OV5640_PID || s->id.PID == OV3660_PID){
        for(int reg = 0x3400; reg < 0x3406; reg+=2){
            p+=print_reg(p, s, reg, 0xFFF);//12 bit
        }
        p+=print_reg(p, s, 0x3406, 0xFF);

        p+=print_reg(p, s, 0x3500, 0xFFFF0);//16 bit
        p+=print_reg(p, s, 0x3503, 0xFF);
        p+=print_reg(p, s, 0x350a, 0x3FF);//10 bit
        p+=print_reg(p, s, 0x350c, 0xFFFF);//16 bit

        for(int reg = 0x5480; reg <= 0x5490; reg++){
            p+=print_reg(p, s, reg, 0xFF);
        }

        for(int reg = 0x5380; reg <= 0x538b; reg++){
            p+=print_reg(p, s, reg, 0xFF);
        }

        for(int reg = 0x5580; reg < 0x558a; reg++){
            p+=print_reg(p, s, reg, 0xFF);
        }
        p+=print_reg(p, s, 0x558a, 0x1FF);//9 bit
    } else if(s->id.PID == OV2640_PID){
        p+=print_reg(p, s, 0xd3, 0xFF);
        p+=print_reg(p, s, 0x111, 0xFF);
        p+=print_reg(p, s, 0x132, 0xFF);
    }

    p += sprintf(p, "\"xclk\":%u,", s->xclk_freq_hz / 1000000);
    p += sprintf(p, "\"pixformat\":%u,", s->pixformat);
    p += sprintf(p, "\"framesize\":%u,", s->status.framesize);
    p += sprintf(p, "\"quality\":%u,", s->status.quality);
    p += sprintf(p, "\"brightness\":%d,", s->status.brightness);
    p += sprintf(p, "\"contrast\":%d,", s->status.contrast);
    p += sprintf(p, "\"saturation\":%d,", s->status.saturation);
    p += sprintf(p, "\"sharpness\":%d,", s->status.sharpness);
    p += sprintf(p, "\"special_effect\":%u,", s->status.special_effect);
    p += sprintf(p, "\"wb_mode\":%u,", s->status.wb_mode);
    p += sprintf(p, "\"awb\":%u,", s->status.awb);
    p += sprintf(p, "\"awb_gain\":%u,", s->status.awb_gain);
    p += sprintf(p, "\"aec\":%u,", s->status.aec);
    p += sprintf(p, "\"aec2\":%u,", s->status.aec2);
    p += sprintf(p, "\"ae_level\":%d,", s->status.ae_level);
    p += sprintf(p, "\"aec_value\":%u,", s->status.aec_value);
    p += sprintf(p, "\"agc\":%u,", s->status.agc);
    p += sprintf(p, "\"agc_gain\":%u,", s->status.agc_gain);
    p += sprintf(p, "\"gainceiling\":%u,", s->status.gainceiling);
    p += sprintf(p, "\"bpc\":%u,", s->status.bpc);
    p += sprintf(p, "\"wpc\":%u,", s->status.wpc);
    p += sprintf(p, "\"raw_gma\":%u,", s->status.raw_gma);
    p += sprintf(p, "\"lenc\":%u,", s->status.lenc);
    p += sprintf(p, "\"hmirror\":%u,", s->status.hmirror);
    p += sprintf(p, "\"dcw\":%u,", s->status.dcw);
    p += sprintf(p, "\"colorbar\":%u", s->status.colorbar);
#if CONFIG_LED_ILLUMINATOR_ENABLED
    p += sprintf(p, ",\"led_intensity\":%d", led_duty);
#else
    p += sprintf(p, ",\"led_intensity\":%d", -1);
#endif
    p += sprintf(p, ",\"recording\":%d", recorder::active() ? 1 : 0);
#if CONFIG_ESP_FACE_DETECT_ENABLED
    p += sprintf(p, ",\"face_detect\":%u", detection_enabled);
#if CONFIG_ESP_FACE_RECOGNITION_ENABLED
    p += sprintf(p, ",\"face_enroll\":%u,", is_enrolling);
    p += sprintf(p, "\"face_recognize\":%u", recognition_enabled);
#endif
#endif
    *p++ = '}';
    *p++ = 0;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json_response, strlen(json_response));
}

static int parse_get_var(char *buf, const char * key, int def)
{
    char _int[16];
    if(httpd_query_key_value(buf, key, _int, sizeof(_int)) != ESP_OK){
        return def;
    }
    return atoi(_int);
}

// 从查询串中取字符串参数, 如 dir=up&action=down
static bool parse_get_str(char *buf, const char *key, char *out, size_t outlen)
{
    size_t keylen = strlen(key);
    char *p = buf;
    while ((p = strstr(p, key)) != NULL) {
        if (p[keylen] == '=' && (p == buf || p[-1] == '&')) {
            p += keylen + 1;
            size_t i = 0;
            while (*p && *p != '&' && i < outlen - 1) {
                out[i++] = *p++;
            }
            out[i] = 0;
            return true;
        }
        p++;
    }
    return false;
}

// ===== 云台遥控 =====
// 协议: GET /ptz?dir=up|down|left|right|talk&action=down|up
//   方向键: action=down 按下开始调整, action=up 松开停止 -> 舵机
//   中间键(对话): action=down 长按开始说话, action=up 松开结束 -> 麦克风
static void ptz_apply(const char *dir, const char *action)
{
    bool pressed = (strcmp(action, "down") == 0);
    if (strcmp(dir, "talk") == 0) {
        if (pressed) talk::start();
        else         talk::stop();
        return;
    }
    gimbal::move(dir, pressed);
}

static esp_err_t ptz_handler(httpd_req_t *req)
{
    char *buf = NULL;

    if (parse_get(req, &buf) != ESP_OK) {
        return ESP_FAIL;
    }

    char dir[16] = {0}, action[16] = {0};
    parse_get_str(buf, "dir", dir, sizeof(dir));
    parse_get_str(buf, "action", action, sizeof(action));
    free(buf);

    if (dir[0] == 0 || action[0] == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "dir and action are required");
        return ESP_FAIL;
    }

    ptz_apply(dir, action);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

// ===== 录制 =====
// 协议: GET /rec?state=start|stop
static esp_err_t rec_handler(httpd_req_t *req)
{
    char *buf = NULL;
    if (parse_get(req, &buf) != ESP_OK) {
        return ESP_FAIL;
    }
    char state[8] = {0};
    parse_get_str(buf, "state", state, sizeof(state));
    free(buf);

    const char *msg;
    if (!strcmp(state, "start")) {
        if (recorder::start()) {
            msg = "1";
        } else {
            msg = recorder::configured() ? "启动失败, 查看串口日志" : "未配置 NAS 服务器 (hardware_config.h)";
        }
    } else if (!strcmp(state, "stop")) {
        recorder::stop();
        msg = "1";
    } else {
        msg = "state must be start|stop";
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, msg, strlen(msg));
}

static esp_err_t stream_stop_handler(httpd_req_t *req)
{
    ++stream_generation;
#if CONFIG_LED_ILLUMINATOR_ENABLED
    isStreaming = false;
    enable_led(false);
#endif
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

// 通用静态文件处理器: 从 LittleFS 读取网页文件, 按 1KB 分块发送
// user_ctx 携带文件路径, 如 "/index.html"
static esp_err_t file_handler(httpd_req_t *req)
{
    const char *path = (const char *)req->user_ctx;
    File f = LittleFS.open(path, "r");
    if (!f) {
        log_e("LittleFS 中找不到文件: %s (是否忘记执行 'pio run -t uploadfs'?)", path);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found. Run 'pio run -t uploadfs' to upload web page files.");
        return ESP_FAIL;
    }
    String p(path);
    const char *type = "text/plain";
    if (p.endsWith(".html")) type = "text/html";
    else if (p.endsWith(".css")) type = "text/css";
    else if (p.endsWith(".js")) type = "application/javascript";
    else if (p.endsWith(".png")) type = "image/png";
    else if (p.endsWith(".ico")) type = "image/x-icon";
    httpd_resp_set_type(req, type);
    // 禁止浏览器缓存, 避免更新网页后浏览器仍用旧文件
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    uint8_t buf[1024];
    size_t n;
    while ((n = f.read(buf, sizeof(buf))) > 0) {
        if (httpd_resp_send_chunk(req, (const char *)buf, n) != ESP_OK) {
            f.close();
            return ESP_FAIL;
        }
    }
    f.close();
    return httpd_resp_send_chunk(req, NULL, 0);
}

void startCameraServer()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // esp-idf 5 起请求头缓冲为运行时配置; 默认 1024 字节, 浏览器带 Cookie
    // 会超限返回 431 ("Header fields are too large"), 调大到 4KB
    config.max_req_hdr_len = 4096;
    config.max_uri_handlers = 16;

    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = file_handler,
        .user_ctx = (void *)"/index.html"
#ifdef CONFIG_HTTPD_WS_SUPPORT
        ,
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
#endif
    };

    httpd_uri_t style_uri = {
        .uri = "/style.css",
        .method = HTTP_GET,
        .handler = file_handler,
        .user_ctx = (void *)"/style.css"
    };

    httpd_uri_t script_uri = {
        .uri = "/script.js",
        .method = HTTP_GET,
        .handler = file_handler,
        .user_ctx = (void *)"/script.js"
    };

    httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_handler,
        .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
        ,
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
#endif
    };

    httpd_uri_t cmd_uri = {
        .uri = "/control",
        .method = HTTP_GET,
        .handler = cmd_handler,
        .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
        ,
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
#endif
    };

    httpd_uri_t capture_uri = {
        .uri = "/capture",
        .method = HTTP_GET,
        .handler = capture_handler,
        .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
        ,
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
#endif
    };

    httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_handler,
        .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
        ,
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
#endif
    };

    httpd_uri_t ptz_uri = {
        .uri = "/ptz",
        .method = HTTP_GET,
        .handler = ptz_handler,
        .user_ctx = NULL
    };

    httpd_uri_t rec_uri = {
        .uri = "/rec",
        .method = HTTP_GET,
        .handler = rec_handler,
        .user_ctx = NULL
    };

    httpd_uri_t stream_stop_uri = {
        .uri = "/stop",
        .method = HTTP_GET,
        .handler = stream_stop_handler,
        .user_ctx = NULL
    };

    ra_filter_init(&ra_filter, 20);

#if CONFIG_ESP_FACE_RECOGNITION_ENABLED
    recognizer.set_partition(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "fr");

    // load ids from flash partition
    recognizer.set_ids_from_flash();
#endif
    log_i("Starting web server on port: '%d'", config.server_port);
    if (httpd_start(&camera_httpd, &config) == ESP_OK)
    {
        httpd_register_uri_handler(camera_httpd, &index_uri);
        httpd_register_uri_handler(camera_httpd, &style_uri);
        httpd_register_uri_handler(camera_httpd, &script_uri);
        httpd_register_uri_handler(camera_httpd, &cmd_uri);
        httpd_register_uri_handler(camera_httpd, &status_uri);
        httpd_register_uri_handler(camera_httpd, &capture_uri);

        httpd_register_uri_handler(camera_httpd, &ptz_uri);
        httpd_register_uri_handler(camera_httpd, &rec_uri);
        httpd_register_uri_handler(camera_httpd, &stream_stop_uri);
    }

    config.server_port += 1;
    config.ctrl_port += 1;
    log_i("Starting stream server on port: '%d'", config.server_port);
    if (httpd_start(&stream_httpd, &config) == ESP_OK)
    {
        httpd_register_uri_handler(stream_httpd, &stream_uri);
    }
}

void setupLedFlash(int pin) 
{
    #if CONFIG_LED_ILLUMINATOR_ENABLED
    s_led_pin = pin;

    // Configure the LED directly through ESP-IDF so Timer 1 is explicit.
    // ledcAttach()/ledcAttachChannel() chooses a free timer by Arduino's
    // bookkeeping and cannot see the camera driver's Timer 0 reservation.
    ledc_timer_config_t timer_config = {};
    timer_config.speed_mode = LED_PWM_MODE;
    timer_config.timer_num = LED_PWM_TIMER;
    timer_config.duty_resolution = LEDC_TIMER_8_BIT;
    timer_config.freq_hz = LED_PWM_FREQ_HZ;
    timer_config.clk_cfg = LEDC_AUTO_CLK;

    ledc_channel_config_t channel_config = {};
    channel_config.gpio_num = static_cast<gpio_num_t>(pin);
    channel_config.speed_mode = LED_PWM_MODE;
    channel_config.channel = LED_PWM_CHANNEL;
    channel_config.intr_type = LEDC_INTR_DISABLE;
    channel_config.timer_sel = LED_PWM_TIMER;
    channel_config.duty = 0;
    channel_config.hpoint = 0;

    const esp_err_t timer_res = ledc_timer_config(&timer_config);
    const esp_err_t channel_res = (timer_res == ESP_OK)
        ? ledc_channel_config(&channel_config)
        : timer_res;

    if (channel_res == ESP_OK) {
        s_led_pwm_ready = true;
        s_led_applied_duty = 0;
        log_i("LED flash PWM ready: GPIO%d, %dHz, 8-bit, LEDC timer %d/channel %d",
              pin, LED_PWM_FREQ_HZ, LED_PWM_TIMER, LED_PWM_CHANNEL);
    } else {
        s_led_pwm_ready = false;
        pinMode(pin, OUTPUT);
        digitalWrite(pin, LOW);
        log_w("LED flash PWM unavailable (0x%x), using digital on/off fallback", channel_res);
    }
    #else
    log_i("LED flash is disabled -> CONFIG_LED_ILLUMINATOR_ENABLED = 0");
    #endif
}
