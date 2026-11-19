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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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
    std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();   // (for the host without a mixer)
    Adpcm decoder;
    std::vector<std::uint8_t> buffer;   // sectors read ahead
    std::uint32_t bufferAt = 0;         // the buffer's first byte, of the file
    std::int16_t frame[28] = {};
    int frameAt = 28;                   // samples of frame used
    bool ended = false;                 // the last frame has been decoded
    double phase = 0.0;                 // the resampler's place between prev and next
    std::int16_t prev = 0, next = 0;

    bool Open()
    {
        std::uint8_t head[2048];
        const bool read = ReadSectors(sector, 1, head);
        static const bool log = std::getenv("SLY1_SND_LOG") != nullptr;
        if (log)
            std::fprintf(stderr, "sndlog: stream at sector %u: %s, first bytes %02X %02X %02X %02X %02X %02X %02X %02X, name %.16s\n", sector, read ? "read" : "not read",
                         head[0], head[1], head[2], head[3], head[4], head[5], head[6], head[7], read ? (const char*)head + 32 : "");
        if (!read || std::memcmp(head, "VAGp", 4) != 0)
            return false;
        auto be = [&](int o) { return (std::uint32_t)head[o] << 24 | (std::uint32_t)head[o + 1] << 16 | (std::uint32_t)head[o + 2] << 8 | head[o + 3]; };
        dataBytes = be(12);
        rate = be(16);
        if (log)
            std::fprintf(stderr, "sndlog:   %u bytes at %u Hz (%.1f s), words at 4, 8, 20, 24: %08X %08X %08X %08X\n", dataBytes, rate, dataBytes / 16.0 * 28 / (rate ? rate : 1),
                         be(4), be(8), be(20), be(24));
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
            // the frame whose flags have bit 0 set is the sample's last: its samples are played,
            // and nothing after it
            std::uint8_t f[16];
            if (ended || !Bytes(f))
                return false;
            decoder.Frame(f, frame);
            frameAt = 0;
            ended = (f[1] & 1) != 0;
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

// A global sound bank (sly1-decomp docs/DISC.md, "Sound banks"): its sound block (`SBlk`: the
// records, a sound each, and the tones a record is made of) and its samples (the SPU's ADPCM).
// Of a tone that plays a sample, what is used: its volume (+0x09, of 127), its sample's place
// (+0x18), and the word at +0x0A taken as the sample's rate in Hz. That last is a reading of the
// numbers, not a measurement (docs/AUDIO.md): 17,066 for most tones and about 31,920 for the
// rest, the second 15/8 of the first.
struct Bank {
    std::uint32_t sector = 0;
    bool global = false;
    std::vector<std::uint8_t> block, samples;
    std::uint32_t records = 0, tones = 0, recordAt = 0, toneAt = 0;
};

// One sample of a bank playing: a part of a voice.
struct Note {
    std::shared_ptr<Bank> bank;
    std::uint32_t at = 0;        // the next frame's place in the samples
    std::uint32_t loopAt = ~0u;  // the frame a loop goes back to, when one has been passed
    float rate = 22050.0f, volume = 1.0f;
    Adpcm decoder;
    std::int16_t frame[28] = {};
    int frameAt = 28;
    bool last = false, over = false;
    double phase = 1.0;
    std::int16_t prev = 0, next = 0;

    bool Sample(std::int16_t* out)
    {
        if (frameAt >= 28) {
            if (last) {
                if (loopAt == ~0u)
                    return false;
                at = loopAt;   // a loop: back to the frame marked as its start
                last = false;
            }
            if (at + 16 > bank->samples.size())
                return false;
            const std::uint8_t* f = &bank->samples[at];
            if (f[1] & 4)
                loopAt = at;
            decoder.Frame(f, frame);
            frameAt = 0;
            if (f[1] & 1) {
                last = true;
                if (!(f[1] & 2))
                    loopAt = ~0u;   // the sample's end, not a loop's
            }
            at += 16;
        }
        *out = frame[frameAt++];
        return true;
    }
};

// A voice: a bank's sound started by the game (command 0x11), which it then sets the volume, the
// pan and the pitch of, asks about, and stops.
struct Voice {
    std::vector<Note> notes;
    float volume = 1.0f, left = 0.7071f, right = 0.7071f, pitch = 1.0f;
    float gain = 1.0f, gainStep = 0.0f;   // a stop or a fade: the gain and what it loses a frame
    int wait = 2;                          // chunks before it sounds (its volume comes in the commands after its start)
    bool paused = false, over = false, program = false;
    int overFor = 0;   // chunks since it ended

    void Mix(std::int32_t* out, int frames, float master)
    {
        if (over || paused)
            return;
        if (wait > 0) {
            wait--;
            return;
        }
        bool any = program;
        for (Note& n : notes) {
            if (n.over)
                continue;
            const double step = (double)n.rate * pitch / kRate;
            const float l = volume * n.volume * left * master, r = volume * n.volume * right * master;
            float g = gain;
            for (int k = 0; k < frames; k++) {
                while (n.phase >= 1.0) {
                    n.prev = n.next;
                    if (!n.Sample(&n.next)) {
                        n.over = true;
                        break;
                    }
                    n.phase -= 1.0;
                }
                if (n.over)
                    break;
                const float v = ((float)n.prev + (float)(n.next - n.prev) * (float)n.phase) * g;
                out[k * 2] += (std::int32_t)(v * l);
                out[k * 2 + 1] += (std::int32_t)(v * r);
                n.phase += step;
                g -= gainStep;
                if (g <= 0.0f) {
                    n.over = true;
                    break;
                }
            }
            any = any || !n.over;
        }
        gain -= gainStep * (float)frames;
        if (!any || gain <= 0.0f)
            over = true;
    }
};

std::mutex g_lock;
std::map<std::uint32_t, std::shared_ptr<Bank>> g_banks;
std::map<std::uint32_t, std::shared_ptr<Voice>> g_voices;
std::uint64_t g_voicesStarted, g_voicesEmpty;
std::map<std::uint32_t, std::uint64_t> g_startedBy;   // by bank handle and record, for the report
std::map<std::uint32_t, std::shared_ptr<Stream>> g_streams;
std::uint32_t g_nextHandle = 0x100, g_nextBank = 1;
float g_channelVolume[16];
bool g_channelSet;
std::atomic<bool> g_started, g_mixing;
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
        for (auto& kv : g_streams)
            kv.second->Mix(sum, kChunk, 0.6f);   // (a finished stream stays until the game has asked and been told it is over, or stops it)
        int sounding = 0;
        for (auto it = g_voices.begin(); it != g_voices.end();) {
            Voice& v = *it->second;
            v.Mix(sum, kChunk, 0.35f);
            if (v.over)
                v.overFor++;
            else
                sounding++;
            // stopped by the game (it asks no more about it), or over and not asked about for five seconds
            if (v.over && (v.gainStep != 0.0f || v.overFor > 500))
                it = g_voices.erase(it);
            else
                ++it;
        }
        static const bool log = std::getenv("SLY1_SND_LOG") != nullptr;
        static int chunks;
        if (log && ++chunks % 500 == 0)
            std::fprintf(stderr, "sndlog: %d voices sounding of %zu held, %zu streams; %llu started so far\n", sounding, g_voices.size(), g_streams.size(),
                         (unsigned long long)g_voicesStarted);
    }
    for (int k = 0; k < kChunk * 2; k++)
        out[k] = (std::int16_t)(sum[k] < -32768 ? -32768 : sum[k] > 32767 ? 32767 : sum[k]);
}

}  // namespace

