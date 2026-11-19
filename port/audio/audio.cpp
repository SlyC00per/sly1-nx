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
    bool level = false;                   // a level's bank (`SBv2`): instruments and sequences
    std::vector<std::uint8_t> sequences;  // its third chunk
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
    // the record's program (docs/AUDIO.md): its tones taken in order, a step each
    std::shared_ptr<Bank> bank;
    std::size_t toneAt = 0;       // the first tone's place in the bank's block
    std::uint32_t tones = 0, pc = 0;
    std::int32_t loopPc = -1;     // the step a round starts at, once passed
    std::int32_t waitFrames = 0;  // output frames until the next step
    float recordVolume = 1.0f;
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
        Step(frames);
        program = pc < tones;
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
        for (std::size_t k = 0; k < notes.size();)   // (a round goes on adding notes)
            if (notes[k].over)
                notes.erase(notes.begin() + (std::ptrdiff_t)k);
            else
                k++;
    }

    void Play(const std::uint8_t* tone);
    void Step(int frames);
};

// ---------------------------------------------------------------------------------------------
// A level's music (docs/AUDIO.md, "The level music"). A level bank holds instruments (a record
// each: tones, each a sample over a range of keys) and sequences: MIDI event streams (a note's
// end is 0xDn and its key) at 480 ticks a beat, with the music's own control messages in
// `F0 75 ... F7`. The first sequence is only such messages: it sets the bank's registers,
// keeps blocks of messages and groups of channels, and starts a sequence. The sequences then
// go from one to the next by their own messages, some by the game's sound level (command
// 0x27) and the eight registers the game sets (command 0x29). All of it is read from the 45
// banks' bytes and the game's calls, not measured: see the notes for what each reading rests on.
int g_soundLevel = 0x14;   // what the game last sent as the sound level (0x14 is its 0)

struct Song {
    struct Playing {   // a note sounding
        Note note;
        int track = 0, channel = 0, key = 0;
        const std::uint8_t* tone = nullptr;
        float left = 0.0f, right = 0.0f, gain = 1.0f, release = 0.0f;
        bool released = false;
    };
    struct Track {     // a sequence being played
        int index = 0;
        std::size_t at = 0, end = 0;   // in the bank's sequences
        double ticks = 0.0;            // run, since the last event
        std::uint32_t wait = 0;        // ticks from the last event to the next
        std::uint32_t tempo = 500000, perBeat = 480;
        std::uint8_t status = 0;
        std::uint8_t program[16] = {};
        float bend[16] = {};           // of the range, -1 to 1
        bool on[16];
        bool done = false, waiting = false;
        int id = 0;
        int next = -1;                 // the sequence its messages chose to go on with
    };
    std::shared_ptr<Bank> bank;
    std::vector<Track> tracks;
    std::vector<Playing> notes;
    std::map<int, std::vector<std::uint8_t>> blocks;
    std::map<int, std::vector<std::uint8_t>> groups;   // triples: a channel and the levels it sounds between
    std::int32_t reg[8] = {};
    float volume = 1.0f, fade = 1.0f, fadeStep = 0.0f;
    bool paused = false, over = false;
    int nextId = 1;
    std::vector<int> starts;   // sequences to start once the messages being read are done

    bool Sequence(int index, std::size_t* at, std::size_t* end, std::uint32_t* tempo, std::uint32_t* perBeat) const
    {
        const std::vector<std::uint8_t>& q = bank->sequences;
        auto le = [&](std::size_t o) { return (std::uint32_t)q[o] | (std::uint32_t)q[o + 1] << 8 | (std::uint32_t)q[o + 2] << 16 | (std::uint32_t)q[o + 3] << 24; };
        if (q.size() < 0x24)
            return false;
        const int count = q[0x17];
        if (index < 0 || index >= count || 0x20 + (std::size_t)count * 4 > q.size())
            return false;
        const std::size_t from = 0x10 + le(0x20 + (std::size_t)index * 4);
        const std::size_t to = index + 1 < count ? 0x10 + le(0x20 + (std::size_t)(index + 1) * 4) : q.size();
        if (from + 0xDC > to || to > q.size())
            return false;
        *at = from + 0xDC;
        *end = to;
        *tempo = le(from + 0x20);
        *perBeat = le(from + 0x24);
        return *tempo != 0 && *perBeat != 0;
    }

