// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The receiver: a small program that waits on the network for a build of the port, writes it to
// the SD card and starts it, and that the build starts again when its run ends (the next program
// to load, envSetNextLoad). With it tools/switch_run.py runs one build after another on a Switch
// nobody is at; the homebrew menu's own receiver (nxlink) needs Y pressed for each.
//
// What is sent (tools/switch_run.py), on port 28299:
//   "SLY1RX1\n", the arguments' length (4 bytes, little-endian) and the arguments, the file's
//   length (8 bytes) and the file. The answer is "OK\n" once the file is written, or a line
//   saying what failed.
// The build is started with the sender's address as nxlink gives it (the last argument,
// "xxxxxxxx_NXLINK_"), so its output goes to the sender as it does under nxlink (port 28771).
#include <arpa/inet.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <switch.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr int kPort = 28299;
// Where the receiver keeps itself: a folder of its own, so that the homebrew menu lists it (the
// menu shows one program a folder), and where a build finds it when its run ends.
const char* const kFolder = "sdmc:/switch/sly1_receiver";
const char* const kSelf = "sdmc:/switch/sly1_receiver/sly1_receiver.nro";
const char* const kBuild = "sdmc:/switch/sly1/received.nro";

bool ReadAll(int s, void* to, std::size_t n)
{
    auto* p = static_cast<unsigned char*>(to);
    while (n) {
        const ssize_t got = recv(s, p, n, 0);
        if (got <= 0)
            return false;
        p += got;
        n -= (std::size_t)got;
    }
    return true;
}

void Answer(int s, const char* text)
{
    send(s, text, std::strlen(text), 0);
}

// One build taken from a sender: true when it is written and set as the next program.
bool Take(int s, const sockaddr_in& from)
{
    char magic[8];
    std::uint32_t argsLength = 0;
    std::uint64_t size = 0;
    if (!ReadAll(s, magic, 8) || std::memcmp(magic, "SLY1RX1\n", 8) != 0) {
        Answer(s, "not a build for this receiver\n");
        return false;
    }
    if (!ReadAll(s, &argsLength, 4) || argsLength > 4096) {
        Answer(s, "the arguments are too long\n");
        return false;
    }
    std::string args(argsLength, '\0');
    if (!ReadAll(s, args.data(), argsLength) || !ReadAll(s, &size, 8) || size < 0x1000 || size > (256u << 20)) {
        Answer(s, "the file's length is not a build's\n");
        return false;
    }
    std::printf("a build of %.1f MB is coming\n", (double)size / 1048576.0);
    consoleUpdate(nullptr);
    std::FILE* f = std::fopen(kBuild, "wb");
    if (!f) {
        Answer(s, "the SD card's file could not be opened\n");
        return false;
    }
    std::vector<unsigned char> buffer(1 << 18);
    std::uint64_t left = size;
    bool good = true;
    while (left && good) {
        const std::size_t n = left < buffer.size() ? (std::size_t)left : buffer.size();
        good = ReadAll(s, buffer.data(), n) && std::fwrite(buffer.data(), 1, n, f) == n;
        left -= n;
    }
    good = std::fclose(f) == 0 && good;
    if (!good) {
        Answer(s, "the file did not arrive whole or could not be written\n");
        return false;
    }
    // (the program's own path first, as the homebrew menu gives it; the sender's address last)
    char link[32];
    std::snprintf(link, sizeof link, "%08lx_NXLINK_", (unsigned long)from.sin_addr.s_addr);
    const std::string argv = std::string(kBuild) + " " + args + " " + link;
    if (R_FAILED(envSetNextLoad(kBuild, argv.c_str()))) {
        Answer(s, "the build could not be set as the next program\n");
        return false;
    }
    Answer(s, "OK\n");
    return true;
}

// The receiver copied to its own place when it was started from elsewhere (sent through nxlink,
// or through a receiver already waiting): true when it is there now.
bool Install(const char* from)
{
    if (!from || std::strcmp(from, kSelf) == 0)
        return true;
    mkdir(kFolder, 0777);
    std::FILE* in = std::fopen(from, "rb");
    if (!in)
        return false;
    std::FILE* out = std::fopen(kSelf, "wb");
    bool good = out != nullptr;
    std::vector<unsigned char> buffer(1 << 16);
    for (std::size_t n; good && (n = std::fread(buffer.data(), 1, buffer.size(), in)) > 0;)
        good = std::fwrite(buffer.data(), 1, n, out) == n;
    std::fclose(in);
    if (out)
        good = std::fclose(out) == 0 && good;
    return good;
}

}  // namespace

int main(int argc, char** argv)
{
    consoleInit(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    std::printf("sly1-nx receiver\n\n");
    if (!Install(argc > 0 ? argv[0] : nullptr))
        std::printf("The receiver could not be copied to %s:\nruns will not come back to it.\n\n", kSelf);
    if (!envHasNextLoad()) {
        std::printf("This start of the homebrew menu cannot start another program.\nPress + to leave.\n");
    }
    int listener = -1;
    const bool sockets = R_SUCCEEDED(socketInitializeDefault());
    if (sockets) {
        listener = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in at{};
        at.sin_family = AF_INET;
        at.sin_port = htons(kPort);
        at.sin_addr.s_addr = INADDR_ANY;
        const int yes = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
        if (listener < 0 || bind(listener, (sockaddr*)&at, sizeof at) != 0 || listen(listener, 1) != 0) {
            std::printf("The network port could not be opened.\n");
            if (listener >= 0)
                close(listener);
            listener = -1;
        } else {
            fcntl(listener, F_SETFL, fcntl(listener, F_GETFL, 0) | O_NONBLOCK);
            const in_addr own{gethostid()};
            std::printf("Waiting for a build at %s, port %d.\nPress + to leave.\n", inet_ntoa(own), kPort);
        }
    } else {
        std::printf("The network could not be started.\n");
    }
    // (the screen kept on and the console awake while it waits: a night's runs)
    appletSetMediaPlaybackState(true);
    bool taken = false;
    while (appletMainLoop() && !taken) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        if (listener >= 0) {
            sockaddr_in from{};
            socklen_t length = sizeof from;
            const int s = accept(listener, (sockaddr*)&from, &length);
            if (s >= 0) {
                fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) & ~O_NONBLOCK);
                timeval limit{20, 0};   // (a sender that stops: not waited for for ever)
                setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit);
                taken = Take(s, from);
                std::printf(taken ? "starting it\n" : "that one was not taken; waiting again\n");
                close(s);
            }
        }
        consoleUpdate(nullptr);
        svcSleepThread(50'000'000);
    }
    appletSetMediaPlaybackState(false);
    if (listener >= 0)
        close(listener);
    if (sockets)
        socketExit();
    consoleExit(nullptr);
    return 0;
}
