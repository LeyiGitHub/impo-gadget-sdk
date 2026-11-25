/*
 * Copyright (c) Impo contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The StackChan's commands. Each is a tool M5Stack's StackChan firmware v1.6.2
 * gives its AI, done the way it does it: firmware/main/hal/hal_mcp.cpp for the
 * robot's own, and xiaozhi-esp32 v2.5.2's main/mcp_server.cc, which that
 * firmware is built on, for the speaker and the screen.
 *
 *   head.status             self.robot.get_head_angles
 *   head.move               self.robot.set_head_angles
 *   lights.set              self.robot.set_led_color
 *   speaker.set_volume      self.audio_speaker.set_volume
 *   display.set_brightness  self.screen.set_brightness
 *
 * A command M5Stack's firmware has no tool for is not here, and nothing here
 * is Impo's own. description.json describes each in M5Stack's words.
 *
 * M5Stack's robot object, GetStackChan(), owns the head and the LEDs. As in
 * M5Stack's firmware a task updates it every 20 ms, and everything that
 * touches it holds one lock (M5Stack's LVGL lock; Impo's face is not drawn by
 * this code, so here a lock of its own).
 */
#include "m5stack_cores3.h"

#include <mutex>

#include <stackchan/stackchan.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal.h"
#include "hal_impo.h"
#include "impo_commands.h"
#include "impo_state.h"
extern "C" {   /* C headers without C++ guards */
#include "impo_audio.h"
#include "impo_settings.h"
}

using namespace stackchan;

namespace {

constexpr const char *TAG = "stackchan";

std::recursive_mutex s_lock;   /* M5Stack's LvglLockGuard */

/* M5Stack's _stackchan_update_task (hal.cpp), less its reminders and status
 * bar, which are drawn on its own face. While xiaozhi is not idle (listening,
 * speaking), it waits 100 ms more each pass. */
void update_task(void *)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(20));
        std::lock_guard<std::recursive_mutex> lock(s_lock);
        if (impo_state_mode(nullptr) != IMPO_MODE_IDLE) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        GetStackChan().update();
    }
}

/* An integer parameter as xiaozhi's PropertyList takes it: `dflt` when it is
 * left out (none when `required`), and refused outside lo..hi. */
bool integer(const cJSON *params, const char *name, bool required, int dflt, int lo, int hi, int *out)
{
    const cJSON *v = cJSON_GetObjectItem(params, name);
    if (!v) {
        *out = dflt;
        return !required;
    }
    if (!cJSON_IsNumber(v) || v->valueint < lo || v->valueint > hi) {
        return false;
    }
    *out = v->valueint;
    return true;
}

constexpr int NOT_GIVEN = -9999;   /* M5Stack's default for an angle left out */

/* self.robot.get_head_angles */
cJSON *head_status(const cJSON *params)
{
    (void)params;
    std::lock_guard<std::recursive_mutex> lock(s_lock);
    auto &motion = GetStackChan().motion();
    int current_yaw = motion.yawServo().getCurrentAngle() / 10;
    int current_pitch = motion.pitchServo().getCurrentAngle() / 10;
    cJSON *result = impo_command_ok();
    cJSON_AddNumberToObject(result, "yaw", current_yaw);
    cJSON_AddNumberToObject(result, "pitch", current_pitch);
    return result;
}

/* self.robot.set_head_angles */
cJSON *head_move(const cJSON *params)
{
    int yaw, pitch, speed;
    if (!integer(params, "yaw", false, NOT_GIVEN, NOT_GIVEN, 128, &yaw)
        || !integer(params, "pitch", false, NOT_GIVEN, NOT_GIVEN, 90, &pitch)
        || !integer(params, "speed", false, 150, 100, 1000, &speed)) {
        return impo_command_error("invalid_params", "yaw -128..128, pitch 0..90, speed 100..1000");
    }
    ESP_LOGI(TAG, "motion set_angles: yaw: %d, pitch: %d, speed: %d", yaw, pitch, speed);
    std::lock_guard<std::recursive_mutex> lock(s_lock);
    auto &motion = GetStackChan().motion();
    if (pitch != NOT_GIVEN) {
        motion.pitchServo().moveWithSpeed(pitch * 10, speed);
    }
    if (yaw != NOT_GIVEN) {
        motion.yawServo().moveWithSpeed(yaw * 10, speed);
    }
    return impo_command_ok();
}

