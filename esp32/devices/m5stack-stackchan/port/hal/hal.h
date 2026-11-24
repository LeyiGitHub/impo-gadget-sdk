/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 * SPDX-FileCopyrightText: Impo contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * M5Stack's Hal (StackChan firmware/main/hal/hal.h), cut down to what the code
 * in vendor/m5stack uses: the declarations below are M5's, word for word. The
 * rest of M5's Hal (xiaozhi, BLE, apps, OTA, the LVGL boot logo) is not part
 * of this build. port/hal_impo.cpp defines what M5's hal.cpp defined.
 */
#pragma once
#include <memory>
#include <cstdint>
#include <string>
#include <lvgl.h>
#include <functional>
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include <smooth_lvgl.hpp>
#include <array>
#include <string_view>
/* M5Stack's hal.h brought these in through its xiaozhi headers; the HAL
 * files use vTaskDelay without including them. */
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

enum class HeadPetGesture { None, Press, Release, SwipeForward, SwipeBackward };

enum class ImuMotionEvent {
    None = 0,
    Shake,
    PickUp,
};

class Hal {
public:
    void init();

    /* --------------------------------- System --------------------------------- */
    void delay(std::uint32_t ms);
    std::uint32_t millis();

    /* --------------------------------- HeadPet -------------------------------- */
    uitk::Signal<HeadPetGesture> onHeadPetGesture;

    /* ----------------------------------- RGB ---------------------------------- */
    void setRgbColor(uint8_t index, uint8_t r, uint8_t g, uint8_t b);
    void showRgbColor(uint8_t r, uint8_t g, uint8_t b);
    void refreshRgb();

    /* ---------------------------------- Servo --------------------------------- */
    void setServoPowerEnabled(bool enabled);
    /* Brings back servos that stopped answering (hal_impo.cpp). */
    void reviveServos();
    bool servoPowerLatched();

    /* ----------------------------------- IMU ---------------------------------- */
    uitk::Signal<ImuMotionEvent> onImuMotionEvent;

private:
    void servo_init();
    void head_touch_init();
    void io_expander_init();
};

Hal& GetHAL();
