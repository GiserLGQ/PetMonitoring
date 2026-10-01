#pragma once

#include <Arduino.h>

namespace camera_sync {

void begin();
bool lock(uint32_t timeout_ms);
void unlock();

class LockGuard {
public:
    explicit LockGuard(uint32_t timeout_ms) : m_locked(lock(timeout_ms)) {}
    ~LockGuard() { if (m_locked) unlock(); }

    bool locked() const { return m_locked; }
    LockGuard(const LockGuard &) = delete;
    LockGuard &operator=(const LockGuard &) = delete;

private:
    bool m_locked;
};

} // namespace camera_sync
