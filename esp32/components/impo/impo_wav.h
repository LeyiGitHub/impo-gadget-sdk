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
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The header of a WAV stream: 16-bit PCM, one or two channels, 8–48 kHz. */
typedef struct {
    int rate;
    int channels;
    size_t skip;        /* where the samples start */
    /* How many bytes of samples the data chunk holds; SIZE_MAX when it doesn't
     * say (0 or 0xFFFFFFFF, as a streaming writer leaves it). Only these bytes
     * are sound: Impo's text to speech (Gemini) puts a C2PA manifest of about
     * 7 KB after them, and played as PCM that is a loud buzz at the end. */
    size_t data_bytes;
} impo_wav_t;

/* Reads the header from the first len bytes of a stream. False until enough has
 * arrived to read it, and for a WAV that isn't 16-bit PCM: the caller tells
 * them apart by how much it has. */
bool impo_wav_header(const uint8_t *data, size_t len, impo_wav_t *out);

#ifdef __cplusplus
}
#endif
