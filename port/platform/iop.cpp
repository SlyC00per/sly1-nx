// SPDX-License-Identifier: AGPL-3.0-or-later
// The IOP's services as the port answers them (port/iop.h).
#include "port/iop.h"

#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <utility>

namespace port {

namespace {
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
        std::fprintf(stderr, "iop: service 0x%08X command 0x%X (send %d, receive %d bytes): not answered yet\n",
                     service, (unsigned)cmd, (int)sendSize, (int)recvSize);
}

}  // namespace port
