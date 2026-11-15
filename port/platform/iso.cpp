// SPDX-License-Identifier: AGPL-3.0-or-later
// The player's disc image (port/iso.h): sectors, the ISO 9660 root directory's executable, and
// its loadable segments as an ELF file. Written from the public ISO 9660 and ELF formats.
#include "port/iso.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

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

bool OpenIso(const std::string& path)
{
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
    if (std::fseek(g_iso, (long)((std::uint64_t)lsn * kSectorSize), SEEK_SET) != 0)
        return false;
    return std::fread(dst, kSectorSize, count, g_iso) == count;
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