    void StartTrack(int index)
    {
        Track t;
        if (!Sequence(index, &t.at, &t.end, &t.tempo, &t.perBeat) || tracks.size() >= 8)
            return;
        for (const Track& playing : tracks)   // one that is playing is not started again beside itself
            if (playing.index == index && !playing.done)
                return;
        t.index = index;
        t.id = nextId++;
        for (bool& b : t.on)
            b = true;
        t.waiting = false;
        tracks.push_back(t);
        static const bool log = std::getenv("SLY1_SND_LOG") != nullptr;
        if (log)
            std::fprintf(stderr, "sndlog: music: sequence %d started (sound level %d; registers %d %d %d %d)\n", index, g_soundLevel, reg[0], reg[1], reg[2], reg[3]);
    }

    void ReleaseTrack(int id)
    {
        for (Playing& n : notes)
            if (n.track == id && !n.released) {
                n.released = true;
                n.release = 1.0f / (kRate * 0.05f);
            }
    }

    void StopSequence(int index)
    {
        for (Track& t : tracks)
            if (t.index == index && !t.done) {
                t.done = true;
                ReleaseTrack(t.id);
            }
    }

    // The control messages: one statement at p, done or only passed over. Returns the place
    // after it, or null where the bytes are not understood (the rest is left).
    const std::uint8_t* Statement(const std::uint8_t* p, const std::uint8_t* end, bool run, Track* track, int depth)
    {
        if (p >= end || depth > 8)
            return nullptr;
        const std::uint8_t op = *p++;
        auto need = [&](int n) { return p + n <= end; };
        switch (op) {
        case 0x00:   // if the sound level is at least this
        case 0x02:   // if it is under this
            if (!need(1))
                return nullptr;
            {
                const bool is = op == 0x00 ? g_soundLevel >= p[0] : g_soundLevel < p[0];
                return Statement(p + 1, end, run && is, track, depth + 1);
            }
        case 0x06:   // if a register is under a value, over it, or it (`07 00 00 S` is used: over 0)
        case 0x07:
        case 0x16:
            if (!need(2))
                return nullptr;
            {
                const std::int32_t r = p[0] < 8 ? reg[p[0]] : 0, v = p[1];
                const bool is = op == 0x06 ? r < v : op == 0x07 ? r > v : r == v;
                return Statement(p + 2, end, run && is, track, depth + 1);
            }
        case 0x04:   // statements up to 0x05, as one
            while (p < end && *p != 0x05) {
                p = Statement(p, end, run, track, depth + 1);
                if (!p)
                    return nullptr;
            }
            return p < end ? p + 1 : nullptr;
        case 0x13:   // a register set
            if (!need(2))
                return nullptr;
            if (run && p[0] < 8)
                reg[p[0]] = p[1];
            return p + 2;
        case 0x11:   // the sequence to go on with, in this one's place
        case 0x0D:   // the same, the one a register names (kept as the sequence's number and one)
            if (!need(1))
                return nullptr;
            if (run) {
                const int to = op == 0x11 ? p[0] : (p[0] < 8 ? reg[p[0]] - 1 : -1);
                if (track)
                    track->next = to;
                else
                    starts.push_back(to);
            }
            return p + 1;
        case 0x12:   // a sequence started beside this one
            if (!need(1))
                return nullptr;
            if (run)
                starts.push_back(p[0]);
            return p + 1;
        case 0x03:   // a sequence stopped
            if (!need(1))
                return nullptr;
            if (run) {
                if (track && track->index == p[0])
                    track->done = true, ReleaseTrack(track->id);
                else
                    StopSequence(p[0]);
            }
            return p + 1;
        case 0x0C:   // a kept block's statements
            if (!need(1))
                return nullptr;
            if (run) {
                auto b = blocks.find(p[0]);
                if (b != blocks.end()) {
                    const std::uint8_t* q = b->second.data();
                    const std::uint8_t* qEnd = q + b->second.size();
                    while (q && q < qEnd)
                        q = Statement(q, qEnd, true, track, depth + 1);
                }
            }
            return p + 1;
        case 0x10:   // a group's channels switched by the sound level
            if (!need(1))
                return nullptr;
            if (run && track) {
                auto g = groups.find(p[0]);
                if (g != groups.end())
                    for (std::size_t k = 0; k + 3 <= g->second.size(); k += 3) {
                        const std::uint8_t* c = &g->second[k];
                        if (c[0] < 16)
                            track->on[c[0]] = g_soundLevel >= c[1] && g_soundLevel <= c[2];
                    }
            }
            return p + 1;
        case 0x14:   // not understood, one byte each
        case 0x0E:
            return need(1) ? p + 1 : nullptr;
        default:
            return nullptr;
        }
    }