/* self.robot.set_led_color */
cJSON *lights_set(const cJSON *params)
{
    int r, g, b;
    if (!integer(params, "red", false, 0, 0, 168, &r) || !integer(params, "green", false, 0, 0, 168, &g)
        || !integer(params, "blue", false, 0, 0, 168, &b)) {
        return impo_command_error("invalid_params", "red, green, blue: 0..168");
    }
    ESP_LOGI(TAG, "set_led_color: r=%d, g=%d, b=%d", r, g, b);
    std::lock_guard<std::recursive_mutex> lock(s_lock);
    GetStackChan().leftNeonLight().setColor(r, g, b);
    GetStackChan().rightNeonLight().setColor(r, g, b);
    return impo_command_ok();
}

/* self.audio_speaker.set_volume: xiaozhi's codec->SetOutputVolume, which it
 * keeps in its settings; Impo keeps the volume in its own. */
cJSON *set_volume(const cJSON *params)
{
    int volume;
    if (!integer(params, "volume", true, 0, 0, 100, &volume)) {
        return impo_command_error("invalid_params", "volume: 0..100");
    }
    impo_settings_set_volume(volume);
    impo_audio_set_volume(impo_settings_volume());
    return impo_command_ok();
}

/* self.screen.set_brightness: xiaozhi's backlight->SetBrightness(brightness,
 * true), which keeps it in its settings; Impo keeps it in its own, and its
 * screen goes no darker than 10%. */
cJSON *set_brightness(const cJSON *params)
{
    int brightness;
    if (!integer(params, "brightness", true, 0, 0, 100, &brightness)) {
        return impo_command_error("invalid_params", "brightness: 0..100");
    }
    impo_settings_set_brightness(brightness);
    return impo_command_ok();
}

esp_err_t start(i2c_master_bus_handle_t bus)
{
    hal_impo_set_i2c_bus(bus);
    /* M5Stack's start: the touch strip's task, the PY32 (servo power, LEDs)
     * and the servos, whose motion it attaches to GetStackChan(). */
    GetHAL().init();
    /* What M5Stack's Hal::startXiaozhi sets before its AI starts. */
    auto &motion = GetStackChan().motion();
    motion.setAutoAngleSyncEnabled(true);
    motion.setAutoTorqueReleaseEnabled(true);
    BaseType_t ok = xTaskCreatePinnedToCore(update_task, "stackchan", 4096, nullptr, 3, nullptr, 1);
    ESP_LOGI(TAG, "body started");
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

const impo_command_t s_commands[] = {
    { "head.status", head_status },
    { "head.move", head_move },
    { "lights.set", lights_set },
    { "speaker.set_volume", set_volume },
    { "display.set_brightness", set_brightness },
};

}  // namespace

// What the StackChan is and what every command of it does (description.json,
// embedded as written by CMakeLists.txt).
extern const char description_json[] asm("_binary_description_json_start");

const m5stack_cores3_body_t m5stack_cores3_body = {
    "M5Stack StackChan",
    start,
    s_commands,
    sizeof(s_commands) / sizeof(s_commands[0]),
    nullptr,
    0,
    "head,lights",
    description_json,
    /* Of the commands outside these, the ones M5Stack's firmware also has: its
     * AI's speech (speaker.play), its caption (display.show_text) and its
     * camera (camera.capture). */
    "speaker.play,display.show_text,camera.capture",
};
