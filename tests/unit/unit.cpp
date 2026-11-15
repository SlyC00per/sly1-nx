// SPDX-License-Identifier: AGPL-3.0-or-later
#include "unit.h"

#include <cstdio>

namespace unit {

static Test* g_tests;
static int g_failures;
static void (*g_log)(const char*);

void Register(Test* t)
{
    t->next = g_tests;
    g_tests = t;
}

void Fail(const char* file, int line, const char* expr)
{
    char buf[512];
    std::snprintf(buf, sizeof buf, "FAIL %s:%d: %s", file, line, expr);
    g_log(buf);
    g_failures++;
}

int RunAll(void (*log)(const char* line))
{
    g_log = log;
    g_failures = 0;
    int count = 0;
    for (Test* t = g_tests; t; t = t->next) {
        int before = g_failures;
        t->fn();
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s %s", g_failures == before ? "ok  " : "FAIL", t->name);
        log(buf);
        count++;
    }
    char buf[128];
    std::snprintf(buf, sizeof buf, "unit: %d tests, %d failures", count, g_failures);
    log(buf);
    return g_failures;
}

}  // namespace unit
