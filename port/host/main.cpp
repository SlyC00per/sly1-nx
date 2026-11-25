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
#if defined(__SWITCH__)
namespace port { void CheckDevTable(const char* where); }
#else
namespace port { inline void CheckDevTable(const char*) {} }
#endif
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <cstdlib>
#include <cstring>
namespace port {
void MemoryReport(int level);   // (dmac.cpp)
#if defined(__SWITCH__)
__attribute__((weak)) void HeapProfileReport(int level);   // (switch/heap_profile.cpp, SLY1_HEAP_PROFILE)
#endif
}
#if defined(__SWITCH__)
#include <malloc.h>
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#endif
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
#include <cstdarg>
#include <sys/stat.h>
namespace port { void ScreenText(const char* text); }   // (switch/screen_text.cpp)
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
#include "port/crashlog.h"
struct Player;
struct Vec4;
void func_001BED48();   // the game's PauseStagedBankVoice (SLY1_RETRY_TEST)
void func_00178008(Player*, Vec4*);   // the game's SetPlayerPosition (SLY1_WARP)
struct World;
std::int32_t func_0015A4F0(World* world, std::uint32_t flags, std::int32_t id, void* scope, std::int32_t max, void* found);   // GatherObjectsById (SLY1_TOUR=arrivals)
struct TargetCurve;
void func_00169E20(TargetCurve* self, Vec4* out, float* angleOut, float t);   // PlaceOnTargetCurve (SLY1_TOUR=ledges)
struct BaseObject;
void func_001F3BD8(BaseObject* self, Vec4* out);   // GetObjectWorldPosition
struct GameProgress;
struct IdentityKey;
void func_001F0428(IdentityKey*, std::int32_t, std::int32_t);   // the game's StartObjectFade (SLY1_FADE_TO)
std::int32_t func_001602A0();                 // the game's ComputeCompletionMask (SLY1_COMPLETION)
std::int32_t func_00160B28(GameProgress*);    // and ComputeCompletionPercent
std::int32_t func_00160650(GameProgress*);    // ChecksumGameProgress (SLY1_COMPLETION=FILE)
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

#if defined(__SWITCH__)
// The link to nxlink (main): its socket, on which the standard output and error are too.
bool g_socketsUp;
int g_nxlinkSocket = -1;

// The link given back before the program returns to the homebrew menu, which goes on in this
// process: left open, the menu faulted when it was itself closed (2026-11-24, after the
// no-disc message). The standard output and error are taken off the socket first: what the
// program still writes on its way out then fails as a write to a closed file, where a write to
// a socket whose service is gone hung the program. Only where no other thread of the program runs.
void CloseLink()
{
    if (!g_socketsUp)
        return;
    std::fflush(stdout);
    std::fflush(stderr);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    if (g_nxlinkSocket >= 0)
        close(g_nxlinkSocket);
    socketExit();
    g_socketsUp = false;
}
#endif

#if defined(__SWITCH__)
// The program's heap: the largest stretch of the one the loader gives that is plain memory. libnx
// takes all of it, and newlib grows through it from the start; where a run before this one in the
// process (the receiver's) was stopped with pieces of it lent out -- a thread's stack, a transfer
// memory -- the first allocation that reached one faulted (2026-11-25). At a clean start the one
// piece is the loader's copy of this program, and the stretch after it is the same but for 5 MB.
extern "C" {
extern char* fake_heap_start;
extern char* fake_heap_end;
void __libnx_initheap(void)
{
    u64 addr = 0, size = 0;
    if (envHasHeapOverride()) {
        addr = (u64)envGetHeapOverrideAddr();
        size = envGetHeapOverrideSize();
    } else {
        u64 total = 0, used = 0;
        svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        if (total > used + 0x200000)
            size = (total - used - 0x200000) & ~0x1FFFFFull;
        if (size == 0)
            size = 0x2000000 * 16;
        void* at = nullptr;
        if (R_FAILED(svcSetHeapSize(&at, size)))
            svcBreak(BreakReason_Panic, 0, 0);
        addr = (u64)at;
    }
    u64 best = addr, bestSize = 0, at = addr;
    u64 run = 0, runAt = addr;
    const u64 end = addr + size;
    while (at < end) {
        MemoryInfo info;
        u32 page = 0;
        if (R_FAILED(svcQueryMemory(&info, &page, at)) || info.size == 0)
            break;
        const u64 stop = std::min(info.addr + info.size, end);
        if (info.perm == Perm_Rw && info.attr == 0) {
            if (run == 0)
                runAt = at;
            run += stop - at;
            if (run > bestSize) {
                bestSize = run;
                best = runAt;
            }
        } else {
            run = 0;
        }
        at = stop;
    }
    if (bestSize == 0) {   // (nothing plain: as libnx)
        best = addr;
        bestSize = size;
    }
    fake_heap_start = (char*)best;
    fake_heap_end = (char*)(best + bestSize);
}
}
#endif

