# SPDX-License-Identifier: AGPL-3.0-or-later
"""Named patches: the few places in the decompilation's sources that tools/gen.py cannot rewrite
by rule (docs/DESIGN.md, section 3). Each is an exact replacement in the generated copy, with the
reason; the decompilation itself is never changed.

A patch applies before the pointer stage, to the text the decompilation has. It must match
exactly once: a patch that no longer matches (after a submodule bump) stops the generation
instead of being skipped, so a patch never goes stale silently.
"""

# (file under sly1-decomp/, the exact text, its replacement, why)
PATCHES = [
    ("src/main/property_registry.cpp",
     "#define PROP(id, n) { id, &g_objectPropertyDescriptors[n] }",
     "#define PROP(id, n) { id, port::P32(&g_objectPropertyDescriptors[n]) }",
     "the entry's pointer is written in the macro's body, so its stored-pointer initialiser "
     "(port::P32, ptr32.h) goes there, once for every entry"),

    # The variadic functions that read the PS2's saved argument registers as 8-byte slots
    # (port/varargs.h). Each fills such a run from its arguments, by the types its callers pass,
    # and reads it as before.
    ("src/main/curve_segment.cpp",
     "    char* ap = (char*)__builtin_next_arg(n) - 56;",
     "    port::Slot port_slots[2 * 32 + 1];\n"
     "    char* ap = (char*)port_slots;\n"
     "    { std::va_list va; va_start(va, n); s32 k = 0;\n"
     "      for (s32 q = 0; q < n && q < 32; q++) { port_slots[k++] = port::SlotPtr(va_arg(va, void*));"
     " port_slots[k++] = port::SlotDouble(va_arg(va, double)); }\n"
     "      port_slots[k++] = port::SlotPtr(va_arg(va, void*)); va_end(va); }",
     "variadic arguments: n pairs of (curve coefficients, limit as a double) and the result's "
     "address (object_cycler.cpp's call)"),
    ("src/main/object_properties.cpp",
     "    void* ap = (char*)__builtin_next_arg(kinds) - 32;",
     "    port::Slot port_slots[32];\n"
     "    void* ap = (char*)port_slots;\n"
     "    { std::va_list va; va_start(va, kinds);\n"
     "      for (s32 q = 0; q < count && q < 32; q++) port_slots[q] = port::SlotPtr(va_arg(va, void*));\n"
     "      va_end(va); }",
     "variadic arguments: count out-parameter addresses (object_host_functions.cpp's calls)"),
    ("src/main/object_properties.cpp",
     "    ap = (char*)__builtin_next_arg(size) - (8 - __builtin_args_info(0)) * 8;",
     "    port::Slot port_slots[2 * 16 + 2];\n"
     "    ap = (char*)port_slots;\n"
     "    { std::va_list va; va_start(va, size); s32 k = 0;\n"
     "      port_slots[k++] = port::SlotPtr(va_arg(va, void*));\n"
     "      while (k < 2 * 16) { s32 s = va_arg(va, s32); port_slots[k++] = port::SlotInt(s);"
     " port_slots[k++] = port::SlotPtr(va_arg(va, void*)); if (s == 0) break; }\n"
     "      va_end(va); }",
     "variadic arguments: the first buffer's address, then (size, address) pairs, a zero size "
     "and a null address last (world_listed_object.cpp's calls)"),
    ("src/main/object_properties.cpp",
     "    void* ap = (char*)__builtin_next_arg(first) - 56;",
     "    port::Slot port_slots[16];\n"
     "    void* ap = (char*)port_slots;\n"
     "    { std::va_list va; va_start(va, first);\n"
     "      for (s32 q = 0; q < 16; q++) { void* a = va_arg(va, void*); port_slots[q] = port::SlotPtr(a);"
     " if (a == 0) break; }\n"
     "      va_end(va); }",
     "variadic arguments: buffer addresses, a null one last (world_listed_object.cpp's call)"),
    ("src/main/player_request.cpp",
     "    u64* ap = (u64*)((char*)__builtin_next_arg(out) - 32);",
     "    port::Slot port_slots[32];\n"
     "    u64* ap = (u64*)port_slots;\n"
     "    { std::va_list va; va_start(va, out);\n"
     "      for (s32 q = 0; q < (n - 1) + (m - 1) && q < 32; q++) port_slots[q] = port::SlotDouble(va_arg(va, double));\n"
     "      va_end(va); }",
     "variadic arguments: n - 1 then m - 1 weights as doubles (FillOuterProductTable's caller)"),
    ("src/main/text_format.cpp",
     "    void* ap = (char*)__builtin_next_arg(fmt) - 56;",
     "    port::Slot port_slots[64];\n"
     "    void* ap = (char*)port_slots;\n"
     "    { std::va_list va; va_start(va, fmt); port::FormatSlots(fmt, va, port_slots, 64); va_end(va); }",
     "variadic arguments of PrintFormatted: as the format consumes them (port::FormatSlots)"),
    ("src/main/text_format.cpp",
     "    void* ap = (char*)__builtin_next_arg(fmt) - 48;",
     "    port::Slot port_slots[64];\n"
     "    void* ap = (char*)port_slots;\n"
     "    { std::va_list va; va_start(va, fmt); port::FormatSlots(fmt, va, port_slots, 64); va_end(va); }",
     "variadic arguments of FormatToString: as the format consumes them (port::FormatSlots)"),
    ("src/main/text_format.cpp",
     "    void* ap = (char*)__builtin_next_arg(fmt) - 40;",
     "    port::Slot port_slots[64];\n"
     "    void* ap = (char*)port_slots;\n"
     "    { std::va_list va; va_start(va, fmt); port::FormatSlots(fmt, va, port_slots, 64); va_end(va); }",
     "variadic arguments of FormatToStringBounded: as the format consumes them (port::FormatSlots)"),
    # The anti-tamper checksums: a CRC over the game's own code (from 0x100000) and one over
    # memory. On a genuine retail copy both come out right and their flags stay 0; the port's
    # machine code is not retail's, so they could never pass, and a failed flag changes the
    # gameplay (player_target.cpp after 30 to 120 s, state_notifier.cpp, object_gatherer.cpp).
    # The port keeps the genuine copy's outcome: the flags are never set.
    ("src/main/render_frame.cpp",
     "        g_codeChecksumFailed = 1;",
     "        /* port: the genuine copy's outcome, the flag stays 0 (tools/patches.py) */",
     "the code checksum covers retail's machine code, which the port does not have"),
    ("src/main/warp_menu.cpp",
     "        D_00275C34 = 1;",
     "        /* port: the genuine copy's outcome, the flag stays 0 (tools/patches.py) */",
     "the memory checksum covers retail's code and data addresses, which the port does not have"),
    # The handler of interrupt cause 5 is retail's address 0x15E6A8, written as an offset from a
    # stripped function's leftover (func_0015E678 + 0x30): the function that begins there.
    ("src/main/render_frame.cpp",
     "(void (*)(void))((u8*)func_0015E678 + 0x30)",
     "(void (*)(void))func_0015E6A8",
     "the port has no addresses inside code: the handler by the function retail's address names"),
    # Three property accessors, assembly in retail, are declared with no parameters: the
    # property table calls them with the object and a vector, which the PS2's registers carried.
    # The port's C++ versions (port/game_asm/game_asm.cpp) take them as parameters.
    ("src/main/object_host_functions.cpp",
     "void GetPlacedObjectLocalVelocity();",
     "void GetPlacedObjectLocalVelocity(void* self, void* out);",
     "the getter's real parameters, for the port's C++ version of the assembly"),
    ("src/main/object_host_functions.cpp",
     "void StorePlacedObjectLocalVelocity();",
     "void StorePlacedObjectLocalVelocity(void* self, void* in);",
     "the setter's real parameters, for the port's C++ version of the assembly"),
    ("src/main/object_host_functions.cpp",
     "void StorePlacedObjectLocalSpin();",
     "void StorePlacedObjectLocalSpin(void* self, void* in);",
     "the setter's real parameters, for the port's C++ version of the assembly"),
    # Their callers end a list of addresses with a bare 0: an int, which a 64-bit ABI need not
    # widen into a pointer's slot (Apple's arm64 passes variadic ints in 8-byte stack slots with
    # undefined upper halves). A null pointer there instead.
    ("src/main/world_listed_object.cpp",
     "StartToSprChain(node->count * 16, p, node->count * 16, p + 0x2C, node->count * 16, p + 0x58, 0, 0);",
     "StartToSprChain(node->count * 16, p, node->count * 16, p + 0x2C, node->count * 16, p + 0x58, 0, (void*)0);",
     "a null address, not an int 0, as the last variadic argument"),
    ("src/main/world_listed_object.cpp",
     "StartToSprChain(next->count * 16, p, next->count * 16, p + 0x2C, next->count * 16, p + 0x58, 0, 0);",
     "StartToSprChain(next->count * 16, p, next->count * 16, p + 0x2C, next->count * 16, p + 0x58, 0, (void*)0);",
     "a null address, not an int 0, as the last variadic argument"),
    ("src/main/world_listed_object.cpp",
     "StartFromSprChain(p, p + 0x2C, p + 0x58, 0);",
     "StartFromSprChain(p, p + 0x2C, p + 0x58, (void*)0);",
     "a null address, not an int 0, as the last variadic argument"),

    # 64-bit hazards: 32-bit unsigned arithmetic added to a pointer, which wraps on the PS2's
    # 32-bit addresses and lands 4 GB away here. Reported to the decompilation (byte-neutral there).
    ("src/main/placed_object_model.cpp",
     "strip->unpack = (StripUnpack*)((u8*)strip->unpack + (strip->addr[0] - starts[0]));",
     "strip->unpack = (StripUnpack*)((u8*)strip->unpack + (s32)(strip->addr[0] - starts[0]));",
     "the unpack block moves by the difference of two u32 addresses, which may be negative"),
    ("src/main/placed_object_model.cpp",
     "m->p = (u16*)((u8*)m->p + (m->addr - start));",
     "m->p = (u16*)((u8*)m->p + (s32)(m->addr - start));",
     "the morph target's words move by the difference of two u32 addresses, which may be negative"),

    # Frame layout: g++ 2.95 put t right after arr[32] in the frame, and the cleanup walks back from
    # &t; a modern compiler places locals as it likes.
    ("src/main/script_machine.cpp",
     "    p = &t;\n    while (arr != p) {",
     "    p = arr + 32;\n    while (arr != p) {",
     "the call's argument array is destroyed from its end, which is &t only in retail's frame"),

    # AddDmaRefTag takes the block's address as an s32 (the tag's upper word); four callers pass a
    # pointer through a cast of the function, which on 64 bits keeps the pointer's low half. Each
    # passes the pointer's game address instead.
    ('src/main/render_frame.cpp',
     '((void (*)(PacketWriter*, s32, void*))AddDmaRefTag)((PacketWriter*)list, ((u32)((u8*)end - (u8*)value) + 15) >> 4, value);',
     'AddDmaRefTag((PacketWriter*)list, ((u32)((u8*)end - (u8*)value) + 15) >> 4, (s32)port::GameAddrOf(value));',
     "a DMA reference tag's address: the pointer's game address, not its low 32 bits"),
    ('src/main/game_progress.cpp',
     '((void (*)(PacketWriter*, s32, void*))AddDmaRefTag)(&g_vifPacketWriter, self->tex[t]->n, self->tex[t]->data);',
     'AddDmaRefTag(&g_vifPacketWriter, self->tex[t]->n, (s32)port::GameAddrOf(self->tex[t]->data));',
     "a DMA reference tag's address: the pointer's game address, not its low 32 bits"),
    ('src/main/display.cpp',
     '((void (*)(PacketWriter*, s32, void*))AddDmaRefTag)((PacketWriter*)&a, 0x2C, g_screenClearPacketC);',
     'AddDmaRefTag((PacketWriter*)&a, 0x2C, (s32)port::GameAddrOf(g_screenClearPacketC));',
     "a DMA reference tag's address: the pointer's game address, not its low 32 bits"),
    ('src/main/warp_menu.cpp',
     '((void (*)(PacketWriter*, s32, void*))AddDmaRefTag)(self, n >> 4, c);',
     'AddDmaRefTag(self, n >> 4, (s32)port::GameAddrOf(c));',
     "a DMA reference tag's address: the pointer's game address, not its low 32 bits"),

    # PS3 parity (docs/PS3_PARITY.md): no memory card questions. The port's card is always there,
    # formatted and roomy (port/platform/memory_card.cpp); where the game would ask whether to
    # create its save file (page 7), it creates it, as the answer "Yes" does.
    ("src/main/memory_card.cpp",
     "            case 3: SetPauseMenuLayerPage(&g_pauseMenuElement, 1, 7); break;",
     "            case 3: { s32 CreateMemoryCardSaveFiles(MemoryCard* self);\n"
     "                StartBackgroundJob(&g_backgroundJob, (void*)CreateMemoryCardSaveFiles, self); break; }",
     "the save file is created without asking, as the PS3 release has no memory card questions"),
]


class PatchError(Exception):
    pass


def apply(rel, text):
    """The text of file rel with its patches applied, and how many applied."""
    n = 0
    for f, old, new, why in PATCHES:
        if f != rel:
            continue
        count = text.count(old)
        if count != 1:
            raise PatchError("patch for %s (%s) matches %d times, not once: %r" % (f, why, count, old[:80]))
        text = text.replace(old, new)
        n += 1
    return text, n


def files():
    return {f for f, *_ in PATCHES}