void MixerMain()
{
    g_mixing.store(true);
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
    {
        std::vector<std::pair<std::uint64_t, std::uint32_t>> top;
        for (const auto& kv : g_startedBy)
            top.push_back({kv.second, kv.first});
        std::sort(top.rbegin(), top.rend());
        for (std::size_t k = 0; k < top.size() && k < 8; k++)
            std::fprintf(stderr, "audio: bank %u record %u started %llu times\n", top[k].second >> 16, top[k].second & 0xFFFF, (unsigned long long)top[k].first);
    }
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
    std::fprintf(stderr, "audio: %.1f s written to %s; %llu commands, %llu of them not understood; %llu voices started, %llu of them with nothing to play\n",
                 g_wav.size() / 2.0 / kRate, g_wavPath, (unsigned long long)g_commands, (unsigned long long)g_unknown, (unsigned long long)g_voicesStarted,
                 (unsigned long long)g_voicesEmpty);
}

std::uint32_t LoadBank(std::uint32_t sector)
{
    auto bank = std::make_shared<Bank>();
    bank->sector = sector;
    std::uint8_t head[2048];
    if (ReadSectors(sector, 1, head)) {
        auto le = [](const std::uint8_t* p) { return (std::uint32_t)p[0] | (std::uint32_t)p[1] << 8 | (std::uint32_t)p[2] << 16 | (std::uint32_t)p[3] << 24; };
        const std::uint32_t chunks = le(head + 4);
        if (chunks >= 2 && chunks <= 4) {
            const std::uint32_t blockAt = le(head + 8), blockSize = le(head + 12), samplesAt = le(head + 16), samplesSize = le(head + 20);
            const std::uint32_t total = samplesAt + samplesSize;
            if (blockAt < total && blockSize < (8u << 20) && samplesSize < (64u << 20)) {
                std::vector<std::uint8_t> whole(((std::size_t)total + 2047) / 2048 * 2048);
                if (ReadSectors(sector, (std::uint32_t)(whole.size() / 2048), whole.data())) {
                    bank->block.assign(whole.begin() + blockAt, whole.begin() + blockAt + blockSize);
                    bank->samples.assign(whole.begin() + samplesAt, whole.begin() + samplesAt + samplesSize);
                    if (bank->block.size() >= 0x34 && std::memcmp(bank->block.data(), "SBlk", 4) == 0) {
                        const std::uint8_t* b = bank->block.data();
                        bank->global = true;
                        bank->records = b[0x16] | b[0x17] << 8;
                        bank->tones = b[0x18] | b[0x19] << 8;
                        bank->recordAt = le(b + 0x1C);
                        bank->toneAt = le(b + 0x20);
                    }
                }
            }
        }
    }
    std::lock_guard<std::mutex> lk(g_lock);
    const std::uint32_t handle = g_nextBank++;
    g_banks[handle] = bank;
    static const bool log = std::getenv("SLY1_SND_LOG") != nullptr;
    if (log)
        std::fprintf(stderr, "sndlog: bank %u at sector %u: %s, %u records, %u tones, %zu bytes of samples\n", handle, sector, bank->global ? "global" : "a level's (not played yet)",
                     bank->records, bank->tones, bank->samples.size());
    return handle;
}