#if defined(__SWITCH__)
// The end of a run that another program follows in this process (SLY1_NEXT: the receiver,
// port/switch/receiver.cpp, which waits for the next build, for runs with nobody at the Switch).
// The loader gives the next program this process and its heap, so everything this one holds of
// the system is given back first, in order, by the first thread (the one the loader started and
// that returns to it):
//   the mixer ends and closes the audio output itself;
//   the GS thread stops between two items, and the GPU's objects are destroyed (deko3d closes
//   the GPU's services with its device);
//   every other thread ends itself where it next waits (port/platform/thread.cpp's wrapped
//   waits); one that does not is stopped for good; their stacks' mappings are given back;
//   the link to nxlink is closed.
// Found on hardware, 2026-11-24 and 25: with the threads left running the next program faulted at
// once; with them stopped but the GPU still held, the second build started this way brought the
// whole system down; the GPU's session closed from outside (nvExit) hung there.
std::atomic<int> g_endStatus{-1};
bool g_endByGsThread;
pthread_t g_firstThread;

// A run that ends without giving the GPU back leaves this process holding it, and the next run in
// it, coming to the GPU, faults or brings the whole system down (both seen, 2026-11-25). Such a run
// writes the process's number here, and a run started in the same process after it (SLY1_NEXT)
// does not begin: it goes back to the receiver, which is to be closed from HOME and started anew.
const char* const kTainted = "sdmc:/switch/sly1/tainted.txt";
u64 ProcessNumber()
{
    u64 pid = 0;
    svcGetProcessId(&pid, CUR_PROCESS_HANDLE);
    return pid;
}
bool ProcessTainted()
{
    std::FILE* f = std::fopen(kTainted, "r");
    if (!f)
        return false;
    unsigned long long pid = 0;
    const bool read = std::fscanf(f, "%llu", &pid) == 1;
    std::fclose(f);
    if (read && pid == ProcessNumber())
        return true;
    std::remove(kTainted);   // (another process's, which is gone)
    return false;
}

[[noreturn]] void HandOver(int status, const char* next)
{
    const bool quiet = port::audio::WaitEnded(1000);
    const bool parked = g_endByGsThread || port::ParkGsThread(3000);
    std::fprintf(stderr, "sly1: %s comes next; the mixer %s, the GS thread %s\n", next, quiet ? "ended" : "DID NOT END",
                 parked ? "stopped" : "DID NOT STOP (the GPU is not given back)");
    std::fflush(stderr);
    if (!parked && port::gpu::Started()) {
        if (std::FILE* f = std::fopen(kTainted, "w")) {
            std::fprintf(f, "%llu\n", (unsigned long long)ProcessNumber());
            std::fclose(f);
        }
        std::fprintf(stderr, "sly1: the GPU is still held: no other run begins in this process\n");
        std::fflush(stderr);
    }
    port::HeapReport("before the end", false);
    if (parked) {
        port::gpu::Shutdown();
        std::fprintf(stderr, "sly1: the GPU given back\n");
        std::fflush(stderr);
        port::HeapReport("the GPU given back", false);
    }
    int made = 0;
    const int ended = port::EndOtherThreads(3000, &made);
    std::fprintf(stderr, "sly1: %d of %d threads ended\n", ended, made);
    std::fflush(stderr);
    const int stopped = port::StopOtherThreads();
    std::fprintf(stderr, "sly1: %d threads stopped; closing the link\n", stopped);
    std::fflush(stderr);
    port::HeapReport("at the end", std::getenv("SLY1_HEAP_MAP") != nullptr);   // (as at the start, if all is given back)
    CloseLink();
    envSetNextLoad(next, next);
    std::_Exit(status);
}
#endif

// The deadline's thread (main): told to end and waited for before the program does. The program
// that comes next (the homebrew menu, or SLY1_NEXT's) is loaded into this same process, and a
// thread of this one that woke after that ran code no longer there: "the software closed because
// an error occurred" a second after the first run through the receiver (2026-11-24).
std::atomic<bool> g_ending;
pthread_t g_deadlineThread;
bool g_deadlineThreadUp;

