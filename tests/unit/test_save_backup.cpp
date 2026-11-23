// SPDX-License-Identifier: AGPL-3.0-or-later
// Unit tests of the saves' backups (port/platform/save_backup.cpp): a save written over keeps its
// first copy, this run's copy and the recent copies; the same bytes are not copied twice.
#include "port/save_backup.h"
#include "unit.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

std::vector<std::string> List(const std::string& dir)
{
    std::vector<std::string> all;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d))
            if (e->d_name[0] != '.')
                all.push_back(e->d_name);
        closedir(d);
    }
    return all;
}

std::string Read(const std::string& p)
{
    std::string s;
    if (std::FILE* f = std::fopen(p.c_str(), "rb")) {
        char buf[256];
        for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;)
            s.append(buf, n);
        std::fclose(f);
    }
    return s;
}

void Write(const std::string& p, const std::string& s)
{
    if (std::FILE* f = std::fopen(p.c_str(), "wb")) {
        std::fwrite(s.data(), 1, s.size(), f);
        std::fclose(f);
    }
}

std::string TempRoot()
{
#if defined(__SWITCH__)
    std::string base = "sdmc:/switch/sly1_unit_tmp";
#else
    const char* t = std::getenv("TMPDIR");
    std::string base = std::string(t ? t : "/tmp") + "/sly1_unit_backup";
#endif
    char tail[32];
    std::snprintf(tail, sizeof tail, "-%d", (int)getpid());
    return base + tail;
}

}  // namespace

UNIT_TEST(save_backup_keeps_first_session_and_recent)
{
    const std::string root = TempRoot() + "/save";
    mkdir(TempRoot().c_str(), 0755);
    mkdir(root.c_str(), 0755);
    const std::string file = root + "/save1";

    // nothing there: nothing copied
    CHECK(!port::BackUpSave(root, file, "/BASCUS-97198/save1"));

    Write(file, "progress 70%");
    CHECK(port::BackUpSave(root, file, "/BASCUS-97198/save1"));
    const std::string dir = port::BackupFolder(root);
    CHECK(Read(dir + "/first/BASCUS-97198_save1") == "progress 70%");
    const std::vector<std::string> runs = List(dir + "/sessions");
    CHECK(runs.size() == 1);
    CHECK(!runs.empty() && Read(dir + "/sessions/" + runs[0] + "/BASCUS-97198_save1") == "progress 70%");
    CHECK(List(dir + "/recent").size() == 1);

    // the same bytes again: no new copy
    CHECK(!port::BackUpSave(root, file, "/BASCUS-97198/save1"));
    CHECK(List(dir + "/recent").size() == 1);

    // the game writes a new game over it; the next write copies that, and the first and this run's
    // copies still hold the player's progress
    Write(file, "new game 0%");
    CHECK(port::BackUpSave(root, file, "/BASCUS-97198/save1"));
    CHECK(Read(dir + "/first/BASCUS-97198_save1") == "progress 70%");
    CHECK(!runs.empty() && Read(dir + "/sessions/" + runs[0] + "/BASCUS-97198_save1") == "progress 70%");
    CHECK(List(dir + "/recent").size() == 2);

    // a long run of different saves: the recent copies stay at 60
    for (int k = 0; k < 70; k++) {
        Write(file, "save " + std::to_string(k));
        port::BackUpSave(root, file, "/BASCUS-97198/save1");
    }
    CHECK(List(dir + "/recent").size() == 60);
    CHECK(Read(dir + "/first/BASCUS-97198_save1") == "progress 70%");

    // (cleaned up)
    for (const std::string& sub : {std::string("/first"), std::string("/recent")}) {
        for (const std::string& e : List(dir + sub))
            std::remove((dir + sub + "/" + e).c_str());
        rmdir((dir + sub).c_str());
    }
    for (const std::string& r : List(dir + "/sessions")) {
        for (const std::string& e : List(dir + "/sessions/" + r))
            std::remove((dir + "/sessions/" + r + "/" + e).c_str());
        rmdir((dir + "/sessions/" + r).c_str());
    }
    rmdir((dir + "/sessions").c_str());
    rmdir(dir.c_str());
    std::remove(file.c_str());
    rmdir(root.c_str());
    rmdir(TempRoot().c_str());
}
