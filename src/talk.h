#pragma once

#include <Arduino.h>

// 对话功能: INMP441 I2S 麦克风采集, 长按对话键时录音并以 WAV 流式 POST
// 到 TALK_SERVER(hardware_config.h 配置); 松开按键结束上传。
// 服务器未配置时仅打印串口日志(方便先接好硬件后逐项调试)。
// 注意: ESP32-CAM 没有喇叭, 回复音频建议在网页/浏览器端播放(NAS 端 TTS 推回)。
namespace talk {

void begin();        // 初始化 I2S(硬件未接也不会报错, 采到的是静音)
bool start();        // 长按开始: 启动采集+上传任务
void stop();         // 松开结束: 停止采集并收尾上传
bool active();

} // namespace talk
