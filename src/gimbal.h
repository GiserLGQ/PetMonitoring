#pragma once

#include <Arduino.h>

// 云台舵机控制(360°连续旋转舵机): 水平(左右) + 垂直(上下) 两轴
// - begin(): 初始化 LEDC PWM 并创建运动任务, 上电空挡刹车
// - move(): 非阻塞, 由 /ptz 接口调用; 按住方向键持续转动, 松开刹车
//   360°舵机无角度概念, 脉宽=油门(1.5ms 停止), 只能速度控制不能定位
// 运动逻辑在独立 FreeRTOS 任务里以 20ms 周期执行, 不影响网络与摄像头
namespace gimbal {

void begin();
void move(const char *dir, bool pressed);  // dir: up|down|left|right

} // namespace gimbal
