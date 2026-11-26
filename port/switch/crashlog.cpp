// SPDX-License-Identifier: AGPL-3.0-or-later
// Reports of a fault or a hang on the SD card (port/crashlog.h, docs/CRASHES.md).
#include "port/crashlog.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

#include "build_id.h"
#include "port/gs.h"
#include "port/ptr32.h"
#include "port/retail_data.h"

namespace port::crashlog {
namespace {

// The last of what the program printed: standard error goes through a device of ours that keeps
// it here and hands it on to where it went before (nxlink's socket, or the debug output).
constexpr std::size_t kTail = 12 * 1024;
char g_tail[kTail];
std::atomic<std::uint64_t> g_tailAt{0};
std::atomic<bool> g_tailHeld{false};   // (a report is being made: what it prints is not the tail)
int g_before = -1;
std::atomic<bool> g_forward{false};   // (what is printed also goes on to where it went before: a link is there to read it)

ssize_t TailWrite(struct _reent*, void*, const char* text, size_t size)
{
    if (!g_tailHeld.load(std::memory_order_relaxed)) {
        std::uint64_t at = g_tailAt.fetch_add(size, std::memory_order_relaxed);
        for (size_t i = 0; i < size; i++)
            g_tail[(at + i) % kTail] = text[i];
    }
    if (g_before >= 0 && g_forward.load(std::memory_order_relaxed))
        write(g_before, text, size);
    return (ssize_t)size;
}
int TailOpen(struct _reent*, void*, const char*, int, int) { return 0; }
int TailClose(struct _reent*, void*) { return 0; }
devoptab_t g_tailDevice;

char g_report[40 * 1024];
std::size_t g_reportSize;
const char* g_kind = "fault";
std::uint64_t g_startTick;
std::atomic<int> g_hangs{0};

void Add(const char* text)
{
    const std::size_t n = std::strlen(text);
    if (g_reportSize + n + 1 >= sizeof g_report)
        return;
    std::memcpy(g_report + g_reportSize, text, n);
    g_reportSize += n;
    g_report[g_reportSize++] = '\n';
}

}  // namespace

void Forward(bool on)
{
    std::fflush(stderr);
    g_forward.store(on);
}

void Start(bool forward)
{
    g_forward.store(forward);
    g_startTick = armGetSystemTick();
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/sly1", 0777);
    mkdir("sdmc:/switch/sly1/crashes", 0777);
    g_tailDevice.name = "sly1tail";
    g_tailDevice.structSize = sizeof(int);
    g_tailDevice.open_r = TailOpen;
    g_tailDevice.close_r = TailClose;
    g_tailDevice.write_r = TailWrite;
    if (AddDevice(&g_tailDevice) < 0)
        return;
    std::fflush(stderr);
    const int before = dup(STDERR_FILENO);
    const int ours = open("sly1tail:", O_WRONLY);
    if (ours < 0) {
        if (before >= 0)
            close(before);
        return;
    }
    g_before = before;
    dup2(ours, STDERR_FILENO);
    close(ours);
}

void Begin(const char* kind)
{
    g_tailHeld.store(true);
    g_kind = kind;
    g_reportSize = 0;
    char line[256];
    std::snprintf(line, sizeof line, "sly1-nx %s, build %s: a %s", SLY1_VERSION, SLY1_BUILD_ID, kind);
    Add(line);
    const std::time_t now = std::time(nullptr);
    std::tm utc = {};
    gmtime_r(&now, &utc);
    const double up = (double)(armGetSystemTick() - g_startTick) / (double)armGetSystemTickFreq();
    std::snprintf(line, sizeof line, "at %04d-%02d-%02d %02d:%02d:%02d UTC, %.0f s after the start (%d min), frame %llu", utc.tm_year + 1900,
                  utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, up, (int)(up / 60.0),
                  (unsigned long long)port::gs::FramesShown());
    Add(line);
    if (port::gs::FramesShown() > 0) {   // (the game's memory is there: the level last loaded, kernel.cpp's NoteWorldLoad)
        constexpr std::uint32_t kLevels = 0x00247AF0;
        const std::uint32_t entry = port::LastLevelLoaded();
        if (entry >= kLevels && entry < kLevels + 46 * 0x2C && (entry - kLevels) % 0x2C == 0) {
            const std::uint32_t name = *(const std::uint32_t*)port::Native(entry + 0x24);
            char text[48] = "(no name)";
            if (name >= 0x100000 && name < 0x2000000 - sizeof text)
                std::snprintf(text, sizeof text, "%.40s", (const char*)port::Native(name));
            std::snprintf(line, sizeof line, "level last loaded: %u of 46, code 0x%03X, %s", (entry - kLevels) / 0x2C + 1,
                          *(const std::uint32_t*)port::Native(entry + 0x20), text);
            Add(line);
        }
    }
}

void Line(const char* text)
{
    Add(text);
}

void Write()
{
    // The last lines printed, oldest first.
    {
        Add("");
        Add("the last lines printed before it:");
        const std::uint64_t at = g_tailAt.load();
        const std::size_t have = at < kTail ? (std::size_t)at : kTail;
        std::size_t from = (std::size_t)((at - have) % kTail), left = have;
        if (have == kTail)   // (from the first whole line)
            while (left && g_tail[from] != '\n') {
                from = (from + 1) % kTail;
                left--;
            }
        for (; left && g_reportSize + 1 < sizeof g_report; left--, from = (from + 1) % kTail)
            g_report[g_reportSize++] = g_tail[from];
    }
    // The file, through the file system's own calls: the C library's take the heap, and a fault may
    // have come inside it.
    const std::time_t now = std::time(nullptr);
    std::tm utc = {};
    gmtime_r(&now, &utc);
    char path[FS_MAX_PATH] = {};
    std::snprintf(path, sizeof path, "/switch/sly1/crashes/%s-%04d%02d%02d-%02d%02d%02d.txt", g_kind, utc.tm_year + 1900, utc.tm_mon + 1,
                  utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    bool written = false;
    if (FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc")) {
        fsFsDeleteFile(fs, path);
        if (R_SUCCEEDED(fsFsCreateFile(fs, path, (s64)g_reportSize, 0))) {
            FsFile file;
            if (R_SUCCEEDED(fsFsOpenFile(fs, path, FsOpenMode_Write, &file))) {
                written = R_SUCCEEDED(fsFileWrite(&file, 0, g_report, g_reportSize, FsWriteOption_Flush));
                fsFileClose(&file);
            }
        }
    }
    g_tailHeld.store(false);
    std::fprintf(stderr, "sly1: the report %s sdmc:%s\n", written ? "is in" : "COULD NOT BE WRITTEN to", path);
    // SLY1_CRASHLOG_ECHO=1: the file read back and printed (a trial of this from the Mac: the card
    // cannot be read from there)
    if (written && std::getenv("SLY1_CRASHLOG_ECHO")) {
        g_forward.store(true);   // (the trial reads it at the Mac, whatever the run put out)
        char full[FS_MAX_PATH + 8];
        std::snprintf(full, sizeof full, "sdmc:%s", path);
        if (std::FILE* f = std::fopen(full, "r")) {
            char text[512];
            while (std::fgets(text, sizeof text, f))
                std::fprintf(stderr, "| %s", text);
            std::fclose(f);
        }
    }
    std::fflush(stderr);
}

void Watch()
{
    // Counted in this thread's own turns, not by the clock: with the console asleep or the program
    // set aside, no turn passes and nothing is taken for a hang.
    static const int limit = (std::getenv("SLY1_HANG_SECONDS") ? std::atoi(std::getenv("SLY1_HANG_SECONDS")) : 30) * 10;
    static std::uint64_t was;
    static int still;
    static bool said;
    const std::uint64_t now = port::gs::FramesShown();
    if (now != was || now == 0) {
        was = now;
        still = 0;
        said = false;
        return;
    }
    if (limit <= 0 || ++still < limit || said)
        return;
    said = true;
    if (g_hangs.fetch_add(1) >= 8)   // (a run's most)
        return;
    Begin("hang");
    char line[96];
    std::snprintf(line, sizeof line, "no frame shown for %d s", limit / 10);
    Add(line);
    Add("");
    Add("every other thread (addresses for tools/crash_report.py):");
    port::DescribeThreads(Line);
    Write();
}

}  // namespace port::crashlog