    void Script(const std::uint8_t* p, const std::uint8_t* end, Track* track)
    {
        while (p && p < end)
            p = Statement(p, end, true, track, 0);
    }

    // The first sequence: messages only, in parts ended by 0xF7. 0x0B keeps the rest of its part
    // as a block, 0x0F as a group; the others are done at once.
    void Begin()
    {
        std::size_t at, end;
        std::uint32_t tempo, perBeat;
        if (!Sequence(0, &at, &end, &tempo, &perBeat))
            return;
        const std::uint8_t* q = bank->sequences.data();
        std::size_t p = at;
        while (p + 2 < end && !(q[p] == 0xF0 && q[p + 1] == 0x75))
            p++;
        p += 2;
        while (p < end) {
            std::size_t stop = p;
            while (stop < end && q[stop] != 0xF7)
                stop++;
            if (stop >= end)
                break;
            const std::uint8_t* s = q + p;
            const std::uint8_t* e = q + stop;
            while (s && s < e) {
                if (*s == 0x0B && s + 2 <= e) {
                    blocks[s[1]].assign(s + 2, e);
                    break;
                }
                if (*s == 0x0F && s + 3 <= e) {
                    groups[s[1]].assign(s + 3, e);
                    break;
                }
                s = Statement(s, e, true, nullptr, 0);
            }
            p = stop + 1;
        }
        Starts();
    }

    void Starts()
    {
        std::vector<int> now;
        now.swap(starts);
        for (int index : now)
            StartTrack(index);
    }

    void NoteOn(Track& t, int channel, int key, int velocity)
    {
        if (!t.on[channel] || notes.size() >= 48)
            return;
        const std::uint32_t program = t.program[channel];
        const std::vector<std::uint8_t>& b = bank->block;
        if (program >= bank->records || bank->recordAt + (program + 1) * 8 > b.size())
            return;
        const std::uint8_t* r = &b[bank->recordAt + program * 8];
        const std::uint32_t first = (std::uint32_t)r[4] | (std::uint32_t)r[5] << 8 | (std::uint32_t)r[6] << 16 | (std::uint32_t)r[7] << 24;
        for (int k = 0; k < r[0]; k++) {
            const std::size_t o = (std::size_t)first + (std::size_t)k * 24;
            if (o + 24 > b.size())
                break;
            const std::uint8_t* tone = &b[o];
            if (key < tone[6] || key > tone[7])
                continue;
            Playing n;
            n.track = t.id;
            n.channel = channel;
            n.key = key;
            n.tone = tone;
            n.note.bank = bank;
            n.note.at = (std::uint32_t)tone[0x10] | (std::uint32_t)tone[0x11] << 8 | (std::uint32_t)tone[0x12] << 16 | (std::uint32_t)tone[0x13] << 24;
            n.note.rate = Rate(tone, key, t.bend[channel]);
            const float level = (float)velocity / 127.0f * (float)tone[1] / 127.0f * (float)r[1] / 127.0f;
            const float sine = std::sin((float)(std::int16_t)(tone[4] | tone[5] << 8) * 3.14159265f / 180.0f);
            n.left = level * std::sqrt(0.5f * (1.0f - sine));
            n.right = level * std::sqrt(0.5f * (1.0f + sine));
            notes.push_back(n);
        }
    }

    // A tone's rate for a key: the SPU's 48,000 Hz where the key is the tone's note (the negative
    // of its signed byte at +2, less its fine part at +3 in 128ths), a semitone a key; a bend
    // moves it by the tone's range (+8 down, +9 up, in semitones).
    static float Rate(const std::uint8_t* tone, int key, float bend)
    {
        const float centre = (float)-(std::int8_t)tone[2] - (float)tone[3] / 128.0f;
        const float range = bend < 0.0f ? (float)tone[8] : (float)tone[9];
        return 48000.0f * std::pow(2.0f, ((float)key - centre + bend * range) / 12.0f);
    }

