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

#include "port/iso.h"
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
    port::KernelDump();
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

namespace port {
int FillRetailBytes();
}

int main(int argc, char** argv)
{
    std::string iso;
    int& seconds = g_seconds;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--iso") && i + 1 < argc)
            iso = argv[++i];
        else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = std::atoi(argv[++i]);
        else {
            std::fprintf(stderr, "usage: %s --iso DISC.iso [--seconds N]\n", argv[0]);
            return 2;
        }
    }
    if (iso.empty() || !port::OpenIso(iso)) {
        std::fprintf(stderr, "sly1: needs the NTSC-U disc image (--iso)\n");
        return 2;
    }
    std::fprintf(stderr, "sly1: %d labels of retail bytes filled from the executable\n", port::FillRetailBytes());

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

