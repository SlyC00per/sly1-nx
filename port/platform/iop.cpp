// SPDX-License-Identifier: AGPL-3.0-or-later
// The IOP's services as the port answers them (port/iop.h).
#include "port/iop.h"

#include <cstring>

#include "port/ptr32.h"

// The sound client's armed reply buffer and its word count (iop_commands.cpp's g_iopReplyBuffer,
// a game pointer, and g_iopReplyWordCount).
extern "C" std::uint32_t D_0025C340;
extern "C" std::int32_t D_0025C344;

#include <cstdio>
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
    std::fprintf(stderr, "iop: bound service 0x%08X\n", service);
}

void CallIopService(RemoteCallClient* client, std::int32_t cmd, const void*, std::int32_t sendSize, void*,
                    std::int32_t recvSize)
{
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_services.find(client);
    std::uint32_t service = it == g_services.end() ? 0 : it->second;
    if (g_logged.insert({service, cmd}).second)
        std::fprintf(stderr, "iop: service 0x%08X command 0x%X (send %d, receive %d bytes)\n",
                     service, (unsigned)cmd, (int)sendSize, (int)recvSize);
    // The sound driver's command channel: the driver writes its reply straight into the buffer the
    // game armed, between two marker words 0xFFFFFFFF (IsIopReplyReady waits for both). Until the
    // sound engine answers (milestone 3), the reply words are zero.
    if (service == kSoundCommands && D_0025C340 != 0) {
        std::uint32_t* reply = static_cast<std::uint32_t*>(Native(D_0025C340));
        std::int32_t n = D_0025C344;
        std::memset(reply + 1, 0, sizeof(std::uint32_t) * (std::size_t)(n > 0 ? n : 0));
        reply[0] = 0xFFFFFFFFu;
        reply[n + 1] = 0xFFFFFFFFu;
    }
}

}  // namespace port
