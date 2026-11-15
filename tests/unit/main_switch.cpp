// SPDX-License-Identifier: AGPL-3.0-or-later
// The unit tests' entry point on the Switch: the results go to the debug-output channel, and the
// last line is the "<name>: done, status N" that tools/run_ryujinx.py waits for.
#include "unit.h"

#include <cstdio>
#include <cstring>

#include <switch.h>

static void Log(const char* line) { svcOutputDebugString(line, std::strlen(line)); }

int main()
{
    consoleDebugInit(debugDevice_SVC);  // stderr to the debug-output channel too
    int failures = unit::RunAll(Log);
    char buf[64];
    std::snprintf(buf, sizeof buf, "unit: done, status %d", failures ? 1 : 0);
    Log(buf);
    return failures ? 1 : 0;
}
