/*
 * Copyright (c) Impo contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Impo on top of M5Stack's StackChan code (vendor/m5stack): the body's
 * commands (CAPABILITIES.md, head.* and lights.*), its event and the reactions
 * M5Stack's firmware has that need no agent.
 *
 * M5Stack's robot object, GetStackChan(), owns the head (two ScsServo with
 * spring motion, stall protection on the pitch servo, torque let go at rest)
 * and the LEDs (two NeonLight halves that fade). As in M5Stack's firmware, a
 * task updates it every 20 ms and everything that touches it holds one lock;
 * M5Stack uses the LVGL lock, since its face is drawn in LVGL. Impo keeps its
 * own face on the screen: the robot gets a face without pixels (ImpoFace), so
 * M5Stack's keyframes and dances, which move eyes and a mouth as well as the
 * head, run unchanged and only the head and LEDs show them.
 *
 * Touch and shake reactions follow M5Stack's modifiers (head_pet.h, imu.h)
 * for the head and LEDs; their on-screen hearts and dizzy eyes become Impo's
 * happy face.
 */
#include "m5stack_cores3.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <strings.h>

#include <stackchan/stackchan.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal.h"
#include "hal_impo.h"
#include "impo_commands.h"
#include "impo_link.h"
#include "impo_state.h"
#include "m5stack_cores3_imu.h"

using namespace stackchan;

