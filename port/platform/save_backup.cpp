// SPDX-License-Identifier: AGPL-3.0-or-later
// Copies of the saves before the game writes over them (port/include/port/save_backup.h). Twice a
// run has started a new game in place of the player's and saved it over the first slot, the cause
// not found: with these, nothing is lost.

#include "port/save_backup.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace port {
namespace {

std::mutex g_lock;
std::map<std::string, std::uint64_t> g_lastHash;   // by save: the last copy's bytes, hashed
std::map<std::string, bool> g_sessionDone;         // by save: copied for this run already
std::string g_session;                             // this run's folder name, made at the first copy
unsigned g_sequence;

void MakeDirectory(const std::string& p)
{
#ifdef _WIN32
    mkdir(p.c_str());
#else
    mkdir(p.c_str(), 0755);
#endif
}

bool Exists(const std::string& p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

bool ReadAll(const std::string& p, std::vector<char>& bytes)
{
    std::FILE* in = std::fopen(p.c_str(), "rb");
    if (!in)
        return false;
    char chunk[4096];
    for (std::size_t n; (n = std::fread(chunk, 1, sizeof chunk, in)) > 0;)
        bytes.insert(bytes.end(), chunk, chunk + n);
    std::fclose(in);
    return true;
}

bool WriteAll(const std::string& p, const std::vector<char>& bytes)
{
    std::FILE* out = std::fopen(p.c_str(), "wb");
    if (!out)
        return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), out) == bytes.size();
    return (std::fclose(out) == 0) && ok;
}

std::vector<std::string> Entries(const std::string& dir)
{
    std::vector<std::string> all;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d))
            if (e->d_name[0] != '.')
                all.push_back(e->d_name);
        closedir(d);
    }
    std::sort(all.begin(), all.end());
    return all;
}

void RemoveTree(const std::string& p)
{
    for (const std::string& e : Entries(p))
        std::remove((p + "/" + e).c_str());
    rmdir(p.c_str());
}

std::string Stamp()
{
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    if (const std::tm* t = std::localtime(&now))
        std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", t);
    else
        std::snprintf(stamp, sizeof stamp, "%lld", (long long)now);
    char out[48];
    std::snprintf(out, sizeof out, "%s-%04u", stamp, g_sequence++ % 10000);   // (ordered within a second too)
    return out;
}

}  // namespace

std::string BackupFolder(const std::string& root) { return root + "-backups"; }

bool BackUpSave(const std::string& root, const std::string& host, const std::string& name)
{
    std::lock_guard<std::mutex> lk(g_lock);
    std::vector<char> bytes;
    if (!ReadAll(host, bytes) || bytes.empty())
        return false;
    std::uint64_t h = 1469598103934665603ull;
    for (char c : bytes)
        h = (h ^ (std::uint8_t)c) * 1099511628211ull;
    std::string flat = name;
    for (char& c : flat)
        if (c == '/' || c == '\\' || c == '!' || c == ':')
            c = '_';
    while (!flat.empty() && flat[0] == '_')
        flat.erase(0, 1);
    const std::string dir = BackupFolder(root);
    MakeDirectory(dir);
    bool made = false;
    // the first ever: kept for good
    MakeDirectory(dir + "/first");
    if (!Exists(dir + "/first/" + flat))
        made |= WriteAll(dir + "/first/" + flat, bytes);
    // this run's, once a save
    if (!g_sessionDone[flat]) {
        if (g_session.empty())
            g_session = Stamp();
        MakeDirectory(dir + "/sessions");
        MakeDirectory(dir + "/sessions/" + g_session);
        if (WriteAll(dir + "/sessions/" + g_session + "/" + flat, bytes)) {
            g_sessionDone[flat] = true;
            made = true;
        }
        const std::vector<std::string> runs = Entries(dir + "/sessions");
        for (std::size_t k = 0; k + 20 < runs.size(); k++)
            if (runs[k] != g_session)
                RemoveTree(dir + "/sessions/" + runs[k]);
    }
    // every copy that differs from the last
    auto last = g_lastHash.find(flat);
    if (last == g_lastHash.end() || last->second != h) {
        MakeDirectory(dir + "/recent");
        if (WriteAll(dir + "/recent/" + Stamp() + "-" + flat, bytes)) {
            g_lastHash[flat] = h;
            made = true;
        }
        const std::vector<std::string> recent = Entries(dir + "/recent");
        for (std::size_t k = 0; k + 60 < recent.size(); k++)
            std::remove((dir + "/recent/" + recent[k]).c_str());
    }
    return made;
}

}  // namespace port
