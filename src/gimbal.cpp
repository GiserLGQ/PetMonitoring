#include "gimbal.h"
#include "hardware_config.h"
#include <ESP32Servo.h>

// The SG90 variant used here is a continuous-rotation servo. It has no
// absolute angle; the pulse width around the calibrated stop value controls
// direction and speed.
static Servo s_pan_servo;
static Servo s_tilt_servo;
static volatile uint8_t s_flags = 0;

enum : uint8_t {
    DIR_UP    = 1 << 0,
    DIR_DOWN  = 1 << 1,
    DIR_LEFT  = 1 << 2,
    DIR_RIGHT = 1 << 3,
};

static int velocityPulse(int stop_us, int direction)
{
    // GIMBAL_SPEED is a percentage. A 275us offset is a practical starting
    // point for continuous SG90 servos and remains inside the safe range.
    const int pulse = stop_us + direction * GIMBAL_SPEED * 5;
    return constrain(pulse, 1000, 2000);
}

static void startServo(Servo &servo, int pin, int pulse_us)
{
    if (!servo.attached()) {
        servo.attach(pin, 500, 2400);
    }
    servo.writeMicroseconds(pulse_us);
}

static void stopServo(Servo &servo, int stop_us)
{
    if (servo.attached()) {
        // Keep sending the neutral pulse. Detaching immediately after this
        // write may remove the signal before a continuous-rotation servo sees
        // a complete stop command; the servo timers are isolated from camera
        // and flash timers, so idle PWM is safe to leave attached.
        servo.writeMicroseconds(stop_us);
    }
}

static void updateAxis(Servo &servo, int pin, int stop_us, int direction)
{
    if (direction == 0) {
        stopServo(servo, stop_us);
        return;
    }

    startServo(servo, pin, velocityPulse(stop_us, direction));
}

static void gimbalTask(void *)
{
    for (;;) {
        const uint8_t flags = s_flags;
        int pan_direction = 0;
        int tilt_direction = 0;

        if (flags & DIR_LEFT)  pan_direction--;
        if (flags & DIR_RIGHT) pan_direction++;
        if (flags & DIR_UP)    tilt_direction--;
        if (flags & DIR_DOWN)  tilt_direction++;

        updateAxis(s_pan_servo, GIMBAL_PIN_PAN, GIMBAL_PAN_STOP_US, pan_direction * GIMBAL_PAN_DIRECTION);
        updateAxis(s_tilt_servo, GIMBAL_PIN_TILT, GIMBAL_TILT_STOP_US, tilt_direction * GIMBAL_TILT_DIRECTION);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

namespace gimbal {

void begin()
{
    // Camera XCLK owns LEDC timer 0 and the flash owns timer 1. Explicitly
    // reserve only timers 2 and 3 for ESP32Servo before the camera starts.
    ESP32PWM::allocateTimer(2);
    ESP32PWM::allocateTimer(3);
    s_pan_servo.setPeriodHertz(50);
    s_tilt_servo.setPeriodHertz(50);

    // No servo is attached at boot; the first PTZ command attaches each axis.
    xTaskCreatePinnedToCore(gimbalTask, "gimbal", 3072, NULL, 2, NULL, 1);
    Serial.printf("[Gimbal] 360°连续舵机已就绪(仅动作时输出): 左右=GPIO%d 上下=GPIO%d, 速度=%d%%\n",
                  GIMBAL_PIN_PAN, GIMBAL_PIN_TILT, GIMBAL_SPEED);
}

void move(const char *dir, bool pressed)
{
    uint8_t bit = 0;
    if      (!strcmp(dir, "up"))    bit = DIR_UP;
    else if (!strcmp(dir, "down"))  bit = DIR_DOWN;
    else if (!strcmp(dir, "left"))  bit = DIR_LEFT;
    else if (!strcmp(dir, "right")) bit = DIR_RIGHT;
    else return;

    if (pressed) {
        s_flags |= bit;
    } else {
        s_flags &= (uint8_t)~bit;
    }
}

} // namespace gimbal
