// SPDX-License-Identifier: AGPL-3.0-or-later
// Saves without a memory card, as the PS3 release has them (docs/PS3_PARITY.md): the memory card
// library's requests (sly1-decomp src/lib/memory_card_client.cpp; their numbers and results in
// src/main/memory_card.cpp's MapMemoryCardErrorCode) are answered from a folder of the player's,
// as an 8 MB card in port 0 that is always there and formatted. The game's own save code runs as
// it is, so it never shows a "no memory card" page; port 1 has no card.
//
// Each request is done when it is made and its result kept for SyncCardRequest, which hands it
// over (1) or says nothing is in flight (-1), as the library does. A card path ("/DIR/FILE") is
// the same path under the save folder. Written from the public description of the PS2's memory
// card file system (entries, attributes, times) and the game's own use of it.
#include "port/memory_card.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>

#include "common.h"

namespace port {

namespace {

constexpr s32 kCardKilobytes = 8000;   // an 8 MB card's free clusters when empty
constexpr s32 kNoCard = -10;
constexpr s32 kNotFound = -4;
constexpr int kHandles = 8;

std::mutex g_card;
std::string g_root = "save";
s32 g_inFlight;    // the request whose result waits for SyncCardRequest, or 0
s32 g_result;
std::FILE* g_files[kHandles];

// The request is done: its number and result wait for SyncCardRequest.
s32 Done(s32 op, s32 result)
{
    static const bool trace = std::getenv("SLY1_CARD_TRACE") != nullptr;
    if (trace)
        std::fprintf(stderr, "card: request 0x%X -> %d\n", op, result);
    g_inFlight = op;
    g_result = result;
    return 0;
}

// The host path of a card path; empty when it tries to leave the card.
std::string HostPath(const char* card)
{
    std::string p = card ? card : "";
    if (p.find("..") != std::string::npos)
        return std::string();
    while (!p.empty() && p[0] == '/')
        p.erase(0, 1);
    return p.empty() ? g_root : g_root + "/" + p;
}

bool IsDirectory(const std::string& p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void MakeRoot()
{
#ifdef _WIN32
    mkdir(g_root.c_str());
#else
    mkdir(g_root.c_str(), 0755);
#endif
}

// The kilobytes the card's files take, as clusters of 1 KB (directories one each).
s32 UsedKilobytes(const std::string& dir)
{
    s32 used = 0;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, ".."))
                continue;
            std::string p = dir + "/" + e->d_name;
            struct stat st;
            if (stat(p.c_str(), &st) != 0)
                continue;
            used += S_ISDIR(st.st_mode) ? 1 + UsedKilobytes(p) : (s32)((st.st_size + 1023) / 1024);
        }
        closedir(d);
    }
    return used;
}

// A directory entry as the card gives it: creation and modification times, size, attributes,
// name (64 bytes).
struct CardEntry {
    u8 created[8];
    u8 modified[8];
    u32 size;
    u16 attributes;
    u16 reserved1;
    u32 reserved2;
    u32 pdaApplication;
    char name[32];
};
static_assert(sizeof(CardEntry) == 64, "the card's directory entry");

void CardTime(u8* out, std::time_t t)
{
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    out[0] = 0;
    out[1] = (u8)tm.tm_sec;
    out[2] = (u8)tm.tm_min;
    out[3] = (u8)tm.tm_hour;
    out[4] = (u8)tm.tm_mday;
    out[5] = (u8)(tm.tm_mon + 1);
    u16 year = (u16)(tm.tm_year + 1900);
    std::memcpy(out + 6, &year, 2);
}

void FillEntry(CardEntry* e, const char* name, const struct stat& st)
{
    std::memset(e, 0, sizeof *e);
    CardTime(e->created, st.st_mtime);
    CardTime(e->modified, st.st_mtime);
    bool dir = S_ISDIR(st.st_mode);
    e->size = dir ? 0 : (u32)st.st_size;
    // readable, writable, executable, a file or a directory, closed, present
    e->attributes = dir ? 0x8427 : 0x8497;
    std::strncpy(e->name, name, sizeof e->name - 1);
}

// A name against a card pattern: '*' any run, '?' one character.
bool Matches(const char* p, const char* s)
{
    if (*p == 0)
        return *s == 0;
    if (*p == '*')
        return Matches(p + 1, s) || (*s && Matches(p, s + 1));
    return *s && (*p == '?' || *p == *s) && Matches(p + 1, s + 1);
}

}  // namespace

void SetSaveFolder(const std::string& path)
{
    std::lock_guard<std::mutex> lk(g_card);
    g_root = path;
}

}  // namespace port

using namespace port;

// What UsedKilobytes(g_root) gave, kept until the card is written to (under g_card; -1: not known).
// The game asks for the card's free space three times a second from a thread of its own, which
// runs in turn with the game's other threads here: walking the save folder each time held the
// main thread about 12 ms on the Switch's SD card, one frame in eighteen (docs/RENDERER.md).
s32 g_usedKilobytes = -1;

s32 InitMemoryCardClient()
{
    MakeRoot();
    return 1;
}

s32 GetCardInfo(s32 port, s32, s32* type, s32* free, s32* format)
{
    std::lock_guard<std::mutex> lk(g_card);
    if (port != 0)
        return Done(1, kNoCard);
    if (type) *type = 2;   // a PS2 card
    if (g_usedKilobytes < 0)
        g_usedKilobytes = UsedKilobytes(g_root);
    if (free) *free = kCardKilobytes - g_usedKilobytes;
    if (format) *format = 1;
    return Done(1, 0);
}

