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

/*
 * A voice note as AAC (impo_chat_session.cpp): 16 kHz mono AAC-LC at 24 kbit/s
 * in ADTS frames, as the phone apps record theirs. About a tenth of the bytes
 * of the PCM, which went up as a WAV before.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IMPO_NOTE_AAC_BITRATE 24000
/* One frame of 16 kHz PCM: AAC-LC takes 1024 samples at a time. */
#define IMPO_NOTE_FRAME_BYTES (1024 * 2)
/* Room for one encoded frame; the encoder asks for no more (checked at open). */
#define IMPO_NOTE_OUT_MAX 1024

/* Opens the encoder; false when it can't (no memory). Called once. */
bool impo_note_aac_open(void);
/* Starts a new note. */
void impo_note_aac_reset(void);
/* Encodes one frame into  (IMPO_NOTE_OUT_MAX bytes): the bytes written,
 * none while the encoder fills, or -1 on failure. */
int impo_note_aac_encode(const uint8_t pcm[IMPO_NOTE_FRAME_BYTES], uint8_t *out);

#ifdef __cplusplus
}
#endif
