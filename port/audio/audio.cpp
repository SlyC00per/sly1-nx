// SPDX-License-Identifier: AGPL-3.0-or-later
// The sound engine (port/audio.h). What each command means is from the game's own code
// (sly1-decomp src/main/iop_commands.cpp and sound_system.cpp) and from the disc's bytes
// (sly1-decomp docs/DISC.md); the driver itself is never read.
//
// So far: every command answered, and the streams (command 0x2C: a VAG file by its first
// sector, the SPU's ADPCM) decoded and mixed. The banks' sounds (command 0x11) and the level
// banks' sequences are not played yet: a voice started is at once over.
#include "port/audio.h"
#include "port/iso.h"
#include "port/system.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__SWITCH__)
#include <switch.h>
#include <malloc.h>
#endif

namespace port {
namespace audio {

namespace {

constexpr int kRate = 48000;
constexpr int kChunk = 480;   // frames mixed at a time: 10 ms

// The SPU's ADPCM: a 16-byte frame is a shift and filter byte, a flags byte and 28 four-bit
// samples; each sample is the nibble shifted, plus the filter's two taps on the two before.
struct Adpcm {
    int h1 = 0, h2 = 0;
    void Frame(const std::uint8_t* f, std::int16_t out[28])
    {
        static const int kF0[5] = {0, 60, 115, 98, 122}, kF1[5] = {0, 0, -52, -55, -60};
        int shift = f[0] & 0xF, filter = (f[0] >> 4) & 0xF;
        if (filter > 4)
            filter = 0;
        if (shift > 12)
            shift = 9;
        for (int k = 0; k < 28; k++) {
            int n = (f[2 + k / 2] >> ((k & 1) * 4)) & 0xF;
            int s = (std::int16_t)(n << 12) >> shift;
            s += (h1 * kF0[filter] + h2 * kF1[filter] + 32) >> 6;
            s = s < -32768 ? -32768 : s > 32767 ? 32767 : s;
            out[k] = (std::int16_t)s;
            h2 = h1;
            h1 = s;
        }
    }
};

// A stream: a VAG file read from the disc as it plays.
struct Stream {
    std::uint32_t handle = 0;
    std::uint32_t sector = 0;       // the file's first
    std::uint32_t dataBytes = 0;    // of ADPCM, after the 0x30-byte header
    std::uint32_t rate = 44100;
    std::uint32_t at = 0;           // bytes of ADPCM decoded
    float volume = 1.0f, pan = 0.0f;
    float fade = 1.0f, fadeStep = 0.0f;   // a fade out: the gain and what it loses each frame mixed
    bool paused = false, done = false, opened = false;
    Adpcm decoder;
    std::vector<std::uint8_t> buffer;   // sectors read ahead
    std::uint32_t bufferAt = 0;         // the buffer's first byte, of the file
    std::int16_t frame[28] = {};
    int frameAt = 28;                   // samples of frame used
    double phase = 0.0;                 // the resampler's place between prev and next
    std::int16_t prev = 0, next = 0;

