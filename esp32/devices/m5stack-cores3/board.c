/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * M5Stack CoreS3 (K128). Pin map, power rails, LCD, touch and codecs come from
 * Espressif's BSP: https://github.com/espressif/esp-bsp/tree/master/bsp/m5stack_core_s3
 * 320x240 ILI9342C LCD, FT6336U touch, AW88298 speaker, ES7210 dual mic.
 * The AXP2101 PMU powers the LCD backlight, codecs and camera through its LDOs
 * (the BSP switches them on as each part starts) and reports the battery.
 * There is no user button besides PWR, which only the PMU sees: it is talk,
 * read from the PMU's key IRQ latches. The touchscreen runs the menus.
 *
 * The GC0308 camera is camera.c's. A body the CoreS3 sits in (the M5Stack
 * StackChan, devices/m5stack-stackchan) plugs in through m5stack_cores3.h:
 * this file powers it, starts it and adds its commands.
 */
#include <stdio.h>

#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "impo_board.h"
#include "impo_audio.h"
#include "impo_mem.h"
#include "impo_pmu.h"
#include "m5stack_cores3.h"
#include "m5stack_cores3_camera.h"
#include "m5stack_cores3_imu.h"

#define PMU_KEY_EVERY 2         /* poll the PMU over I2C every 20 ms */

static const char *TAG = "board";
static esp_codec_dev_handle_t s_spk, s_mic;
static bool s_pmu;

#if CONFIG_M5STACK_CORES3_BODY
/* The body takes 5 V from the CoreS3's bus: its output switch (AW9523 P0_1)
 * and the boost converter (P1_7), which M5Stack's StackChan firmware turns
 * on and Espressif's BSP leaves alone. */
static void body_start(void)
{
    esp_io_expander_handle_t aw = bsp_io_expander_init();
    const uint32_t pins = IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_15;
    if (!aw || esp_io_expander_set_dir(aw, pins, IO_EXPANDER_OUTPUT) != ESP_OK ||
        esp_io_expander_set_level(aw, pins, 1) != ESP_OK) {
        ESP_LOGW(TAG, "could not switch on the 5 V bus output");
    }
    esp_err_t err = m5stack_cores3_body.start(bsp_i2c_get_handle());
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s body unavailable (%s)", m5stack_cores3_body.name, esp_err_to_name(err));
    }
}
#endif

static esp_err_t init(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c init");
    /* Only the PMU sees PWR: latch its edges for poll_buttons(). Leave its
     * rails alone; the BSP owns them. */
    esp_err_t err = impo_pmu_init(bsp_i2c_get_handle(), true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PMU unavailable (%s): no talk button or battery status", esp_err_to_name(err));
    } else {
        s_pmu = true;
    }
    /* Streams from now on (camera.c): its DMA buffer needs internal RAM that
     * Wi-Fi and BLE take later. */
    camera_register(m5stack_cores3_camera());
    err = m5stack_cores3_camera_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "camera unavailable (%s)", esp_err_to_name(err));
    }
    err = m5stack_cores3_imu_start(bsp_i2c_get_handle());
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "motion sensor unavailable (%s)", esp_err_to_name(err));
    }
#if CONFIG_M5STACK_CORES3_BODY
    body_start();
#endif
    return ESP_OK;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    /* Two 6.4 KB internal DMA buffers, not the BSP's default full-height one.
     * Its LVGL port supplies byte swapping and the panel/touch orientation. */
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT,
        .double_buffer = true,
        .flags = { .buff_dma = true, .buff_spiram = false },
    };
    cfg.lvgl_port_cfg.task_affinity = IMPO_UI_CORE;
    cfg.lvgl_port_cfg.task_priority = IMPO_UI_PRIORITY;
    lv_display_t *disp = bsp_display_start_with_config(&cfg);
    if (!disp) {
        return NULL;
    }
    *touch = bsp_display_get_input_dev();
    return *touch ? disp : NULL;
}

static bool display_lock(int timeout_ms)
{
    /* Impo uses -1 for forever; esp_lvgl_port uses 0. */
    return bsp_display_lock(timeout_ms < 0 ? 0 : (uint32_t)timeout_ms);
}

