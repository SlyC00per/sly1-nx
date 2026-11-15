// SPDX-License-Identifier: AGPL-3.0-or-later
// Unit tests of game addresses and Ptr32 (include/port/ptr32.h). Built for the host (ctest) and
// into the Switch test program, so the same checks run on both.
#include "port/ptr32.h"
#include "unit.h"

#include <cstring>

namespace {

struct Node {
    int value;
    Ptr32<Node> next;
    Ptr32<void (int*)> fn;
    Ptr32<const char> name;
};
static_assert(sizeof(Node) == 16, "retail's layout: four 4-byte fields");

struct Base { int a; };
struct Derived : Base { int b; };

int g_counter;
void Bump(int* p) { ++*p; }

alignas(16) int g_array[8];
Node g_nodes[3];

}  // namespace

UNIT_TEST(ptr32_round_trip_in_image)
{
    Ptr32<int> p = &g_array[2];
    CHECK(p.get() == &g_array[2]);
    CHECK(p.addr() != 0 && p.addr() < port::kGameMemorySize);
    CHECK(port::FromGameAddr<int>(p.addr()) == &g_array[2]);
    Ptr32<int> null = nullptr;
    CHECK(null.addr() == 0 && null.get() == nullptr);
    CHECK(!null);
    CHECK(p);
}

UNIT_TEST(ptr32_words_carried_unchanged)
{
    // Float bits and other words the game passes as pointers come back as the same word.
    const std::uint32_t words[] = {0x3F800000u, 0xBF800000u, 0x42C80000u, 0x12000000u,
                                   0xFFFFFFFFu, 0x40000000u, 0x0FFFFFFFu, 5u};
    for (std::uint32_t w : words) {
        void* native = port::Native(w);
        CHECK(port::GameAddr(native) == w);
        Ptr32<void> stored = native;
        CHECK(stored.addr() == w);
    }
}

UNIT_TEST(ptr32_aliases_and_scratchpad)
{
    Ptr32<int> p = &g_array[1];
    CHECK(port::Native(p.addr() | 0x20000000u) == &g_array[1]);  // uncached alias
    CHECK(port::Native(p.addr() | 0x30000000u) == &g_array[1]);  // uncached accelerated
    CHECK(port::Native(0x70002000u) == port::Scratchpad() + 0x2000);
}

UNIT_TEST(ptr32_operators)
{
    Ptr32<int> p = &g_array[0];
    p++;
    CHECK(p.get() == &g_array[1]);
    p += 3;
    CHECK(p.get() == &g_array[4]);
    --p;
    CHECK(p == &g_array[3]);
    CHECK(p - &g_array[0] == 3);
    CHECK(p[1] == g_array[4]);
    *p = 7;
    CHECK(g_array[3] == 7);
    int* native = p;
    CHECK(native == &g_array[3]);
    CHECK((char*)p == (char*)&g_array[3]);
}

UNIT_TEST(ptr32_struct_links_and_calls)
{
    g_nodes[0].next = &g_nodes[1];
    g_nodes[1].next = &g_nodes[2];
    g_nodes[2].next = nullptr;
    g_nodes[2].value = 42;
    g_nodes[0].fn = Bump;
    g_nodes[0].name = "node";
    CHECK(g_nodes[0].next->next->value == 42);
    int n = 0;
    for (Node* it = &g_nodes[0]; it; it = it->next)
        n++;
    CHECK(n == 3);
    g_counter = 0;
    g_nodes[0].fn(&g_counter);
    CHECK(g_counter == 1);
    CHECK(std::strcmp(g_nodes[0].name, "node") == 0);
}

UNIT_TEST(ptr32_conversions)
{
    static Derived d;
    Ptr32<Derived> pd = &d;
    Ptr32<Base> pb = pd;
    CHECK(pb.get() == static_cast<Base*>(&d));
    Ptr32<void> pv = &d;
    Derived* back = (Derived*)pv;
    CHECK(back == &d);
    volatile bool yes = true;
    Ptr32<Derived> pick = yes ? pd : Ptr32<Derived>(nullptr);
    CHECK(pick.get() == &d);
    Ptr32<Derived> none = !yes ? pd : Ptr32<Derived>(nullptr);
    CHECK(none.get() == nullptr);
}
