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

void Retail(int* p) { *p += 10; }   // stands for a game function, at retail address 0x00123450

alignas(16) int g_array[8];
Node g_nodes[3];

const port::FunctionEntry g_functions[] = {{0x00123450u, (void*)&Retail}};

}  // namespace

// The function table tools/gen.py writes for the game; here, one function.
extern "C" const port::FunctionTable port_function_tables[] = {{g_functions, 1, nullptr}};
extern "C" const std::size_t port_function_table_count = 1;

UNIT_TEST(ptr32_functions_by_retail_address)
{
    // Stored as its retail address, as the executable's data holds functions; called natively.
    Ptr32<void (int*)> fn = port::P32(Retail);
    CHECK(fn.addr() == 0x00123450u);
    int x = 1;
    fn(&x);
    CHECK(x == 11);
    Ptr32<void (int*)> from_data = Ptr32<void (int*)>::FromAddr(0x00123450u);
    CHECK(from_data.get() == &Retail);
    CHECK(port::FromGameAddr<void (int*)>(0x00123450u) == &Retail);
    CHECK(port::GameAddrOf(&Retail) == 0x00123450u);
    // A function outside the table: an image address, still a round trip.
    Ptr32<void (int*)> other = port::P32(Bump);
    CHECK(other.addr() >= port::kImageAddr && other.get() == &Bump);
}

UNIT_TEST(ptr32_round_trip_in_image)
{
    Ptr32<int> p = port::P32(&g_array[2]);
    CHECK(p.get() == &g_array[2]);
    CHECK(p.addr() >= port::kImageAddr && p.addr() < port::kImageAddr + port::kGameMemorySize);
    CHECK(port::FromGameAddr<int>(p.addr()) == &g_array[2]);
    Ptr32<int> null = {};
    CHECK(null.addr() == 0 && null.get() == nullptr);
    null = &g_array[0];
    null = 0;
    CHECK(null.addr() == 0);
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
        Ptr32<void> stored;
        stored = native;
        CHECK(stored.addr() == w);
    }
}

UNIT_TEST(ptr32_aliases_and_scratchpad)
{
    // Main memory: a game address is its offset in port_ram, also through the uncached alias.
    Ptr32<unsigned char> p = port::P32(port_ram + 0x00275BEC);
    CHECK(p.addr() == 0x00275BECu);
    CHECK(port::Native(port::kRamSize) == port_ram + port::kRamSize);   // the end, as the heap's top
    CHECK(port::GameAddr(port_ram + port::kRamSize) == port::kRamSize);
    CHECK(port::Native(0x20275BECu) == port_ram + 0x00275BEC);  // uncached alias
    CHECK(port::GameAddr(port::Native(0x30275BECu)) == 0x30275BECu);  // a word, kept
    CHECK(port::Native(0x70002000u) == port::Scratchpad() + 0x2000);
    // A hardware register: memory that keeps what is written, and its address back.
    std::uint32_t* csr = (std::uint32_t*)port::Native(0x12001000u);
    *csr = 0x2000;
    CHECK(*(std::uint32_t*)port::Native(0x12001000u) == 0x2000);
    CHECK(port::GameAddr(csr) == 0x12001000u);
}

UNIT_TEST(ptr32_operators)
{
    Ptr32<int> p = port::P32(&g_array[0]);
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
    Ptr32<Derived> pd = port::P32(&d);
    Ptr32<Base> pb;
    pb = pd;
    CHECK(pb.get() == static_cast<Base*>(&d));
    Ptr32<void> pv = port::P32(&d);
    Derived* back = (Derived*)pv;
    CHECK(back == &d);
    volatile bool yes = true;
    Derived* pick = yes ? pd : (Derived*)0;
    CHECK(pick == &d);
    Derived* none = !yes ? pd : (Derived*)0;
    CHECK(none == nullptr);
    // A table of stored pointers, as the generated code initialises one.
    static Node table[2] = {{1, port::P32(&g_nodes[0]), port::P32(Bump), port::P32("a")},
                            {2, {}, 0, 0}};
    CHECK(table[0].next.get() == &g_nodes[0] && table[1].next.get() == nullptr);
    CHECK(table[0].name[0] == 'a' && table[1].fn.addr() == 0);
}
