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
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <set>
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
#include "port/gpu.h"
#include "port/gs.h"
#include "port/gs_backend.h"
#include "port/vu1.h"
#include "port/iso.h"
#include "port/memory_card.h"
#include "port/retail_data.h"
#include "port/audio.h"
struct Player;
struct Vec4;
void func_00178008(Player*, Vec4*);   // the game's SetPlayerPosition (SLY1_WARP)
struct GameProgress;
struct IdentityKey;
void func_001F0428(IdentityKey*, std::int32_t, std::int32_t);   // the game's StartObjectFade (SLY1_FADE_TO)
std::int32_t func_001602A0();                 // the game's ComputeCompletionMask (SLY1_COMPLETION)
std::int32_t func_00160B28(GameProgress*);    // and ComputeCompletionPercent
struct WarpPage;
void func_001F1590(WarpPage*, int, int);   // the game's SetWarpCursor (SLY1_MAP)
#include "port/system.h"
#include "port/thread.h"

extern "C" void func_00185758(int argc, int argv);   // RunMainLoop

namespace {

// Page-aligned (16 KiB pages on Apple Silicon): port::StartThread needs whole pages.
alignas(16384) unsigned char g_mainStack[4 << 20];
// The interrupt handlers are game code too (they link stack frames into game structures), so
// the clock that calls them runs on a stack in game memory as well.
alignas(16384) unsigned char g_clockStack[1 << 20];
// The render thread's (port/dmac.h): it may run the VIF's and the GS's interrupt handlers.
alignas(16384) unsigned char g_renderStack[1 << 20];
alignas(16384) unsigned char g_gsStack[1 << 20];
alignas(16384) unsigned char g_helperStack[1 << 18];
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
    port::audio::Finish();
    std::fprintf(stderr, "%s\n", line);
    std::fflush(stderr);
#if defined(__SWITCH__)
    svcOutputDebugString(line, std::strlen(line));
#endif
    std::_Exit(status);
}

// When the program started, for the report's frame rate.
const auto g_programStart = std::chrono::steady_clock::now();
// The frames shown 10 s in, and when, for the report's rate after the start and the first load.
std::uint64_t g_warmFrames;
std::chrono::steady_clock::time_point g_warmAt;

// The run's report: the kernel's threads, the DMA counts, the frame semaphores, the card.
void Report()
{
    port::RenderFlush();
    {
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_programStart).count();
        std::fprintf(stderr, "sly1: %llu frames shown in %.1f s since the program started\n",
                     (unsigned long long)port::gs::FramesShown(), s);
        if (g_warmFrames) {
            double w = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_warmAt).count();
            std::uint64_t n = port::gs::FramesShown() - g_warmFrames;
            std::fprintf(stderr, "sly1: %llu frames shown in the %.1f s after the first 10 (%.1f a second)\n",
                         (unsigned long long)n, w, w > 0 ? n / w : 0.0);
        }
    }
    port::KernelDump();
    port::DmacDump();
    port::SamplerReport();
    {
        std::uint64_t whole, skipped, other;
        port::gs::UploadCounts(&whole, &skipped, &other);
        std::uint64_t restored, wrong;
        port::gs::UploadsRestored(&restored, &wrong);
        std::fprintf(stderr, "gs: %llu uploads in one run of data, %llu of them the same as last time and not written, %llu put back from the pages kept of "
                     "an earlier time (%llu of them wrong); %llu others\n", (unsigned long long)whole, (unsigned long long)skipped, (unsigned long long)restored,
                     (unsigned long long)wrong, (unsigned long long)other);
    }
    port::gpu::Report();
    if (std::getenv("SLY1_GS_SURVEY"))
        port::gs::SurveyReport();
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
    // SLY1_LOCKSTEP=N: N vblanks in all, each once the game's threads all wait and the render
    // thread has drawn what they gave it, the game's clock made of them (port::SetLockstep): every
    // run sees the same frames, however fast it runs (for comparing runs; not for playing)
    if (const char* ls = std::getenv("SLY1_LOCKSTEP")) {
        long total = std::atol(ls);
        port::SetLockstep(true);
        while (!g_started)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        for (long n = 0; n < total; n++) {
            // idle in three looks a millisecond apart; a game busy without waiting (a load polls the
            // IOP) is given its vblank after 100 ms all the same
            for (int quiet = 0, looks = 0; quiet < 3 && looks < 100; looks++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                port::RenderFlush();
                quiet = port::KernelIdle() ? quiet + 1 : 0;
            }
            port::LockstepTick();
            port::RaiseInterrupt(2);
        }
        std::fprintf(stderr, "sly1: stopping after %ld vblanks in lockstep\n", total);
        Report();
        Finish(0);
    }
    const auto warm = next + std::chrono::seconds(10);
    bool warmed = false;
