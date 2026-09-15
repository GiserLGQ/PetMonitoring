#pragma once

#include <Arduino.h>

// 录制功能: 开始后持续抓取摄像头帧, 以连续 JPEG(MJPEG) 流 POST 到
// REC_SERVER(hardware_config.h 配置)。NAS 端接收并保存, 例如用 ffmpeg:
//   ffmpeg -f mjpeg -i received.mjpeg -c copy out.avi
// 本接口与网页视频流互不干扰(共用摄像头帧缓冲, 帧率会适当分摊)。
// 服务器未配置时 start() 返回 false, 由 /rec 接口向网页返回提示。
namespace recorder {

void begin();
bool start();
void stop();
bool active();
bool configured();   // 是否已配置 REC_SERVER_HOST

} // namespace recorder
