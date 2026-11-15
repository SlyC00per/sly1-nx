// SPDX-License-Identifier: AGPL-3.0-or-later
// The IOP's services as the port answers them. The game binds a client to a service id
// (BindRemoteService) and calls it with a command, a send buffer and a receive buffer
// (CallRemoteService); the port routes each call to the service the client was bound to. The
// sound driver's commands (src/main/iop_commands.cpp, docs/DISC.md "Sound banks") are the main
// ones, answered by the sound engine (milestone 3); until then every call is logged once per
// service and command and its reply left zeroed.
#ifndef PORT_IOP_H
#define PORT_IOP_H

#include <cstdint>

struct RemoteCallClient;

namespace port {

void BindIopService(RemoteCallClient* client, std::uint32_t service);
void CallIopService(RemoteCallClient* client, std::int32_t cmd, const void* send, std::int32_t sendSize,
                    void* recv, std::int32_t recvSize);

}  // namespace port

#endif