#if defined(__SWITCH__)
    // SLY1_VSYNC=1: the game's vertical blank is the display's own (its event), not a clock of this
    // program's beside it: a frame given to the display is then shown a whole number of the
    // game's blanks later, the same number each time.
    Event vsync;
    bool displayBlank = false;
    if (const char* e = std::getenv("SLY1_VSYNC")) {
        ViDisplay display;
        Result opened = viOpenDefaultDisplay(&display), had = R_SUCCEEDED(opened) ? viGetDisplayVsyncEvent(&display, &vsync) : opened;
        // The display is open already (the program's window has it, and a second opening is refused):
        // its event is asked for by the display's number, which the window does not give out. The
        // numbers are small ones, tried in turn.
        for (std::uint64_t id = 0; R_FAILED(had) && id < 4; id++) {
            display = ViDisplay();
            display.display_id = id;
            display.initialized = true;
            had = viGetDisplayVsyncEvent(&display, &vsync);
            std::fprintf(stderr, "sly1: the event of display %llu: result 0x%X\n", (unsigned long long)id, (unsigned)had);
        }
        displayBlank = e[0] == '1' && R_SUCCEEDED(had);
        std::fprintf(stderr, "sly1: the vertical blank is %s (results 0x%X, 0x%X)\n", displayBlank ? "the display's" : "this program's clock: the display's event was not had", (unsigned)opened, (unsigned)had);
    }
