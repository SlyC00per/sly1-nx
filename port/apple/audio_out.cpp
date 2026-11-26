// SPDX-License-Identifier: AGPL-3.0-or-later
// The sound output on Apple's systems: an audio queue (AudioToolbox, the Mac's and the iPhone's
// alike) that plays what the mixer hands it. The mixer's thread makes 10 ms at a time
// (port/audio/audio.cpp's MixerMain) and is paced by Write, which waits while the queue has
// enough to go on with; the queue's own thread takes from a ring between them, and plays
// silence when the ring is empty.
#include "port/apple.h"

#include <AudioToolbox/AudioToolbox.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "port/audio.h"

namespace port {
namespace apple {

namespace {

constexpr int kRate = 48000;
constexpr int kBufferFrames = 480;     // 10 ms a buffer of the queue's
constexpr int kBuffers = 3;
constexpr int kAhead = 3 * 480;        // the mixer waits while the ring holds this much (30 ms)
constexpr int kRingFrames = 1 << 14;

std::mutex g_lock;
std::condition_variable g_room;
std::vector<std::int16_t> g_ring(kRingFrames * 2);
int g_read, g_held;   // frames
AudioQueueRef g_queue;
unsigned long long g_underruns;

void Fill(void*, AudioQueueRef queue, AudioQueueBufferRef buffer)
{
    std::int16_t* out = static_cast<std::int16_t*>(buffer->mAudioData);
    {
        std::lock_guard<std::mutex> lk(g_lock);
        const int take = g_held < kBufferFrames ? g_held : kBufferFrames;
        for (int k = 0; k < take; k++) {
            const int at = (g_read + k) % kRingFrames;
            out[k * 2] = g_ring[at * 2];
            out[k * 2 + 1] = g_ring[at * 2 + 1];
        }
        if (take < kBufferFrames) {
            std::memset(out + take * 2, 0, (std::size_t)(kBufferFrames - take) * 4);
            g_underruns++;
        }
        g_read = (g_read + take) % kRingFrames;
        g_held -= take;
    }
    g_room.notify_one();
    buffer->mAudioDataByteSize = kBufferFrames * 4;
    AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
}

// The mixer's: frames of 16-bit stereo, taken once the queue has room for them (or, if it has
// stopped taking any, after a fifth of a second, dropped: the mixer keeps its time).
void Write(const std::int16_t* stereo, int frames)
{
    std::unique_lock<std::mutex> lk(g_lock);
    if (!g_room.wait_for(lk, std::chrono::milliseconds(200), [] { return g_held <= kAhead; }))
        return;
    for (int k = 0; k < frames && g_held < kRingFrames; k++) {
        const int at = (g_read + g_held) % kRingFrames;
        g_ring[at * 2] = stereo[k * 2];
        g_ring[at * 2 + 1] = stereo[k * 2 + 1];
        g_held++;
    }
}

}  // namespace

// What the output did, for the run's report.
void AudioReport()
{
    if (g_queue)
        std::fprintf(stderr, "audio: the queue found the ring short %llu times\n", g_underruns);
}

bool StartAudio()
{
    AudioStreamBasicDescription format;
    std::memset(&format, 0, sizeof format);
    format.mSampleRate = kRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    format.mChannelsPerFrame = 2;
    format.mBitsPerChannel = 16;
    format.mBytesPerFrame = 4;
    format.mFramesPerPacket = 1;
    format.mBytesPerPacket = 4;
    OSStatus status = AudioQueueNewOutput(&format, Fill, nullptr, nullptr, nullptr, 0, &g_queue);
    if (status != noErr) {
        std::fprintf(stderr, "audio: no audio queue (status %d)\n", (int)status);
        return false;
    }
    for (int b = 0; b < kBuffers; b++) {
        AudioQueueBufferRef buffer;
        if (AudioQueueAllocateBuffer(g_queue, kBufferFrames * 4, &buffer) != noErr)
            return false;
        std::memset(buffer->mAudioData, 0, kBufferFrames * 4);
        buffer->mAudioDataByteSize = kBufferFrames * 4;
        AudioQueueEnqueueBuffer(g_queue, buffer, 0, nullptr);
    }
    if (const char* volume = std::getenv("SLY1_SOUND_VOLUME"))   // SLY1_SOUND_VOLUME=N: hundredths (0: played, and not heard)
        AudioQueueSetParameter(g_queue, kAudioQueueParam_Volume, (Float32)std::atoi(volume) / 100.0f);
    status = AudioQueueStart(g_queue, nullptr);
    if (status != noErr) {
        std::fprintf(stderr, "audio: the audio queue did not start (status %d)\n", (int)status);
        return false;
    }
    audio::SetOutput(Write);
    std::fprintf(stderr, "audio: an audio queue at %d Hz, %d ms a buffer\n", kRate, kBufferFrames * 1000 / kRate);
    return true;
}

}  // namespace apple
}  // namespace port
