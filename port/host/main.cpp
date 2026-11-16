// SPDX-License-Identifier: AGPL-3.0-or-later
// sly1_headless: the game on the host, without a window or sound, for the tests.
//
//   sly1_headless --iso DISC.iso [--seconds N]
//
// As retail's entry does (EnterProgram), it starts the game's main loop, RunMainLoop. The game
// runs on a thread of its own whose stack lies in game memory, as every game thread's must: the
// game links stack frames into its global lists (ListFrame), and a stored pointer is a game
// address (port/ptr32.h). The memory retail's entry clears is static here, and zero.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <pthread.h>

#include "port/dmac.h"
#include "port/iso.h"
#include "port/memory_card.h"
#include "port/retail_data.h"
#include "port/system.h"

extern "C" void func_00185758(int argc, int argv);   // RunMainLoop

namespace {

// Page-aligned (16 KiB pages on Apple Silicon): pthread_attr_setstack refuses anything less.
alignas(16384) unsigned char g_mainStack[4 << 20];
// The interrupt handlers are game code too (they link stack frames into game structures), so
// the clock that calls them runs on a stack in game memory as well.
alignas(16384) unsigned char g_clockStack[1 << 20];
std::atomic<bool> g_started;
int g_seconds = 10;

// The run's report: the kernel's threads, the DMA counts, the frame semaphores, the card.
void Report()
{
    port::KernelDump();
    port::DmacDump();
    {   // the display's and the render frames' semaphores and flags, for the report
        auto w = [](std::uint32_t a) { return *(const std::int32_t*)port::Native(a); };
        std::fprintf(stderr, "sly1: semaphores: vblank %d, GS finish %d, frame ready %d, frame done %d; "
                     "render thread waiting %d, GS finish awaited %d\n", w(0x002BE460), w(0x002BE464),
                     w(0x002623B0), w(0x002623B4), w(0x002623B8), w(0x002BE468));
        std::fprintf(stderr, "sly1: memory card: state %d, target %d, display mode %d, free %d KB\n",
                     w(0x00264A40 + 0x0C), w(0x00264A40 + 0x14), w(0x00264A40 + 0x18), w(0x00264A40 + 0x28));
    }
}

// The vertical blank, 60 times a second (NTSC), for the game's handler (cause 2); then the end.
void* Clock(void*)
{
    port::SetGameFloatMode();
    auto next = std::chrono::steady_clock::now();
    auto stop = next + std::chrono::seconds(g_seconds);
    while (std::chrono::steady_clock::now() < stop) {
        next += std::chrono::microseconds(16683);
        std::this_thread::sleep_until(next);
        if (g_started)
            port::RaiseInterrupt(2);
    }
    std::fprintf(stderr, "sly1: stopping after %d s\n", g_seconds);
    Report();
    std::fflush(stderr);
    std::_Exit(0);
}

// --verify-levels: every level of the level table (0x247AF0, 46 records of 0x2C bytes: the
// disc record the load reads, the level code at +0x20, the name at +0x24), queued in turn as the
// warp menu queues a load (the world load request at 0x275AF0); each must load and then run 60
// frames (the display's frame counter at 0x2626C0). The game thread does the load; this host
// thread only watches and writes the request between loads.
int g_verifyFrom = -1;
int g_verifyTo = 46;   // --level N: that level alone

void VerifyLevels()
{
    auto word = [](std::uint32_t a) -> volatile std::int32_t& { return *(volatile std::int32_t*)port::Native(a); };
    constexpr std::uint32_t kLevels = 0x00247AF0, kRequest = 0x00275AF0, kFrames = 0x002626C0;
    auto waitFor = [](auto done, int seconds) {
        auto stop = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (!done()) {
            if (std::chrono::steady_clock::now() > stop)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return true;
    };
    std::this_thread::sleep_for(std::chrono::seconds(3));   // past start-up and the card check
    auto fail = [](const char* what, int i) {
        std::fprintf(stderr, "verify: FAILED at level %d: %s\n", i, what);
        Report();
        std::fflush(stderr);
        std::_Exit(1);
    };
    for (int i = g_verifyFrom; i < g_verifyTo; i++) {
        std::uint32_t entry = kLevels + i * 0x2C;
        std::int32_t code = word(entry + 0x20);
        std::uint32_t name = (std::uint32_t)word(entry + 0x24);
        std::fprintf(stderr, "verify: level %2d/46, code 0x%03X, %s\n", i + 1, code,
                     name ? (const char*)port::Native(name) : "(no name)");
        auto t0 = std::chrono::steady_clock::now();
        if (!waitFor([&] { return word(kRequest) == 0; }, 120))
            fail("the previous load never ended", i + 1);
        word(kRequest + 0x04) = (std::int32_t)entry;   // the key: the level's record
        word(kRequest + 0x08) = -1;
        word(kRequest + 0x0C) = -1;
        word(kRequest + 0x10) = 0;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        word(kRequest) = 1;
        if (!waitFor([&] { return word(kRequest) == 0; }, 120))
            fail("the load did not end within 120 s", i + 1);
        std::int32_t frames = word(kFrames);
        if (!waitFor([&] { return word(kFrames) - frames >= 60; }, 60))
            fail("60 frames did not run within 60 s of the load", i + 1);
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "verify: level %2d loaded and ran 60 frames (%.1f s)\n", i + 1, s);
    }
    std::fprintf(stderr, "verify: all %d levels loaded and ran 60 frames\n", g_verifyTo - g_verifyFrom);
    Report();
    std::fflush(stderr);
    std::_Exit(0);
}

void* GameMain(void*)
{
    port::SetGameFloatMode();
    port::KernelStartMainThread();
    g_started = true;
    std::fprintf(stderr, "sly1: RunMainLoop\n");
    func_00185758(0, 0);
    std::fprintf(stderr, "sly1: RunMainLoop returned\n");
    return nullptr;
}

}  // namespace

