// SPDX-License-Identifier: AGPL-3.0-or-later
// The game's entry: sly1_headless on the host (no window or sound, for the tests) and sly1.nro
// on the Switch.
//
//   sly1_headless --iso DISC.iso [--seconds N]
//
// On the Switch the disc image and the saves default to sdmc:/switch/sly1/ (sly1.iso, save/);
// arguments come from nxlink, or else from sdmc:/switch/sly1/args.txt when it exists (for runs
// in the emulator, which passes none). The output goes to nxlink when it is listening, and to
// the debug-output channel (svcOutputDebugString) otherwise. A run that ends prints
// "sly1: done, status N" (tools/run_ryujinx.py waits for it).
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
#include <vector>

#include <pthread.h>

#if !defined(__SWITCH__) && !defined(_WIN32)
#include <csignal>
#include <execinfo.h>
#include <unistd.h>
#endif

#if defined(__SWITCH__)
#include <arpa/inet.h>
#include <switch.h>
#endif

#include "port/dmac.h"
#include "port/gs.h"
#include "port/iso.h"
#include "port/memory_card.h"
#include "port/retail_data.h"
#include "port/system.h"
#include "port/thread.h"

extern "C" void func_00185758(int argc, int argv);   // RunMainLoop

namespace {

// Page-aligned (16 KiB pages on Apple Silicon): port::StartThread needs whole pages.
alignas(16384) unsigned char g_mainStack[4 << 20];
// The interrupt handlers are game code too (they link stack frames into game structures), so
// the clock that calls them runs on a stack in game memory as well.
alignas(16384) unsigned char g_clockStack[1 << 20];
std::atomic<bool> g_started;
#if defined(__SWITCH__)
int g_seconds = 0;   // until the program is closed
#else
int g_seconds = 10;
#endif

// The end of a run: its status, in the line tools/run_ryujinx.py waits for.
[[noreturn]] void Finish(int status)
{
    char line[64];
    std::snprintf(line, sizeof line, "sly1: done, status %d", status);
    std::fprintf(stderr, "%s\n", line);
    std::fflush(stderr);
#if defined(__SWITCH__)
    svcOutputDebugString(line, std::strlen(line));
#endif
    std::_Exit(status);
}

// When the program started, for the report's frame rate.
const auto g_programStart = std::chrono::steady_clock::now();

// The run's report: the kernel's threads, the DMA counts, the frame semaphores, the card.
void Report()
{
    {
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_programStart).count();
        std::fprintf(stderr, "sly1: %llu frames shown in %.1f s since the program started\n",
                     (unsigned long long)port::gs::FramesShown(), s);
    }
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
    port::PlaceThread(port::ThreadRole::Clock);
    port::SetGameFloatMode();
    auto next = std::chrono::steady_clock::now();
    auto stop = next + std::chrono::seconds(g_seconds);
    while (g_seconds == 0 || std::chrono::steady_clock::now() < stop) {
        next += std::chrono::microseconds(16683);
        std::this_thread::sleep_until(next);
        if (g_started)
            port::RaiseInterrupt(2);
    }
    std::fprintf(stderr, "sly1: stopping after %d s\n", g_seconds);
    Report();
    Finish(0);
}

// --verify-levels: every level of the level table (0x247AF0, 46 records of 0x2C bytes: the
// disc record the load reads, the level code at +0x20, the name at +0x24), queued in turn as the
// warp menu queues a load (the world load request at 0x275AF0); each must load and then run 60
// frames (the display's frame counter at 0x2626C0). The game thread does the load; this host
// thread only watches and writes the request between loads.
int g_verifyFrom = -1;
int g_verifyTo = 46;   // --level N: that level alone
std::vector<int> g_verifyList;   // --levels a,b,...: these, in this order
int g_watchSeconds;               // --watch N: the state every N seconds
int g_dumpEvery = 60;             // --dump-every N: every N-th frame shown, with --dump-fb
std::string g_dumpDirArg;