    void NoteOff(Track& t, int channel, int key)
    {
        for (Playing& n : notes)
            if (n.track == t.id && n.channel == channel && n.key == key && !n.released) {
                // the release from the tone's second envelope word: its low five bits a rate, a
                // step of 8 of 32,768 each 2^(rate - 11) samples (the SPU's linear release)
                const int rate = n.tone[0x0C] & 0x1F;
                const float frames = 4096.0f * std::pow(2.0f, (float)(rate - 11));
                n.released = true;
                n.release = 1.0f / (frames < 96.0f ? 96.0f : frames > kRate * 4.0f ? kRate * 4.0f : frames);
            }
    }

    // A track's events up to now.
    void Run(Track& t, double ticks)
    {
        const std::vector<std::uint8_t>& q = bank->sequences;
        t.ticks += ticks;
        int guard = 0;
        while (!t.done && guard++ < 4096) {
            if (!t.waiting) {   // the next event's time
                std::uint32_t v = 0;
                while (t.at < t.end) {
                    const std::uint8_t c = q[t.at++];
                    v = v << 7 | (c & 0x7F);
                    if (c < 0x80)
                        break;
                }
                t.wait = v;
                t.waiting = true;
            }
            if (t.ticks < (double)t.wait)
                break;
            t.ticks -= (double)t.wait;
            t.waiting = false;
            if (t.at >= t.end) {
                t.done = true;
                break;
            }
            std::uint8_t c = q[t.at];
            if (c == 0xF0) {
                std::size_t stop = t.at + 1;
                while (stop < t.end && q[stop] != 0xF7)
                    stop++;
                if (stop < t.end && t.at + 2 <= stop && q[t.at + 1] == 0x75)
                    Script(&q[t.at + 2], &q[stop], &t);
                t.at = stop + 1;
                continue;
            }
            if (c == 0xFF) {
                if (t.at + 3 > t.end) {
                    t.done = true;
                    break;
                }
                const std::uint8_t type = q[t.at + 1], size = q[t.at + 2];
                if (type == 0x51 && size == 3 && t.at + 6 <= t.end)
                    t.tempo = (std::uint32_t)q[t.at + 3] << 16 | (std::uint32_t)q[t.at + 4] << 8 | q[t.at + 5];
                t.at += 3 + (std::size_t)size;
                if (type == 0x2F)
                    t.done = true;
                continue;
            }
            if (c >= 0x80) {
                t.status = c;
                t.at++;
            }
            const int kind = t.status >> 4, channel = t.status & 15;
            const int size = kind == 0xC || kind == 0xD ? 1 : 2;
            if (kind < 8 || t.at + (std::size_t)size > t.end) {
                t.done = true;
                break;
            }
            const std::uint8_t a = q[t.at], b = size == 2 ? q[t.at + 1] : 0;
            t.at += (std::size_t)size;
            if (kind == 9 && b != 0)
                NoteOn(t, channel, a, b);
            else if (kind == 9 || kind == 8 || kind == 0xD)
                NoteOff(t, channel, a);
            else if (kind == 0xC)
                t.program[channel] = a;
            else if (kind == 0xE) {
                t.bend[channel] = (float)((a | b << 7) - 8192) / 8192.0f;
                for (Playing& n : notes)
                    if (n.track == t.id && n.channel == channel)
                        n.note.rate = Rate(n.tone, n.key, t.bend[channel]);
            }
        }
        if (t.done) {
            if (t.next >= 0)
                starts.push_back(t.next);
        } else if (t.next >= 0) {   // its messages chose another before its end
            t.done = true;
            ReleaseTrack(t.id);
            starts.push_back(t.next);
        }
    }

