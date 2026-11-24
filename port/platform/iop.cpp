// SPDX-License-Identifier: AGPL-3.0-or-later
// The IOP's services as the port answers them (port/iop.h).
#include "port/iop.h"

#if defined(__SWITCH__)
namespace port { void CheckDevTable(const char* where); }
#else
namespace port { inline void CheckDevTable(const char*) {} }
#endif
#include <cstring>

#include "port/ptr32.h"

// The sound client's armed reply buffer and its word count (iop_commands.cpp's g_iopReplyBuffer,
// a game pointer, and g_iopReplyWordCount), at their retail addresses.
static std::uint32_t ReplyBuffer() { return *static_cast<std::uint32_t*>(port::Native(0x0025C340)); }
static std::int32_t ReplyWordCount() { return *static_cast<std::int32_t*>(port::Native(0x0025C344)); }

#include "port/audio.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <utility>

namespace port {

namespace {
constexpr std::uint32_t kSoundCommands = 0x123456;   // the sound driver's command channel
std::mutex g_lock;
std::map<const RemoteCallClient*, std::uint32_t> g_services;
std::set<std::pair<std::uint32_t, std::int32_t>> g_logged;
}  // namespace

void BindIopService(RemoteCallClient* client, std::uint32_t service)
{
    std::lock_guard<std::mutex> lock(g_lock);
    g_services[client] = service;
    port::CheckDevTable("an IOP service bound");
    std::fprintf(stderr, "iop: bound service 0x%08X\n", service);
}

void CallIopService(RemoteCallClient* client, std::int32_t cmd, const void* send, std::int32_t sendSize, void* recv,
                    std::int32_t recvSize)
{
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_services.find(client);
    std::uint32_t service = it == g_services.end() ? 0 : it->second;
    {   // SLY1_SND_LOG=1: each command with the words it sends (the first 40 of each), for the sound engine's design
        static const bool log = std::getenv("SLY1_SND_LOG") != nullptr;
        static std::map<std::pair<std::uint32_t, std::int32_t>, int> seen;
        if (log && seen[{service, cmd}]++ < 40) {
            std::fprintf(stderr, "sndlog: %08X cmd %02X:", service, (unsigned)cmd);
            for (std::int32_t k = 0; send && k + 4 <= sendSize && k < 40; k += 4) {
                std::uint32_t w;
                std::memcpy(&w, static_cast<const char*>(send) + k, 4);
                std::fprintf(stderr, " %08X", w);
            }
            std::fprintf(stderr, "\n");
        }
    }
    if (g_logged.insert({service, cmd}).second) {   // (each command once)
        port::CheckDevTable("an IOP command");
        std::fprintf(stderr, "iop: service 0x%08X command 0x%X (send %d, receive %d bytes)\n",
                     service, (unsigned)cmd, (int)sendSize, (int)recvSize);
    }
    // The sound driver's command channel: the driver writes its reply straight into the buffer the
    // game armed, between two marker words 0xFFFFFFFF (IsIopReplyReady waits for both): one word
    // for a command sent alone, or one for each command of a buffer of them (service command
    // 0x4D: a count, then each command as two halfwords, its number and its data's length, and
    // the data, padded to four bytes). port/audio answers each.
    if (service == kSoundCommands && ReplyBuffer() != 0) {
        std::uint32_t* reply = static_cast<std::uint32_t*>(Native(ReplyBuffer()));
        std::int32_t n = ReplyWordCount();
        std::memset(reply + 1, 0, sizeof(std::uint32_t) * (std::size_t)(n > 0 ? n : 0));
        const std::uint8_t* bytes = static_cast<const std::uint8_t*>(send);
        if (cmd == 0x4D && bytes && sendSize >= 4) {
            std::int32_t count;
            std::memcpy(&count, bytes, 4);
            std::int32_t at = 4;
            for (std::int32_t k = 0; k < count && k < n && at + 4 <= sendSize; k++) {
                std::int16_t one, len;
                std::memcpy(&one, bytes + at, 2);
                std::memcpy(&len, bytes + at + 2, 2);
                if (len < 0 || at + 4 + len > sendSize)
                    break;
                reply[1 + k] = audio::Command(one, bytes + at + 4, len);
                at += 4 + ((len + 3) & ~3);
            }
        } else if (n >= 1) {
            reply[1] = audio::Command(cmd, bytes, sendSize);
        }
        reply[0] = 0xFFFFFFFFu;
        reply[n + 1] = 0xFFFFFFFFu;
    } else if (service == kSoundCommands) {
        audio::Command(cmd, static_cast<const std::uint8_t*>(send), sendSize);
    }
    // The second service loads a sound bank from its first sector (command 3) and answers with the
    // bank's handle, in the call's own reply.
    if (service == 0x123457 && cmd == 3 && send && sendSize >= 4 && recv && recvSize >= 4) {
        std::uint32_t sector;
        std::memcpy(&sector, send, 4);
        const std::uint32_t handle = audio::LoadBank(sector);
        std::memcpy(recv, &handle, 4);
    }
}

}  // namespace port
