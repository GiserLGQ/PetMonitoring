#include "gimbal.h"
#include "hardware_config.h"
#include "esp32-hal-ledc.h"

// 360°连续旋转舵机: 周期 50Hz(20ms), 脉宽=油门(不是角度!)
// 1.5ms=停止(空挡), 1.0ms=全速正转, 2.0ms=全速反转, 中间值=对应速度
// 舵机内部无位置反馈, 固件只控制"转多快/往哪转", 不知道"转到哪"
// arduino 3.x LEDC 按"引脚"操作, 内部自动分配通道, 无需手动指定
static const int PAN_PIN  = GIMBAL_PIN_PAN;//水平轴
static const int TILT_PIN = GIMBAL_PIN_TILT;//俯仰轴

// 方向标志(位掩码), /ptz 接口与运动任务之间共享
enum : uint8_t {
    DIR_UP    = 1 << 0,
    DIR_DOWN  = 1 << 1,
    DIR_LEFT  = 1 << 2,
    DIR_RIGHT = 1 << 3,
};
static volatile uint8_t s_flags = 0;

static inline uint32_t speedToDuty(float percent)
{
    // 速度(-100~+100) -> 脉宽(1000~2000us, 1500为空挡) -> 16 位占空比
    if (percent < -100) percent = -100;
    if (percent >  100) percent =  100;
    int us = 1500 + (int)(percent * 5.0f);
    return (uint32_t)us * 65536UL / 20000UL;
}

static void writePan(float v)
{
    ledcWrite(PAN_PIN, speedToDuty(v));
}

static void writeTilt(float v)
{
    ledcWrite(TILT_PIN, speedToDuty(v));
}

// 运动任务: 每 20ms 根据按住的方向输出"油门", 松手输出 0(空挡刹车)
static void gimbalTask(void *)
{
    for (;;) {
        uint8_t f = s_flags;
        float panV = 0, tiltV = 0;
        if (f & DIR_LEFT)  panV -= GIMBAL_SPEED;
        if (f & DIR_RIGHT) panV += GIMBAL_SPEED;
        if (f & DIR_UP)    tiltV -= GIMBAL_SPEED;  // 上=抬头
        if (f & DIR_DOWN)  tiltV += GIMBAL_SPEED;
        writePan(panV);
        writeTilt(tiltV);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

namespace gimbal {

void begin()
{
    ledcAttach(PAN_PIN, 50, 16);
    ledcAttach(TILT_PIN, 50, 16);
    writePan(0);   // 上电空挡: 两个舵机立刻刹车
    writeTilt(0);
    xTaskCreatePinnedToCore(gimbalTask, "gimbal", 3072, NULL, 2, NULL, 1);
    Serial.printf("[Gimbal] 360°舵机已就绪(速度模式): 左右=GPIO%d 上下=GPIO%d, 按住速度=%d%%\n",
                  GIMBAL_PIN_PAN, GIMBAL_PIN_TILT, GIMBAL_SPEED);
}

void move(const char *dir, bool pressed)
{
    uint8_t bit;
    if      (!strcmp(dir, "up"))    bit = DIR_UP;
    else if (!strcmp(dir, "down"))  bit = DIR_DOWN;
    else if (!strcmp(dir, "left"))  bit = DIR_LEFT;
    else if (!strcmp(dir, "right")) bit = DIR_RIGHT;
    else return;

    if (pressed) s_flags |= bit;    // 按住: 置位, 任务持续输出油门
    else         s_flags &= ~bit;   // 松开: 清除, 任务输出空挡刹车
}

} // namespace gimbal
