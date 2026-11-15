// SPDX-License-Identifier: AGPL-3.0-or-later
// The platform layer: the rest of the library calls the game makes (sly1-decomp src/lib; their
// roles in config/names/lib). C++ linkage, with the parameter types the game declares.
//
// Memory and strings: the C library's. Random numbers: the library's own generator, exact
// (state * 0x41C64E6D + 0x3039, the low 31 bits), seeded with the record's initial seed from the
// player's executable. Files and IOP modules: the port has no IOP; module loads and resets
// succeed, the console file handles go to the log. Memory card: no card yet (the PS3 release's
// saves without a card are milestone 2, docs/PS3_PARITY.md). MPEG: no decoder yet (milestone 3):
// every movie ends at once. The GS paths: always idle.
#include <cstdio>
#include <cstring>

#include "common.h"
#include "port/iso.h"

struct ModuleBlob;
struct MpegHandle;
struct MpegStreamPacket;

void* AllocateObjectMemory(u32 n);

namespace {

// The library state record's seed (+0x58 of the record at 0x276680), from the executable.
u32 g_seed;
bool g_seedLoaded;

void LoadSeed()
{
    if (!g_seedLoaded) {
        g_seedLoaded = true;
        if (!port::ReadExecutable(0x276680 + 0x58, &g_seed, 4))
            g_seed = 1;
    }
}

}  // namespace

// --- memory, strings, random numbers

s32 CompareMemory(const void* a, const void* b, s32 n)
{
    const u8* p = (const u8*)a;
    const u8* q = (const u8*)b;
    for (s32 i = 0; i < n; i++)
        if (p[i] != q[i])
            return (s32)p[i] - (s32)q[i];
    return 0;
}

void* MoveMemory(void* dst, void* src, s32 n) { return std::memmove(dst, src, (size_t)n); }

void SeedRandom(u32 value)
{
    g_seedLoaded = true;
    g_seed = value;
}

u32 NextRandom()
{
    LoadSeed();
    g_seed = g_seed * 0x41C64E6Du + 0x3039u;
    return g_seed & 0x7FFFFFFFu;
}

char* FindSubstring(char* s, char* t) { return std::strstr(s, t); }

// --- the IOP's file client and module loader

s32 ForgetFileClient() { return 0; }
s32 CloseFile(s32) { return 0; }
s32 ReadFile(s32, void*, s32) { return 0; }

// The console's handles (stdout 1, stderr 2) go to the log; others are dropped.
s32 WriteFile(s32 handle, void* buf, s32 size)
{
    if ((handle == 1 || handle == 2) && size > 0)
        std::fwrite(buf, 1, (size_t)size, stderr);
    return size;
}

s32 ControlFile(s32, s32, u32*) { return 0; }
s32 ForgetModuleLoader() { return 0; }
s32 LoadIopModuleFile(ModuleBlob*, s32, ModuleBlob*) { return 0; }
s32 IsIopResetDone() { return 1; }
s32 ResetIop(char*) { return 1; }

// --- the C++ runtime

// The host's runtime runs the game's static constructors before main.
// The game's global constructors, once: the list at 0x0027B130 in retail's data (its count, then
// the static initialisers by retail address), walked from its end as retail's runtime does. The
// ones the decompilation writes as functions are called; the ones g++ generated from C++
// initialisers are the host compiler's here and ran before main, on the initial values start-up
// copies into main memory; the library's (its exception frames' registration) has no role.
void RunConstructorsOnce()
{
    static bool done;
    if (done)
        return;
    done = true;
    const u32* list = (const u32*)port::Native(0x0027B130u);
    int called = 0, skipped = 0;
    for (u32 i = list[0]; i >= 1; i--) {
        void* fn = port::FindFunction(list[i]);
        if (fn) {
            ((void (*)())fn)();
            called++;
        } else {
            skipped++;
        }
    }
    std::fprintf(stderr, "sly1: %d global constructors run, %d the host's own\n", called, skipped);
}

void* AllocateOrThrow(u32 size)
{
    void* p = AllocateObjectMemory(size);
    if (!p) {
        std::fprintf(stderr, "port: out of game memory (bad_alloc of %u bytes)\n", (unsigned)size);
        std::abort();
    }
    return p;
}

// --- the GS paths: always idle

void ResetGsPaths() {}
s32 WaitGsPaths(s32, s32) { return 0; }

// --- the memory card: none yet

namespace {
s32 g_cardOp;
}

s32 InitMemoryCardClient() { return 1; }
s32 OpenCardFile(s32, s32, const char*, s32) { g_cardOp = 2; return 0; }
s32 MakeCardDirectory(s32, s32, const char*) { g_cardOp = 2; return 0; }
s32 CloseCardFile(s32) { g_cardOp = 3; return 0; }
s32 ReadCardFile(s32, void*, s32) { g_cardOp = 5; return 0; }
s32 WriteCardFile(s32, const void*, s32) { g_cardOp = 6; return 0; }
s32 GetCardInfo(s32, s32, s32* type, s32* free, s32* format)
{
    g_cardOp = 1;
    if (type) *type = 0;
    if (free) *free = 0;
    if (format) *format = 0;
    return 0;
}
s32 ReadCardDirectory(s32, s32, const char*, s32, s32, void*) { g_cardOp = 0xD; return 0; }
s32 RequestCardFormat(s32, s32) { g_cardOp = 0x10; return 0; }
s32 RequestCardSpaceForPath(s32, s32, const char*) { g_cardOp = 0x12; return 0; }

// The request in flight has ended: its operation, and the result "no card" (-10).
s32 SyncCardRequest(s32, s32* op, s32* result)
{
    if (op) *op = g_cardOp;
    if (result) *result = -10;
    return 1;
}

// --- MPEG: no decoder yet

s32 DemultiplexBuffer(MpegHandle*, s32, s32) { return 0; }
// Returns the callback it replaces: none.
void* SetStreamCallback(MpegHandle*, s32, s32, s32 (*)(MpegHandle*, MpegStreamPacket*, void*), void*) { return nullptr; }
void InitMpegLibrary() {}
s32 CreateMpegDecoder(MpegHandle*, u8*, u32) { return 1; }
s32 DeleteMpegDecoder(MpegHandle*) { return 1; }
s32 DecodeMpegPicture(MpegHandle*, void*, s32) { return 0; }
s32 GetMpegDecoderStatus(MpegHandle*) { return 0; }
void ResetMpegDecoder(MpegHandle*) {}
void* SetMpegCallback(MpegHandle*, s32, void*, void*) { return nullptr; }
