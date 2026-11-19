// SPDX-License-Identifier: AGPL-3.0-or-later
// The MPEG library's calls, as the game's movie player makes them (sly1-decomp
// src/main/movie_player.cpp; the library is src/lib/mpeg_*.cpp, which drives the PS2's IPU and
// is not built here). docs/VIDEO.md has the whole path. What this file is:
//
//  - the demultiplexer: the movie is an MPEG-2 program stream; each packet of the video
//    stream and of the private stream that carries the sound is handed to the handler the
//    game registered for it, which may refuse it (no room yet), and then it is offered again;
//  - the decoder's side: the game sends the video stream on to the IPU by DMA
//    (IpuDmaSink::Put, where a named patch calls port::movie::VideoBytes), and asks for a
//    picture at a time; the pictures come from port/video/mpeg2.cpp, in display order, and
//    are written as rows of 32-bit pixels, which the game uploads to the GS (in one transfer:
//    a named patch, where the IPU's conversion left 16 by 16 blocks) and draws.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "common.h"
#include "port/mpeg2.h"
#include "port/ptr32.h"
#include "port/retail_data.h"

struct MpegHandle;
struct MpegStreamPacket;

namespace {

// The packet a stream's handler is given, in retail's layout (include/mpeg_decoder.h's
// MpegStreamPacket, movie_player.cpp's MovieDataBlock): addresses are the game's 32-bit ones.
struct Packet {
    std::uint32_t kind;      // 6: a packet
    std::uint32_t packet;    // its start
    std::uint32_t payload;   // its payload's start
    std::int32_t payloadSize;
    std::uint64_t pts, dts;
};

typedef s32 (*StreamFn)(MpegHandle*, void*, void*);
typedef s32 (*EventFn)(void*, void*, void*);

struct State {
    port::video::Mpeg2 decoder;
    StreamFn stream[4] = {};
    void* streamArg[4] = {};
    EventFn event[8] = {};
    void* eventArg[8] = {};
    std::uint8_t* work = nullptr;   // the library's work memory, the game's: a packet cut by the buffer's end is put together here
    std::uint32_t workSize = 0;
    std::uint32_t carried = 0;      // bytes of such a packet held in it
    bool ended = false;
    long pictures = 0, fed = 0, packets[2] = {};
    double decodeSeconds = 0.0, convertSeconds = 0.0;
};
State* g_state;

std::uint32_t Be16(const std::uint8_t* p) { return (std::uint32_t)p[0] << 8 | p[1]; }

// What a program stream's unit at p is: its whole length when its header is all there (0 when
// more bytes are needed to tell), and for a packet of a stream the payload's place.
std::uint32_t UnitLength(const std::uint8_t* p, std::uint32_t have, int* stream, std::uint32_t* payload)
{
    *stream = -1;
    if (have < 4)
        return 0;
    const std::uint8_t code = p[3];
    if (code == 0xBA) {   // a pack's header: 14 bytes and its stuffing (MPEG-2), 12 (MPEG-1)
        if (have < 14)
            return 0;
        return (p[4] >> 6) == 1 ? 14 + (p[13] & 7u) : 12;
    }
    if (code == 0xB9)   // the program's end
        return 4;
    if (code < 0xBB)    // (a video start code out of place: passed over)
        return 4;
    if (have < 6)
        return 0;
    const std::uint32_t length = 6 + Be16(p + 4);
    if (code == 0xE0 || code == 0xBD) {
        if (have < 9)
            return 0;
        *stream = code == 0xE0 ? 0 : 2;
        *payload = (p[6] >> 6) == 2 ? 9 + p[8] : 6;   // an MPEG-2 packet's header says its own length
        if (*payload > length)
            *payload = length;
    }
    return length;
}

// A whole packet offered to its stream's handler; false when the handler refuses it (no room
// yet: it is offered again).
bool Offer(State& s, MpegHandle* handle, int stream, const std::uint8_t* p, std::uint32_t length, std::uint32_t payload)
{
    if (stream < 0 || !s.stream[stream])
        return true;
    Packet packet = {};
    packet.kind = 6;
    packet.packet = port::AddrOf(const_cast<std::uint8_t*>(p));
    packet.payload = port::AddrOf(const_cast<std::uint8_t*>(p + payload));
    packet.payloadSize = (std::int32_t)(length - payload);
    const s32 taken = s.stream[stream](handle, &packet, s.streamArg[stream]);
    if (taken == 0)
        return false;
    s.packets[stream == 0 ? 0 : 1]++;
    return true;
}

inline std::uint8_t Clip(int v) { return (std::uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

// A picture written as 32-bit pixels, a row after another (the game's UploadMovieFrame, as patched,
// sends them to the GS so; the IPU's own conversion wrote 16 by 16 blocks). The conversion is
// ITU-R BT.601's from the video range, each colour sample for the four pixels it covers; alpha
// 0x80, the GS's 1.
void Convert(const port::video::Picture& pic, std::uint8_t* out)
{
    for (int y = 0; y < pic.height; y++) {
        const std::uint8_t* ys = pic.y + (std::size_t)y * (std::size_t)pic.stride;
        const std::uint8_t* cbs = pic.cb + (std::size_t)(y / 2) * (std::size_t)pic.chromaStride;
        const std::uint8_t* crs = pic.cr + (std::size_t)(y / 2) * (std::size_t)pic.chromaStride;
        for (int x = 0; x < pic.width; x += 2) {
            const int d = cbs[x >> 1] - 128, e = crs[x >> 1] - 128;
            const int r = 409 * e + 128, g = -100 * d - 208 * e + 128, b = 516 * d + 128;
            for (int k = 0; k < 2; k++) {
                const int c = 298 * (ys[x + k] - 16);
                out[0] = Clip((c + r) >> 8);
                out[1] = Clip((c + g) >> 8);
                out[2] = Clip((c + b) >> 8);
                out[3] = 0x80;
                out += 4;
            }
        }
    }
}

}  // namespace

namespace port {
namespace movie {

// The video stream's bytes on their way to the IPU (a named patch in IpuDmaSink::Put).
void VideoBytes(const void* bytes, int size)
{
    if (g_state && size > 0) {
        g_state->decoder.Feed(static_cast<const std::uint8_t*>(bytes), (std::size_t)size);
        g_state->fed += size;
    }
}

float Zoom() { return WideFactor(); }

}  // namespace movie
}  // namespace port

void InitMpegLibrary() {}

s32 CreateMpegDecoder(MpegHandle* handle, u8* work, u32 size)
{
    delete g_state;
    g_state = new State();
    g_state->work = work;
    g_state->workSize = size;
    std::memset(handle, 0, 0x48);
    return 1;
}

s32 DeleteMpegDecoder(MpegHandle*)
{
    if (g_state)
        std::fprintf(stderr, "movie: %ld pictures shown from %ld bytes of video in %ld packets, and %ld packets of sound; %ld errors in the stream; %.1f ms a picture decoding, %.1f converting\n",
                     g_state->pictures, g_state->fed, g_state->packets[0], g_state->packets[1], g_state->decoder.Errors(),
                     g_state->pictures ? g_state->decodeSeconds * 1000.0 / (double)g_state->pictures : 0.0, g_state->pictures ? g_state->convertSeconds * 1000.0 / (double)g_state->pictures : 0.0);
    delete g_state;
    g_state = nullptr;
    return 1;
}

void ResetMpegDecoder(MpegHandle*)
{
    if (g_state) {
        g_state->decoder.Reset();
        g_state->carried = 0;
    }
}

// The handler of a stream's packets: 0 the video (0xE0), 2 the private stream with the sound
// (0xBD). Returns the one it replaces: none.
void* SetStreamCallback(MpegHandle*, s32 stream, s32, s32 (*handler)(MpegHandle*, MpegStreamPacket*, void*), void* arg)
{
    if (g_state && stream >= 0 && stream < 4) {
        g_state->stream[stream] = (StreamFn)handler;
        g_state->streamArg[stream] = arg;
    }
    return nullptr;
}

// The decoder's own callbacks: 1 is asked for more of the video stream (FlushMovieToIpu).
void* SetMpegCallback(MpegHandle*, s32 kind, void* handler, void* arg)
{
    if (g_state && kind >= 0 && kind < 8) {
        g_state->event[kind] = (EventFn)handler;
        g_state->eventArg[kind] = arg;
    }
    return nullptr;
}

// Bytes of the program stream given to the demultiplexer: how many it took. It stops at a packet
// its handler refuses (the bytes before it are taken, and it is given again), and keeps the
// start of a packet that the bytes end inside.
s32 DemultiplexBuffer(MpegHandle* handle, s32 address, s32 count)
{
    State* s = g_state;
    if (!s || count <= 0)
        return 0;
    const std::uint8_t* in = static_cast<const std::uint8_t*>(port::Native((std::uint32_t)address));
    std::uint32_t taken = 0;
    const std::uint32_t have = (std::uint32_t)count;
    int stream;
    std::uint32_t payload = 0;

    // a packet begun before: completed from these bytes, then offered (and offered again, the next
    // time, if it is refused)
    while (s->carried) {
        if (s->carried >= 3 && !(s->work[0] == 0 && s->work[1] == 0 && s->work[2] == 1)) {   // not a start code after all
            std::memmove(s->work, s->work + 1, --s->carried);
            continue;
        }
        if (s->carried >= 4 && s->work[3] == 0xB9)   // the program's end, cut by a buffer's end: nothing more is taken
            return (s32)taken;
        const std::uint32_t length = UnitLength(s->work, s->carried, &stream, &payload);
        if (length == 0 || s->carried < length) {
            const std::uint32_t want = length == 0 ? 1 : length - s->carried;   // (its header a byte at a time)
            const std::uint32_t n = want < have - taken ? want : have - taken;
            if (n == 0 || s->carried + n > s->workSize)
                return (s32)taken;
            std::memcpy(s->work + s->carried, in + taken, n);
            s->carried += n;
            taken += n;
            continue;
        }
        if (!Offer(*s, handle, stream, s->work, length, payload))
            return (s32)taken;
        s->carried = 0;
    }

    while (taken < have) {
        const std::uint8_t* p = in + taken;
        const std::uint32_t left = have - taken;
        if (left >= 3 && !(p[0] == 0 && p[1] == 0 && p[2] == 1)) {   // not a start code: on to the next byte
            taken++;
            continue;
        }
        if (left >= 4 && p[3] == 0xB9)   // the program's end: left where it is, as the library leaves it, so that
            return (s32)taken;           // every call from here on takes nothing (the game then stops feeding)
        const std::uint32_t length = left >= 3 ? UnitLength(p, left, &stream, &payload) : 0;
        if (length == 0 || length > left) {   // it ends past these bytes: kept for the next ones
            if (left > s->workSize)
                return (s32)taken;
            std::memcpy(s->work, p, left);
            s->carried = left;
            return (s32)have;
        }
        if (!Offer(*s, handle, stream, p, length, payload))
            return (s32)taken;
        taken += length;
    }
    return (s32)taken;
}

// The next picture, converted into `picture` (as many 16 by 16 blocks as the picture has, of at
// most `blocks`): 0, or -1 when the sequence is over. More of the stream is asked for through
// callback 1 until a picture is whole.
s32 DecodeMpegPicture(MpegHandle* handle, void* picture, s32 blocks)
{
    State* s = g_state;
    if (!s || s->ended)
        return -1;
    int dry = 0;
    for (;;) {
        port::video::Picture pic;
        const auto t0 = std::chrono::steady_clock::now();
        const bool got = s->decoder.Next(&pic);
        s->decodeSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (got) {
            if ((pic.width / 16) * (pic.height / 16) > blocks || (pic.width & 15) || (pic.height & 15)) {
                std::fprintf(stderr, "movie: a picture of %d by %d is not one the player has room for\n", pic.width, pic.height);
                s->ended = true;
                return -1;
            }
            const auto t1 = std::chrono::steady_clock::now();
            Convert(pic, static_cast<std::uint8_t*>(picture));
            s->convertSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
            s32* head = reinterpret_cast<s32*>(handle);
            head[0] = pic.width;    // MovieDecoder::width, height, and skipPicture: 0, it is to be shown
            head[1] = pic.height;
            head[2] = 0;
            s->pictures++;
            return 0;
        }
        if (s->decoder.Over() || !s->event[1]) {
            s->ended = true;
            return -1;
        }
        const long before = s->fed;
        s->event[1](handle, nullptr, s->eventArg[1]);
        // (a stream that ends without its end code: nothing new comes; what is held is given out)
        dry = s->fed == before ? dry + 1 : 0;
        if (dry > 64)
            s->decoder.Finish();
    }
}

// Non-zero once the sequence is over.
s32 GetMpegDecoderStatus(MpegHandle*) { return !g_state || g_state->ended ? 1 : 0; }