#endif
    while (g_seconds == 0 || std::chrono::steady_clock::now() < stop) {
#if defined(__SWITCH__)
        if (displayBlank) {
            if (R_FAILED(eventWait(&vsync, 100000000ull)))   // (a tenth of a second without one: the clock's again)
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            next = std::chrono::steady_clock::now();
        } else
#endif
        {
        next += std::chrono::microseconds(16683);
        std::this_thread::sleep_until(next);
        }
        if (g_started) {
            port::TimelineEvent("vblank");
            port::RaiseInterrupt(2);
            port::TimelineEvent("vblank serviced");
        }
        if (!warmed && next >= warm) {   // past the start and the first load: for the steady rate
            warmed = true;
            g_warmFrames = port::gs::FramesShown();
            g_warmAt = std::chrono::steady_clock::now();
        }
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
        if (const char* arrival = std::getenv("SLY1_ARRIVAL")) {
            // SLY1_ARRIVAL=a,b[,flags]: the request's two arrival keys and flags, as a level's own way
            // in gives them (a level entered from another starts elsewhere, with its own sequence)
            int a = -1, b = -1, fl = 0;
            std::sscanf(arrival, "%d,%d,%d", &a, &b, &fl);
            word(kRequest + 0x08) = a;
            word(kRequest + 0x0C) = b;
            word(kRequest + 0x10) = fl;
        }
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
        // SLY1_LEVEL_FRAMES=N: N of the game's frames in each level, not 60 (with SLY1_PERF_LOG, what
        // each level costs where the player starts)
        static const int perLevel = std::getenv("SLY1_LEVEL_FRAMES") ? std::max(60, std::atoi(std::getenv("SLY1_LEVEL_FRAMES"))) : 60;
        if (!waitFor([&] { return word(kFrames) - frames >= perLevel; }, 60 + perLevel / 10))
            fail("60 frames did not run within 60 s of the load", i + 1);
        if (const char* to = std::getenv("SLY1_FADE_TO")) {
            // SLY1_FADE_TO=n[,style[,frames]]: from this level, the game's own fade to level n (as its map
            // page asks for it: StartObjectFade, style 2), and frames more (1,800) run there
            static std::uint32_t key;
            static int style;
            int n = 0, more = 1800;
            style = 2;
            std::sscanf(to, "%d,%d,%d", &n, &style, &more);
            key = kLevels + (std::uint32_t)(n - 1) * 0x2C;
            port::CallOnGameThread([] { ::func_001F0428((::IdentityKey*)port::Native(key), -1, style); });
            const std::int32_t from = word(kFrames);
            waitFor([&] { return word(kFrames) - from >= more; }, 60 + more / 10);
        }
        if (std::getenv("SLY1_COMPLETION")) {
            // SLY1_COMPLETION=1: the game's own completion mask and percentage for the save as it is
            port::CallOnGameThread([] {
                std::fprintf(stderr, "completion: mask 0x%X, %d%%\n", (unsigned)::func_001602A0(),
                             (int)::func_00160B28((::GameProgress*)port::Native(*(std::uint32_t*)port::Native(0x002623C0))));
            });
            const std::int32_t from = word(kFrames);
            waitFor([&] { return word(kFrames) - from >= 30; }, 30);
        }
        if (const char* at = std::getenv("SLY1_WARP")) {
            // SLY1_WARP=x,y,z[,frames]: the player placed there (the game's own SetPlayerPosition, on its
            // thread) once the level has run, and that many frames more (300) run after: for looking
            // at a place on the host (with --dump-fb)
            static float pos[4] __attribute__((aligned(16)));
            int more = 300;
            std::sscanf(at, "%f,%f,%f,%d", &pos[0], &pos[1], &pos[2], &more);
            pos[3] = 1.0f;
            port::CallOnGameThread([] {
                ::func_00178008((::Player*)port::Native(*(std::uint32_t*)port::Native(0x00262E10)), (::Vec4*)pos);
            });
            const std::int32_t from = word(kFrames);
            waitFor([&] { return word(kFrames) - from >= more; }, 120);
            const std::uint32_t player = (std::uint32_t)word(0x00262E10);
            float now[3];
            std::memcpy(now, port::Native(player + 0x100), sizeof now);
            std::fprintf(stderr, "warp: the player now at %.0f %.0f %.0f\n", (double)now[0], (double)now[1], (double)now[2]);
        }
        if (std::getenv("SLY1_MAP")) {
            // SLY1_MAP=1: the level's map page opened (the game's SetWarpCursor, kind 2) and 300 frames
            // run with it: for looking at the map (with --dump-fb)
            if (word(0x0026FA68) == 0) {
                std::fprintf(stderr, "map: the level has no map page\n");
            } else {
                port::CallOnGameThread([] {
                    ::func_001F1590((::WarpPage*)port::Native(*(std::uint32_t*)port::Native(0x0026FA68)), 10, 2);
                });
                const std::int32_t from = word(kFrames);
                waitFor([&] { return word(kFrames) - from >= 300; }, 120);
                std::fprintf(stderr, "map: opened, mode %d\n", word((std::uint32_t)word(0x0026FA68) + 0x2D0));
            }
        }
        if (std::getenv("SLY1_LIGHTS")) {
            // SLY1_LIGHTS=1: the level's lights (the groups CollectLightsForSphere goes through) and the
            // player's position, for finding a place to look at (diagnosis)
            auto f = [&](std::uint32_t a) { float v; std::int32_t b = word(a); std::memcpy(&v, &b, 4); return (double)v; };
            const std::uint32_t player = (std::uint32_t)word(0x00262E10);
            std::fprintf(stderr, "lights: player at 0x%08X: %.0f %.0f %.0f\n", player, f(player + 0x100), f(player + 0x104), f(player + 0x108));
            const int groups = word(0x00264668);
            for (int g = 0; g < groups && g < 64; g++) {
                const std::uint32_t grp = 0x00264670 + (std::uint32_t)g * 16, list = (std::uint32_t)word(grp);
                const int count = word(grp + 4);
                std::fprintf(stderr, "lights: group %d: %d lights, flags %X, kind %d\n", g, count, word(grp + 8), word(grp + 12) & 0xFFFF);
                for (int k = 0; k < count && k < 64 && list; k++) {
                    const std::uint32_t l = (std::uint32_t)word(list + (std::uint32_t)k * 4);
                    if (!l)
                        continue;
                    std::fprintf(stderr, "lights:   0x%08X class 0x%08X at %.0f %.0f %.0f; shape %d colour %.0f %.0f %.0f range %.0f to %.0f dir %.2f %.2f %.2f\n", l,
                                 (std::uint32_t)word(l), f(l + 0x100), f(l + 0x104), f(l + 0x108), word(l + 0x2D4), f(l + 0x2E0), f(l + 0x2E4), f(l + 0x2E8),
                                 f(l + 0x330), f(l + 0x334), f(l + 0x310), f(l + 0x314), f(l + 0x318));
                }
            }
        }
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "verify: level %2d loaded and ran 60 frames (%.1f s)\n", i + 1, s);
        {
            char what[32];
            std::snprintf(what, sizeof what, "after level %d", i + 1);
            port::PartCountsLine(what);
        }
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

    pthread_t game, clock, render, gsThread;
    // SLY1_GS_COUNT: a renderer that only counts what the GS hands it (the way to a GPU renderer
    // exercised and timed on the host).
    if (std::getenv("SLY1_GS_COUNT")) {
        struct Counting : port::gs::Backend {
            std::uint64_t runs = 0, vertices = 0, indices = 0, frames = 0;
            std::uint64_t batches = 0;
            std::uint64_t hash = 1469598103934665603ull;   // of everything drawn, for comparing runs
            // SLY1_GS_HASH: everything drawn hashed, eight bytes at a time (costly: for comparing runs)
            void Mix(const void* p, std::size_t n)
            {
                static const bool on = std::getenv("SLY1_GS_HASH") != nullptr;
                if (!on)
                    return;
                const unsigned char* b = static_cast<const unsigned char*>(p);
                std::size_t k = 0;
                for (; k + 8 <= n; k += 8) {
                    std::uint64_t w;
                    std::memcpy(&w, b + k, 8);
                    hash = (hash ^ w) * 0x9E3779B97F4A7C15ull;
                    hash ^= hash >> 31;
                }
                for (; k < n; k++)
                    hash = (hash ^ b[k]) * 1099511628211ull;
            }
            bool TakesBatches() const override { static const bool on = std::getenv("SLY1_GS_COUNT_BATCHES") != nullptr; return on; }
            // SLY1_GS_LIST=N: frame N's runs and batches, with their depth test and depths
            long listAt = std::getenv("SLY1_GS_LIST") ? std::atol(std::getenv("SLY1_GS_LIST")) : -1;
            void List(const char* what, const port::gs::DrawState& s, std::size_t n, double zmin, double zmax)
            {
                if ((long)frames + 1 != listAt)
                    return;
                unsigned test = (unsigned)s.test;
                std::fprintf(stderr, "list: %-5s %5zu  FBP %3u  ZTE %u ZTST %u ZMSK %u  ATE %u ATST %u AFAIL %u  ABE %u ALPHA %02X  tme %u  z %.0f..%.0f\n",
                             what, n, (unsigned)(s.frame & 0x1FF), (test >> 16) & 1, (test >> 17) & 3, (unsigned)((s.zbuf >> 32) & 1),
                             test & 1, (test >> 1) & 7, (test >> 12) & 3, s.abe, (unsigned)(s.alpha & 0xFF), s.tme, zmin, zmax);
            }
            // SLY1_GS_REUSE=1: how much of what a frame draws the frame before drew too. Every 240
            // frames: of the batches, how many are the same as a batch of the frame before in their
            // vertices' x y z, control words, S and T (by a hash; not Q and the word after it,
            // which hold what was left there), in their colours, in both, and in their matrix.
            static std::uint64_t HashOf(const void* p, std::size_t n, std::uint64_t h)
            {
                const unsigned char* b = static_cast<const unsigned char*>(p);
                for (std::size_t k = 0; k + 8 <= n; k += 8) {
                    std::uint64_t w;
                    std::memcpy(&w, b + k, 8);
                    h = (h ^ w) * 0x9E3779B97F4A7C15ull;
                    h ^= h >> 31;
                }
                return h;
            }
            void NoteReuse(const port::gs::Batch& b)
            {
                std::uint64_t shape = b.count * 1000003ull + b.stride, colour = 7;
                for (std::uint32_t k = 0; k < b.count; k++) {
                    const std::uint32_t* q = b.quads + k * b.stride * 4;
                    shape = HashOf(q + b.positionAt * 4, 16, shape);
                    shape = HashOf(q + b.stqAt * 4, 8, shape);
                    colour = HashOf(q + b.colourAt * 4, 16, colour);
                }
                static std::set<std::uint64_t> was[4], now[4];
                static std::uint64_t same[4], all, vertices, frame;
                const std::uint64_t part[4] = {shape, colour, shape ^ (colour * 0xC2B2AE3D27D4EB4Full), HashOf(b.matrix, sizeof b.matrix, 19)};
                if (frame != frames) {   // a new frame
                    frame = frames;
                    for (int c = 0; c < 4; c++) {
                        was[c].swap(now[c]);
                        now[c].clear();
                    }
                    if (frames % 240 == 0 && all) {
                        std::fprintf(stderr, "reuse: frames to %llu: %.0f batches a frame (%.0f vertices); the same as one of the frame before in x y z, the control "
                                     "words, S and T %.1f%%, in the colours %.1f%%, in all of those %.1f%%, in the matrix %.1f%%\n", (unsigned long long)frames,
                                     all / 240.0, vertices / 240.0, 100.0 * same[0] / all, 100.0 * same[1] / all, 100.0 * same[2] / all, 100.0 * same[3] / all);
                        all = vertices = 0;
                        for (auto& x : same)
                            x = 0;
                    }
                }
                all++;
                vertices += b.count;
                for (int c = 0; c < 4; c++) {
                    same[c] += was[c].count(part[c]);
                    now[c].insert(part[c]);
                }
            }
            bool DrawBatch(const port::gs::DrawState& s, const port::gs::Batch& b) override
            {
                batches++;
                vertices += b.count;
                static const bool reuseOn = std::getenv("SLY1_GS_REUSE") != nullptr;
                if (reuseOn)
                    NoteReuse(b);
                static const long batchesOf = std::getenv("SLY1_GS_HASH_RUNS") ? std::atol(std::getenv("SLY1_GS_HASH_RUNS")) : -1;
                static const long batchesTo = std::getenv("SLY1_GS_HASH_RUNS_TO") ? std::atol(std::getenv("SLY1_GS_HASH_RUNS_TO")) : batchesOf;   // (to frame M)
                if ((long)frames + 1 >= batchesOf && (long)frames + 1 <= batchesTo && batchesOf >= 0) {   // SLY1_GS_HASH_RUNS=N: frame N's batches too, each with its parts' hashes
                    const std::uint64_t was = hash;
                    std::uint64_t part[4];
                    hash = 1; Mix(&s, sizeof s); part[0] = hash;
                    hash = 1;
                    for (std::uint32_t k = 0; k < b.count; k++) {
                        const std::uint32_t* q = b.quads + k * b.stride * 4;
                        Mix(q + b.positionAt * 4, 16);
                        Mix(q + b.stqAt * 4, 8);
                    }
                    part[1] = hash;
                    hash = 1;
                    for (std::uint32_t k = 0; k < b.count; k++)
                        Mix(b.quads + (k * b.stride + b.colourAt) * 4, 16);
                    part[2] = hash;
                    hash = 1; Mix(b.matrix, sizeof b.matrix); Mix(b.scale, sizeof b.scale); Mix(b.offset, sizeof b.offset); part[3] = hash;
                    hash = was;
                    std::fprintf(stderr, "gsbatch: %llu state %016llX shape %016llX colours %016llX matrix %016llX count %u cull %d fog %g %g tex0 %016llX test %llX alpha %llX\n",
                                 (unsigned long long)batches, (unsigned long long)part[0], (unsigned long long)part[1], (unsigned long long)part[2],
                                 (unsigned long long)part[3], b.count, (int)b.cull, b.fogScale, b.wMax, (unsigned long long)s.tex0, (unsigned long long)s.test,
                                 (unsigned long long)s.alpha);
                }
                Mix(&s, sizeof s);
                // of a vertex, what is drawn: x y z and the control word, S and T, the colour (Q and
                // the word after it hold what was last in VU1's memory there)
                for (std::uint32_t k = 0; k < b.count; k++) {
                    const std::uint32_t* q = b.quads + k * b.stride * 4;
                    Mix(q + b.positionAt * 4, 16);
                    Mix(q + b.stqAt * 4, 8);
                    Mix(q + b.colourAt * 4, 16);
                }
                Mix(b.matrix, sizeof b.matrix);
                Mix(b.scale, sizeof b.scale);
                Mix(b.offset, sizeof b.offset);
                double zmin = 1e30, zmax = -1e30;
                if ((long)frames + 1 == listAt)
                    for (std::uint32_t k = 0; k < b.count; k++) {
                        port::gs::BatchVertex v = port::gs::VertexOf(b, k);
                        if (v.clip[3] > 0) { zmin = std::min<double>(zmin, v.z); zmax = std::max<double>(zmax, v.z); }
                    }
                List("batch", s, b.count, zmin, zmax);
                return true;
            }
            std::vector<std::uint32_t> pixels;
            std::uint64_t dateRuns = 0;
            bool lastDate = false;
            std::uint32_t beganHash = 0, beganEntry = 0;
            void Begin(const port::gs::DrawState& s) override
            {
                lastDate = (s.test >> 14) & 1;
                port::vu1::CurrentProgram(&beganHash, &beganEntry);
                if (false) {
                    std::uint32_t hash, entry;
                    port::vu1::CurrentProgram(&hash, &entry);
                    std::fprintf(stderr, "date: frame %llu program %08X entry 0x%03X test %016llX alpha %016llX tex0 %016llX\n",
                                 (unsigned long long)frames, hash, entry, (unsigned long long)s.test, (unsigned long long)s.alpha,
                                 (unsigned long long)s.tex0);
                }
                int w, h;
                static const bool check = std::getenv("SLY1_GS_CHECK") != nullptr;
                if (s.tme && check)   // the decoder exercised (and checked)
                    port::gs::DecodeTexture(s, pixels, w, h);
            }
            void Draw(const port::gs::DrawState& st, const port::gs::DrawVertex* vv, std::size_t v, const std::uint32_t* ii,
                      std::size_t i) override
            {
                runs++; vertices += v; indices += i;
                // SLY1_GS_HASH_RUNS=N: frame N's runs, each with the hashes of its state, vertices and indices alone
                static const long runsOf = std::getenv("SLY1_GS_HASH_RUNS") ? std::atol(std::getenv("SLY1_GS_HASH_RUNS")) : -1;
                static const long runsTo = std::getenv("SLY1_GS_HASH_RUNS_TO") ? std::atol(std::getenv("SLY1_GS_HASH_RUNS_TO")) : runsOf;
                if ((long)frames + 1 >= runsOf && (long)frames + 1 <= runsTo && runsOf >= 0) {
                    const std::uint64_t was = hash;
                    std::uint64_t part[3];
                    hash = 1; Mix(&st, sizeof st); part[0] = hash;
                    hash = 1; Mix(vv, v * sizeof *vv); part[1] = hash;
                    hash = 1; Mix(ii, i * sizeof *ii); part[2] = hash;
                    hash = was;
                    std::fprintf(stderr, "gsrun: %llu state %016llX vertices %016llX (%zu) indices %016llX (%zu) tex0 %016llX test %llX alpha %llX; first %.2f %.2f %08X %02X%02X%02X%02X %g %g %g fog %g\n",
                                 (unsigned long long)runs, (unsigned long long)part[0], (unsigned long long)part[1], v, (unsigned long long)part[2], i,
                                 (unsigned long long)st.tex0, (unsigned long long)st.test, (unsigned long long)st.alpha, v ? vv[0].x : 0.0f, v ? vv[0].y : 0.0f,
                                 v ? (unsigned)vv[0].z : 0u, v ? vv[0].r : 0, v ? vv[0].g : 0, v ? vv[0].b : 0, v ? vv[0].a : 0, v ? vv[0].s : 0.0f,
                                 v ? vv[0].t : 0.0f, v ? vv[0].q : 0.0f, v ? vv[0].fog : 0.0f);
                }
                Mix(&st, sizeof st);
                Mix(vv, v * sizeof *vv);
                Mix(ii, i * sizeof *ii);
                bool odd = false;
                for (std::size_t k = 0; k < v; k++)
                    odd = odd || std::fabs(vv[k].s) > 1e30f || std::fabs(vv[k].x) > 3000 || std::fabs(vv[k].y) > 3000;
                static std::uint64_t oddRuns, allDate;
                if (lastDate) {
                    allDate++;
                    if (odd)
                        oddRuns++;
                    if ((allDate & 1023) == 0)
                        std::fprintf(stderr, "date count: %llu runs with DATE, %llu with odd vertices\n", (unsigned long long)allDate,
                                     (unsigned long long)oddRuns);
                }
                static const bool seqTrace = std::getenv("SLY1_VU1_SEQ") != nullptr;
                if (lastDate && seqTrace)
                    std::fprintf(stderr, "seq   draw with DATE: %s (begun in %08X 0x%03X)\n", odd ? "ODD" : "plain", beganHash, beganEntry);
                static const bool dateTrace = std::getenv("SLY1_GS_DATE_TRACE") != nullptr;
                if (lastDate && odd && dateTrace && dateRuns++ < 40)
                    for (std::size_t k = 0; k < v && k < 8; k++)
                        std::fprintf(stderr, "date:   v%zu %.1f %.1f z %u colour %u %u %u %u stq %g %g %g\n", k, vv[k].x, vv[k].y, vv[k].z,
                                     vv[k].r, vv[k].g, vv[k].b, vv[k].a, vv[k].s, vv[k].t, vv[k].q);
                double zmin = 1e30, zmax = -1e30, xmin = 1e30, xmax = -1e30, ymin = 1e30, ymax = -1e30;
                for (std::size_t k = 0; k < v; k++) {
                    zmin = std::min<double>(zmin, vv[k].z); zmax = std::max<double>(zmax, vv[k].z);
                    xmin = std::min<double>(xmin, vv[k].x); xmax = std::max<double>(xmax, vv[k].x);
                    ymin = std::min<double>(ymin, vv[k].y); ymax = std::max<double>(ymax, vv[k].y);
                }
                List("run", st, i / 3, zmin, zmax);
                if ((long)frames + 1 == listAt)
                    std::fprintf(stderr, "list:       x %.1f..%.1f y %.1f..%.1f scissor %016llX kind %u\n", xmin, xmax, ymin, ymax,
                                 (unsigned long long)st.scissor, st.kind);
            }
            void Present(std::uint64_t, std::uint64_t) override
            {
                // SLY1_GS_HASH_FRAMES=N: each of the first N frames' own hash, and the runs so far, at its end
                static const long each = std::getenv("SLY1_GS_HASH_FRAMES") ? std::atol(std::getenv("SLY1_GS_HASH_FRAMES")) : 0;
                if ((long)frames < each) {   // the hash started again at each frame, so that one frame's difference shows as one line
                    std::fprintf(stderr, "gshash: frame %llu: %016llX after %llu runs\n", (unsigned long long)frames + 1, (unsigned long long)hash,
                                 (unsigned long long)runs);
                    hash = 1469598103934665603ull;
                }
                if ((++frames & 255) == 0)
                    std::fprintf(stderr, "gs count: %llu frames, %llu runs, %llu vertices, %llu indices, all drawn hashed %016llX\n",
                                 (unsigned long long)frames, (unsigned long long)runs, (unsigned long long)vertices,
                                 (unsigned long long)indices, (unsigned long long)hash);
            }
        };
        static Counting counting;
        port::gs::SetBackend(&counting);
    }
    // The Switch's GPU renderer, before any thread draws (SLY1_NO_GPU: without it, nothing shown).
