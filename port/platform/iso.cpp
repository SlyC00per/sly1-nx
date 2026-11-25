// SPDX-License-Identifier: AGPL-3.0-or-later
// The player's disc image (port/iso.h): sectors, the ISO 9660 root directory's executable, and
// its loadable segments as an ELF file. Written from the public ISO 9660 and ELF formats.
#include "port/iso.h"
#include "port/dmac.h"

#include "port/ptr32.h"

#if defined(__SWITCH__)
namespace port { void CheckDevTable(const char* where); }
#else
namespace port { inline void CheckDevTable(const char*) {} }
#endif
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace port {

namespace {

std::FILE* g_iso;
std::mutex g_isoLock;
std::vector<std::uint8_t> g_exe;   // the executable's file

struct Segment {
    std::uint32_t vaddr, offset, filesz;
};
std::vector<Segment> g_segments;

std::uint32_t Le32(const std::uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (std::uint32_t)p[3] << 24; }
std::uint16_t Le16(const std::uint8_t* p) { return (std::uint16_t)(p[0] | p[1] << 8); }

// The executable named by the root directory (SCUS_971.98 on the NTSC-U disc).
bool LoadExecutable()
{
    std::uint8_t pvd[kSectorSize];
    if (!ReadSectors(16, 1, pvd) || std::memcmp(pvd + 1, "CD001", 5) != 0) {
        std::fprintf(stderr, "port: the image has no ISO 9660 volume descriptor\n");
        return false;
    }
    const std::uint8_t* root = pvd + 156;
    std::uint32_t lba = Le32(root + 2), size = Le32(root + 10);
    std::vector<std::uint8_t> dir((size + kSectorSize - 1) / kSectorSize * kSectorSize);
    if (!ReadSectors(lba, (std::uint32_t)(dir.size() / kSectorSize), dir.data()))
        return false;
    for (std::size_t pos = 0; pos < size;) {
        std::uint8_t len = dir[pos];
        if (len == 0) {
            pos = (pos / kSectorSize + 1) * kSectorSize;
            continue;
        }
        const std::uint8_t* rec = &dir[pos];
        std::string name((const char*)rec + 33, rec[32]);
        if (name.rfind("SCUS_971.98", 0) == 0) {
            std::uint32_t flba = Le32(rec + 2), fsize = Le32(rec + 10);
            g_exe.resize((fsize + kSectorSize - 1) / kSectorSize * kSectorSize);
            if (!ReadSectors(flba, (std::uint32_t)(g_exe.size() / kSectorSize), g_exe.data()))
                return false;
            g_exe.resize(fsize);
            break;
        }
        pos += len;
    }
    if (g_exe.size() < 52 || std::memcmp(g_exe.data(), "\x7F" "ELF", 4) != 0) {
        std::fprintf(stderr, "port: SCUS_971.98 not found on the image: the port needs the NTSC-U disc\n");
        return false;
    }
    std::uint32_t phoff = Le32(&g_exe[28]);
    std::uint16_t phentsize = Le16(&g_exe[42]), phnum = Le16(&g_exe[44]);
    for (std::uint16_t i = 0; i < phnum; i++) {
        const std::uint8_t* ph = &g_exe[phoff + (std::size_t)i * phentsize];
        if (Le32(ph) == 1)   // PT_LOAD
            g_segments.push_back({Le32(ph + 8), Le32(ph + 4), Le32(ph + 16)});
    }
    return !g_segments.empty();
}

}  // namespace

namespace {
// SHA-1 (FIPS 180), for telling the disc by its executable.
void Sha1(const std::uint8_t* data, std::size_t size, std::uint8_t out[20])
{
    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    const auto rol = [](std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    const auto block = [&](const std::uint8_t* p) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (std::uint32_t)p[i * 4] << 24 | (std::uint32_t)p[i * 4 + 1] << 16 | (std::uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (int i = 16; i < 80; i++)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            std::uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    };
    std::size_t at = 0;
    for (; at + 64 <= size; at += 64)
        block(data + at);
    std::uint8_t last[128] = {};
    const std::size_t rest = size - at;
    std::memcpy(last, data + at, rest);
    last[rest] = 0x80;
    const std::size_t total = rest + 9 <= 64 ? 64 : 128;
    const std::uint64_t bits = (std::uint64_t)size * 8;
    for (int i = 0; i < 8; i++)
        last[total - 1 - i] = (std::uint8_t)(bits >> (8 * i));
    block(last);
    if (total == 128)
        block(last + 64);
    for (int i = 0; i < 5; i++)
        for (int k = 0; k < 4; k++)
            out[i * 4 + k] = (std::uint8_t)(h[i] >> (24 - 8 * k));
}

// SCUS_971.98's SHA-1 on the NTSC-U disc (sly1-decomp config/versions.json).
const std::uint8_t kExecutableSha1[20] = {0x57, 0xdc, 0x30, 0x5d, 0xb5, 0x79, 0x32, 0xad, 0x3f, 0x11,
                                          0x22, 0x96, 0x6c, 0xdb, 0x69, 0x5d, 0x2e, 0x62, 0xa4, 0x7a};
}  // namespace

DiscKind IdentifyDisc(const std::string& path, std::string* what)
{
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return DiscKind::Unreadable;
    const auto sectors = [&](std::uint32_t lsn, std::uint32_t count, void* dst) {
        return fseeko(f, (off_t)((std::uint64_t)lsn * kSectorSize), SEEK_SET) == 0 && std::fread(dst, kSectorSize, count, f) == count;
    };
    DiscKind kind = DiscKind::NotADisc;
    std::uint8_t pvd[kSectorSize];
    if (sectors(16, 1, pvd) && std::memcmp(pvd + 1, "CD001", 5) == 0) {
        kind = DiscKind::OtherGame;
        const std::uint8_t* root = pvd + 156;
        const std::uint32_t lba = Le32(root + 2), size = Le32(root + 10);
        std::vector<std::uint8_t> dir(size <= (1u << 20) ? (size + kSectorSize - 1) / kSectorSize * kSectorSize : 0);
        if (!dir.empty() && sectors(lba, (std::uint32_t)(dir.size() / kSectorSize), dir.data())) {
            for (std::size_t pos = 0; pos + 34 <= size;) {
                const std::uint8_t len = dir[pos];
                if (len == 0) {
                    pos = (pos / kSectorSize + 1) * kSectorSize;
                    continue;
                }
                const std::uint8_t* rec = &dir[pos];
                const std::string name((const char*)rec + 33, rec[32]);
                pos += len;
                // (a PlayStation 2 executable's name: four letters, an underscore, digits and a dot)
                const bool boot = name.size() >= 11 && name[4] == '_' && name[8] == '.' && (name.compare(0, 2, "SC") == 0 || name.compare(0, 2, "SL") == 0);
                if (!boot)
                    continue;
                const std::string plain = name.substr(0, name.find(';'));
                if (what)
                    *what = plain;
                if (plain != "SCUS_971.98") {
                    kind = (plain == "SCES_509.17" || plain == "SCPS_150.36" || plain == "SCAJ_200.17" || plain == "SCKA_200.04") ? DiscKind::OtherVersion : DiscKind::OtherGame;
                    continue;
                }
                const std::uint32_t flba = Le32(rec + 2), fsize = Le32(rec + 10);
                kind = DiscKind::Altered;
                if (fsize > 0 && fsize <= (8u << 20)) {
                    std::vector<std::uint8_t> exe((fsize + kSectorSize - 1) / kSectorSize * kSectorSize);
                    std::uint8_t sum[20];
                    if (sectors(flba, (std::uint32_t)(exe.size() / kSectorSize), exe.data())) {
                        Sha1(exe.data(), fsize, sum);
                        if (std::memcmp(sum, kExecutableSha1, 20) == 0) {
                            // (and the whole volume is there: a copy cut short would fail in play)
                            struct stat st;
                            const std::uint64_t volume = (std::uint64_t)Le32(pvd + 80) * kSectorSize;
                            if (fstat(fileno(f), &st) == 0 && (std::uint64_t)st.st_size >= volume)
                                kind = DiscKind::Good;
                            else if (what)
                                *what = "the file is shorter than the disc";
                        }
                    } else {
                        kind = DiscKind::Unreadable;
                    }
                }
                break;
            }
        }
    }
    std::fclose(f);
    return kind;
}

const char* DiscKindReason(DiscKind kind)
{
    switch (kind) {
    case DiscKind::Good: return "the right disc";
    case DiscKind::Unreadable: return "could not be read";
    case DiscKind::NotADisc: return "not a disc image";
    case DiscKind::OtherGame: return "a disc of another game";
    case DiscKind::OtherVersion: return "Sly Cooper, but not the North American (NTSC-U) version";
    case DiscKind::Altered: return "the North American disc, but changed or damaged";
    }
    return "?";
}

std::string FindDisc(const std::string& folder, std::vector<DiscCandidate>* seen)
{
    std::string found;
    DIR* d = opendir(folder.c_str());
    if (!d)
        return found;
    std::vector<std::string> names;
    while (dirent* e = readdir(d))
        if (e->d_name[0] != '.')
            names.push_back(e->d_name);
    closedir(d);
    std::sort(names.begin(), names.end());
    for (const std::string& name : names) {
        const std::string path = folder + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < (off_t)(1 << 20))
            continue;   // (a folder, or too small to be a disc: the saves, a note)
        DiscCandidate c;
        c.name = name;
        c.kind = IdentifyDisc(path, &c.what);
        if (c.kind == DiscKind::Good && found.empty())
            found = path;
        if (seen)
            seen->push_back(c);
    }
    return found;
}

bool OpenIso(const std::string& path)
{
    std::string what;
    const DiscKind kind = IdentifyDisc(path, &what);
    if (kind != DiscKind::Good) {
        std::fprintf(stderr, "port: %s is not used: %s%s%s\n", path.c_str(), DiscKindReason(kind), what.empty() ? "" : ", ", what.c_str());
        return false;
    }
    g_iso = std::fopen(path.c_str(), "rb");
    if (!g_iso) {
        std::fprintf(stderr, "port: cannot open %s\n", path.c_str());
        return false;
    }
    return LoadExecutable();
}

bool IsoOpen() { return g_iso != nullptr; }

bool ReadSectors(std::uint32_t lsn, std::uint32_t count, void* dst)
{
    std::lock_guard<std::mutex> lock(g_isoLock);
    if (!g_iso)
        return false;
    static const bool trace = std::getenv("SLY1_CD_TRACE") != nullptr;
    if (std::fseek(g_iso, (long)((std::uint64_t)lsn * kSectorSize), SEEK_SET) != 0) {
        if (trace)
            std::fprintf(stderr, "cd: seek to sector %u failed\n", lsn);
        return false;
    }
    const auto readFrom = std::chrono::steady_clock::now();
    std::size_t got = std::fread(dst, kSectorSize, count, g_iso);
    {   // (SLY1_TIMELINE: each read and how long it took)
        char line[96];
        std::snprintf(line, sizeof line, "disc: %u sectors at %u read in %.2f ms", count, lsn,
                      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - readFrom).count());
        TimelineEvent(line);
    }
    if (trace) {   // each read with a checksum of its bytes, to compare runs
        std::uint32_t sum = 2166136261u;
        for (std::size_t i = 0; i < got * kSectorSize; i++)
            sum = (sum ^ static_cast<const unsigned char*>(dst)[i]) * 16777619u;
        std::fprintf(stderr, "cd: sectors %u+%u read %zu, sum %08X\n", lsn, count, got, sum);
    }
    return got == count;
}

bool LoadExecutableSegments()
{
    for (const Segment& s : g_segments) {
        if (s.vaddr + s.filesz > kRamSize)
            return false;
        std::memcpy(port_ram + s.vaddr, &g_exe[s.offset], s.filesz);
    }
    return !g_segments.empty();
}

bool ReadExecutable(std::uint32_t addr, void* dst, std::uint32_t size)
{
    for (const Segment& s : g_segments) {
        if (addr >= s.vaddr && addr + size <= s.vaddr + s.filesz) {
            std::memcpy(dst, &g_exe[s.offset + (addr - s.vaddr)], size);
            return true;
        }
    }
    return false;
}

}  // namespace port
