/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 * SPDX-FileCopyrightText: Impo contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * What M5Stack's hal.cpp and hal_bridge.cc defined for the code in
 * vendor/m5stack. Hal::init starts the body's parts in the order M5Stack's
 * Hal::init does (head touch, IO expander, servos); NVS is already up, and
 * the CoreS3's own parts (display, audio, motion sensor) are
 * devices/m5stack-cores3's. delay() and millis() are M5Stack's.
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
    /* M5Stack's firmware starts from a cold boot, with the servos newly
     * powered. A reset of the ESP32 alone (flashing, a crash, a restart)
     * leaves the PY32 and the servo power on, and a servo that stopped
     * answering (after a stall) answers again only once its power has gone
     * off and on. So start as from a cold boot: io_expander_init has switched
     * the power on; switch it off and on again before the servos start. */
    setServoPowerEnabled(false);
    delay(500);
    setServoPowerEnabled(true);
    delay(500);     /* the servos answer about half a second after power-on */
    servo_init();
}

/* During pitch moves both servos sometimes stop answering at once, while the
 * PY32 still answers and its latch still holds their power on (seen on a
 * StackChan on USB power, 2026-11-24). Only a power cycle brings them back.
 * If the PY32 itself forgot its setup (latch reading off), set it up first. */
void Hal::reviveServos()
{
    if (!servoPowerLatched()) {
        io_expander_init();
    }
    setServoPowerEnabled(false);
    delay(500);
    setServoPowerEnabled(true);
    delay(600);     /* the servos answer about half a second after power-on */
}

void Hal::delay(std::uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

std::uint32_t Hal::millis()
{
    return esp_timer_get_time() / 1000;
}