#if defined(__SWITCH__)
    // SLY1_PERF_CONFIG=hex: asks the system for a performance configuration in handheld (normal)
    // mode, and says what it had and has. A trial: the default has the GPU at its lowest handheld
    // clock, and the roof with the steam is the GPU's.
    if (const char* config = std::getenv("SLY1_PERF_CONFIG")) {
        Result opened = apmInitialize();
        u32 was = 0, now = 0;
        Result got = apmGetPerformanceConfiguration(ApmPerformanceMode_Normal, &was);
        Result set = apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, (u32)std::strtoul(config, nullptr, 16));
        apmGetPerformanceConfiguration(ApmPerformanceMode_Normal, &now);
        u32 current = 0;
        Result cur = appletGetCurrentPerformanceConfiguration(&current);
        std::fprintf(stderr, "sly1: performance configuration: was %08X (result %X, apm %X), asked %s (result %X), now %08X; the applet's current %08X (result %X)\n", was, got, opened,
                     config, set, now, current, cur);
    }
#endif
    if (!std::getenv("SLY1_NO_GPU"))
        port::gpu::Start();
    // The GS on a thread of its own, after the render thread's VIF1 and VU1 (docs/RENDERER.md): on
    // the Switch, where the two have a core each (27.0 frames a second in the opening against
    // 26.2 without); SLY1_GS_THREAD=1 or 0 says otherwise.
