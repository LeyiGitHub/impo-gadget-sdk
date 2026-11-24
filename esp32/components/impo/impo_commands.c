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

#include "impo_commands.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "impo_audio.h"
#include "impo_board.h"
#include "impo_camera.h"
#include "impo_link.h"
#include "impo_settings.h"
#include "impo_sound.h"
#include "impo_state.h"

static const char *TAG = "impo_commands";

#define TEXT_MAX 200
#define SOUND_TIMEOUT_MS 15000

cJSON *impo_command_ok(void)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    return result;
}

cJSON *impo_command_error(const char *code, const char *message)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON *error = cJSON_AddObjectToObject(result, "error");
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    return result;
}

/* Wakes the screen so the person sees what the command did. */
static void wake(void)
{
    impo_state_set_asleep(false);
    impo_state_poke();
}

static cJSON *show_text(const cJSON *params)
{
    const cJSON *text = cJSON_GetObjectItem(params, "text");
    if (!cJSON_IsString(text) || !text->valuestring[0] || strlen(text->valuestring) > TEXT_MAX) {
        return impo_command_error("invalid_param", "text must be 1 to 200 bytes of UTF-8");
    }
    wake();
    impo_state_set_caption("%s", text->valuestring);
    return impo_command_ok();
}

static cJSON *avatar_cheer(const cJSON *params)
{
    (void)params;
    wake();
    impo_state_make_happy();
    return impo_command_ok();
}

static cJSON *set_volume(const cJSON *params)
{
    const cJSON *percent = cJSON_GetObjectItem(params, "percent");
    if (!cJSON_IsNumber(percent) || percent->valuedouble < 0 || percent->valuedouble > 100) {
        return impo_command_error("invalid_param", "percent must be 0 to 100");
    }
    impo_settings_set_volume(percent->valueint);
    impo_audio_set_volume(impo_settings_volume());
    cJSON *result = impo_command_ok();
    cJSON_AddNumberToObject(result, "percent", impo_settings_volume());
    return result;
}

/*
 * speaker.play and speaker.play_url: the bytes of a sound the cloud prepared,
 * played as they arrive. speaker.play takes them from the gateway over Link's
 * own session (GET /audio/<id>, a piece at a time while the player has room);
 * speaker.play_url downloads a public URL. Either answers once the sound has
 * played to its end, been stopped, or failed, saying which.
 *
 * One sound at a time: a new one stops the one playing, which answers
 * "stopped". The memory is impo_sound's fixed buffers whatever the length.
 */
#define SOUND_MAX_MS 110000          /* the answer must come within the command's 120 s */
#define SOUND_WRITE_WAIT_MS 20000    /* for room: the decoder frees it as it plays */
#define PIECE_BYTES (32 * 1024)      /* asked of the gateway at a time */
#define PIECE_WAIT_MS 15000

typedef struct {
    impo_reply_t *reply;
    char *url;                       /* speaker.play_url */
    char path[96];                   /* speaker.play: /audio/<id>?offset= */
} play_job_t;

static SemaphoreHandle_t s_play_lock;    /* held by the job that owns the player */
static char s_last_url[128];             /* what speaker.play_url last tried, for speaker.status */

/* One piece from the gateway; the callback runs on the Link session task. */
static SemaphoreHandle_t s_piece_done;
static atomic_uint s_piece;              /* the piece being fetched; callbacks of others are ignored */
static int s_piece_status;
static size_t s_piece_bytes;

static void on_piece(void *ctx, int status, const uint8_t *data, size_t len, bool end)
{
    if ((unsigned)(uintptr_t)ctx != atomic_load(&s_piece)) {
        return;
    }
    if (status < 0) {
        s_piece_status = -1;
        xSemaphoreGive(s_piece_done);
        return;
    }
    if (status > 0) {
        s_piece_status = status;
    }
    if (len && s_piece_status == 200) {
        /* Room was there before the piece was asked for. */
        if (impo_sound_write(data, len, 50)) {
            s_piece_bytes += len;
        } else {
            s_piece_status = -2;
        }
    }
    if (end) {
        xSemaphoreGive(s_piece_done);
    }
}

static cJSON *failed(const char *code, const char *message)
{
    return impo_command_error(code, message);
}

