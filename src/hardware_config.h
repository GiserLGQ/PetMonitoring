#pragma once

// =====================================================================
// 硬件接线与后端服务器集中配置
// 接线前先改这里, 硬件插上即可用, 无需改动其他文件
// =====================================================================

// ---------------- 云台舵机 (SG90/MG90S, 50Hz PWM) ----------------
// 接线: 棕=GND, 红=5V(独立供电!), 橙/黄=信号
// 警告: 舵机务必用独立 5V 电源(≥2A), 与板子共地, 不要从 ESP32-CAM 取电
#define GIMBAL_PIN_PAN    13    // 左右(水平)舵机信号线
#define GIMBAL_PIN_TILT   12    // 上下(垂直)舵机信号线
// 注意: GPIO12 是启动 strap 引脚(上电时为高会导致 Flash 电压错误无法启动),
// 舵机信号线是输入不驱动电平, 正常接线无影响; 但切勿在 GPIO12 上接上拉电阻
#define GIMBAL_PAN_MIN     0    // 水平角度限位(度)
#define GIMBAL_PAN_MAX   180
#define GIMBAL_TILT_MIN   30    // 垂直角度限位(预留云台机械行程, 防撞)
#define GIMBAL_TILT_MAX  150
#define GIMBAL_START_ANGLE 90   // 上电回中角度
#define GIMBAL_SPEED     100    // 按住移动时的角速度(度/秒)

// ---------------- INMP441 I2S 数字麦克风 (对话用) ----------------
// 接线: VDD=3.3V, GND=GND, L/R=GND(左声道), SCK/CLK=BCLK, WS=LRCL, SD=DOUT
// 注意: GPIO33 同时是板载红色小 LED, 接麦克风后红 LED 不可用(白色闪光灯不受影响)
// 重要: SD 卡占用 GPIO 2/14/15, 与本麦克风引脚冲突 -> 本方案不支持 SD 卡,
//       录制请走 NAS(见下), 两者只能二选一
#define TALK_I2S_PORT    I2S_NUM_1  // I2S0 被摄像头占用, 必须用 I2S1
#define TALK_I2S_BCLK    14         // INMP441 SCK
#define TALK_I2S_WS      15         // INMP441 WS
#define TALK_I2S_DATA    33         // INMP441 SD
#define TALK_SAMPLE_RATE 16000      // 采样率 16kHz 足够语音识别

// ---------------- 对话/录制 后端服务器 (NAS 或任何局域网主机) ----------------
// 对话: 长按按钮时, 麦克风录音以 WAV(POST body) 持续推送到 TALK_SERVER
//       推荐后端接 语音识别->大模型->TTS 流程, 回复音频可在浏览器端播放
// 录制: 开始录制后, MJPEG 帧流(连续 JPEG)推送到 REC_SERVER,
//       NAS 端可用 ffmpeg -f mjpeg -i ... 直接封装成视频文件
// HOST 留空("")则对应功能只打印串口日志, 不发送网络数据(硬件未配好时也能安全测试)
#define TALK_SERVER_HOST ""        // 例: "192.168.0.100"
#define TALK_SERVER_PORT 8000
#define TALK_SERVER_PATH "/talk"   // 接收 WAV 的接口路径
#define REC_SERVER_HOST  ""        // 例: "192.168.0.100"
#define REC_SERVER_PORT  8000
#define REC_SERVER_PATH  "/rec"    // 接收 MJPEG 流的接口路径