    bool Open()
    {
        std::uint8_t head[2048];
        if (!ReadSectors(sector, 1, head) || std::memcmp(head, "VAGp", 4) != 0)
            return false;
        auto be = [&](int o) { return (std::uint32_t)head[o] << 24 | (std::uint32_t)head[o + 1] << 16 | (std::uint32_t)head[o + 2] << 8 | head[o + 3]; };
        dataBytes = be(12);
        rate = be(16);
        if (rate < 4000 || rate > 96000)
            rate = 44100;
        opened = true;
        return true;
    }
    // The file's bytes from at (after the header), 16 of them; false at the file's end.
    bool Bytes(std::uint8_t out[16])
    {
        if (at + 16 > dataBytes)
            return false;
        const std::uint32_t file = 0x30 + at;
        if (buffer.empty() || file < bufferAt || file + 16 > bufferAt + buffer.size()) {
            const std::uint32_t first = file / 2048;
            buffer.resize(16 * 2048);
            if (!ReadSectors(sector + first, 16, buffer.data()))
                return false;
            bufferAt = first * 2048;
        }
        std::memcpy(out, &buffer[file - bufferAt], 16);
        at += 16;
        return true;
    }
    bool Sample(std::int16_t* out)
    {
        if (frameAt >= 28) {
            std::uint8_t f[16];
            if (!Bytes(f) || (f[1] & 1)) {   // the file's end, or the frame that ends the sample
                if (at <= dataBytes && (f[1] & 1) && at > 16) {
                    // (the last frame's samples are played)
                } else {
                    return false;
                }
            }
            decoder.Frame(f, frame);
            frameAt = 0;
            if (f[1] & 1)
                dataBytes = at;   // nothing after it
        }
        *out = frame[frameAt++];
        return true;
    }
    // frames of 48 kHz stereo added to out (interleaved, 32-bit sums).
    void Mix(std::int32_t* out, int frames, float master)
    {
        if (done || paused)
            return;
        if (!opened && !Open()) {
            done = true;
            return;
        }
        const double step = (double)rate / kRate;
        const float left = volume * master * (pan > 0 ? 1.0f - pan : 1.0f), right = volume * master * (pan < 0 ? 1.0f + pan : 1.0f);
        for (int k = 0; k < frames; k++) {
            while (phase >= 1.0) {
                prev = next;
                if (!Sample(&next)) {
                    done = true;
                    return;
                }
                phase -= 1.0;
            }
            const float s = ((float)prev + (float)(next - prev) * (float)phase) * fade;
            out[k * 2] += (std::int32_t)(s * left);
            out[k * 2 + 1] += (std::int32_t)(s * right);
            phase += step;
            if (fadeStep != 0.0f) {
                fade -= fadeStep;
                if (fade <= 0.0f) {
                    done = true;
                    return;
                }
            }
        }
    }
};

std::mutex g_lock;
std::map<std::uint32_t, std::shared_ptr<Stream>> g_streams;
std::uint32_t g_nextHandle = 0x100, g_nextBank = 1;
float g_channelVolume[16];
bool g_channelSet;
std::atomic<bool> g_started;
std::atomic<bool> g_quit;
std::vector<std::int16_t> g_wav;   // the host's file, kept in memory until the end
const char* g_wavPath;
std::uint64_t g_commands, g_unknown;

std::uint32_t Word(const std::uint8_t* data, std::int32_t size, int index)
{
    std::uint32_t w = 0;
    if (data && (index + 1) * 4 <= size)
        std::memcpy(&w, data + index * 4, 4);
    return w;
}

void MixChunk(std::int16_t* out)
{
    std::int32_t sum[kChunk * 2] = {};
    {
        std::lock_guard<std::mutex> lk(g_lock);
        for (auto it = g_streams.begin(); it != g_streams.end();) {
            it->second->Mix(sum, kChunk, 0.5f);
            // (a finished stream stays until the game has asked and been told it is over, or stops it)
            ++it;
        }
    }
    for (int k = 0; k < kChunk * 2; k++)
        out[k] = (std::int16_t)(sum[k] < -32768 ? -32768 : sum[k] > 32767 ? 32767 : sum[k]);
}

}  // namespace

void MixerMain()
{
#if defined(__SWITCH__)
    if (R_FAILED(audoutInitialize()) || R_FAILED(audoutStartAudioOut())) {
        std::fprintf(stderr, "audio: the Switch's audio output could not be started\n");
        return;
    }
    constexpr int kBuffers = 4, kFrames = kChunk * 2;   // 20 ms a buffer
    AudioOutBuffer buffers[kBuffers];
    for (int b = 0; b < kBuffers; b++) {
        const std::size_t bytes = (kFrames * 4 + 0xFFF) & ~std::size_t(0xFFF);
        void* mem = memalign(0x1000, bytes);
        std::memset(mem, 0, bytes);
        buffers[b].next = nullptr;
        buffers[b].buffer = mem;
        buffers[b].buffer_size = bytes;
        buffers[b].data_size = kFrames * 4;
        buffers[b].data_offset = 0;
        audoutAppendAudioOutBuffer(&buffers[b]);
    }
    while (!g_quit.load()) {
        AudioOutBuffer* released = nullptr;
        u32 count = 0;
        if (R_FAILED(audoutWaitPlayFinish(&released, &count, UINT64_MAX)) || !released)
            continue;
        std::int16_t* out = static_cast<std::int16_t*>(released->buffer);
        MixChunk(out);
        MixChunk(out + kChunk * 2);
        released->data_size = kFrames * 4;
        audoutAppendAudioOutBuffer(released);
    }
#else
    auto next = std::chrono::steady_clock::now();
    std::int16_t out[kChunk * 2];
    while (!g_quit.load()) {
        MixChunk(out);
        if (g_wavPath) {
            std::lock_guard<std::mutex> lk(g_lock);
            g_wav.insert(g_wav.end(), out, out + kChunk * 2);
        }
        next += std::chrono::milliseconds(10);
        std::this_thread::sleep_until(next);
    }
#endif
}

namespace {
Stream* Find(std::uint32_t handle)
{
    auto it = g_streams.find(handle);
    return it == g_streams.end() ? nullptr : it->second.get();
}

}  // namespace

bool Wanted()
{
#if defined(__SWITCH__)
    return true;
#else
    g_wavPath = std::getenv("SLY1_AUDIO_WAV");
    return g_wavPath != nullptr;   // (the host makes sound only into a file)
#endif
}

void Finish()
{
    g_quit.store(true);
    if (!g_wavPath)
        return;
    std::lock_guard<std::mutex> lk(g_lock);
    std::FILE* f = std::fopen(g_wavPath, "wb");
    if (!f)
        return;
    const std::uint32_t bytes = (std::uint32_t)(g_wav.size() * 2);
    auto u32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 4, 1, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 8, 1, f); u32(16); u16(1); u16(2); u32(kRate); u32(kRate * 4); u16(4); u16(16);
    std::fwrite("data", 4, 1, f); u32(bytes);
    std::fwrite(g_wav.data(), 2, g_wav.size(), f);
    std::fclose(f);
    std::fprintf(stderr, "audio: %.1f s written to %s; %llu commands, %llu of them not understood\n", g_wav.size() / 2.0 / kRate, g_wavPath,
                 (unsigned long long)g_commands, (unsigned long long)g_unknown);
}

std::uint32_t LoadBank(std::uint32_t)
{
    std::lock_guard<std::mutex> lk(g_lock);
    return g_nextBank++;
}

std::uint32_t Command(std::int32_t cmd, const std::uint8_t* data, std::int32_t size)
{
    std::lock_guard<std::mutex> lk(g_lock);
    g_commands++;
    if (!g_channelSet) {
        for (float& v : g_channelVolume)
            v = 1.0f;
        g_channelSet = true;
    }
    const std::uint32_t a = Word(data, size, 0), b = Word(data, size, 1), c = Word(data, size, 2);
    switch (cmd) {
    case 0x00:   // the packet shared with the driver
    case 0x06:   // a bank unloaded
    case 0x08:   // the banks changed
    case 0x0B:   // the sound option
    case 0x0D:
    case 0x4E:   // a channel's set-up
    case 0x27:   // the sound level
    case 0x3B: case 0x3C: case 0x3D:
        return 0;
    case 0x09:   // a volume channel's value, 0x400 is 1
        if (a < 16)
            g_channelVolume[a] = (float)(std::int32_t)b / 1024.0f;
        return 0;
    case 0x0A:   // a volume channel's value asked for
        return a < 16 ? (std::uint32_t)(g_channelVolume[a] * 1024.0f) : 0;
    case 0x11:   // a bank's sound started: a voice (not played yet: over at once)
        return g_nextHandle++;
    case 0x1A:   // whether a sound is in a bank
        return 1;
    case 0x1C:   // a sound's base pitch
        return 0;
    case 0x2A:   // the driver to read the disc for the game: not taken (the game reads it itself)
        return 0;
    case 0x2C: { // a stream started: its first sector, the volume in the third word's high half
        auto s = std::make_shared<Stream>();
        s->handle = g_nextHandle++;
        s->sector = a;
        s->volume = (float)(c >> 16) / 1024.0f;
        s->phase = 1.0;
        g_streams[s->handle] = s;
        return s->handle;
    }
    case 0x13:   // a voice paused
        if (Stream* s = Find(a))
            s->paused = true;
        return 0;
    case 0x14:   // and resumed
        if (Stream* s = Find(a))
            s->paused = false;
        return 0;
    case 0x15:   // a voice stopped
        g_streams.erase(a);
        return 0;
    case 0x16:   // voices suspended by mask, and resumed: every stream
        for (auto& kv : g_streams)
            kv.second->paused = true;
        return 0;
    case 0x17:
        for (auto& kv : g_streams)
            kv.second->paused = false;
        return 0;
    case 0x19:   // whether a voice is still playing
    case 0x2E:
    case 0x4F: {
        Stream* s = Find(a);
        if (!s)
            return 0;
        if (s->done) {
            g_streams.erase(a);
            return 0;
        }
        return 1;
    }
    case 0x1B:   // a voice's volume (0x400 is 1) and pan
        if (Stream* s = Find(a))
            s->volume = (float)(std::int32_t)b / 1024.0f;
        return 0;
    case 0x1E:   // a voice's pitch
    case 0x29:   // a voice's parameter set
        return 0;
    case 0x22:   // a voice faded out: over a fifth of a second until its time's unit is known
        if (Stream* s = Find(a))
            s->fadeStep = 1.0f / (kRate * 0.2f);
        return 0;
    case 0x28:   // a voice's parameter asked for
        return 0;
    case 0x34:   // every sound stopped
        g_streams.clear();
        return 0;
    case 0x36:   // and waited for
        return 0;
    default:
        g_unknown++;
        static std::map<std::int32_t, int> seen;
        if (seen[cmd]++ == 0)
            std::fprintf(stderr, "audio: command 0x%X is not understood (answered 0)\n", (unsigned)cmd);
        return 0;
    }
}

}  // namespace audio
}  // namespace port
