#pragma once

#include <Arduino.h>

// 云台舵机控制(360°连续旋转 SG90): 水平(左右) + 垂直(上下) 两轴
// - begin(): 预留舵机 PWM 定时器并创建运动任务
// - move(): 非阻塞, 由 /ptz 接口调用; 按住方向键转动, 松开停止
namespace gimbal {

void begin();
void move(const char *dir, bool pressed);  // dir: up|down|left|right

} // namespace gimbal