namespace {

constexpr const char *TAG = "stackchan";
/* M5Stack's animations step a fixed 20 ms per update and skip an update that
 * comes less than 20 ms after the last, so the loop runs every 21 ms counted
 * from the start of each pass: the servo bus takes several ms a pass, and a
 * longer period slows and shrinks every move. */
constexpr int64_t PERIOD_US = 21000;
/* Head limits in degrees; M5Stack's servos allow yaw -128..128, pitch 3..87. */
constexpr double YAW_MAX = 120, PITCH_MIN = 0, PITCH_MAX = 85;
constexpr int SPEED_DEFAULT = 150;          /* M5Stack: "150 is natural" */
constexpr int FRAME_SPEED_DEFAULT = 700;    /* a frame of head.play is quick */
constexpr int SPEED_MIN = 100, SPEED_MAX = 1000;
constexpr int LED_MAX = 168;                /* M5Stack's safe LED level */
constexpr int BRIGHTNESS_DEFAULT = 40;
constexpr uint32_t PINK = 0xFF3070;
constexpr uint32_t PAT_MIN_MS = 100;        /* shorter touches are noise */
constexpr uint32_t EVENT_GAP_MS = 5000;
constexpr int LOCK_WAIT_MS = 200;
/* head.move waits this long for the head to settle; head.play's limits. */
constexpr uint32_t SETTLE_MS = 4000;
constexpr int REACHED_TENTHS = 50;          /* within 5 degrees of where it was sent */
constexpr int FRAMES_MAX = 40;
constexpr uint32_t FRAME_MS_MIN = 80, FRAME_MS_MAX = 3000, PLAY_MS_MAX = 20000;

/* A face with nothing to draw, for M5Stack code that moves eyes and a mouth. */
class ImpoFace : public avatar::Avatar {
public:
    ImpoFace()
    {
        _key_elements.leftEye = std::make_unique<avatar::Feature>();
        _key_elements.rightEye = std::make_unique<avatar::Feature>();
        _key_elements.mouth = std::make_unique<avatar::Feature>();
    }
};

std::recursive_timed_mutex s_lock;
QueueHandle_t s_touch;

/* Lights: what lights.set asked for, at the LED level. */
uint32_t s_rgb;

/* Touch, as HeadPetModifier tracks it. */
bool s_touch_valid, s_stroked;
uint32_t s_press_ms, s_last_event_ms;
bool s_event_sent;

uint32_t now_ms()
{
    return GetHAL().millis();
}

/* 0xRRGGBB at brightness 1..100 into LED levels, full brightness at LED_MAX. */
uint32_t led_level(uint32_t rgb, int brightness)
{
    auto ch = [&](int shift) {
        return (uint32_t)((((rgb >> shift) & 0xFF) * brightness * LED_MAX + 50 * 255) / (100 * 255));
    };
    return ch(16) << 16 | ch(8) << 8 | ch(0);
}

void show(uint32_t rgb)
{
    GetStackChan().leftNeonLight().setColor(rgb);
    GetStackChan().rightNeonLight().setColor(rgb);
}

bool colour_parse(const char *text, uint32_t *rgb)
{
    static const struct {
        const char *name;
        uint32_t rgb;
    } names[] = {
        { "red", 0xFF0000 }, { "orange", 0xFF6000 }, { "yellow", 0xFFC000 }, { "green", 0x00FF00 },
        { "cyan", 0x00FFFF }, { "blue", 0x0000FF }, { "purple", 0x8000FF }, { "pink", PINK },
        { "white", 0xFFFFFF }, { "warm", 0xFF9040 }, { "off", 0x000000 },
    };
    if (!text) {
        return false;
    }
    for (const auto &n : names) {
        if (!strcasecmp(text, n.name)) {
            *rgb = n.rgb;
            return true;
        }
    }
    const char *h = text[0] == '#' ? text + 1 : text;
    if (strlen(h) != 6) {
        return false;
    }
    char *end;
    unsigned long v = strtoul(h, &end, 16);
    if (*end) {
        return false;
    }
    *rgb = (uint32_t)v;
    return true;
}

void on_touch(HeadPetGesture g, uint32_t now)
{
    auto &stackchan = GetStackChan();
    switch (g) {
    case HeadPetGesture::Press:
        /* The strip reads the servos' noise while they move. */
        s_touch_valid = !stackchan.motion().isMoving();
        s_stroked = false;
        s_press_ms = now;
        break;
    case HeadPetGesture::SwipeForward:
    case HeadPetGesture::SwipeBackward:
        s_stroked = s_touch_valid;
        break;
    case HeadPetGesture::Release:
        /* Only the event: how the robot answers a touch is the agent's. */
        if (!s_touch_valid || (!s_stroked && now - s_press_ms < PAT_MIN_MS)) {
            break;
        }
        if (!s_event_sent || now - s_last_event_ms >= EVENT_GAP_MS) {
            cJSON *data = cJSON_CreateObject();
            cJSON_AddStringToObject(data, "how", s_stroked ? "stroke" : "pat");
            if (impo_link_send_event("head_touched", data)) {
                s_event_sent = true;
                s_last_event_ms = now;
            }
        }
        ESP_LOGI(TAG, "head %s", s_stroked ? "stroked" : "patted");
        break;
    default:
        break;
    }
}

void update_task(void *)
{
    int64_t next = esp_timer_get_time();
    while (true) {
        next += PERIOD_US;
        int64_t wait_us = next - esp_timer_get_time();
        if (wait_us < 1000) {
            /* A pass overran (a servo did not answer): start the next one
             * from now rather than catch up with passes in a row. */
            next = esp_timer_get_time() + 1000;
            wait_us = 1000;
        }
        vTaskDelay(pdMS_TO_TICKS((wait_us + 999) / 1000));
        std::lock_guard<std::recursive_timed_mutex> lock(s_lock);
        auto &stackchan = GetStackChan();
        uint32_t now = now_ms();
        HeadPetGesture g;
        while (xQueueReceive(s_touch, &g, 0) == pdTRUE) {
            on_touch(g, now);
        }
        stackchan.update();
    }
}

/* ---- Commands ---- */

class Locked {
public:
    Locked() : _ok(s_lock.try_lock_for(std::chrono::milliseconds(LOCK_WAIT_MS)))
    {
    }
    ~Locked()
    {
        if (_ok) {
            s_lock.unlock();
        }
    }
    explicit operator bool() const
    {
        return _ok;
    }

private:
    bool _ok;
};

cJSON *busy()
{
    return impo_command_error("busy", "the head is busy; try again");
}

bool number(const cJSON *params, const char *name, double lo, double hi, double *out, bool *present)
{
    const cJSON *v = cJSON_GetObjectItem(params, name);
    *present = v != nullptr;
    if (!v) {
        return true;
    }
    if (!cJSON_IsNumber(v) || v->valuedouble < lo || v->valuedouble > hi) {
        return false;
    }
    *out = v->valuedouble;
    return true;
}

int to_tenths(double degrees)
{
    return (int)std::lround(degrees * 10);
}

/* Where the head points, read from the servos themselves (with s_lock held).
 * False when either did not answer: the servo code then gives its own
 * animation's value, which says nothing about the head. */
bool read_head(int *yaw, int *pitch)
{
    unsigned before = hal_impo_servo_short_reads();
    auto &motion = GetStackChan().motion();
    *yaw = motion.yawServo().getCurrentAngle();
    *pitch = motion.pitchServo().getCurrentAngle();
    return hal_impo_servo_short_reads() == before;
}

/* A head at rest: the bus drops replies now and then even so, so a reading
 * is tried every 40 ms for up to `ms` (with s_lock held). */
bool read_resting(int *yaw, int *pitch, uint32_t ms)
{
    for (uint32_t tried = 0;; tried += 40) {
        if (read_head(yaw, pitch)) {
            return true;
        }
        if (tried >= ms) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

/* A servo that stopped answering (after a stall, or a dip in its power)
 * answers again only once its power has gone off and on, as at start-up
 * (hal_impo.cpp). Hardware upkeep, not behaviour; true when they answer. */
bool revive(int *yaw, int *pitch)
{
    std::lock_guard<std::recursive_timed_mutex> lock(s_lock);
    ESP_LOGW(TAG, "the servos stopped answering (PY32 power latch %s): power off and on",
             GetHAL().servoPowerLatched() ? "on" : "off: the PY32 is set up again");
    GetHAL().reviveServos();
    show(s_rgb);   /* the lights as they were: a PY32 set up again starts dark */
    bool read = read_resting(yaw, pitch, 1500);
    ESP_LOGI(TAG, "servos %s after the power cycle", read ? "answer" : "still silent");
    return read;
}

void add_angles(cJSON *to, int yaw, int pitch)
{
    cJSON_AddNumberToObject(to, "yaw", yaw / 10.0);
    cJSON_AddNumberToObject(to, "pitch", pitch / 10.0);
}

/*
 * head.move and head.play: moves the agent composed, answered once they are
 * done with where the servos say the head went. One at a time: a new one, or
 * head.stop, ends the one under way, which answers "stopped".
 */
struct frame_t {
    int yaw, pitch;            /* tenths of a degree */
    bool has_yaw, has_pitch;
    int speed;
    uint32_t ms;               /* head.play: how long the frame lasts */
};

struct head_job_t {
    impo_reply_t *reply;
    bool play;
    int count;
    frame_t frames[FRAMES_MAX];
    unsigned id;
};

std::atomic<unsigned> s_head_job{0};   /* the job that owns the head */
SemaphoreHandle_t s_head_busy;

bool current(const head_job_t *job)
{
    return s_head_job.load() == job->id;
}

/* Sends the head towards a frame's angles. */
void aim(const frame_t &f)
{
    std::lock_guard<std::recursive_timed_mutex> lock(s_lock);
    auto &motion = GetStackChan().motion();
    if (f.has_pitch) {
        motion.pitchServo().moveWithSpeed(f.pitch, f.speed);
    }
    if (f.has_yaw) {
        motion.yawServo().moveWithSpeed(f.yaw, f.speed);
    }
}

/* Waits up to ms, false once another job took the head. */
bool wait(const head_job_t *job, uint32_t ms)
{
    for (uint32_t waited = 0; waited < ms; waited += 20) {
        if (!current(job)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return current(job);
}

cJSON *stopped()
{
    return impo_command_error("stopped", "another head command or head.stop ended it");
}

bool at_target(bool has_yaw, int to_yaw, bool has_pitch, int to_pitch, int yaw, int pitch)
{
    return (!has_yaw || std::abs(yaw - to_yaw) <= REACHED_TENTHS)
           && (!has_pitch || std::abs(pitch - to_pitch) <= REACHED_TENTHS);
}

/* Until the spring has arrived and the servos say they stopped; false once another job took the head. */
bool settle(const head_job_t *job)
{
    for (uint32_t waited = 0; waited < SETTLE_MS; waited += 50) {
        if (!wait(job, 50)) {
            return false;
        }
        std::lock_guard<std::recursive_timed_mutex> lock(s_lock);
        if (!GetStackChan().motion().isMoving()) {
            break;
        }
    }
    return true;
}

/* Where the head rests, reviving the servos once if they do not answer. */
bool rest_reading(int *yaw, int *pitch, bool *revived)
{
    {
        std::lock_guard<std::recursive_timed_mutex> lock(s_lock);
        if (read_resting(yaw, pitch, 400)) {
            return true;
        }
    }
    *revived = true;
    return revive(yaw, pitch);
}

cJSON *run_move(head_job_t *job)
{
    const frame_t &f = job->frames[0];
    int yaw, pitch;
    bool revived = false;
    /* Servos that went silent take no move: bring them back first. */
    if (!rest_reading(&yaw, &pitch, &revived)) {
        return impo_command_error("servos_silent", "the servos do not answer, even after a power cycle");
    }
    aim(f);
    if (!settle(job)) {
        return stopped();
    }
    if (!rest_reading(&yaw, &pitch, &revived)) {
        return impo_command_error("servos_silent", "the servos stopped answering during the move");
    }
    bool reached = at_target(f.has_yaw, f.yaw, f.has_pitch, f.pitch, yaw, pitch);
    if (!reached) {
        /* A quick move on both axes can dip the servos' power, which resets
         * them where they are (or silences them until revived): once more. */
        aim(f);
        if (!settle(job)) {
            return stopped();
        }
        if (!rest_reading(&yaw, &pitch, &revived)) {
            return impo_command_error("servos_silent", "the servos stopped answering during the move");
        }
        reached = at_target(f.has_yaw, f.yaw, f.has_pitch, f.pitch, yaw, pitch);
    }
    cJSON *result = reached ? impo_command_ok()
                            : impo_command_error("not_reached", "the head stopped short of where it was sent");
    add_angles(result, yaw, pitch);
    if (revived) {
        cJSON_AddBoolToObject(result, "servos_revived", true);
    }
    return result;
}

cJSON *run_play(head_job_t *job)
{
    int yaw, pitch;
    bool revived = false;
    if (!rest_reading(&yaw, &pitch, &revived)) {
        return impo_command_error("servos_silent", "the servos do not answer, even after a power cycle");
    }
    int64_t started = esp_timer_get_time();
    cJSON *reached = cJSON_CreateArray();
    for (int i = 0; i < job->count; i++) {
        aim(job->frames[i]);
        if (!wait(job, job->frames[i].ms)) {
            cJSON_Delete(reached);
            return stopped();
        }
        /* Where the frame got to, as it ends; null when a servo did not answer (they often don't while moving). */
        bool read;
        {
            std::lock_guard<std::recursive_timed_mutex> lock(s_lock);
            read = read_head(&yaw, &pitch);
        }
        cJSON *at = read ? cJSON_CreateObject() : cJSON_CreateNull();
        if (read) {
            add_angles(at, yaw, pitch);
        }
        cJSON_AddItemToArray(reached, at);
    }
    int64_t took = (esp_timer_get_time() - started) / 1000;
    /* And where it came to rest. */
    if (!settle(job)) {
        cJSON_Delete(reached);
        return stopped();
    }
    bool rested = rest_reading(&yaw, &pitch, &revived);
    /* Where the frames left each axis: the last frame that named it. */
    bool has_yaw = false, has_pitch = false;
    int to_yaw = 0, to_pitch = 0;
    for (int i = 0; i < job->count; i++) {
        const frame_t &f = job->frames[i];
        if (f.has_yaw) {
            has_yaw = true;
            to_yaw = f.yaw;
        }
        if (f.has_pitch) {
            has_pitch = true;
            to_pitch = f.pitch;
        }
    }
    cJSON *result = !rested ? impo_command_error("servos_silent", "the servos stopped answering during the frames")
                    : !at_target(has_yaw, to_yaw, has_pitch, to_pitch, yaw, pitch)
                        ? impo_command_error("not_reached", "the head did not end where the last frames sent it")
                        : impo_command_ok();
    cJSON_AddNumberToObject(result, "duration_ms", (double)took);
    cJSON_AddItemToObject(result, "reached", reached);
    if (rested) {
        cJSON *rest = cJSON_AddObjectToObject(result, "rest");
        add_angles(rest, yaw, pitch);
    }
    if (revived) {
        cJSON_AddBoolToObject(result, "servos_revived", true);
    }
    return result;
}

void head_task(void *arg)
{
    auto *job = static_cast<head_job_t *>(arg);
    cJSON *result;
    if (xSemaphoreTake(s_head_busy, pdMS_TO_TICKS(3000)) != pdTRUE) {
        result = impo_command_error("busy", "the head command before this one did not stop");
    } else {
        result = current(job) ? (job->play ? run_play(job) : run_move(job)) : stopped();
        xSemaphoreGive(s_head_busy);
    }
    impo_command_reply(job->reply, result);
    heap_caps_free(job);
    vTaskDelete(nullptr);
}

cJSON *start_head(head_job_t *job, impo_reply_t *reply)
{
    job->reply = reply;
    job->id = ++s_head_job;   /* the job under way now ends */
    if (xTaskCreatePinnedToCore(head_task, "head", 4096, job, 3, nullptr, 1) != pdPASS) {
        heap_caps_free(job);
        return impo_command_error("out_of_memory", "no memory for the move");
    }
    return impo_command_later();
}

head_job_t *new_job()
{
    return static_cast<head_job_t *>(heap_caps_calloc(1, sizeof(head_job_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

/* A frame's yaw, pitch and speed from `o`; false when out of range or neither angle is given. */
bool frame(const cJSON *o, int speed_default, frame_t *f)
{
    double yaw = 0, pitch = 0, speed = speed_default;
    bool has_speed;
    if (!cJSON_IsObject(o) || !number(o, "yaw", -YAW_MAX, YAW_MAX, &yaw, &f->has_yaw)
        || !number(o, "pitch", PITCH_MIN, PITCH_MAX, &pitch, &f->has_pitch)
        || !number(o, "speed", SPEED_MIN, SPEED_MAX, &speed, &has_speed) || (!f->has_yaw && !f->has_pitch)) {
        return false;
    }
    f->yaw = to_tenths(yaw);
    f->pitch = to_tenths(pitch);
    f->speed = (int)speed;
    return true;
}

cJSON *head_move(const cJSON *params, impo_reply_t *reply)
{
    head_job_t *job = new_job();
    if (!job) {
        return impo_command_error("out_of_memory", "no memory for the move");
    }
    if (!frame(params, SPEED_DEFAULT, &job->frames[0])) {
        heap_caps_free(job);
        return impo_command_error("invalid_params", "yaw -120..120 and/or pitch 0..85 degrees, speed 100..1000");
    }
    job->count = 1;
    return start_head(job, reply);
}

cJSON *head_play(const cJSON *params, impo_reply_t *reply)
{
    const cJSON *frames = cJSON_GetObjectItem(params, "frames");
    int count = cJSON_IsArray(frames) ? cJSON_GetArraySize(frames) : 0;
    if (count < 1 || count > FRAMES_MAX) {
        return impo_command_error("invalid_params", "frames: 1 to 40 of {yaw?, pitch?, ms, speed?}");
    }
    head_job_t *job = new_job();
    if (!job) {
        return impo_command_error("out_of_memory", "no memory for the moves");
    }
    uint32_t total = 0;
    for (int i = 0; i < count; i++) {
        const cJSON *o = cJSON_GetArrayItem(frames, i);
        const cJSON *ms = cJSON_GetObjectItem(o, "ms");
        if (!frame(o, FRAME_SPEED_DEFAULT, &job->frames[i]) || !cJSON_IsNumber(ms) || ms->valuedouble < FRAME_MS_MIN
            || ms->valuedouble > FRAME_MS_MAX) {
            heap_caps_free(job);
            return impo_command_error("invalid_params", "each frame: yaw -120..120 and/or pitch 0..85, ms 80..3000, speed 100..1000");
        }
        job->frames[i].ms = (uint32_t)ms->valuedouble;
        total += job->frames[i].ms;
    }
    if (total > PLAY_MS_MAX) {
        heap_caps_free(job);
        return impo_command_error("invalid_params", "the frames last at most 20 seconds in all");
    }
    job->count = count;
    job->play = true;
    return start_head(job, reply);
}

cJSON *head_stop(const cJSON *params)
{
    (void)params;
    ++s_head_job;   /* the job under way answers "stopped" */
    Locked lock;
    if (!lock) {
        return busy();
    }
    GetStackChan().motion().stop();
    return impo_command_ok();
}

cJSON *head_status(const cJSON *params)
{
    (void)params;
    Locked lock;
    if (!lock) {
        return busy();
    }
    auto &motion = GetStackChan().motion();
    int yaw, pitch;
    bool revived = false;
    bool read = read_resting(&yaw, &pitch, motion.isMoving() ? 0 : 400);
    if (!read && s_head_busy && xSemaphoreTake(s_head_busy, 0) == pdTRUE) {
        /* No move under way, so silent servos are not just busy. */
        revived = true;
        read = revive(&yaw, &pitch);
        xSemaphoreGive(s_head_busy);
    }
    cJSON *result = read ? impo_command_ok() : impo_command_error("servos_silent", "the servos do not answer");
    if (revived) {
        cJSON_AddBoolToObject(result, "servos_revived", true);
    }
    if (read) {
        add_angles(result, yaw, pitch);
    }
    bool moving = motion.isMoving();
    cJSON_AddBoolToObject(result, "moving", moving);
    /* A moving head is held; at rest, the servos' torque registers say. */
    cJSON_AddBoolToObject(result, "holding",
                          moving || motion.yawServo().getTorqueEnabled() || motion.pitchServo().getTorqueEnabled());
    return result;
}

cJSON *lights_set(const cJSON *params)
{
    const cJSON *color = cJSON_GetObjectItem(params, "color");
    double brightness = BRIGHTNESS_DEFAULT;
    bool has_brightness;
    uint32_t rgb;
    if (!cJSON_IsString(color) || !colour_parse(color->valuestring, &rgb) ||
        !number(params, "brightness", 1, 100, &brightness, &has_brightness)) {
        return impo_command_error("invalid_params", "color: a name or #rrggbb; brightness 1..100");
    }
    Locked lock;
    if (!lock) {
        return busy();
    }
    s_rgb = led_level(rgb, (int)brightness);
    show(s_rgb);
    return impo_command_ok();
}

cJSON *lights_off(const cJSON *params)
{
    (void)params;
    Locked lock;
    if (!lock) {
        return busy();
    }
    s_rgb = 0;
    show(0);
    return impo_command_ok();
}

esp_err_t start(i2c_master_bus_handle_t bus)
{
    hal_impo_set_i2c_bus(bus);
    s_touch = xQueueCreate(8, sizeof(HeadPetGesture));
    if (!s_touch) {
        return ESP_ERR_NO_MEM;
    }
    GetHAL().onHeadPetGesture.connect([](HeadPetGesture g) { xQueueSend(s_touch, &g, 0); });
    GetStackChan().attachAvatar(std::make_unique<ImpoFace>());
    /* M5Stack's start: the touch strip's task, the PY32 (servo power, LEDs)
     * and the servos, whose motion it attaches to GetStackChan(). */
    GetHAL().init();
    s_head_busy = xSemaphoreCreateMutex();
    BaseType_t ok = xTaskCreatePinnedToCore(update_task, "stackchan", 4096, nullptr, 3, nullptr, 1);
    ESP_LOGI(TAG, "body started");
    return ok == pdPASS && s_head_busy ? ESP_OK : ESP_ERR_NO_MEM;
}

const impo_command_t s_commands[] = {
    { "head.stop", head_stop },
    { "head.status", head_status },
    { "lights.set", lights_set },
    { "lights.off", lights_off },
};

/* Answered once the head is done: 4 s to settle, or the frames' 20 s. */
const impo_later_command_t s_later_commands[] = {
    { "head.move", head_move, 0 },
    { "head.play", head_play, 30000 },
};

}  // namespace

const m5stack_cores3_body_t m5stack_cores3_body = {
    "M5Stack StackChan",
    start,
    s_commands,
    sizeof(s_commands) / sizeof(s_commands[0]),
    s_later_commands,
    sizeof(s_later_commands) / sizeof(s_later_commands[0]),
    "head,head_touch,lights",
};