#if defined(__SWITCH__)
    const bool gsThreadDefault = true;
#else
    const bool gsThreadDefault = false;
#endif
    const char* gsThreadAsked = std::getenv("SLY1_GS_THREAD");
    if ((gsThreadAsked ? gsThreadAsked[0] != '0' : gsThreadDefault) &&
        port::StartThread(&gsThread, g_gsStack, sizeof g_gsStack,
                          [](void*) -> void* {
                              port::PlaceThread(port::ThreadRole::Gs);
                              port::SetGameFloatMode();
                              port::GsThreadMain();
                              return nullptr;
                          },
                          nullptr, true) != 0) {
        std::fprintf(stderr, "sly1: cannot start the GS thread on its stack in game memory\n");
        return 1;
    }
    // the render thread's helper, with the GS thread (the parts are drawn again only with it)
    // (SLY1_PART_HELPER=1 with SLY1_PART_HASH_NOW=1: the helper for the parts' jobs alone, every hash still
    // before its decision: the jobs' output checked exactly with the helper running them)
    if ((gsThreadAsked ? gsThreadAsked[0] != '0' : gsThreadDefault) && (!std::getenv("SLY1_PART_HASH_NOW") || std::getenv("SLY1_PART_HELPER"))) {
        static pthread_t helper;
        if (port::StartThread(&helper, g_helperStack, sizeof g_helperStack,
                              [](void*) -> void* {
                                  port::SetGameFloatMode();   // (it runs parts' jobs: VU1's arithmetic, as the render thread's)
                                  port::HashHelperMain();
                                  return nullptr;
                              },
                              nullptr, true) != 0)
            std::fprintf(stderr, "sly1: cannot start the helper thread: every hash on the render thread\n");
    }
    if (port::audio::Wanted()) {   // the sound engine's mixer (port/audio)
        static pthread_t mixer;
        static unsigned char mixerStack[1 << 18] __attribute__((aligned(16)));
        if (port::StartThread(&mixer, mixerStack, sizeof mixerStack,
                              [](void*) -> void* {
                                  port::audio::MixerMain();
                                  return nullptr;
                              },
                              nullptr, true) != 0)
            std::fprintf(stderr, "sly1: cannot start the sound mixer's thread: no sound\n");
    }
    {   // the movies' decoding (port/video/movie.cpp): idle but while a movie plays
        static pthread_t movie;
        static unsigned char movieStack[1 << 18] __attribute__((aligned(16384)));
        if (port::StartThread(&movie, movieStack, sizeof movieStack,
                              [](void*) -> void* {
                                  port::PlaceThread(port::ThreadRole::Render, false);   // the render thread's core: it has nothing to do in a movie
                                  port::movie::WorkerMain();
                                  return nullptr;
                              },
                              nullptr, true) != 0)
            std::fprintf(stderr, "sly1: cannot start the movies' decoding thread\n");
        static pthread_t movieHelper;
        static unsigned char movieHelperStack[1 << 18] __attribute__((aligned(16384)));
        if (port::StartThread(&movieHelper, movieHelperStack, sizeof movieHelperStack,
                              [](void*) -> void* {
                                  port::PlaceThread(port::ThreadRole::Gs, false);   // the GS thread's core: little to do in a movie
                                  port::movie::HelperMain();
                                  return nullptr;
                              },
                              nullptr, true) != 0)
            std::fprintf(stderr, "sly1: cannot start the movies' second decoding thread: one alone\n");
    }
    port::StartSampler();
    if (port::StartThread(&render, g_renderStack, sizeof g_renderStack,
                          [](void*) -> void* {
                              port::PlaceThread(port::ThreadRole::Render);
                              port::SetGameFloatMode();
                              port::RenderThreadMain();
                              return nullptr;
                          },
                          nullptr, true) != 0) {
        std::fprintf(stderr, "sly1: cannot start the render thread on its stack in game memory\n");
        return 1;
    }
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