// --watch: the frame count and the kernel's threads every few seconds, for a run that stops
// without a report (on its own core, at the watchers' priority, on the Switch).
void* Watch(void*)
{
    port::PlaceThread(port::ThreadRole::Watch);
    for (int n = 1;; n++) {
        std::this_thread::sleep_for(std::chrono::seconds(g_watchSeconds));
        auto w = [](std::uint32_t a) { return *(const std::int32_t*)port::Native(a); };
        // the pad record (g_padRecord, 0x262C70): device type, connection mode and step, buttons
        std::fprintf(stderr, "watch: %d s, frame %d; pad device 0x%X, mode %d, step %d, buttons 0x%04X\n",
                     n * g_watchSeconds, w(0x002626C0), w(0x262C70 + 0x10), w(0x262C70 + 0x14),
                     w(0x262C70 + 0x18), *(const std::uint16_t*)port::Native(0x262C70 + 0xA8));
        port::KernelDump();
    }
}

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
    port::PlaceThread(port::ThreadRole::Watch);
    std::this_thread::sleep_for(std::chrono::seconds(3));   // past start-up and the card check
    auto fail = [](const char* what, int i) {
        std::fprintf(stderr, "verify: FAILED at level %d: %s\n", i, what);
        Report();
        Finish(1);
    };
    std::vector<int> order = g_verifyList;
    if (order.empty())
        for (int i = g_verifyFrom; i < g_verifyTo; i++)
            order.push_back(i);
    for (int i : order) {
        std::uint32_t entry = kLevels + i * 0x2C;
        std::int32_t code = word(entry + 0x20);
        std::uint32_t name = (std::uint32_t)word(entry + 0x24);
        std::fprintf(stderr, "verify: level %2d/46, code 0x%03X, %s\n", i + 1, code,
                     name ? (const char*)port::Native(name) : "(no name)");
        auto t0 = std::chrono::steady_clock::now();
        if (!waitFor([&] { return word(kRequest) == 0; }, 120))
            fail("the previous load never ended", i + 1);
        int closes = port::g_levelStreamCloses.load();
        word(kRequest + 0x04) = (std::int32_t)entry;   // the key: the level's record
        word(kRequest + 0x08) = -1;
        word(kRequest + 0x0C) = -1;
        word(kRequest + 0x10) = 0;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        word(kRequest) = 1;
        if (!waitFor([&] { return word(kRequest) == 0; }, 120))
            fail("the load did not end within 120 s", i + 1);
        // this load's stream, before the level's own play can load another (a story level whose
        // movie ends at once goes on to the next)
        // the first close after the request is this level's (one whose movie ends at once goes on
        // to load the next)
        waitFor([&] { return port::g_levelStreamCloses.load() != closes; }, 10);
        port::LevelStreamEnd end = port::g_levelStreamEnds[(closes + 1) % 16];
        std::int32_t frames = word(kFrames);
        if (!waitFor([&] { return word(kFrames) - frames >= 60; }, 60))
            fail("60 frames did not run within 60 s of the load", i + 1);
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "verify: level %2d loaded and ran 60 frames (%.1f s)\n", i + 1, s);
        std::fprintf(stderr, "verify: level %2d stream at close: %d decompressed bytes unread, %d compressed bytes "
                     "unread, %d read but not unpacked, %d carried, texture block %d bytes %s\n", i + 1, end.remaining,
                     end.bytesToRead, end.nextCount, end.carry, end.blockSize, end.blockLoad ? "read" : "kept");
    }
    std::fprintf(stderr, "verify: all %d levels loaded and ran 60 frames\n", (int)order.size());
    Report();
    Finish(0);
}

void* GameMain(void*)
{
    port::PlaceThread(port::ThreadRole::Game);
    port::SetGameFloatMode();
    port::KernelStartMainThread();
    g_started = true;
    std::fprintf(stderr, "sly1: RunMainLoop\n");
    func_00185758(0, 0);
    std::fprintf(stderr, "sly1: RunMainLoop returned\n");
    return nullptr;
}

}  // namespace

#if !defined(__SWITCH__) && !defined(_WIN32) && !defined(__SANITIZE_ADDRESS__) && \
    !(defined(__has_feature) && __has_feature(address_sanitizer))
// A fatal signal in a build without the sanitizers: the stack, then an exit. Left to the system,
// macOS's crash reporter holds a process with the game's large address space in an exit that does
// not end until a restart.
void FatalSignal(int sig, siginfo_t* info, void* ctx)
{
    char line[160];
    int n = std::snprintf(line, sizeof line, "sly1: signal %d at address %p\n", sig, info ? info->si_addr : nullptr);
    write(2, line, n);
#if defined(__APPLE__)
    {   // where: the faulting instruction and the frame chain from it
        ucontext_t* uc = static_cast<ucontext_t*>(ctx);
#if defined(__x86_64__)
        std::uintptr_t pc = uc->uc_mcontext->__ss.__rip, fp = uc->uc_mcontext->__ss.__rbp;
#else
        std::uintptr_t pc = uc->uc_mcontext->__ss.__pc, fp = uc->uc_mcontext->__ss.__fp;
#endif
        n = std::snprintf(line, sizeof line, "sly1: pc %p (image at %p: atos -l)\n", (void*)pc, (void*)port::ImageBase());
        write(2, line, n);
        for (int i = 0; i < 16 && fp && fp % 8 == 0; i++) {
            std::uintptr_t ret = reinterpret_cast<std::uintptr_t*>(fp)[1];
            n = std::snprintf(line, sizeof line, "sly1:   from %p\n", (void*)ret);
            write(2, line, n);
            std::uintptr_t next = reinterpret_cast<std::uintptr_t*>(fp)[0];
            if (next <= fp)
                break;
            fp = next;
        }
    }
#endif
    void* frames[48];
    int count = backtrace(frames, 48);
    backtrace_symbols_fd(frames, count, 2);
    n = std::snprintf(line, sizeof line, "sly1: done, status %d\n", 128 + sig);
    write(2, line, n);
    _exit(128 + sig);
}

