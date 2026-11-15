// SPDX-License-Identifier: AGPL-3.0-or-later
// The platform layer: the EE kernel's calls the game makes (semaphores, thread ids, the data
// cache, transfers to the IOP), and the IOP's remote calls (sly1-decomp src/lib's
// system_calls.cpp and remote_call.cpp; their roles in config/names/lib).
//
// The IOP is not emulated. A bind succeeds at once (the client's server word becomes nonzero, as
// the game's loops wait for); a call completes at once, with its reply zeroed and its callback
// run, so nothing the game polls stays pending. The services' own answers (sound, files, the
// pad) are the port's to give: port/iop.h routes each call to the service it was bound to.
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "common.h"
#include "remote_call_client.h"
#include "semaphore_params.h"
#include "port/iop.h"
#include "port/system.h"

namespace {

struct Semaphore {
    bool used = false;
    s32 count = 0;
    s32 max = 0;
    std::mutex m;
    std::condition_variable cv;
};

constexpr int kSemaphores = 256;
Semaphore g_semaphores[kSemaphores];
std::mutex g_semaphoreTable;

Semaphore* Find(s32 id) { return id >= 1 && id < kSemaphores && g_semaphores[id].used ? &g_semaphores[id] : nullptr; }

}  // namespace

extern "C" {

s32 GetCurrentThreadId(void) { return port::CurrentThreadId(); }

s32 NewSemaphore(void* self)
{
    const SemaphoreParams* p = (const SemaphoreParams*)self;
    std::lock_guard<std::mutex> lock(g_semaphoreTable);
    for (int i = 1; i < kSemaphores; i++) {
        if (!g_semaphores[i].used) {
            g_semaphores[i].used = true;
            g_semaphores[i].count = p->initCount;
            g_semaphores[i].max = p->maxCount;
            return i;
        }
    }
    std::fprintf(stderr, "port: out of semaphores\n");
    return -1;
}

s32 PostSemaphore(void* a)
{
    Semaphore* s = Find((s32)port::GameAddr(a));
    if (!s)
        return -1;
    {
        std::lock_guard<std::mutex> lock(s->m);
        if (s->max <= 0 || s->count < s->max)
            s->count++;
    }
    s->cv.notify_one();
    return (s32)port::GameAddr(a);
}

void PostSemaphoreFromInterrupt(s32 sema) { PostSemaphore(port::FromGameAddr<void>((u32)sema)); }

s32 AwaitSemaphore(void* a)
{
    Semaphore* s = Find((s32)port::GameAddr(a));
    if (!s)
        return -1;
    std::unique_lock<std::mutex> lock(s->m);
    s->cv.wait(lock, [s] { return s->count > 0; });
    s->count--;
    return (s32)port::GameAddr(a);
}

// The data cache: the port has none to write back.
void SyncDataCache(s32) {}

// Transfers to the IOP complete at once: a transfer's id is never still running.
s32 GetIopDmaStatus(void*) { return -1; }
void* StartIopDma(void*, s32) { return (void*)1; }

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
