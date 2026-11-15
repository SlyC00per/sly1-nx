// SPDX-License-Identifier: AGPL-3.0-or-later
// A minimal test registry, so the unit tests build the same way for the host and the Switch
// (no test framework to port). UNIT_TEST(name) defines a test; CHECK(cond) records a failure.
#ifndef PORT_TESTS_UNIT_H
#define PORT_TESTS_UNIT_H

namespace unit {

using TestFn = void (*)();
struct Test {
    const char* name;
    TestFn fn;
    Test* next;
};
void Register(Test* t);
void Fail(const char* file, int line, const char* expr);
// Runs every test; prints each failure through log and returns the number of failures.
int RunAll(void (*log)(const char* line));

}  // namespace unit

#define UNIT_TEST(name)                                              \
    static void unit_test_##name();                                  \
    static unit::Test unit_entry_##name = {#name, unit_test_##name, nullptr}; \
    static const int unit_reg_##name = (unit::Register(&unit_entry_##name), 0); \
    static void unit_test_##name()

#define CHECK(cond) \
    do { if (!(cond)) unit::Fail(__FILE__, __LINE__, #cond); } while (0)

#endif