namespace {
// The pan the game sends: an angle in degrees, 0 ahead, 90 to the right, 270 to the left.
void SetPan(Voice& v, std::int32_t degrees)
{
    const float s = std::sin((float)degrees * 3.14159265f / 180.0f);
    v.left = std::sqrt(0.5f * (1.0f - s));
    v.right = std::sqrt(0.5f * (1.0f + s));
}

// A tone's sample rate from its word at +0x0A. A reading (docs/AUDIO.md): the word is taken as a
// pitch on a scale of equal ratios, its commonest value (0x42AA, 1,242 of 1,429 tones) as
// 22,050 Hz and its second (0x7CB0) as an octave above, 44,100 Hz; the values between then fall
// near 24,000, 32,000 and 37,800 Hz.
float ToneRate(const std::uint8_t* tone)
{
    const int word = tone[10] | tone[11] << 8;
    return 22050.0f * std::pow(2.0f, (float)(word - 0x42AA) / (float)(0x7CB0 - 0x42AA));
}

// A bank's record started: a voice of its tones. Of the tones' kinds only two are acted on: 1
// plays a sample; 25 is followed by a count of tones of which one is played, taken at random
// (docs/AUDIO.md: a reading of the bytes). The others are passed over.
std::shared_ptr<Voice> StartVoice(const std::shared_ptr<Bank>& bank, std::uint32_t record)
{
    auto v = std::make_shared<Voice>();
    if (!bank || !bank->global || record >= bank->records)
        return v;
    auto le = [](const std::uint8_t* p) { return (std::uint32_t)p[0] | (std::uint32_t)p[1] << 8 | (std::uint32_t)p[2] << 16 | (std::uint32_t)p[3] << 24; };
    const std::uint8_t* r = bank->block.data() + bank->recordAt + record * 12;
    if (bank->recordAt + (record + 1) * 12 > bank->block.size())
        return v;
    const std::uint32_t count = r[4] | r[5] << 8;
    const std::int32_t first = (std::int32_t)le(r + 8);
    if (first < 0)
        return v;
    const float recordVolume = (float)r[0] / 127.0f;
    static std::uint32_t seed = 12345;
    for (std::uint32_t k = 0; k < count; k++) {
        const std::size_t at = bank->toneAt + (std::size_t)first + (std::size_t)k * 0x28;
        if (at + 0x28 > bank->block.size())
            break;
        const std::uint8_t* t = bank->block.data() + at;
        const std::uint32_t kind = le(t);
        if (kind == 25) {   // one of the next n
            const std::uint32_t n = t[8] | t[9] << 8;
            if (n >= 1 && k + n < count + 1) {
                seed = seed * 1664525u + 1013904223u;
                const std::uint32_t pick = (seed >> 16) % n;
                const std::uint8_t* c = t + (std::size_t)(1 + pick) * 0x28;
                if (at + (std::size_t)(2 + pick) * 0x28 <= bank->block.size() && le(c) == 1) {
                    Note note;
                    note.bank = bank;
                    note.at = le(c + 0x18);
                    note.volume = recordVolume * (float)c[9] / 127.0f;
                    note.rate = ToneRate(c);
                    v->notes.push_back(note);
                }
                k += n;
            }
            continue;
        }
        if (kind == 21)
            v->program = true;   // a record that goes round (21 to 22) and plays nothing here: held until stopped
        if (kind != 1)
            continue;
        Note note;
        note.bank = bank;
        note.at = le(t + 0x18);
        note.volume = recordVolume * (float)t[9] / 127.0f;
        note.rate = ToneRate(t);
        v->notes.push_back(note);
    }
    return v;
}
}  // namespace

