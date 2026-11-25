/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 * SPDX-FileCopyrightText: Impo contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * What M5Stack's hal.cpp and hal_bridge.cc defined for the code in
 * vendor/m5stack. Hal::init starts the body's parts as M5Stack's Hal::init
 * does, in its order (head touch, IO expander, servos); NVS is already up,
 * and the CoreS3's own parts (display, audio) are devices/m5stack-cores3's.
 * delay() and millis() are M5Stack's.
 */
#include "hal.h"
#include "hal_impo.h"
#include "board/hal_bridge.h"
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static i2c_master_bus_handle_t s_bus;

void hal_impo_set_i2c_bus(i2c_master_bus_handle_t bus)
{
    s_bus = bus;
}

i2c_master_bus_handle_t hal_bridge::board_get_i2c_bus()
{
    return s_bus;
}

Hal& GetHAL()
{
    static Hal hal;
    return hal;
}

void Hal::init()
{
    head_touch_init();
    io_expander_init();
    servo_init();
}

void Hal::delay(std::uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

std::uint32_t Hal::millis()
{
    return esp_timer_get_time() / 1000;
}
