// SPDX-License-Identifier: AGPL-3.0-or-later
// The unit tests' entry point on the host (ctest runs it).
#include "unit.h"

#include <cstdio>

static void Log(const char* line) { std::printf("%s\n", line); }

int main() { return unit::RunAll(Log) ? 1 : 0; }
