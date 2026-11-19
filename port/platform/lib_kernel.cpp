// SPDX-License-Identifier: AGPL-3.0-or-later
// The platform layer: the EE kernel's calls for the data cache and transfers to the IOP (the
// threads, semaphores and interrupts are port/platform/kernel.cpp's), and the IOP's remote calls (sly1-decomp src/lib's
// system_calls.cpp and remote_call.cpp; their roles in config/names/lib).
//
// The IOP is not emulated. A bind succeeds at once (the client's server word becomes nonzero, as
// the game's loops wait for); a call completes at once, with its reply zeroed and its callback
// run, so nothing the game polls stays pending. The services' own answers (sound, files, the
// pad) are the port's to give: port/iop.h routes each call to the service it was bound to.
#include <cstdio>
#include <cstring>

#include "common.h"
#include "remote_call_client.h"
#include "port/audio.h"
#include "port/iop.h"
#include "port/system.h"

extern "C" {

// The data cache: the port has none to write back.
void SyncDataCache(s32) {}

// Transfers to the IOP complete at once: a transfer's id is never still running.
s32 GetIopDmaStatus(void*) { return -1; }
void* StartIopDma(void* transfers, s32 count)
{
    // a transfer is four words: the address here, the IOP's, the size, a flag. The only ones the
    // game makes are a movie's sound into the driver's ring (movie_player.cpp, WriteIopRingBlock).
    const u32* t = (const u32*)transfers;
    for (s32 k = 0; k < count; k++, t += 4)
        port::audio::MovieRingWrite(t[1], (const std::uint8_t*)port::Native(t[0]), t[2]);
    return (void*)1;
}

// --- the IOP's remote calls

void InitRemoteCalls(s32) {}

s32 BindRemoteService(RemoteCallClient* self, u32 cmd, s32 mode)
{
    (void)mode;
    self->packet = {};
    self->f10 = 0;
    self->server = 1;          // bound: the word the game's loops wait to become nonzero
    port::BindIopService(self, cmd);
    return 0;
}

s32 CallRemoteService(RemoteCallClient* self, s32 cmd, s32 mode, void* send, s32 sendSize, void* recv,
                      s32 recvSize, RemoteCallEndFn callback, void* callbackArg)
{
    (void)mode;
    if (recv && recvSize > 0)
        std::memset(recv, 0, (size_t)recvSize);
    port::CallIopService(self, cmd, send, sendSize, recv, recvSize);
    if (callback)
        callback(callbackArg);
    return 0;
}

s32 IsRemoteCallPending(RemoteCallClient*) { return 0; }

}  // extern "C"
