# Copyright (c) Impo contributors.
# SPDX-License-Identifier: Apache-2.0
"""WAV speech stops at the end of its data chunk.

Impo's text to speech (Gemini) answers with a WAV whose data chunk is followed
by a C2PA manifest of about 7 KB. Played as PCM, those bytes are a loud buzz
right after the speech. These tests run the production header parser
(impo_wav.c), speaker.say's decoder (impo_sound.c decode_one) and a reply's
WAV path (impo_chat_session.cpp decode/play_wav) on such a WAV, and on one
from a streaming writer that leaves the size unknown.
"""
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
IMPO = ROOT / 'components/impo'
MINIMP3 = ROOT / 'components/minimp3/include'

RATE = 24000
SPEECH_SAMPLES = 2400          # 0.1 s at 24 kHz, all of value QUIET
QUIET = 100
TRAILER = 7655                 # the C2PA chunk's size in a real Gemini clip


def wav(data_size_field=None, trailer=True):
    """A Gemini-like WAV: fmt, data of quiet samples, then a loud C2PA chunk."""
    samples = struct.pack('<%dh' % SPEECH_SAMPLES, *([QUIET] * SPEECH_SAMPLES))
    fmt = struct.pack('<4sIHHIIHH', b'fmt ', 16, 1, 1, RATE, RATE * 2, 2, 16)
    size = len(samples) if data_size_field is None else data_size_field
    body = fmt + struct.pack('<4sI', b'data', size) + samples
    if trailer:
        body += struct.pack('<4sI', b'C2PA', TRAILER) + b'\xff\x7f' * (TRAILER // 2) + b'\x7f'
    return b'RIFF' + struct.pack('<I', 4 + len(body)) + b'WAVE' + body


def c_array(name, data):
    return 'static const uint8_t %s[] = {%s};\nstatic const size_t %s_len = %d;\n' % (
        name, ','.join(str(b) for b in data), name, len(data))


CLIPS = (c_array('clip_sized', wav()) + c_array('clip_streaming', wav(0xFFFFFFFF))
         + c_array('clip_zero', wav(0)) + c_array('clip_plain', wav(trailer=False)))


def compile_and_run(test, harness, cpp=False):
    """Builds harness with the production impo_wav.c and runs it."""
    out = Path(test.tmp.name)
    flags = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-const-variable', '-Wno-unused-variable',
             '-I', str(ROOT / 'tests'), '-I', str(IMPO), '-I', str(MINIMP3)]
    cc = shlex.split(os.environ.get('CC', 'cc'))
    binary = out / (harness.stem + '_bin')
    commands = [[*cc, '-std=gnu11', *flags, '-c', str(IMPO / 'impo_wav.c'), '-o', str(out / 'impo_wav.o')]]
    if cpp:
        commands.append([*shlex.split(os.environ.get('CXX', 'c++')), '-std=gnu++17', *flags,
                         str(harness), str(out / 'impo_wav.o'), '-o', str(binary)])
    else:
        commands.append([*cc, '-std=gnu11', *flags, str(harness), str(out / 'impo_wav.o'), '-o', str(binary)])
    for command in commands:
        result = subprocess.run(command, capture_output=True, text=True)
        test.assertEqual(result.returncode, 0, result.stdout + result.stderr)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    test.assertEqual(result.returncode, 0, result.stdout + result.stderr)
    return result.stdout


def outcome(stdout, name):
    """'name n=… peak=…' from a harness, as (n, peak)."""
    for line in stdout.splitlines():
        if line.startswith(name + ' '):
            fields = dict(part.split('=') for part in line.split()[1:])
            return int(fields['n']), int(fields['peak'])
    raise AssertionError('no result for %s in:\n%s' % (name, stdout))


