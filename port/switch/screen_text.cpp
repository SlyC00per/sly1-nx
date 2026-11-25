// SPDX-License-Identifier: AGPL-3.0-or-later
// Text put on the Switch's screen through libnx's console, whatever the program's standard output
// is (started through nxlink it is the link's): for the message shown when no disc image can be
// played (port/host/main.cpp).
#include <switch.h>

#include <cstring>
#include <sys/iosupport.h>
#include <sys/reent.h>

namespace port {

// consoleInit must have been called.
void ScreenText(const char* text)
{
    const devoptab_t* const screen = GetDeviceOpTab("con:");
    if (screen && screen->write_r)
        screen->write_r(__getreent(), nullptr, text, std::strlen(text));
}

}  // namespace port
