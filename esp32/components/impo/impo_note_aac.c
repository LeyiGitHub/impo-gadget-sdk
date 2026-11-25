/*
 * Copyright (c) Impo contributors.
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

/* impo_note_aac.h on Espressif's AAC-LC encoder (espressif/esp_audio_codec). */

#include "impo_note_aac.h"

#include "esp_aac_enc.h"
#include "esp_log.h"

static const char *TAG = "impo_note_aac";
static void *s_enc;

bool impo_note_aac_open(void)
{
    esp_aac_enc_config_t cfg = ESP_AAC_ENC_CONFIG_DEFAULT();
    cfg.sample_rate = 16000;
    cfg.channel = 1;
    cfg.bits_per_sample = 16;
    cfg.bitrate = IMPO_NOTE_AAC_BITRATE;
    cfg.adts_used = true;
    esp_audio_err_t err = esp_aac_enc_open(&cfg, sizeof(cfg), &s_enc);
    if (err != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "encoder: error %d", err);
        s_enc = NULL;
        return false;
    }
    int in = 0, out = 0;
    esp_aac_enc_get_frame_size(s_enc, &in, &out);
    if (in != IMPO_NOTE_FRAME_BYTES || out <= 0 || out > IMPO_NOTE_OUT_MAX) {
        ESP_LOGE(TAG, "encoder frames: %d bytes in, %d out", in, out);
        esp_aac_enc_close(s_enc);
        s_enc = NULL;
        return false;
    }
    return true;
}

void impo_note_aac_reset(void)
{
    esp_aac_enc_reset(s_enc);
}

int impo_note_aac_encode(const uint8_t pcm[IMPO_NOTE_FRAME_BYTES], uint8_t *out)
{
    esp_audio_enc_in_frame_t in = { .buffer = (uint8_t *)pcm, .len = IMPO_NOTE_FRAME_BYTES };
    esp_audio_enc_out_frame_t o = { .buffer = out, .len = IMPO_NOTE_OUT_MAX };
    return esp_aac_enc_process(s_enc, &in, &o) == ESP_AUDIO_ERR_OK ? (int)o.encoded_bytes : -1;
}