class WavHeader(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def test_header_reports_the_data_chunk_size(self):
        harness = Path(self.tmp.name) / 'header.c'
        harness.write_text('''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "impo_wav.h"
''' + CLIPS + r'''
int main(void) {
    impo_wav_t w;
    assert(impo_wav_header(clip_sized, clip_sized_len, &w));
    assert(w.rate == 24000 && w.channels == 1 && w.skip == 44 && w.data_bytes == 4800);
    assert(impo_wav_header(clip_streaming, clip_streaming_len, &w) && w.data_bytes == SIZE_MAX);
    assert(impo_wav_header(clip_zero, clip_zero_len, &w) && w.data_bytes == SIZE_MAX);
    /* Not enough yet: the data chunk header isn't in. */
    assert(!impo_wav_header(clip_sized, 40, &w));
    /* 8-bit PCM is not a WAV this gadget plays. */
    uint8_t eight[64];
    memcpy(eight, clip_sized, sizeof(eight));
    eight[34] = 8;
    assert(!impo_wav_header(eight, sizeof(eight), &w));
    puts("ok");
    return 0;
}
''')
        compile_and_run(self, harness)


class SpeakerSay(unittest.TestCase):
    """impo_sound.c's decoder, fed through in-memory stream buffers."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def test_bytes_after_the_data_chunk_are_not_played(self):
        source = (IMPO / 'impo_sound.c').read_text()
        decoder = source[source.index('#define OUT_RATE'):source.index('static void decoder_task(')]
        harness = Path(self.tmp.name) / 'sound.c'
        harness.write_text(r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host_compat.h"
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "impo_wav.h"
#define IMPO_AUDIO_RATE 16000
static const char *TAG = "impo_sound";
#define ESP_LOGI(tag, ...) (printf("log %s: ", tag), printf(__VA_ARGS__), printf("\n"))
#define pdMS_TO_TICKS(ms) (ms)
typedef void *TaskHandle_t;
typedef struct fake_stream { uint8_t *data; size_t len, pos, cap, chunk; } *StreamBufferHandle_t;
static size_t xStreamBufferReceive(StreamBufferHandle_t s, void *buf, size_t max, int ticks) {
    (void)ticks;
    size_t n = s->len - s->pos;
    n = n < max ? n : max;
    n = n < s->chunk ? n : s->chunk;
    memcpy(buf, s->data + s->pos, n);
    s->pos += n;
    return n;
}
static size_t xStreamBufferSend(StreamBufferHandle_t s, const void *buf, size_t len, int ticks) {
    (void)ticks;
    if (s->len + len > s->cap) {
        s->cap = (s->len + len) * 2;
        s->data = realloc(s->data, s->cap);
    }
    memcpy(s->data + s->len, buf, len);
    s->len += len;
    return len;
}
static size_t xStreamBufferBytesAvailable(StreamBufferHandle_t s) { return s->len - s->pos; }
''' + decoder + CLIPS + r'''
static void play(const char *name, const uint8_t *clip, size_t len, size_t chunk) {
    struct fake_stream in = { (uint8_t *)clip, len, 0, len, chunk }, out = { 0 };
    static uint8_t buf[IN_CHUNK + MP3_HOLD];
    static int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME], pcm16[MINIMP3_MAX_SAMPLES_PER_FRAME * 2 + 16];
    s_in = &in; s_out = &out; s_buf = buf; s_pcm = pcm; s_pcm16 = pcm16;
    atomic_store(&s_ended, true);
    atomic_store(&s_ok, true);
    decode_one(atomic_load(&s_gen));
    const int16_t *played = (const int16_t *)out.data;
    size_t n = out.len / 2;
    int peak = 0;
    for (size_t i = 0; i < n; i++) peak = abs(played[i]) > peak ? abs(played[i]) : peak;
    printf("%s n=%zu peak=%d\n", name, n, peak);
    free(out.data);
}
int main(void) {
    play("sized", clip_sized, clip_sized_len, 1000);
    play("sized_small_chunks", clip_sized, clip_sized_len, 7);
    play("plain", clip_plain, clip_plain_len, 1000);
    play("streaming", clip_streaming, clip_streaming_len, 1000);
    play("zero", clip_zero, clip_zero_len, 1000);
    return 0;
}
''')
        stdout = compile_and_run(self, harness)
        speech = SPEECH_SAMPLES * 16000 // RATE
        trailer = 8 + TRAILER
        self.assertEqual(stdout.count('decoded, %d bytes after the samples dropped' % trailer), 2, stdout)
        self.assertEqual(stdout.count('decoded, 0 bytes after the samples dropped'), 3, stdout)
        for name in ('sized', 'sized_small_chunks', 'plain'):
            n, peak = outcome(stdout, name)
            self.assertLessEqual(abs(n - speech), 2, name)
            self.assertLessEqual(peak, QUIET, name)
        # A writer that doesn't know the length: everything after the header is played.
        for name in ('streaming', 'zero'):
            n, peak = outcome(stdout, name)
            self.assertGreater(n, speech + 2000, name)
            self.assertGreater(peak, 30000, name)


class ReplyAudio(unittest.TestCase):
    """impo_chat_session.cpp's decode/play_wav on a reply's WAV."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def test_bytes_after_the_data_chunk_are_not_played(self):
        source = (IMPO / 'impo_chat_session.cpp').read_text()
        constants = source[source.index('#define MIC_RATE'):source.index('/* ---- Voice task')]
        types = source[source.index('enum phase_t'):source.index('/* 10 KB')]
        resampler = source[source.index('static void resampler_init('):source.index('/* ---- Turn: dictation')]
        player = source[source.index('static void play_wav('):source.index('static void check_turn(')]
        harness = Path(self.tmp.name) / 'reply.cpp'
        harness.write_text(r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "host_compat.h"
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "impo_chat_priv.h"
#include "impo_wav.h"
static const char *TAG = "impo_chat";
#define ESP_LOGI(tag, ...) (printf("log %s: ", tag), printf(__VA_ARGS__), printf("\n"))
#define ESP_LOGW(...) ((void)0)
''' + constants + types + r'''
static turn_t s_turn;
enum mark_t { M_AUDIO };
static void mark(mark_t) {}
static std::atomic<uint32_t> s_gen;
static std::vector<int16_t> g_out;
typedef void *StreamBufferHandle_t;
static StreamBufferHandle_t s_out;
static size_t xStreamBufferSpacesAvailable(StreamBufferHandle_t) { return 1 << 20; }
static size_t xStreamBufferSend(StreamBufferHandle_t, const void *p, size_t n, int) {
    const int16_t *s = static_cast<const int16_t *>(p);
    g_out.insert(g_out.end(), s, s + n / 2);
    return n;
}
static void pace_silently() {}
static int16_t s_pcm_buf[MINIMP3_MAX_SAMPLES_PER_FRAME], s_pcm16_buf[MINIMP3_MAX_SAMPLES_PER_FRAME * 2 + 16];
static int16_t *s_pcm = s_pcm_buf, *s_pcm16 = s_pcm16_buf;
''' + resampler + player + CLIPS + r'''
static void play(const char *name, const uint8_t *clip, size_t len, size_t chunk) {
    static uint8_t buf[256 * 1024];
    g_out.clear();
    memset(&s_turn, 0, sizeof(s_turn));
    s_turn.mp3 = buf;
    s_turn.tts_msg = 0;
    s_turn.nmsgs = 1;
    s_turn.msgs[0].tts = TTS_ACTIVE;
    s_turn.format = turn_t::F_UNKNOWN;
    for (size_t off = 0; s_turn.tts_msg >= 0;) {
        if (off < len) {
            size_t n = len - off < chunk ? len - off : chunk;
            memcpy(s_turn.mp3 + s_turn.mp3_len, clip + off, n);
            s_turn.mp3_len += n;
            off += n;
            s_turn.mp3_ended = off == len;
        }
        decode();
    }
    assert(s_turn.msgs[0].tts == TTS_FINISHED);
    int peak = 0;
    for (int16_t v : g_out) peak = abs(v) > peak ? abs(v) : peak;
    printf("%s n=%zu peak=%d frames=%u\n", name, g_out.size(), peak, (unsigned)s_turn.msgs[0].pcm_frames);
}
int main() {
    play("sized", clip_sized, clip_sized_len, 1000);
    play("sized_small_chunks", clip_sized, clip_sized_len, 7);
    play("plain", clip_plain, clip_plain_len, 1000);
    play("streaming", clip_streaming, clip_streaming_len, 1000);
    return 0;
}
''')
        stdout = compile_and_run(self, harness, cpp=True)
        speech = SPEECH_SAMPLES * 16000 // RATE
        dropped = [line for line in stdout.splitlines() if 'after the samples dropped' in line]
        self.assertEqual(dropped, ['log impo_chat: reply audio: %d bytes after the samples dropped' % (8 + TRAILER)] * 2, stdout)
        for name in ('sized', 'sized_small_chunks', 'plain'):
            n, peak = outcome(stdout, name)
            self.assertLessEqual(abs(n - speech), 2, name)
            self.assertLessEqual(peak, QUIET, name)
        n, peak = outcome(stdout, 'streaming')
        self.assertGreater(n, speech + 2000)
        self.assertGreater(peak, 30000)


if __name__ == '__main__':
    unittest.main()