// The end of a run: its status, in the line tools/run_ryujinx.py waits for.
[[noreturn]] void Finish(int status)
{
    g_ending.store(true);
    if (g_deadlineThreadUp && !pthread_equal(pthread_self(), g_deadlineThread))
        pthread_join(g_deadlineThread, nullptr);
    char line[64];
    std::snprintf(line, sizeof line, "sly1: done, status %d", status);
    port::audio::Finish();
    std::fprintf(stderr, "%s\n", line);
    std::fflush(stderr);
#if defined(__SWITCH__)
    svcOutputDebugString(line, std::strlen(line));
    // (The link to nxlink is not given back here: socketExit before the end left the screen black
    // and the program hung, 2026-11-24. The program still writes to its standard error on the way
    // out, RemoveDevice's diagnosis among it, which is the link's.)
    // SLY1_NEXT=path: the program to start when this one ends (HandOver)
    if (const char* next = std::getenv("SLY1_NEXT"))
        if (envHasNextLoad()) {
            if (pthread_equal(pthread_self(), g_firstThread))
                HandOver(status, next);
            g_endByGsThread = port::OnGsThread();
            g_endStatus.store(status);   // (the first thread does it, main's wait; this one stays here)
            for (;;)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
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
        // SLY1_VBLANK_SLACK=us: a blank the game is not yet waiting for is held back up to that long,
        // and given the moment the game comes to wait; the blanks after are counted from then. A
        // frame that needs 18 ms is then shown after 18, not after 33.4: the game steps by the time
        // measured (AdvanceGameClock: the cycle counter, between a sixtieth and a thirtieth of a
        // second), and this clock is not the display's in any case. Only after a blank the game was
        // waiting for (a load's blanks come on time), and a blank that runs out of slack is given
        // without moving the ones after it, so a frame of two blanks still takes two.
        {
            static const long slack = std::getenv("SLY1_VBLANK_SLACK") ? std::atol(std::getenv("SLY1_VBLANK_SLACK")) : 0;
            static bool waitedLast = false;
            static std::uint64_t held, heldUs, blanks;
            auto waiting = [] { return *(const volatile std::int32_t*)port::Native(0x002BE46C) != 0; };   // g_vblankWaiting
            const bool was = waitedLast;
            waitedLast = g_started && waiting();
            // SLY1_VBLANK_GRID=1: the blanks after a held one stay where they were, so a frame that
            // came late is followed by a short one that catches up (the GS thread, a frame behind,
            // evens it out at the display), and a blank is lost only when the lateness passes the
            // slack. Without it every frame of a heavy stretch is shown after 18 ms: 55 a second.
            // SLY1_VBLANK_AB=1|2: the slack on in every other 600 frames (2: the other ones), for
            // comparing within one run.
            static const bool grid = std::getenv("SLY1_VBLANK_GRID") != nullptr;
            static const int ab = std::getenv("SLY1_VBLANK_AB") ? std::atoi(std::getenv("SLY1_VBLANK_AB")) : 0;
            const bool phaseOn = ab == 0 || (int)(port::gs::FramesShown() / 600 % 2) == ab - 1;
            if (slack > 0 && phaseOn && g_started && (was || grid) && !waitedLast) {
                const auto until = next + std::chrono::microseconds(slack);
                while (!waiting() && std::chrono::steady_clock::now() < until)
                    std::this_thread::sleep_for(std::chrono::microseconds(150));
                if (waiting()) {
                    waitedLast = true;
                    heldUs += (std::uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - next).count();
                    held++;
                    if (!grid)
                        next = std::chrono::steady_clock::now();
                }
            }
            if (slack > 0 && ++blanks % 600 == 0 && std::getenv("SLY1_PERF_LOG")) {
                std::fprintf(stderr, "perfblank: of the last 600 blanks %llu were held back for the game, %.1f ms each\n", (unsigned long long)held,
                             held ? heldUs / 1000.0 / (double)held : 0.0);
                held = heldUs = 0;
            }
        }
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
        // (a call still waiting for the game's thread, a tour's last stop, is not made during the
        // load: the player it names is gone by then)
        port::CallOnGameThread(nullptr);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        word(kRequest) = 1;
        // (SLY1_LOAD_WAIT=seconds: longer, for a run under lockstep with the software rasteriser)
        static const int loadWait = std::getenv("SLY1_LOAD_WAIT") ? std::max(120, std::atoi(std::getenv("SLY1_LOAD_WAIT"))) : 120;
        if (!waitFor([&] { return word(kRequest) == 0; }, loadWait))
            fail("the load did not end in time (120 s, or SLY1_LOAD_WAIT)", i + 1);
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
        if (const char* after = std::getenv("SLY1_RETRY_TEST")) {
            // SLY1_RETRY_TEST=frames[,more[,0]]: after that many frames, what "Try again" does after a game
            // over: the music's voice paused (the game-over page pauses it in the level 0x308) and the
            // same level loaded again with flags 0x10; more frames (1,800) run after (,0: without the pause)
            int first = 600, more = 1800;
            static int pause;
            pause = 1;
            std::sscanf(after, "%d,%d,%d", &first, &more, &pause);
            std::int32_t from = word(kFrames);
            waitFor([&] { return word(kFrames) - from >= first; }, 120 + first / 30);
            port::CallOnGameThread([] {
                if (pause)
                    ::func_001BED48();
                auto w = [](std::uint32_t a) -> std::int32_t& { return *(std::int32_t*)port::Native(a); };
                constexpr std::uint32_t kLoad = 0x00275AF0;
                if (w(kLoad) == 0) {
                    w(kLoad + 0x04) = (std::int32_t)(kLoad + 0x14);
                    w(kLoad + 0x08) = -1;
                    w(kLoad + 0x0C) = -1;
                    w(kLoad + 0x10) = 0x10;
                    w(kLoad) = 1;
                }
            });
            std::fprintf(stderr, "retry test: the voice paused and the level asked for again\n");
            from = word(kFrames);
            waitFor([&] { return word(kFrames) - from >= more; }, 120 + more / 30);
        }
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
            // SLY1_COMPLETION=1: the game's own completion mask and percentage for the save as it is;
            // =FILE: for that save file (0x1A00 bytes), put in the game's progress block first, with
            // its checksum as ChecksumGameProgress makes it
            static std::vector<unsigned char> saveFile;
            if (const char* path = std::getenv("SLY1_COMPLETION"))
                if (std::FILE* f = std::fopen(path, "rb")) {
                    saveFile.resize(0x1A00);
                    if (std::fread(saveFile.data(), 1, 0x1A00, f) != 0x1A00)
                        saveFile.clear();
                    std::fclose(f);
                }
            port::CallOnGameThread([] {
                if (!saveFile.empty()) {
                    ::GameProgress* g = (::GameProgress*)port::Native(*(std::uint32_t*)port::Native(0x002623C0));
                    std::memcpy((void*)g, saveFile.data(), 0x1A00);
                    std::int32_t stored;
                    std::memcpy(&stored, saveFile.data() + 8, 4);
                    std::fprintf(stderr, "completion: the file's checksum %d, the game's %d\n", (int)stored, (int)::func_00160650(g));
                }
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
            waitFor([&] { return word(kFrames) - from >= more; }, 120 + more / 30);   // (as long as the frames asked for take, for playing on from there)
            const std::uint32_t player = (std::uint32_t)word(0x00262E10);
            float now[3];
            std::memcpy(now, port::Native(player + 0x100), sizeof now);
            std::fprintf(stderr, "warp: the player now at %.0f %.0f %.0f\n", (double)now[0], (double)now[1], (double)now[2]);
        }
        if (const char* tour = std::getenv("SLY1_TOUR")) {
            // SLY1_TOUR=frames:x,y,z;x,y,z;...: the player placed at each place in turn for that many
            // frames (with SLY1_PAD_WALK it walks a circle at each): a walk through a level that is the
            // same each run, for timing. The places are a played route's (SLY1_PERF_LOG's perfpos).
            static float pos[4] __attribute__((aligned(16)));
            int frames = 480, stop = 0;
            std::sscanf(tour, "%d", &frames);
            const char* at = std::strchr(tour, ':');
            // SLY1_TOUR=arrivals[:frames]: the stops are the level's arrival points (class 0x74: where
            // the player comes into the level and where it is put back after a fall, FindArrivalPoint),
            // which lie along the way through it: the same tour for any level, without a played route
            // (the user's idea, 2026-11-22). With --verify-levels: every level's.
            static std::string arrivals;
            // SLY1_TOUR=ledges[:frames]: the stops are the level's target curves the player is held beside
            // (class 0x6F with mode 0 or 1: the ledges it sidles along), a little way along each.
            if (std::strncmp(tour, "ledges", 6) == 0) {
                frames = 240;
                if (tour[6] == ':')
                    frames = std::max(1, std::atoi(tour + 7));
                static std::uint32_t found[64 + 4];
                static float places[64][4] __attribute__((aligned(16)));
                static std::int32_t count;
                static std::atomic<bool> gathered;
                count = 0;
                gathered = false;
                if (word(0x00275710) != 0 && word(0x00262E10) != 0) {
                    port::CallOnGameThread([] {
                        count = ::func_0015A4F0((::World*)port::Native(*(std::uint32_t*)port::Native(0x00275710)), 0x205, 0x6F, nullptr, 64, found);
                        for (std::int32_t k = 0; k < count && k < 64; k++)
                            if (found[k] != 0)
                                ::func_00169E20((::TargetCurve*)port::Native(found[k]), (::Vec4*)places[k], nullptr, 60.0f);
                        gathered = true;
                    });
                    waitFor([&] { return gathered.load(); }, 30);
                }
                arrivals = ":";
                int kept = 0;
                for (std::int32_t k = 0; k < count && k < 64; k++) {
                    if (found[k] == 0)
                        continue;
                    const std::int32_t mode = *(const std::int32_t*)port::Native(found[k] + 0x40);
                    std::fprintf(stderr, "tour: curve %d, mode %d, at %.0f %.0f %.0f\n", (int)k, (int)mode, (double)places[k][0], (double)places[k][1], (double)places[k][2]);
                    if (mode != 0 && mode != 1)
                        continue;
                    char one[96];
                    std::snprintf(one, sizeof one, "%s%.1f,%.1f,%.1f", kept++ ? ";" : "", (double)places[k][0], (double)places[k][1], (double)places[k][2] + 60.0);
                    arrivals += one;
                }
                at = kept > 0 ? arrivals.c_str() : nullptr;
            }
            if (std::strncmp(tour, "arrivals", 8) == 0) {
                frames = 240;
                if (tour[8] == ':')
                    frames = std::max(1, std::atoi(tour + 9));
                static std::uint32_t found[64 + 4];   // (the game's pointers: its own 32-bit addresses)
                static float places[64][4] __attribute__((aligned(16)));
                static std::int32_t count;
                static std::atomic<bool> gathered;
                count = 0;
                gathered = false;
                if (word(0x00275710) != 0 && word(0x00262E10) != 0) {
                    port::CallOnGameThread([] {   // (run by the game's thread in its turn: waited for)
                        count = ::func_0015A4F0((::World*)port::Native(*(std::uint32_t*)port::Native(0x00275710)), 0x205, 0x74, nullptr, 64, found);
                        for (std::int32_t k = 0; k < count && k < 64; k++)
                            if (found[k] != 0)
                                ::func_001F3BD8((::BaseObject*)port::Native(found[k]), (::Vec4*)places[k]);
                        gathered = true;
                    });
                    waitFor([&] { return gathered.load(); }, 30);
                }
                arrivals = ":";
                for (std::int32_t k = 0; k < count && k < 64; k++) {
                    float p[3];
                    if (found[k] == 0)
                        continue;
                    std::memcpy(p, places[k], sizeof p);
                    char one[96];
                    std::snprintf(one, sizeof one, "%s%.1f,%.1f,%.1f", k ? ";" : "", (double)p[0], (double)p[1], (double)p[2]);
                    arrivals += one;
                }
                std::fprintf(stderr, "tour: %d arrival points in the level (world 0x%08X, player 0x%08X)\n", (int)count, (unsigned)word(0x00275710), (unsigned)word(0x00262E10));
                at = count > 0 ? arrivals.c_str() : nullptr;
            }
            while (at && at[1]) {
                at++;
                if (std::sscanf(at, "%f,%f,%f", &pos[0], &pos[1], &pos[2]) != 3)
                    break;
                pos[3] = 1.0f;
                port::CallOnGameThread([] {   // (not once a load is asked for, nor with no player)
                    if (*(std::uint32_t*)port::Native(0x00275AF0) == 0 && *(std::uint32_t*)port::Native(0x00262E10) != 0)
                        ::func_00178008((::Player*)port::Native(*(std::uint32_t*)port::Native(0x00262E10)), (::Vec4*)pos);
                });
                std::fprintf(stderr, "tour: stop %d at %.0f %.0f %.0f, frame %llu shown\n", stop++, (double)pos[0], (double)pos[1], (double)pos[2],
                             (unsigned long long)port::gs::FramesShown());
                const std::int32_t from = word(kFrames);
                waitFor([&] { return word(kFrames) - from >= frames; }, 120);
                at = std::strchr(at, ';');
            }
            std::fprintf(stderr, "tour: over, frame %llu shown\n", (unsigned long long)port::gs::FramesShown());
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
        port::MemoryReport(i + 1);
#if defined(__SWITCH__)
        if (port::HeapProfileReport)
            port::HeapProfileReport(i + 1);
#endif
#if defined(__SWITCH__)
        {   // (the heap in use after each level: what grows from level to level)
            const struct mallinfo mi = mallinfo();
            std::fprintf(stderr, "verify: level %2d: heap in use %.1f MB of %.1f MB taken\n", i + 1, mi.uordblks / 1048576.0, mi.arena / 1048576.0);
        }
#elif defined(__APPLE__)
        {
            malloc_statistics_t st;
            malloc_zone_statistics(nullptr, &st);
            std::fprintf(stderr, "verify: level %2d: heap in use %.1f MB of %.1f MB taken\n", i + 1, st.size_in_use / 1048576.0, st.size_allocated / 1048576.0);
        }
#endif
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
extern "C" const void* devoptab_list[];
void __libnx_exception_handler(ThreadExceptionDump* ctx)
{
    char line[256];
    port::crashlog::Begin("fault");
    std::uintptr_t base = port::g_imageBase;
    std::snprintf(line, sizeof line, "sly1: fault 0x%X at pc +0x%llX, lr +0x%llX, address 0x%llX, sp 0x%llX",
                  ctx->error_desc, (unsigned long long)(ctx->pc.x - base), (unsigned long long)(ctx->lr.x - base),
                  (unsigned long long)ctx->far.x, (unsigned long long)ctx->sp.x);
    svcOutputDebugString(line, std::strlen(line));
    port::crashlog::Line(line);
    std::fprintf(stderr, "%s\n", line);   // to nxlink on hardware
    {   // (diagnosis of the start-up fault) newlib's device table as it is now, and as x1 points at it
        const void* const* seen = reinterpret_cast<const void* const*>(ctx->cpu_gprs[1].x);
        std::fprintf(stderr, "devtable: at the fault: %p %p %p %p %p; the table at %p, x1 %p", devoptab_list[0], devoptab_list[1], devoptab_list[2], devoptab_list[3],
                     devoptab_list[4], (const void*)devoptab_list, (const void*)seen);
        if (seen == devoptab_list)
            std::fprintf(stderr, ", [3] through x1 %p", seen[3]);
        std::fprintf(stderr, "\n");
    }
    {   // the callers, by the frame records
        std::uintptr_t fp = ctx->fp.x;
        for (int i = 0; i < 16 && fp && fp % 8 == 0; i++) {
            std::uintptr_t ret = reinterpret_cast<std::uintptr_t*>(fp)[1];
            if (ret - base >= port::g_imageSize)
                break;
            std::snprintf(line, sizeof line, "sly1:   called from +0x%llX", (unsigned long long)(ret - base));
            svcOutputDebugString(line, std::strlen(line));
            port::crashlog::Line(line);
            std::fprintf(stderr, "%s\n", line);
            fp = reinterpret_cast<std::uintptr_t*>(fp)[0];
        }
    }
    for (int i = 0; i < 29; i += 4) {   // x0 to x28 (cpu_gprs holds 29)
        int n = std::snprintf(line, sizeof line, "sly1:  ");
        for (int k = i; k < i + 4 && k < 29; k++)
            n += std::snprintf(line + n, sizeof line - n, " x%d %llX", k, (unsigned long long)ctx->cpu_gprs[k].x);
        svcOutputDebugString(line, std::strlen(line));
        port::crashlog::Line(line);
        std::fprintf(stderr, "%s\n", line);
    }
    std::snprintf(line, sizeof line, "sly1:   fp %llX lr %llX sp %llX", (unsigned long long)ctx->fp.x,
                  (unsigned long long)ctx->lr.x, (unsigned long long)ctx->sp.x);
    svcOutputDebugString(line, std::strlen(line));
    port::crashlog::Line(line);
    std::fprintf(stderr, "%s\n", line);
    std::fflush(stderr);
    port::crashlog::Line("");
    port::crashlog::Line("every other thread:");
    port::DescribeThreads(port::crashlog::Line);
    port::crashlog::Write();
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

int Main(int argc, char** argv);

// (A start that fails returns from Main: with another program to follow, that too ends by Finish,
// or the receiver would not come back and a night's runs would stop there.)
int main(int argc, char** argv)
{
    const int status = Main(argc, argv);
#if defined(__SWITCH__)
    if (std::getenv("SLY1_NEXT"))
        Finish(status);
#endif
    return status;
}

int Main(int argc, char** argv)
{
    CatchFatalSignals();
#if defined(__SWITCH__)
    g_firstThread = pthread_self();
    consoleDebugInit(debugDevice_SVC);
    if (__nxlink_host.s_addr != 0 && R_SUCCEEDED(socketInitializeDefault())) {
        g_nxlinkSocket = nxlinkStdio();
        g_socketsUp = true;
    }
    port::crashlog::Start();   // (after the link: what is printed goes on to it)
    if (std::getenv("SLY1_HEAP_MAP") || __nxlink_host.s_addr != 0)   // (under nxlink or the receiver: what a run before left)
        port::HeapReport("at the start", std::getenv("SLY1_HEAP_MAP") != nullptr);
    // An exception nothing catches, or any other call of std::terminate, said with where it came
    // from before the program ends: without this the first sign was a fault elsewhere, the SD
    // card having been unmounted by the exit while a game thread still read from it.
    // SLY1_DEADLINE=seconds (set by --env, so read by the thread once the arguments are): a run
    // that has not ended by then is ended, status 4, so that one that hangs does not hold up the
    // runs after it (with SLY1_NEXT).
    {
        g_deadlineThreadUp = pthread_create(&g_deadlineThread, nullptr, [](void*) -> void* {
            const auto t0 = std::chrono::steady_clock::now();
            while (!g_ending.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (g_ending.load())
                    break;
                // SLY1_FAULT_AFTER=seconds: a fault made on purpose then (the fault handler's end
                // and the receiver's start after it, tried)
                if (const char* fault = std::getenv("SLY1_FAULT_AFTER"))
                    if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(std::atoi(fault)))
                        *(volatile int*)8 = 1;
                // SLY1_STALL_AFTER=seconds: every other thread paused then, for 8 s (the hang report, tried)
                if (const char* stall = std::getenv("SLY1_STALL_AFTER")) {
                    static int state;
                    const auto in = std::chrono::steady_clock::now() - t0;
                    if (state == 0 && in > std::chrono::seconds(std::atoi(stall))) {
                        state = 1;
                        port::PauseOtherThreads(true);
                    } else if (state == 1 && in > std::chrono::seconds(std::atoi(stall) + 8)) {
                        state = 2;
                        port::PauseOtherThreads(false);
                    }
                }
                port::crashlog::Watch();
                const char* limit = std::getenv("SLY1_DEADLINE");
                if (limit && std::chrono::steady_clock::now() - t0 > std::chrono::seconds(std::atoi(limit))) {
                    std::fprintf(stderr, "sly1: the run's deadline of %s s passed: ended\n", limit);
                    Finish(4);
                }
            }
            return nullptr;
        }, nullptr) == 0;
    }
    std::set_terminate([] {
        char line[240];
        const std::uint64_t base = (std::uint64_t)port::ImageBase();
        int n = std::snprintf(line, sizeof line, "sly1: std::terminate called from");
        const std::uint64_t* fp = reinterpret_cast<const std::uint64_t*>(__builtin_frame_address(0));
        for (int depth = 0; depth < 10 && fp && n < (int)sizeof line - 20; depth++) {
            n += std::snprintf(line + n, sizeof line - (std::size_t)n, " +0x%llX", (unsigned long long)(fp[1] - base));
            const std::uint64_t* up = reinterpret_cast<const std::uint64_t*>(fp[0]);
            if (up <= fp)
                break;
            fp = up;
        }
        svcOutputDebugString(line, std::strlen(line));
        std::fprintf(stderr, "%s\n", line);
        std::abort();
    });
    if (argc <= 1)
        ArgsFromFile(argc, argv);
    port::CheckDevTable("main");
    std::fprintf(stderr, "sly1: image at %p\n", (void*)port::ImageBase());   // for a debugger's addresses
#if defined(__SWITCH__)
    {   // what the program may use of the console's memory (less when started as an applet), and what it has used
        u64 total = 0, used = 0;
        svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        std::fprintf(stderr, "sly1: memory: %llu MB for the program, %llu MB used at its start\n", (unsigned long long)(total >> 20), (unsigned long long)(used >> 20));
    }
#endif
    std::string iso;   // (found in the folder by what is in it, below, unless --iso names one)
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
#if defined(__SWITCH__)
    if (std::getenv("SLY1_NEXT") && ProcessTainted()) {
        std::fprintf(stderr, "sly1: refused: a run before this one in this process did not give the GPU back; close the receiver from HOME and start it again\n");
        std::fflush(stderr);
        std::this_thread::sleep_for(std::chrono::seconds(2));   // (read at the Mac, and no run follows at once)
        Finish(5);
    }
    {   // The player's disc image: any file of sdmc:/switch/sly1/ that is the NTSC-U disc, whatever
        // its name (port::FindDisc: by its executable's SHA-1). With none, or one that cannot be
        // opened, the screen says so and what to do, and the program ends when a button is pressed.
        static const char* const kFolder = "sdmc:/switch/sly1";
        std::vector<port::DiscCandidate> seen;
        bool named = !iso.empty();
        if (!named) {
            mkdir("sdmc:/switch", 0777);
            mkdir(kFolder, 0777);   // (a place to put it, if there was none)
            iso = port::FindDisc(kFolder, &seen);
        }
        for (const port::DiscCandidate& c : seen)
            std::fprintf(stderr, "sly1: disc: %s: %s%s%s\n", c.name.c_str(), port::DiscKindReason(c.kind), c.what.empty() ? "" : ", ", c.what.c_str());
        if (iso.empty() || !port::OpenIso(iso)) {
            std::fprintf(stderr, "sly1: no disc image to play from\n");
            consoleInit(nullptr);
            // (written to the screen's console itself: started through nxlink, the program's
            // standard output is the link's, and the first try of this left the screen black)
            const auto say = [&](const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
                char line[512];
                va_list va;
                va_start(va, fmt);
                const int n = std::vsnprintf(line, sizeof line, fmt, va);
                va_end(va);
                if (n <= 0)
                    return;
                port::ScreenText(line);
            };
            say("\n  Sly Cooper\n\n");
            if (named)
                say("  The disc image could not be used:\n    %s\n\n", iso.c_str());
            else if (seen.empty())
                say("  No disc image was found.\n\n");
            else
                say("  No disc image that can be played was found.\n\n");
            say("  Copy your own disc image of\n"
                "  \"Sly Cooper and the Thievius Raccoonus\"\n"
                "  (North American version, SCUS-97198) to this folder\n"
                "  on the SD card. Any file name will do.\n\n"
                "    %s/\n\n", kFolder);
            if (!seen.empty()) {
                say("  In that folder now:\n");
                int shown = 0;
                for (const port::DiscCandidate& c : seen) {
                    if (shown++ == 6) {
                        say("    ...\n");
                        break;
                    }
                    say("    %.40s%s\n      %s\n", c.name.c_str(), c.name.size() > 40 ? "..." : "", port::DiscKindReason(c.kind));
                }
                say("\n");
            }
            say("  Press any button to exit.\n");
            PadState pad;
            padConfigureInput(1, HidNpadStyleSet_NpadStandard);
            padInitializeDefault(&pad);
            while (appletMainLoop()) {
                padUpdate(&pad);
                if (padGetButtonsDown(&pad) != 0)
                    break;
                consoleUpdate(nullptr);
            }
            consoleExit(nullptr);
            std::fprintf(stderr, "sly1: done, status 2\n");
            CloseLink();
            std::_Exit(2);   // (as the program ends at any other time: not by main's return, which runs every global's destructor)
        }
        std::fprintf(stderr, "sly1: disc: playing from %s\n", iso.c_str());
    }
#else
    if (iso.empty() || !port::OpenIso(iso)) {
        std::fprintf(stderr, "sly1: needs the NTSC-U disc image (--iso): SCUS_971.98, SHA-1 57dc305d...\n");
        return 2;
    }
#endif
#if defined(SLY1_INSTALL_SAVE_SLOT)
    {   // (cmake -DSLY1_INSTALL_SAVE=FILE [-DSLY1_INSTALL_SLOT=n]: a build that puts that save in the slot
        // when it starts, the one there backed up first; the file is in the build alone, never the repository)
        static const unsigned char kInstallSave[] = {
#include "install_save.inc"
        };
        port::InstallSave(SLY1_INSTALL_SAVE_SLOT, kInstallSave, sizeof kInstallSave);
    }
#endif
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
    {   // The memory card's state and the title's (the element stack's) as they change, with the time:
        // pressing Start on the title continues from the newest save only once the card's slots are
        // read (state 5); before, it starts a new game. (The first-start bug: a run began a new game
        // with three saves on the card.) Cheap: two words read every 20 ms.
        pthread_t cardWatch;
        pthread_create(&cardWatch, nullptr, [](void*) -> void* {
            auto word = [](std::uint32_t a) -> std::int32_t { return *(volatile std::int32_t*)port::Native(a); };
            constexpr std::uint32_t kCard = 0x00264A40, kStack = 0x00275BC0;
            const auto t0 = std::chrono::steady_clock::now();
            std::int32_t was[4] = {-99, -99, -99, -99};
            for (int said = 0; said < 60;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                if (!g_started)
                    continue;
                const std::int32_t now[4] = {word(kCard + 0x0C), word(kCard + 0x14), word(kCard + 0x4F48) != 0, word(kStack + 4)};
                if (std::memcmp(now, was, sizeof now) != 0) {
                    std::memcpy(was, now, sizeof was);
                    said++;
                    std::fprintf(stderr, "cardstate: %.2f s, frame %llu: card state %d, target %d, a save picked %d; title state %d\n",
                                 std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), (unsigned long long)port::gs::FramesShown(), now[0], now[1], now[2], now[3]);
                }
            }
            return nullptr;
        }, nullptr);
        pthread_detach(cardWatch);
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
    } else if (std::getenv("SLY1_GOTO")) {
        // SLY1_GOTO=n: in ordinary play, L, R and minus held together (L1, R1 and SELECT) load level n
        // where the player stands in a level, as the verifier asks for it (the game's own request,
        // with no arrival keys): for playing a level the save has not opened yet. Again each time
        // they are pressed. SLY1_GOTO_AFTER=frames: once, that many frames after a player is first
        // in a level, with no one pressing (for trying it on the host)
        pthread_t go;
        pthread_create(&go, nullptr, [](void*) -> void* {
            auto word = [](std::uint32_t a) -> volatile std::int32_t& { return *(volatile std::int32_t*)port::Native(a); };
            constexpr std::uint32_t kLevels = 0x00247AF0, kRequest = 0x00275AF0, kFrames = 0x002626C0, kPlayer = 0x00262E10;
            constexpr std::uint16_t kCombo = 0x0004 | 0x0008 | 0x0100;
            const int n = std::atoi(std::getenv("SLY1_GOTO"));
            const int after = std::getenv("SLY1_GOTO_AFTER") ? std::atoi(std::getenv("SLY1_GOTO_AFTER")) : -1;
            if (n < 1 || n > 46)
                return nullptr;
            std::int32_t since = -1;
            bool armed = true, timedDone = false;
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                const bool held = (port::g_padHeld.load() & kCombo) == kCombo;
                if (!held)
                    armed = true;
                if (!g_started || word(kPlayer) == 0 || word(kRequest) != 0) {
                    continue;
                }
                if (since < 0)
                    since = word(kFrames);
                const bool timed = after >= 0 && !timedDone && word(kFrames) - since >= after;
                if (!(held && armed) && !timed)
                    continue;
                if (timed)
                    timedDone = true;
                armed = false;
                std::fprintf(stderr, "goto: loading level %d\n", n);
                word(kRequest + 0x04) = (std::int32_t)(kLevels + (std::uint32_t)(n - 1) * 0x2C);
                word(kRequest + 0x08) = -1;
                word(kRequest + 0x0C) = -1;
                word(kRequest + 0x10) = 0;
                std::atomic_thread_fence(std::memory_order_seq_cst);
                word(kRequest) = 1;
            }
            return nullptr;
        }, nullptr);
        pthread_detach(go);
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
                    if (b.stOffset[0] != 0.0f || b.stOffset[1] != 0.0f) {   // (a part drawn again with its texture slid on)
                        float st[2];
                        std::memcpy(st, q + b.stqAt * 4, 8);
                        st[0] += b.stOffset[0];
                        st[1] += b.stOffset[1];
                        Mix(st, 8);
                    } else {
                        Mix(q + b.stqAt * 4, 8);
                    }
                    if (!(b.lit && b.litNow) && !b.coloursNow)
                        Mix(q + b.colourAt * 4, 16);
                }
                if (b.coloursNow) {
                    Mix(b.coloursNow, (std::size_t)b.count * 16);
                } else if (b.lit && b.litNow) {   // (a lit part drawn again: its colours by this frame's lights)
                    static std::vector<std::uint32_t> now;
                    now.resize((std::size_t)b.count * 4);
                    port::gs::LitColoursNow(b, now.data());
                    Mix(now.data(), now.size() * 4);
                }
                if (b.alphaScale != 1.0f)
                    Mix(&b.alphaScale, 4);
                if (b.projected) {   // (its S, T, Q and colour are made of these)
                    Mix(b.proj, sizeof b.proj);
                    Mix(b.projLimits, sizeof b.projLimits);
                    Mix(b.projColour, sizeof b.projColour);
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
#if defined(__SWITCH__)
    // (the clock's thread does not end; what this thread waits for is a run's end that another
    // program follows, which it carries out itself: HandOver)
    for (;;) {
        const int status = g_endStatus.load();
        if (status >= 0)
            HandOver(status, std::getenv("SLY1_NEXT"));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#else
    pthread_join(clock, nullptr);
    return 0;
#endif
}