/* The gateway's bytes, a piece at a time. NULL when all of them arrived. */
static cJSON *fetch_cloud(play_job_t *job, unsigned n, size_t *total)
{
    for (;;) {
        while (impo_sound_room() < PIECE_BYTES) {
            if (impo_sound_outcome(n, NULL) != IMPO_SOUND_RUNNING) {
                return NULL;   /* stopped or failed: the outcome says which */
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        char path[128];
        snprintf(path, sizeof(path), "%s?offset=%u&length=%u", job->path, (unsigned)*total, (unsigned)PIECE_BYTES);
        unsigned piece = atomic_fetch_add(&s_piece, 1) + 1;
        s_piece_status = 0;
        s_piece_bytes = 0;
        xSemaphoreTake(s_piece_done, 0);
        int64_t id = impo_link_req_open("GET", path, NULL, true, on_piece, (void *)(uintptr_t)piece);
        if (!id) {
            return failed("download_failed", "the gadget is not connected to Impo");
        }
        if (xSemaphoreTake(s_piece_done, pdMS_TO_TICKS(PIECE_WAIT_MS)) != pdTRUE) {
            atomic_fetch_add(&s_piece, 1);
            impo_link_req_cancel(id);
            return failed("download_failed", "the sound stopped arriving");
        }
        if (s_piece_status == 404) {
            return failed("not_found", "no such sound; it lasts five minutes after it is prepared");
        }
        if (s_piece_status == -2) {
            return NULL;   /* the player gave up on it */
        }
        if (s_piece_status != 200) {
            return failed("download_failed", "the sound broke off");
        }
        *total += s_piece_bytes;
        if (s_piece_bytes < PIECE_BYTES) {
            return NULL;
        }
    }
}

/* A public URL's bytes as they download. NULL when all of them arrived. */
#define REDIRECTS_MAX 5

static cJSON *fetch_url(play_job_t *job, unsigned n, size_t *total)
{
    cJSON *error = NULL;
    uint8_t *chunk = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const esp_http_client_config_t cfg = {
        .url = job->url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = SOUND_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 512,
    };
    esp_http_client_handle_t http = chunk ? esp_http_client_init(&cfg) : NULL;
    if (!http) {
        error = failed("out_of_memory", "no memory for the download");
    }
    /* Follows up to REDIRECTS_MAX redirects (a 301 to the file's real place is common). */
    for (int redirects = 0; !error; redirects++) {
        if (esp_http_client_open(http, 0) != ESP_OK) {
            error = failed("download_failed", "the URL could not be reached");
            break;
        }
        esp_http_client_fetch_headers(http);
        int status = esp_http_client_get_status_code(http);
        if (status == 200) {
            break;
        }
        bool redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
        if (redirect && redirects < REDIRECTS_MAX && esp_http_client_set_redirection(http) == ESP_OK) {
            esp_http_client_close(http);
            continue;
        }
        char why[64];
        snprintf(why, sizeof why, redirect ? "the URL redirected more than %d times" : "the URL answered HTTP %d",
                 redirect ? REDIRECTS_MAX : status);
        error = failed("download_failed", why);
    }
    while (!error) {
        int got = esp_http_client_read(http, (char *)chunk, 4096);
        if (got < 0) {
            error = failed("download_failed", "the download broke off");
        } else if (got == 0) {
            break;
        } else if (!impo_sound_write(chunk, got, SOUND_WRITE_WAIT_MS)) {
            break;   /* the player gave up on it: the outcome says why */
        } else {
            *total += got;
        }
        (void)n;
    }
    if (http) {
        esp_http_client_close(http);
        esp_http_client_cleanup(http);
    }
    free(chunk);
    return error;
}

static void play_task(void *arg)
{
    play_job_t *job = arg;
    /* A sound playing now stops and answers "stopped"; then the player is this one's. */
    impo_sound_stop();
    cJSON *result = NULL;
    if (xSemaphoreTake(s_play_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        result = failed("busy", "the sound before this one did not stop");
        goto out;
    }
    unsigned n = impo_sound_begin(job->url ? "url" : "cloud");
    size_t total = 0;
    int64_t started = esp_timer_get_time();
    result = job->url ? fetch_url(job, n, &total) : fetch_cloud(job, n, &total);
    if (result) {
        impo_sound_end(false);
    } else {
        impo_sound_end(true);
        uint32_t played = 0;
        impo_sound_outcome_t how;
        while ((how = impo_sound_outcome(n, &played)) == IMPO_SOUND_RUNNING) {
            if (played > SOUND_MAX_MS || esp_timer_get_time() - started > (int64_t)SOUND_MAX_MS * 1000) {
                impo_sound_stop();
                result = failed("too_long", "stopped after 110 seconds; a command answers within 120");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (!result && how == IMPO_SOUND_DONE && !total) {
            result = failed("invalid_audio", "the sound was empty");
        } else if (!result && how == IMPO_SOUND_DONE) {
            result = impo_command_ok();
            cJSON_AddNumberToObject(result, "played_ms", played);
            cJSON_AddNumberToObject(result, "bytes", (double)total);
        } else if (!result && how == IMPO_SOUND_STOPPED) {
            result = failed("stopped", "stopped before its end");
            cJSON_AddNumberToObject(result, "played_ms", played);
        } else if (!result) {
            char why[64];
            impo_sound_status(why, sizeof(why));
            result = failed("invalid_audio", why[0] ? why : "the sound could not be played");
        }
    }
    ESP_LOGI(TAG, "%s: %s, %u bytes", job->url ? "play_url" : "play",
             cJSON_IsTrue(cJSON_GetObjectItem(result, "ok")) ? "played" : "not played", (unsigned)total);
    xSemaphoreGive(s_play_lock);
out:
    impo_command_reply(job->reply, result);
    free(job->url);
    free(job);
    vTaskDeleteWithCaps(NULL);
}

static cJSON *start_play(play_job_t *job, impo_reply_t *reply)
{
    if (!impo_settings_speaker_on()) {
        free(job->url);
        free(job);
        return failed("speaker_off", "the speaker is turned off in the gadget's settings");
    }
    if (!s_play_lock) {
        s_play_lock = xSemaphoreCreateMutex();
        s_piece_done = xSemaphoreCreateBinary();
    }
    job->reply = reply;
    if (!s_play_lock || !s_piece_done
        || xTaskCreateWithCaps(play_task, "sound", 6144, job, 4, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        free(job->url);
        free(job);
        return failed("out_of_memory", "no memory to play the sound");
    }
    wake();
    return impo_command_later();
}

static cJSON *play(const cJSON *params, impo_reply_t *reply)
{
    const cJSON *id = cJSON_GetObjectItem(params, "id");
    size_t len = cJSON_IsString(id) ? strlen(id->valuestring) : 0;
    if (!len || len > 64 || strspn(id->valuestring, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != len) {
        return failed("invalid_param", "id must be the gateway's id of a prepared sound");
    }
    play_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        return failed("out_of_memory", "no memory to play the sound");
    }
    snprintf(job->path, sizeof(job->path), "/audio/%s", id->valuestring);
    return start_play(job, reply);
}

static cJSON *play_url(const cJSON *params, impo_reply_t *reply)
{
    const cJSON *url = cJSON_GetObjectItem(params, "url");
    if (!cJSON_IsString(url) || (strncmp(url->valuestring, "https://", 8) && strncmp(url->valuestring, "http://", 7))
        || strlen(url->valuestring) > 512) {
        return failed("invalid_param", "url must be an http or https URL of up to 512 characters");
    }
    play_job_t *job = calloc(1, sizeof(*job));
    char *copy = job ? strdup(url->valuestring) : NULL;
    if (!copy) {
        free(job);
        return failed("out_of_memory", "no memory to play the sound");
    }
    job->url = copy;
    strlcpy(s_last_url, copy, sizeof(s_last_url));
    return start_play(job, reply);
}

static cJSON *speaker_stop(const cJSON *params)
{
    (void)params;
    impo_sound_stop();
    return impo_command_ok();
}

static cJSON *speaker_status(const cJSON *params)
{
    (void)params;
    char error[64];
    const char *status = impo_sound_status(error, sizeof(error));
    cJSON *result = impo_command_ok();
    cJSON_AddStringToObject(result, "status", status);
    if (error[0]) {
        cJSON_AddStringToObject(result, "error", error);
    }
    if (s_last_url[0]) {
        cJSON_AddStringToObject(result, "last_url", s_last_url);
    }
    cJSON_AddNumberToObject(result, "volume_percent", impo_settings_volume());
    cJSON_AddBoolToObject(result, "speaker_on", impo_settings_speaker_on());
    return result;
}

static const impo_command_t COMMON[] = {
    { "display.show_text",  show_text },
    { "avatar.cheer",       avatar_cheer },
    { "speaker.set_volume", set_volume },
    { "speaker.stop",       speaker_stop },
    { "speaker.status",     speaker_status },
};

static const impo_later_command_t COMMON_LATER[] = {
    { "speaker.play",     play,     120000 },
    { "speaker.play_url", play_url, 120000 },
};

static void describe(cJSON *commands, const char *name, int timeout_ms)
{
    cJSON *entry = cJSON_AddObjectToObject(commands, name);
    if (entry && timeout_ms > 30000) {
        cJSON_AddNumberToObject(entry, "timeout_ms", timeout_ms);
    }
}

void impo_commands_capabilities(cJSON *params)
{
    cJSON *caps = cJSON_AddObjectToObject(params, "capabilities");
    if (!caps || !impo_board) {
        return;
    }
    cJSON *screen = cJSON_AddObjectToObject(caps, "screen");
    cJSON_AddNumberToObject(screen, "width", impo_board->width);
    cJSON_AddNumberToObject(screen, "height", impo_board->height);
    cJSON_AddBoolToObject(screen, "color", true);
    cJSON_AddBoolToObject(screen, "round", impo_board->round);
    cJSON_AddBoolToObject(screen, "touch", impo_board->touch);
    cJSON_AddBoolToObject(caps, "avatar", true);
    cJSON_AddBoolToObject(caps, "speaker", impo_board->audio_init != NULL);
    cJSON_AddBoolToObject(caps, "microphone", impo_board->audio_init != NULL);
    cJSON_AddBoolToObject(caps, "push_to_talk", impo_board->audio_init != NULL);
    cJSON_AddBoolToObject(caps, "camera", impo_camera_name() != NULL);
    cJSON_AddBoolToObject(caps, "battery", impo_board->read_power != NULL);
    cJSON_AddBoolToObject(caps, "buttons", impo_board->poll_buttons != NULL);
    cJSON *features = cJSON_AddArrayToObject(caps, "features");
    const char *list = impo_board->features;
    while (list && *list) {
        const char *end = strchr(list, ',');
        size_t len = end ? (size_t)(end - list) : strlen(list);
        if (len) {
            char name[32];
            snprintf(name, sizeof(name), "%.*s", (int)len, list);
            cJSON_AddItemToArray(features, cJSON_CreateString(name));
        }
        list = end ? end + 1 : list + len;
    }
}

void impo_commands_describe(cJSON *commands)
{
    for (size_t i = 0; i < sizeof(COMMON) / sizeof(COMMON[0]); i++) {
        describe(commands, COMMON[i].name, 0);
    }
    for (size_t i = 0; i < sizeof(COMMON_LATER) / sizeof(COMMON_LATER[0]); i++) {
        describe(commands, COMMON_LATER[i].name, COMMON_LATER[i].timeout_ms);
    }
    for (int i = 0; impo_board && i < impo_board->command_count; i++) {
        describe(commands, impo_board->commands[i].name, 0);
    }
    for (int i = 0; impo_board && i < impo_board->later_command_count; i++) {
        describe(commands, impo_board->later_commands[i].name, impo_board->later_commands[i].timeout_ms);
    }
}

/* Link sends back only "ok", "error" and "payload": whatever else a command
 * put in its result (a reading, a state) is gathered under "payload". */
static cJSON *wrap(cJSON *result)
{
    if (!result || cJSON_GetObjectItem(result, "payload") || cJSON_GetObjectItem(result, "_async")) {
        return result;
    }
    cJSON *payload = NULL;
    cJSON *item = result->child;
    while (item) {
        cJSON *next = item->next;
        if (strcmp(item->string, "ok") && strcmp(item->string, "error")) {
            if (!payload) {
                payload = cJSON_CreateObject();
            }
            cJSON_AddItemToObject(payload, item->string, cJSON_DetachItemViaPointer(result, item));
        }
        item = next;
    }
    if (payload) {
        cJSON_AddItemToObject(result, "payload", payload);
    }
    return result;
}

cJSON *impo_command_later(void)
{
    cJSON *later = cJSON_CreateObject();
    cJSON_AddBoolToObject(later, "_async", true);
    return later;
}

bool impo_command_is_later(const cJSON *result)
{
    return cJSON_IsTrue(cJSON_GetObjectItem(result, "_async"));
}

void impo_command_reply(impo_reply_t *reply, cJSON *result)
{
    reply->send(reply, wrap(result));
}

static cJSON *run_later(const impo_later_command_t *command, const cJSON *params, impo_reply_t *reply)
{
    if (!reply) {
        return wrap(impo_command_error("unavailable", "this command answers later and can't be run from here"));
    }
    return wrap(command->run_later(params, reply));
}

cJSON *impo_commands_run(const char *name, const cJSON *params, impo_reply_t *reply)
{
    for (size_t i = 0; i < sizeof(COMMON) / sizeof(COMMON[0]); i++) {
        if (!strcmp(name, COMMON[i].name)) {
            return wrap(COMMON[i].run(params));
        }
    }
    for (size_t i = 0; i < sizeof(COMMON_LATER) / sizeof(COMMON_LATER[0]); i++) {
        if (!strcmp(name, COMMON_LATER[i].name)) {
            return run_later(&COMMON_LATER[i], params, reply);
        }
    }
    for (int i = 0; impo_board && i < impo_board->command_count; i++) {
        if (!strcmp(name, impo_board->commands[i].name)) {
            return wrap(impo_board->commands[i].run(params));
        }
    }
    for (int i = 0; impo_board && i < impo_board->later_command_count; i++) {
        if (!strcmp(name, impo_board->later_commands[i].name)) {
            return run_later(&impo_board->later_commands[i], params, reply);
        }
    }
    return NULL;
}