namespace {
std::uint32_t CommandInner(std::int32_t cmd, const std::uint8_t* data, std::int32_t size);
}

std::uint32_t Command(std::int32_t cmd, const std::uint8_t* data, std::int32_t size)
{
    std::lock_guard<std::mutex> lk(g_lock);
    // SLY1_SND_VOICES=1: every command about a bank's voice with its answer and the time, for
    // reading how the game drives them
    static const bool trace = std::getenv("SLY1_SND_VOICES") != nullptr;
    const std::uint32_t result = CommandInner(cmd, data, size);
    if (trace && cmd != 0x1A && cmd != 0x4E && cmd != 0x09) {
        static const auto t0 = std::chrono::steady_clock::now();
        std::fprintf(stderr, "sndv: %8.3f cmd %02X", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), (unsigned)cmd);
        for (int k = 0; k * 4 + 4 <= size && k < 6; k++)
            std::fprintf(stderr, " %08X", Word(data, size, k));
        std::fprintf(stderr, " -> %08X\n", result);
    }
    return result;
}

namespace {
std::uint32_t CommandInner(std::int32_t cmd, const std::uint8_t* data, std::int32_t size)
{
    g_commands++;
    if (!g_channelSet) {
        for (float& v : g_channelVolume)
            v = 1.0f;
        g_channelSet = true;
    }
    const std::uint32_t a = Word(data, size, 0), b = Word(data, size, 1), c = Word(data, size, 2);
    {   // SLY1_SND_LOG: what is sent about a stream, each time (a question's answer only when it changes)
        static const bool log = std::getenv("SLY1_SND_LOG") != nullptr;
        static std::map<std::uint32_t, int> lastAnswer;
        if (log && cmd != 0x2C && size >= 4 && g_streams.count(a)) {
            Stream* st = g_streams[a].get();
            const bool question = cmd == 0x19 || cmd == 0x2E || cmd == 0x4F;
            const int answer = st->done ? 0 : 1;
            if (!question || lastAnswer[a * 256 + (std::uint32_t)cmd] != answer + 1) {
                std::fprintf(stderr, "sndlog: stream %X: command %02X %08X %08X; at %.1f s of %.1f, %s%s\n", a, (unsigned)cmd, b, c,
                             st->rate ? st->at / 16.0 * 28 / st->rate : 0.0, st->rate ? st->dataBytes / 16.0 * 28 / st->rate : 0.0, st->paused ? "paused" : "playing",
                             st->done ? ", over" : "");
                lastAnswer[a * 256 + (std::uint32_t)cmd] = answer + 1;
            }
        }
        if (log && (cmd == 0x16 || cmd == 0x17 || cmd == 0x34))
            std::fprintf(stderr, "sndlog: command %02X %08X with %zu streams\n", (unsigned)cmd, a, g_streams.size());
    }
    switch (cmd) {
    case 0x00:   // the packet shared with the driver
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
    case 0x11: { // a bank's sound started: the bank's handle, the record, a volume (0x400 is 1; 0: set after), a pan, a pitch
        auto bank = g_banks.find(a);
        std::shared_ptr<Voice> v = StartVoice(bank == g_banks.end() ? nullptr : bank->second, b);
        const std::uint32_t handle = g_nextHandle++;
        g_voicesStarted++;
        g_startedBy[a << 16 | (b & 0xFFFF)]++;
        if (v->notes.empty()) {
            g_voicesEmpty++;
            if (!v->program)
                return handle;   // nothing to play (a level's bank, or a record with no sample): over at once
        }
        if (c != 0)
            v->volume = (float)(std::int32_t)c / 1024.0f;
        const std::int32_t pitch = (std::int32_t)Word(data, size, 4);
        if (pitch != 0)
            v->pitch = std::pow(2.0f, (float)pitch / 1524.0f);
        g_voices[handle] = v;
        return handle;
    }
    case 0x1A: { // whether a bank's sound goes on until it is stopped: the game keeps such a sound's
                 // record and starts it again whenever it is found over (0x1BFCD8), so a sound
                 // that ends by itself must answer 0. Read from the record's word at +6, which is
                 // set for the records whose samples loop (118 of 119 in the seven global banks)
        auto bank = g_banks.find(a);
        if (bank == g_banks.end() || !bank->second->global || b >= bank->second->records)
            return 0;
        const Bank& k = *bank->second;
        const std::size_t at = k.recordAt + (std::size_t)b * 12;
        if (at + 12 > k.block.size())
            return 0;
        return (k.block[at + 6] | k.block[at + 7] << 8) != 0 ? 1 : 0;
    }
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
        if (g_voices.count(a))
            g_voices[a]->paused = true;
        return 0;
    case 0x14:   // and resumed
        if (Stream* s = Find(a))
            s->paused = false;
        if (g_voices.count(a))
            g_voices[a]->paused = false;
        return 0;
    case 0x15:   // a voice stopped: a bank's over a hundredth of a second, with no click
        g_streams.erase(a);
        if (g_voices.count(a))
            g_voices[a]->gainStep = 1.0f / (kRate * 0.01f);
        return 0;
    case 0x16:   // voices suspended by mask, and resumed: all of them
        for (auto& kv : g_streams)
            kv.second->paused = true;
        for (auto& kv : g_voices)
            kv.second->paused = true;
        return 0;
    case 0x17:
        for (auto& kv : g_streams)
            kv.second->paused = false;
        for (auto& kv : g_voices)
            kv.second->paused = false;
        return 0;
    case 0x19:   // whether a voice is still playing
    case 0x2E:
    case 0x4F: {
        auto voice = g_voices.find(a);
        if (voice != g_voices.end()) {
            if (voice->second->over) {
                g_voices.erase(voice);
                return 0;
            }
            return 1;
        }
        Stream* s = Find(a);
        if (!s)
            return 0;
        if (!g_mixing.load()) {
            // no mixer (the host with no file to write): the stream is over when its length has
            // passed, so that what waits for it goes on as it would
            if (!s->opened && !s->Open())
                s->done = true;
            const double length = (double)s->dataBytes / 16.0 * 28.0 / (double)s->rate;
            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - s->began).count() >= length)
                s->done = true;
        }
        if (s->done) {
            g_streams.erase(a);
            return 0;
        }
        return 1;
    }
    case 0x1B:   // a voice's volume (0x400 is 1) and pan (degrees)
        if (Stream* s = Find(a))
            s->volume = (float)(std::int32_t)b / 1024.0f;
        if (g_voices.count(a)) {
            g_voices[a]->volume = (float)(std::int32_t)b / 1024.0f;
            SetPan(*g_voices[a], (std::int32_t)c);
        }
        return 0;
    case 0x1E:   // a voice's pitch: 1,524 an octave (the game adds what it asked for as the sound's base, answered 0)
        if (g_voices.count(a))
            g_voices[a]->pitch = std::pow(2.0f, (float)(std::int32_t)b / 1524.0f);
        return 0;
    case 0x29:   // a voice's parameter set
        return 0;
    case 0x22:   // a voice faded out: over a fifth of a second until its time's unit is known
        if (Stream* s = Find(a))
            s->fadeStep = 1.0f / (kRate * 0.2f);
        if (g_voices.count(a))
            g_voices[a]->gainStep = 1.0f / (kRate * 0.2f);
        return 0;
    case 0x28:   // a voice's parameter asked for
        return 0;
    case 0x34:   // every sound stopped
        g_streams.clear();
        g_voices.clear();
        return 0;
    case 0x06:   // a bank unloaded
        g_banks.erase(a);
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
}  // namespace

}  // namespace audio
}  // namespace port