void CatchFatalSignals()
{
    static char altStack[64 * 1024];
    stack_t ss{};
    ss.ss_sp = altStack;
    ss.ss_size = sizeof altStack;
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = FatalSignal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP})
        sigaction(sig, &sa, nullptr);
}
#else
void CatchFatalSignals() {}
#endif

extern "C" void port_data_init_all();   // written by tools/gen.py (port/retail_data.h)

// The sanitizers' defaults in the host's checking build: a report ends the run with an exit, not
// an abort, which on macOS leaves the process stuck while the crash reporter reads ASan's huge
// address space; UBSan reports carry their stack.
extern "C" const char* __asan_default_options() { return "abort_on_error=0"; }
extern "C" const char* __ubsan_default_options() { return "print_stacktrace=1:abort_on_error=0"; }

#if defined(__SWITCH__)
// A fault on any thread: where, relative to the program's image (addr2line on sly1.elf), then
// the end of the run.
extern "C" {
alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
void __libnx_exception_handler(ThreadExceptionDump* ctx)
{
    char line[256];
    std::uintptr_t base = port::g_imageBase;
    std::snprintf(line, sizeof line, "sly1: fault 0x%X at pc +0x%llX, lr +0x%llX, address 0x%llX, sp 0x%llX",
                  ctx->error_desc, (unsigned long long)(ctx->pc.x - base), (unsigned long long)(ctx->lr.x - base),
                  (unsigned long long)ctx->far.x, (unsigned long long)ctx->sp.x);
    svcOutputDebugString(line, std::strlen(line));
    std::fprintf(stderr, "%s\n", line);   // to nxlink on hardware
    {   // the callers, by the frame records
        std::uintptr_t fp = ctx->fp.x;
        for (int i = 0; i < 16 && fp && fp % 8 == 0; i++) {
            std::uintptr_t ret = reinterpret_cast<std::uintptr_t*>(fp)[1];
            if (ret - base >= port::g_imageSize)
                break;
            std::snprintf(line, sizeof line, "sly1:   called from +0x%llX", (unsigned long long)(ret - base));
            svcOutputDebugString(line, std::strlen(line));
            std::fprintf(stderr, "%s\n", line);
            fp = reinterpret_cast<std::uintptr_t*>(fp)[0];
        }
    }
    for (int i = 0; i < 29; i += 4) {   // x0 to x28 (cpu_gprs holds 29)
        int n = std::snprintf(line, sizeof line, "sly1:  ");
        for (int k = i; k < i + 4 && k < 29; k++)
            n += std::snprintf(line + n, sizeof line - n, " x%d %llX", k, (unsigned long long)ctx->cpu_gprs[k].x);
        svcOutputDebugString(line, std::strlen(line));
        std::fprintf(stderr, "%s\n", line);
    }
    std::snprintf(line, sizeof line, "sly1:   fp %llX lr %llX sp %llX", (unsigned long long)ctx->fp.x,
                  (unsigned long long)ctx->lr.x, (unsigned long long)ctx->sp.x);
    svcOutputDebugString(line, std::strlen(line));
    std::fprintf(stderr, "%s\n", line);
    std::fflush(stderr);
    Finish(3);
}
}

namespace {

std::vector<std::string> g_fileArgs;
std::vector<char*> g_fileArgv;

// The arguments in sdmc:/switch/sly1/args.txt, split at white space, after the program's name.
void ArgsFromFile(int& argc, char**& argv)
{
    std::FILE* f = std::fopen("sdmc:/switch/sly1/args.txt", "r");
    if (!f)
        return;
    g_fileArgs.push_back(argc > 0 ? argv[0] : "sly1");
    char word[512];
    while (std::fscanf(f, "%511s", word) == 1)
        g_fileArgs.push_back(word);
    std::fclose(f);
    for (std::string& a : g_fileArgs)
        g_fileArgv.push_back(&a[0]);
    g_fileArgv.push_back(nullptr);
    argc = (int)g_fileArgs.size();
    argv = g_fileArgv.data();
}

}  // namespace
#endif

