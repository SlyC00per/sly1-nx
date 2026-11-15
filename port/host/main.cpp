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

alignas(16) unsigned char g_mainStack[8 << 20];
std::atomic<bool> g_started;

void* GameMain(void*)
{
    port::SetGameFloatMode();
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
    int seconds = 10;
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
    pthread_attr_setstack(&attr, g_mainStack, sizeof g_mainStack);
    pthread_t game;
    if (pthread_create(&game, &attr, GameMain, nullptr) != 0) {
        std::fprintf(stderr, "sly1: cannot start the game thread\n");
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    std::fprintf(stderr, "sly1: stopping after %d s\n", seconds);
    std::fflush(stderr);
    std::_Exit(0);
}