    void Mix(std::int32_t* out, int frames, float master)
    {
        if (paused || over)
            return;
        constexpr int kStep = 96;   // the events' grain: 2 ms
        for (int from = 0; from < frames; from += kStep) {
            const int n = frames - from < kStep ? frames - from : kStep;
            for (std::size_t k = 0; k < tracks.size(); k++) {
                Track& t = tracks[k];
                if (!t.done)
                    Run(t, (double)n / kRate * 1e6 / (double)t.tempo * (double)t.perBeat);
            }
            Starts();
            for (std::size_t k = 0; k < tracks.size();)
                if (tracks[k].done)
                    tracks.erase(tracks.begin() + (std::ptrdiff_t)k);
                else
                    k++;
            const float g = volume * master * fade;
            for (Playing& p : notes) {
                Note& note = p.note;
                if (note.over)
                    continue;
                const double step = (double)note.rate / kRate;
                for (int k = 0; k < n; k++) {
                    while (note.phase >= 1.0) {
                        note.prev = note.next;
                        if (!note.Sample(&note.next)) {
                            note.over = true;
                            break;
                        }
                        note.phase -= 1.0;
                    }
                    if (note.over)
                        break;
                    const float v = ((float)note.prev + (float)(note.next - note.prev) * (float)note.phase) * p.gain * g;
                    out[(from + k) * 2] += (std::int32_t)(v * p.left);
                    out[(from + k) * 2 + 1] += (std::int32_t)(v * p.right);
                    note.phase += step;
                    if (p.released) {
                        p.gain -= p.release;
                        if (p.gain <= 0.0f) {
                            note.over = true;
                            break;
                        }
                    }
                }
            }
            for (std::size_t k = 0; k < notes.size();)
                if (notes[k].note.over)
                    notes.erase(notes.begin() + (std::ptrdiff_t)k);
                else
                    k++;
            if (fadeStep != 0.0f) {
                fade -= fadeStep * (float)n;
                if (fade <= 0.0f) {
                    over = true;
                    return;
                }
            }
        }
    }
};