int main(int argc, char** argv)
{
    CatchFatalSignals();
#if defined(__SWITCH__)
    consoleDebugInit(debugDevice_SVC);
    if (__nxlink_host.s_addr != 0 && R_SUCCEEDED(socketInitializeDefault()))
        nxlinkStdio();
    if (argc <= 1)
        ArgsFromFile(argc, argv);
    std::fprintf(stderr, "sly1: image at %p\n", (void*)port::ImageBase());   // for a debugger's addresses
    std::string iso = "sdmc:/switch/sly1/sly1.iso";
    port::SetSaveFolder("sdmc:/switch/sly1/save");
#else
    std::string iso;
#endif
    int& seconds = g_seconds;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--iso") && i + 1 < argc)
            iso = argv[++i];
        else if (!std::strcmp(argv[i], "--wait") && i + 1 < argc)
            // seconds before the game starts: time for a debugger to attach
            std::this_thread::sleep_for(std::chrono::seconds(std::atoi(argv[++i])));
        else if (!std::strcmp(argv[i], "--dump-fb") && i + 1 < argc)
        {
            g_dumpDirArg = argv[++i];
            port::gs::SetFrameDump(g_dumpDirArg, g_dumpEvery);
        }
        else if (!std::strcmp(argv[i], "--dump-every") && i + 1 < argc) {
            g_dumpEvery = std::atoi(argv[++i]);
            port::gs::SetFrameDump(g_dumpDirArg, g_dumpEvery);
        }
        else if (!std::strcmp(argv[i], "--watch") && i + 1 < argc)
            g_watchSeconds = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--env") && i + 1 < argc) {
            // NAME=VALUE: the port's trace switches (SLY1_CD_TRACE, ...), for the Switch's runs
            std::string kv = argv[++i];
            std::size_t eq = kv.find('=');
            setenv(kv.substr(0, eq).c_str(), eq == std::string::npos ? "1" : kv.substr(eq + 1).c_str(), 1);
        }
        else if (!std::strcmp(argv[i], "--save") && i + 1 < argc)
            port::SetSaveFolder(argv[++i]);
        else if (!std::strcmp(argv[i], "--verify-levels"))
            g_verifyFrom = 0;
        else if (!std::strcmp(argv[i], "--from-level") && i + 1 < argc)
            g_verifyFrom = std::atoi(argv[++i]) - 1;
        else if (!std::strcmp(argv[i], "--levels") && i + 1 < argc) {
            for (char* s = argv[++i]; *s;) {
                int n = (int)std::strtol(s, &s, 10);
                if (n >= 1 && n <= 46)
                    g_verifyList.push_back(n - 1);
                if (*s == ',')
                    s++;
                else if (*s)
                    break;
            }
            g_verifyFrom = 0;
        }
        else if (!std::strcmp(argv[i], "--level") && i + 1 < argc) {
            g_verifyFrom = std::atoi(argv[++i]) - 1;
            g_verifyTo = g_verifyFrom + 1;
        }
        else {
            std::fprintf(stderr, "usage: %s --iso DISC.iso [--seconds N] [--save FOLDER] [--watch N] [--env NAME=VALUE] [--verify-levels [--from-level N] | --level N | --levels A,B,...]\n", argv[0]);
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
    if (g_watchSeconds > 0) {
        pthread_t watch;
        pthread_create(&watch, nullptr, Watch, nullptr);
        pthread_detach(watch);
    }
    if (g_verifyFrom >= 0) {
        if (g_verifyFrom >= 46)
            g_verifyFrom = 45;
        if (g_verifyTo > 46 || g_verifyTo <= g_verifyFrom)
            g_verifyTo = 46;
        seconds = 24 * 3600;   // the verifier ends the run
        // (a pthread: std::thread's start stalled the starting thread under libnx)
        pthread_t verify;
        pthread_create(&verify, nullptr, [](void*) -> void* { VerifyLevels(); return nullptr; }, nullptr);
        pthread_detach(verify);
    }

    pthread_t game, clock;
    if (port::StartThread(&game, g_mainStack, sizeof g_mainStack, GameMain, nullptr, true) != 0) {
        std::fprintf(stderr, "sly1: cannot start the game thread on its stack in game memory\n");
        return 1;
    }
    if (port::StartThread(&clock, g_clockStack, sizeof g_clockStack, Clock, nullptr, false) != 0) {
        std::fprintf(stderr, "sly1: cannot start the clock thread on its stack in game memory\n");
        return 1;
    }
    pthread_join(clock, nullptr);
    return 0;
}

