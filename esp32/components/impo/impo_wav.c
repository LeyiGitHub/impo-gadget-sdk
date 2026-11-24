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
#include "impo_wav.h"

#include <string.h>

static uint32_t le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

bool impo_wav_header(const uint8_t *data, size_t len, impo_wav_t *out)
{
    if (len < 12 || memcmp(data, "RIFF", 4) || memcmp(data + 8, "WAVE", 4)) {
        return false;
    }
    size_t off = 12;
    bool fmt = false;
    while (off + 8 <= len) {
        uint32_t size = le32(data + off + 4);
        if (!memcmp(data + off, "fmt ", 4)) {
            if (off + 8 + 16 > len) {
                return false;
            }
            int tag = data[off + 8] | data[off + 9] << 8;
            out->channels = data[off + 10] | data[off + 11] << 8;
            out->rate = (int)le32(data + off + 12);
            int bits = data[off + 22] | data[off + 23] << 8;
            fmt = tag == 1 && bits == 16 && (out->channels == 1 || out->channels == 2) &&
                  out->rate >= 8000 && out->rate <= 48000;
        } else if (!memcmp(data + off, "data", 4)) {
            out->skip = off + 8;
            out->data_bytes = size == 0 || size == 0xFFFFFFFFu ? SIZE_MAX : (size_t)size;
            return fmt;
        }
        off += 8 + (size_t)size + (size & 1);
    }
    return false;
}
