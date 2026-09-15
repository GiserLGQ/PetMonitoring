#include "gimbal.h"
#include "hardware_config.h"
#include "esp32-hal-ledc.h"

// 舵机参数: 周期 50Hz(20ms), 脉宽 0.5~2.5ms 对应 0~180 度
// arduino 3.x LEDC 按"引脚"操作, 内部自动分配通道, 无需手动指定
static const int PAN_PIN  = GIMBAL_PIN_PAN;
static const int TILT_PIN = GIMBAL_PIN_TILT;

// 方向标志(位掩码), /ptz 接口与运动任务之间共享
enum : uint8_t {
    DIR_UP    = 1 << 0,
    DIR_DOWN  = 1 << 1,
    DIR_LEFT  = 1 << 2,
    DIR_RIGHT = 1 << 3,
};
static volatile uint8_t s_flags = 0;
static volatile float s_pan  = GIMBAL_START_ANGLE;  // 当前角度
static volatile float s_tilt = GIMBAL_START_ANGLE;

static inline uint32_t angleToDuty(float angle)
{
    // 角度 -> 脉宽(500~2500us) -> 16 位占空比
    uint32_t us = 500 + (uint32_t)(angle * 2000.0f / 180.0f);
    return us * 65536UL / 20000UL;
}

static void writePan(float a)
{
    if (a < GIMBAL_PAN_MIN) a = GIMBAL_PAN_MIN;
    if (a > GIMBAL_PAN_MAX) a = GIMBAL_PAN_MAX;
    s_pan = a;
    ledcWrite(PAN_PIN, angleToDuty(a));
}

static void writeTilt(float a)
{
    if (a < GIMBAL_TILT_MIN) a = GIMBAL_TILT_MIN;
    if (a > GIMBAL_TILT_MAX) a = GIMBAL_TILT_MAX;
    s_tilt = a;
    ledcWrite(TILT_PIN, angleToDuty(a));
}

// 运动任务: 每 20ms 检查按住的方向, 按角速度逼近目标
static void gimbalTask(void *)
{
    const float step = GIMBAL_SPEED * 0.02f;  // 每 tick 步进角度
    for (;;) {
        uint8_t f = s_flags;
        if (f & DIR_LEFT)  writePan(s_pan - step);
        if (f & DIR_RIGHT) writePan(s_pan + step);
        if (f & DIR_UP)    writeTilt(s_tilt - step);  // 上=抬头, 角度减小
        if (f & DIR_DOWN)  writeTilt(s_tilt + step);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

namespace gimbal {

void begin()
{
    ledcAttach(PAN_PIN, 50, 16);
    ledcAttach(TILT_PIN, 50, 16);
    writePan(GIMBAL_START_ANGLE);   // 上电回中
    writeTilt(GIMBAL_START_ANGLE);
    xTaskCreatePinnedToCore(gimbalTask, "gimbal", 3072, NULL, 2, NULL, 1);
    Serial.printf("[Gimbal] 舵机已就绪: 左右=GPIO%d 上下=GPIO%d, 回中(%d,%d)\n",
                  GIMBAL_PIN_PAN, GIMBAL_PIN_TILT, GIMBAL_START_ANGLE, GIMBAL_START_ANGLE);
}

void move(const char *dir, bool pressed)
{
    uint8_t bit;
    if      (!strcmp(dir, "up"))    bit = DIR_UP;
    else if (!strcmp(dir, "down"))  bit = DIR_DOWN;
    else if (!strcmp(dir, "left"))  bit = DIR_LEFT;
    else if (!strcmp(dir, "right")) bit = DIR_RIGHT;
    else return;

    if (pressed) s_flags |= bit;    // 按住: 置位, 任务持续转动
    else         s_flags &= ~bit;   // 松开: 清除
}

} // namespace gimbal