// Opens a file (flags: 1 read, 2 write, 0x200 create) or, with 0x40, makes a directory.
s32 OpenCardFile(s32 port, s32, const char* name, s32 flags)
{
    std::lock_guard<std::mutex> lk(g_card);
    g_usedKilobytes = -1;
    if (port != 0)
        return Done(2, kNoCard);
    std::string p = HostPath(name);
    if (p.empty())
        return Done(2, kNotFound);
    if (flags & 0x40) {
        if (IsDirectory(p))
            return Done(2, kNotFound);
#ifdef _WIN32
        return Done(2, mkdir(p.c_str()) == 0 ? 0 : kNotFound);
#else
        return Done(2, mkdir(p.c_str(), 0755) == 0 ? 0 : kNotFound);
#endif
    }
    int fd = -1;
    for (int i = 0; i < kHandles; i++)
        if (!g_files[i]) {
            fd = i;
            break;
        }
    if (fd < 0)
        return Done(2, -7);
    std::FILE* f = std::fopen(p.c_str(), "r+b");
    if (!f && (flags & 0x200))
        f = std::fopen(p.c_str(), "w+b");
    if (!f)
        return Done(2, kNotFound);
    g_files[fd] = f;
    return Done(2, fd);
}

s32 MakeCardDirectory(s32 port, s32 slot, const char* name)
{
    s32 r = OpenCardFile(port, slot, name, 0x40);
    std::lock_guard<std::mutex> lk(g_card);
    g_inFlight = 0xB;
    return r;
}

s32 CloseCardFile(s32 fd)
{
    std::lock_guard<std::mutex> lk(g_card);
    g_usedKilobytes = -1;
    if (fd < 0 || fd >= kHandles || !g_files[fd])
        return Done(3, kNotFound);
    std::fclose(g_files[fd]);
    g_files[fd] = nullptr;
    return Done(3, 0);
}

s32 ReadCardFile(s32 fd, void* buf, s32 size)
{
    std::lock_guard<std::mutex> lk(g_card);
    if (fd < 0 || fd >= kHandles || !g_files[fd])
        return Done(5, kNotFound);
    return Done(5, (s32)std::fread(buf, 1, (std::size_t)size, g_files[fd]));
}

s32 WriteCardFile(s32 fd, const void* data, s32 size)
{
    std::lock_guard<std::mutex> lk(g_card);
    g_usedKilobytes = -1;
    if (fd < 0 || fd >= kHandles || !g_files[fd])
        return Done(6, kNotFound);
    s32 n = (s32)std::fwrite(data, 1, (std::size_t)size, g_files[fd]);
    std::fflush(g_files[fd]);
    return Done(6, n);
}

// The entries of a path: the path itself, or with a pattern in its last part ("/DIR/*") the
// matching entries of its directory, "." and ".." among them. Up to max into the table (none
// asked when max is negative); the result is the count.
s32 ReadCardDirectory(s32 port, s32, const char* name, s32, s32 max, void* table)
{
    std::lock_guard<std::mutex> lk(g_card);
    if (port != 0)
        return Done(0xD, kNoCard);
    std::string card = name ? name : "";
    std::size_t slash = card.rfind('/');
    std::string dirPart = slash == std::string::npos ? "" : card.substr(0, slash);
    std::string last = slash == std::string::npos ? card : card.substr(slash + 1);
    std::vector<CardEntry> found;
    if (last.find_first_of("*?") == std::string::npos) {
        std::string p = HostPath(card.c_str());
        struct stat st;
        if (!p.empty() && stat(p.c_str(), &st) == 0) {
            found.emplace_back();
            FillEntry(&found.back(), last.c_str(), st);
        }
    } else {
        std::string dir = HostPath(dirPart.c_str());
        if (DIR* d = dir.empty() ? nullptr : opendir(dir.c_str())) {
            while (dirent* e = readdir(d)) {
                bool dots = !std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..");
                if (dots && dirPart.empty())
                    continue;   // the root has no "." or ".."
                if (!Matches(last.c_str(), e->d_name))
                    continue;
                struct stat st;
                if (stat((dir + "/" + e->d_name).c_str(), &st) != 0)
                    continue;
                found.emplace_back();
                FillEntry(&found.back(), e->d_name, st);
            }
            closedir(d);
        }
    }
    s32 n = (s32)found.size();
    if (max >= 0 && n > max)
        n = max;
    if (table && max > 0)
        std::memcpy(table, found.data(), (std::size_t)n * sizeof(CardEntry));
    return Done(0xD, n);
}

s32 RequestCardFormat(s32 port, s32)
{
    std::lock_guard<std::mutex> lk(g_card);
    g_usedKilobytes = -1;
    return Done(0x10, port == 0 ? 0 : kNoCard);
}

// The free entries of a directory: the save folder never runs out.
s32 RequestCardSpaceForPath(s32 port, s32, const char*)
{
    std::lock_guard<std::mutex> lk(g_card);
    return Done(0x12, port == 0 ? 1000 : kNoCard);
}

s32 SyncCardRequest(s32, s32* op, s32* result)
{
    std::lock_guard<std::mutex> lk(g_card);
    if (g_inFlight == 0)
        return -1;
    if (op) *op = g_inFlight;
    if (result) *result = g_result;
    g_inFlight = 0;
    return 1;
}