extern "C" void port_data_init_all();   // written by tools/gen.py (port/retail_data.h)

// The sanitizers' defaults in the host's checking build: a report ends the run with an exit, not
// an abort, which on macOS leaves the process stuck while the crash reporter reads ASan's huge
// address space; UBSan reports carry their stack.
extern "C" const char* __asan_default_options() { return "abort_on_error=0"; }
extern "C" const char* __ubsan_default_options() { return "print_stacktrace=1:abort_on_error=0"; }

int main(int argc, char** argv)
{
    std::string iso;
    int& seconds = g_seconds;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--iso") && i + 1 < argc)
            iso = argv[++i];
        else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--save") && i + 1 < argc)
            port::SetSaveFolder(argv[++i]);
        else if (!std::strcmp(argv[i], "--verify-levels"))
            g_verifyFrom = 0;
        else if (!std::strcmp(argv[i], "--from-level") && i + 1 < argc)
            g_verifyFrom = std::atoi(argv[++i]) - 1;
        else if (!std::strcmp(argv[i], "--level") && i + 1 < argc) {
            g_verifyFrom = std::atoi(argv[++i]) - 1;
            g_verifyTo = g_verifyFrom + 1;
        }
        else {
            std::fprintf(stderr, "usage: %s --iso DISC.iso [--seconds N] [--save FOLDER] [--verify-levels [--from-level N] | --level N]\n", argv[0]);
            return 2;
        }
    }
    if (iso.empty() || !port::OpenIso(iso)) {
        std::fprintf(stderr, "sly1: needs the NTSC-U disc image (--iso)\n");
        return 2;
    }
    if (!port::LoadRetailImage()) {
        std::fprintf(stderr, "sly1: the executable does not fit in main memory\n");
        return 1;
    }
    port_data_init_all();
    if (g_verifyFrom >= 0) {
        if (g_verifyFrom >= 46)
            g_verifyFrom = 45;
        if (g_verifyTo > 46 || g_verifyTo <= g_verifyFrom)
            g_verifyTo = 46;
        seconds = 24 * 3600;   // the verifier ends the run
        std::thread(VerifyLevels).detach();
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (pthread_attr_setstack(&attr, g_mainStack, sizeof g_mainStack) != 0) {
        std::fprintf(stderr, "sly1: the game thread's stack in game memory was refused\n");
        return 1;
    }
    pthread_t game;
    if (pthread_create(&game, &attr, GameMain, nullptr) != 0) {
        std::fprintf(stderr, "sly1: cannot start the game thread\n");
        return 1;
    }
    pthread_attr_t clockAttr;
    pthread_attr_init(&clockAttr);
    if (pthread_attr_setstack(&clockAttr, g_clockStack, sizeof g_clockStack) != 0) {
        std::fprintf(stderr, "sly1: the clock thread's stack in game memory was refused\n");
        return 1;
    }
    pthread_t clock;
    pthread_create(&clock, &clockAttr, Clock, nullptr);
    pthread_join(clock, nullptr);
    return 0;
}