std::map<std::uint32_t, std::shared_ptr<Song>> g_songs;

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
        {   // SLY1_SND_LEVEL=s,v,s,v...: the sound level set to v from second s on (for hearing the music's changes on the host)
            static const char* plan = std::getenv("SLY1_SND_LEVEL");
            static long chunk;
            chunk++;
            if (plan && chunk % 100 == 0) {
                int value = -1;
                for (const char* p = plan; p && *p;) {
                    int at = 0, v = 0;
                    if (std::sscanf(p, "%d,%d", &at, &v) != 2)
                        break;
                    if (chunk / 100 >= at)
                        value = v;
                    p = std::strchr(p, ',');
                    p = p ? std::strchr(p + 1, ',') : nullptr;
                    if (p)
                        p++;
                }
                if (value >= 0)
                    g_soundLevel = value;
            }
        }
        for (auto& kv : g_songs)
            kv.second->Mix(sum, kChunk, 0.3f);
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
            const std::uint32_t seqAt = chunks >= 3 ? le(head + 24) : 0, seqSize = chunks >= 3 ? le(head + 28) : 0;
            const std::uint32_t total = seqSize && seqSize < (8u << 20) && seqAt >= samplesAt + samplesSize ? seqAt + seqSize : samplesAt + samplesSize;
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
                    } else if (bank->block.size() >= 0x50 && std::memcmp(bank->block.data(), "SBv2", 4) == 0 && seqSize && seqAt + seqSize <= whole.size()) {
                        const std::uint8_t* b = bank->block.data();
                        bank->level = true;
                        bank->records = b[0x16] | b[0x17] << 8;
                        bank->tones = b[0x18] | b[0x19] << 8;
                        bank->recordAt = le(b + 0x20);
                        bank->toneAt = le(b + 0x24);
                        bank->sequences.assign(whole.begin() + seqAt, whole.begin() + seqAt + seqSize);
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
        std::fprintf(stderr, "sndlog: bank %u at sector %u: %s, %u records, %u tones, %zu bytes of samples\n", handle, sector, bank->global ? "global" : bank->level ? "a level's" : "not understood",
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

// A tone's sample rate from its two bytes at +0x0A: a note (a signed byte, negative: the note the
// sample plays at its own speed, less one) and a fine part in 128ths of a semitone, read as the
// level banks' tones show them (docs/AUDIO.md): a sound is played as note 60, and the SPU's
// 48,000 Hz is note and fine equal. The commonest pair (86, 66) is then 11,025 Hz, and the
// others 22,050 (74, 66), 16,000 (80, 124) and 8,000 (92, 98). SLY1_SFX_OCTAVE=1 plays them
// an octave up, for telling by ear whether a sound is played as note 72.
float ToneRate(const std::uint8_t* tone)
{
    static const float octave = std::getenv("SLY1_SFX_OCTAVE") ? 2.0f : 1.0f;
    const float centre = (float)-(std::int8_t)tone[10] - (float)tone[11] / 128.0f;
    return 48000.0f * octave * std::pow(2.0f, (60.0f - centre) / 12.0f);
}

// A bank's record started: a voice that runs the record's tones as a program, a step each
// (docs/AUDIO.md; the kinds are readings of the bytes):
//    1  a sample started (the program goes on at once)
//   26  a wait: the word at +8 and a random part up to the word at +4, in 240ths of a second
//   25  one of the next n tones taken at random (n at +8), the others passed over; one that is
//       not a sample is silence
//   21  a round's start, 22 its end: back to the start
// The others (27, 31, 4, 5, 20 and the rare ones) are passed over.
std::shared_ptr<Voice> StartVoice(const std::shared_ptr<Bank>& bank, std::uint32_t record)
{
    auto v = std::make_shared<Voice>();
    if (!bank || !bank->global || record >= bank->records)
        return v;
    auto le = [](const std::uint8_t* p) { return (std::uint32_t)p[0] | (std::uint32_t)p[1] << 8 | (std::uint32_t)p[2] << 16 | (std::uint32_t)p[3] << 24; };
    if (bank->recordAt + (record + 1) * 12 > bank->block.size())
        return v;
    const std::uint8_t* r = bank->block.data() + bank->recordAt + record * 12;
    const std::uint32_t count = r[4] | r[5] << 8;
    const std::int32_t first = (std::int32_t)le(r + 8);
    if (first < 0 || bank->toneAt + (std::size_t)first + (std::size_t)count * 0x28 > bank->block.size())
        return v;
    v->bank = bank;
    v->toneAt = bank->toneAt + (std::size_t)first;
    v->tones = count;
    v->recordVolume = (float)r[0] / 127.0f;
    v->Step(0);   // what starts with the voice
    v->program = v->pc < v->tones;
    return v;
}
}  // namespace

void Voice::Play(const std::uint8_t* tone)
{
    Note note;
    note.bank = bank;
    note.at = (std::uint32_t)tone[0x18] | (std::uint32_t)tone[0x19] << 8 | (std::uint32_t)tone[0x1A] << 16 | (std::uint32_t)tone[0x1B] << 24;
    note.volume = recordVolume * (tone[9] > 127 ? 1.0f : (float)tone[9] / 127.0f);
    note.rate = ToneRate(tone);
    notes.push_back(note);
}

void Voice::Step(int frames)
{
    static std::uint32_t seed = 12345;
    auto random = [&](std::uint32_t n) {   // 0 to n - 1
        seed = seed * 1664525u + 1013904223u;
        return n ? (seed >> 8) % n : 0;
    };
    auto word = [](const std::uint8_t* p) { return (std::int32_t)((std::uint32_t)p[0] | (std::uint32_t)p[1] << 8 | (std::uint32_t)p[2] << 16 | (std::uint32_t)p[3] << 24); };
    waitFrames -= frames;
    int steps = 0;
    while (pc < tones && waitFrames <= 0 && steps++ < 256) {
        const std::uint8_t* t = bank->block.data() + toneAt + (std::size_t)pc * 0x28;
        const std::int32_t kind = word(t);
        pc++;
        if (kind == 1) {
            if (notes.size() < 16)
                Play(t);
        } else if (kind == 26) {
            const std::int32_t base = word(t + 8), more = word(t + 4);
            const std::int32_t ticks = (base > 0 ? base : 0) + (more > 0 ? (std::int32_t)random((std::uint32_t)more + 1) : 0);
            waitFrames += (std::int32_t)((std::int64_t)ticks * kRate / 240);
        } else if (kind == 25) {
            const std::uint32_t n = t[8] | t[9] << 8;
            if (n >= 1 && pc + n <= tones) {
                const std::uint8_t* c = t + (std::size_t)(1 + random(n)) * 0x28;
                if (word(c) == 1 && notes.size() < 16)
                    Play(c);
                pc += n;
            }
        } else if (kind == 21) {
            loopPc = (std::int32_t)pc;
        } else if (kind == 22) {
            if (loopPc >= 0) {
                pc = (std::uint32_t)loopPc;
                if (waitFrames <= 0)
                    waitFrames = kRate / 240;   // (a round with no wait in it)
            }
        }
    }
}

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
    case 0x3B: case 0x3C: case 0x3D:
        return 0;
    case 0x27:   // the sound level: what the level's music goes by
        g_soundLevel = (std::int32_t)a;
        return 0;
    case 0x09:   // a volume channel's value, 0x400 is 1
        if (a < 16)
            g_channelVolume[a] = (float)(std::int32_t)b / 1024.0f;
        return 0;
    case 0x0A:   // a volume channel's value asked for
        return a < 16 ? (std::uint32_t)(g_channelVolume[a] * 1024.0f) : 0;
    case 0x11: { // a bank's sound started: the bank's handle, the record, a volume (0x400 is 1; 0: set after), a pan, a pitch
        auto bank = g_banks.find(a);
        if (bank != g_banks.end() && bank->second->level) {   // a level bank's: its music
            auto song = std::make_shared<Song>();
            song->bank = bank->second;
            if (c != 0)
                song->volume = (float)(std::int32_t)c / 1024.0f;
            song->Begin();
            g_songs.clear();   // one level's music at a time: the last level's ends where the next one's starts
            const std::uint32_t handle = g_nextHandle++;
            g_songs[handle] = song;
            g_voicesStarted++;
            return handle;
        }
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
        if (g_songs.count(a))
            g_songs[a]->paused = true;
        return 0;
    case 0x14:   // and resumed
        if (Stream* s = Find(a))
            s->paused = false;
        if (g_voices.count(a))
            g_voices[a]->paused = false;
        if (g_songs.count(a))
            g_songs[a]->paused = false;
        return 0;
    case 0x15:   // a voice stopped: a bank's over a hundredth of a second, with no click
        g_streams.erase(a);
        g_songs.erase(a);
        if (g_voices.count(a))
            g_voices[a]->gainStep = 1.0f / (kRate * 0.01f);
        return 0;
    case 0x16:   // voices suspended by mask, and resumed: all of them
        for (auto& kv : g_streams)
            kv.second->paused = true;
        for (auto& kv : g_voices)
            kv.second->paused = true;
        for (auto& kv : g_songs)
            kv.second->paused = true;
        return 0;
    case 0x17:
        for (auto& kv : g_streams)
            kv.second->paused = false;
        for (auto& kv : g_voices)
            kv.second->paused = false;
        for (auto& kv : g_songs)
            kv.second->paused = false;
        return 0;
    case 0x19:   // whether a voice is still playing
    case 0x2E:
    case 0x4F: {
        if (g_songs.count(a))
            return g_songs[a]->over ? 0 : 1;
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
        if (g_songs.count(a))
            g_songs[a]->volume = (float)(std::int32_t)b / 1024.0f;
        return 0;
    case 0x1E:   // a voice's pitch: 1,524 an octave (the game adds what it asked for as the sound's base, answered 0)
        if (g_voices.count(a))
            g_voices[a]->pitch = std::pow(2.0f, (float)(std::int32_t)b / 1524.0f);
        return 0;
    case 0x29:   // one of the music's eight registers set: the voice, the register, the value
        if (g_songs.count(a) && b < 8)
            g_songs[a]->reg[b] = (std::int32_t)c;
        return 0;
    case 0x22:   // a voice faded out: over a fifth of a second until its time's unit is known
        if (Stream* s = Find(a))
            s->fadeStep = 1.0f / (kRate * 0.2f);
        if (g_voices.count(a))
            g_voices[a]->gainStep = 1.0f / (kRate * 0.2f);
        if (g_songs.count(a))
            g_songs[a]->fadeStep = 1.0f / (kRate * 0.2f);
        return 0;
    case 0x28:   // a register asked for
        return g_songs.count(a) && b < 8 ? (std::uint32_t)g_songs[a]->reg[b] : 0;
    case 0x34:   // every sound stopped
        // (the level's music goes on: the game stops it by its voice, and after a restart of the
        // level it does not start it again)
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