static void set_brightness(int pct)
{
    bsp_display_brightness_set(pct);
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    /* Impo records two slots; the BSP's no-argument default is 22.05 kHz. */
    const i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(IMPO_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
        },
    };
    ESP_RETURN_ON_ERROR(bsp_audio_init(&cfg), TAG, "duplex audio init");
    *spk = s_spk = bsp_audio_codec_speaker_init();
    *mic = s_mic = bsp_audio_codec_microphone_init();
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static void set_mic_gain(esp_codec_dev_handle_t mic, int db)
{
    /* ES7210 PGA steps are 3 dB; snap so the UI shows what's applied. */
    db = (db / 3) * 3;
    /* esp_codec_dev rounds 33 dB down to 30; the next real step up is 34.5. */
    esp_codec_dev_set_in_gain(mic, db == 33 ? 34.5f : (float)db);
}

static unsigned poll_buttons(void)
{
    static unsigned tick;
    if (!s_pmu || tick++ % PMU_KEY_EVERY) {
        return 0;
    }
    unsigned key = impo_pmu_poll_key();
    return (key & IMPO_PMU_KEY_PRESS ? IMPO_BTN_TALK_PRESS : 0) |
           (key & IMPO_PMU_KEY_RELEASE ? IMPO_BTN_TALK_RELEASE : 0);
}

static esp_err_t read_power(impo_power_t *out)
{
    return s_pmu ? impo_pmu_read_power(out) : ESP_ERR_INVALID_STATE;
}

static esp_err_t power_off(void)
{
    ESP_RETURN_ON_FALSE(s_pmu, ESP_ERR_INVALID_STATE, TAG, "no PMU");
    ESP_RETURN_ON_ERROR(bsp_display_backlight_off(), TAG, "backlight off");
    bsp_display_lock(0);
    esp_err_t err = lvgl_port_stop();
    bsp_display_unlock();
    ESP_RETURN_ON_ERROR(err, TAG, "display stop");
    if (s_spk) {
        esp_codec_dev_close(s_spk);
    }
    if (s_mic) {
        esp_codec_dev_close(s_mic);
    }
    /* PWR turns it back on. */
    return impo_pmu_power_off();
}

static impo_board_t s_board = {
    .name = "M5Stack CoreS3",
    .width = BSP_LCD_H_RES,
    .height = BSP_LCD_V_RES,
    .round = false,
    .touch = true,
    .diagonal_in = 2.0f,
    .talk_button = "power",
    .talk_hint = { LV_ALIGN_TOP_LEFT, 8, 8 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = bsp_display_unlock,
    .set_brightness = set_brightness,
    .audio_init = audio_init,
    .mic_slot = -1,
    .set_mic_gain = set_mic_gain,
    .poll_buttons = poll_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

static const impo_command_t s_commands[] = {
    { "imu.read", m5stack_cores3_imu_read },
};
#define FEATURES "motion_sensor"

const impo_board_t *impo_board_get(void)
{
    static impo_command_t commands[sizeof(s_commands) / sizeof(s_commands[0]) + 16];
    int n = 0;
    for (size_t i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]); i++) {
        commands[n++] = s_commands[i];
    }
    s_board.features = FEATURES;
#if CONFIG_M5STACK_CORES3_BODY
    /* The body's commands after the CoreS3's own. */
    static char features[96];
    s_board.name = m5stack_cores3_body.name;
    for (int i = 0; i < m5stack_cores3_body.command_count && n < (int)(sizeof(commands) / sizeof(commands[0])); i++) {
        commands[n++] = m5stack_cores3_body.commands[i];
    }
    snprintf(features, sizeof(features), FEATURES ",%s", m5stack_cores3_body.features);
    s_board.features = features;
    s_board.later_commands = m5stack_cores3_body.later_commands;
    s_board.later_command_count = m5stack_cores3_body.later_command_count;
#endif
    s_board.commands = commands;
    s_board.command_count = n;
    return &s_board;
}
