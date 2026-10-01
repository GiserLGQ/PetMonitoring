#include "camera_sync.h"

static SemaphoreHandle_t s_camera_mutex = nullptr;

namespace camera_sync {

void begin()
{
    if (!s_camera_mutex) {
        s_camera_mutex = xSemaphoreCreateMutex();
    }
}

bool lock(uint32_t timeout_ms)
{
    if (!s_camera_mutex) return false;
    return xSemaphoreTake(s_camera_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void unlock()
{
    if (s_camera_mutex) {
        xSemaphoreGive(s_camera_mutex);
    }
}

} // namespace camera_sync
