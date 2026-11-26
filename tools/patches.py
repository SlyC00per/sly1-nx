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
     "    double* ap = (double*)((char*)__builtin_next_arg(out) - 32);",
     "    port::Slot port_slots[32];\n"
     "    double* ap = (double*)port_slots;\n"
     "    { std::va_list va; va_start(va, out);\n"
     "      for (s32 q = 0; q < (n - 1) + (m - 1) && q < 32; q++) port_slots[q] = port::SlotDouble(va_arg(va, double));\n"
     "      va_end(va); }",
     "variadic arguments: n - 1 then m - 1 weights, doubles (ConvertFloatToDouble's results, by "
     "FillOuterProductTable's one caller), as the slots the function reads them from"),
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
    # The third of them: a CRC over the code from 0x100010, advanced four bytes at a time while a
    # movie plays (AdvanceCodeChecksum). It began to run when the port's movies did (2026-11-19) and
    # failed at the end of its range: with its flag set the van of the races drives at four tenths
    # of its speed (ControlPathFollower, world.cpp: "Burning Rubber", the user, 2026-11-24) and a
    # turret's aim is refused (tint_blend_collider.cpp).
    ("src/main/movie_player.cpp",
     "            g_movieChecksumFailed = 1;",
     "            /* port: the genuine copy's outcome, the flag stays 0 (tools/patches.py) */",
     "the movie-time code checksum covers retail's machine code, which the port does not have"),
    # The fourth: an object a level places (class CodeChecksum; only "Into the Machine" has one,
    # by a census of the 46 levels on the Mac, 2026-11-24). While it is updated it takes a CRC over
    # 0x148D64 bytes from 0x100018, 64 words a frame, and at the end, when the sum is not 0, sets
    # g_checksumMismatch and sends its level's script event 2: what then happens is the level's
    # script's. In the port it ended with a sum of 0x16F3063. Neither is done now.
    ("src/main/placed_object.cpp",
     "        if (n == 0 && crc != 0) {\n            g_checksumMismatch[0] = 1;\n            TriggerScriptEvent(self, 2, 0, 0);\n        }",
     "        /* port: the genuine copy's outcome, no mismatch and no script event (tools/patches.py) */",
     "the placed code checksum covers retail's machine code, which the port does not have"),
    # The letterbox bars told to the port as the game draws them: a cut scene is on (the renderer
    # may draw the outlines' lines another width then, SLY1_LINE_WIDTH_CUT; perflines logs it).
    ("src/main/screen_element.cpp",
     "void DrawLetterboxElement(LetterboxElement* self)\n{\n    f32 x = self->blend * 66.40001f;",
     "void DrawLetterboxElement(LetterboxElement* self)\n{\n    port::LetterboxNoted(self->blend);\n    f32 x = self->blend * 66.40001f;",
     "the letterbox bars' blend, for the port to know a cut scene"),
    # The game's clock on a 60 Hz screen. The clock's step is the frame's measured time, held between
    # a sixtieth and a thirtieth of a second. An NTSC PS2 shows 59.94 frames a second, so its frames
    # measure a little over a sixtieth and the lower limit never acts: the clock is real time. The
    # Switch shows 60.00: its frames measure a sixtieth give or take the timing's jitter, about half
    # fall under the limit and are raised to it, and the clock gains. Measured in "A Deadly Dance"
    # (SLY1_SND_BEAT, 2026-11-24): 146 ms in 40 s, 190 ms in 48 s, 162 ms in 36 s, about 0.4%, which
    # put the boss's beats that far ahead of the music by a section's end. The lower limit is 15 ms
    # here: a frame's step is its measured time, and the clock is real time as on the PS2. (The upper
    # limit stays: a frame over a thirtieth still loses the rest, as retail's does.)
    ("src/main/game_clock.cpp",
     "    f32 dt = t * 0.0000000033908420f;\n    f32 lo = 0.016666668f;\n    f32 hi = 0.033333335f;",
     "    f32 dt = t * 0.0000000033908420f;\n    f32 lo = 0.015f;   /* port: 60.00 Hz, not 59.94 (tools/patches.py) */\n    f32 hi = 0.033333335f;",
     "the game's clock is real time on a 60 Hz screen"),
    # SLY1_SND_BEAT=1 (diagnosis): the third world's boss's beat clock told to the port as it
    # restarts and at each beat (port::BossBeat), for measuring it against the music and real time.
    ("src/main/warp_menu.cpp",
     "        self->beatInBar = 0;\n        self->beat = 0;\n        if (sub != 0) {",
     "        self->beatInBar = 0;\n        self->beat = 0;\n        port::BossBeat(0, g_gameClock.time, self->beatLength, 0);\n        if (sub != 0) {",
     "the boss's beat clock restarted, for SLY1_SND_BEAT"),
    ("src/main/warp_menu.cpp",
     "        self->lastBeatTime = self->nextBeatTime;\n        self->beat = a;\n        self->beatInBar = b;",
     "        self->lastBeatTime = self->nextBeatTime;\n        self->beat = a;\n        self->beatInBar = b;\n        port::BossBeat(1, g_gameClock.time, self->beatLength, a);",
     "the boss's beats, for SLY1_SND_BEAT"),
    # SLY1_VAN_LOG=1 (diagnosis): what a driven path follower (the van of the races) is given and
    # does, twice a second: the stick's magnitude, its top and target speeds, its speed, its pace,
    # its zone's limit (port::VanLog, kernel.cpp).
    ("src/main/world.cpp",
     "        self->targetSpeed = self->maxSpeed * pad->stickMagnitude;\n        if (g_movieChecksumFailed != 0) {",
     "        self->targetSpeed = self->maxSpeed * pad->stickMagnitude;\n        port::VanLog(pad->stickMagnitude, self->maxSpeed, self->targetSpeed, self->v150.x, self->v150.y, self->v150.z, self->pace, self->activeZone ? self->activeZone->maxSpeed : -1.0f, self->kind, self->lapsLeft, g_movieChecksumFailed);\n        if (g_movieChecksumFailed != 0) {",
     "the driven path follower's numbers, for SLY1_VAN_LOG"),
    # The movie player's record, at retail's address among the other globals, was 12 bytes longer
    # here than retail's 0x148: its stream's sink is a C++ class with virtual methods, whose hidden
    # pointer is 8 bytes and aligned to 8 on a 64-bit machine (4 on the PS2), so everything after
    # it lay 12 further on and the record's last fields lay over the globals after it: `cur` over
    # g_movieChecksumFailed, `remain` over g_keyedMovieWatched. From the first movie on the
    # checksum's flag held a pointer: the races' van at four tenths of its speed whatever the
    # checksum's result ("Burning Rubber", the user, 2026-11-24; SLY1_VAN_LOG showed 1,200 of
    # 3,000 and the flag 0x248D78). The 20 bytes of padding after the two packets, which nothing
    # reads, are 8 here: the fields after them and the record's size are retail's again (gen.py's
    # list of records that differ keeps the two packets, which only this file's code names).
    ("include/movie_player.h",
     "    u8 pad11C[0x14];",
     "    u8 pad11C[0x14 - 0xC];   /* port: the sink before it is 12 bytes longer here (tools/patches.py) */\n    static void port_layout_check();",
     "the movie player's record at retail's size: its tail off the globals after it"),
    ("src/main/movie_player.cpp",
     "void AdvanceCodeChecksum(MoviePlayer* self)\n{",
     "void MoviePlayer::port_layout_check()\n{\n    static_assert(sizeof(MoviePlayer) == 0x148 && __builtin_offsetof(MoviePlayer, frameSemaphore) == 0x130 && __builtin_offsetof(MoviePlayer, cur) == 0x13C, \"the movie player's record must end where retail's does\");\n}\n\nvoid AdvanceCodeChecksum(MoviePlayer* self)\n{",
     "the build fails if the movie player's record is not retail's size"),
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

    # Widescreen: the emblem that closes and opens over the picture between scenes is drawn straight
    # into the GS packet with one scale for both axes; across the screen it is 3/4 of that, about
    # its own centre, so that it keeps its shape in the 16:9 picture.
    ("src/main/colored_triangle_set.cpp",
     "    BuildUniformScaleAboutPoint(0, mat, (1.0f - t) * (1280.0f / self->width) + t * (4.0f / self->width));\n",
     "    BuildUniformScaleAboutPoint(0, mat, (1.0f - t) * (1280.0f / self->width) + t * (4.0f / self->width));\n    mat[0] /= port::WideFactor();\n",
     "the scene-change emblem's shape in the 16:9 picture"),

    # A line in the log for each fade that is to load a level (where it was asked from) and each
    # movie queued by the completion fade: for finding what sends the game to another level.
    ("src/main/warp_menu.cpp",
     "void StartScreenFade(ScreenFade* dst, FadeRequest* args, s32 count)\n{\n    IdentityKey* p;\n    s32 mode;\n",
     "void StartScreenFade(ScreenFade* dst, FadeRequest* args, s32 count)\n{\n    IdentityKey* p;\n    s32 mode;\n    port::NoteFade((const char*)__builtin_return_address(0) - (const char*)&StartScreenFade, args->kind, args->f08, count, dst->mode);\n",
     "each fade to a level noted in the log"),
    ("src/main/game_progress.cpp",
     "    s32 s0 = -1;\n\n    if (kind != 4) { if (kind < 5) { if (kind == 2) { s0 = 0xD; } } } else { s0 = 0xE; }",
     "    s32 s0 = -1;\n\n    port::NoteFade(-1, kind, ComputeCompletionMask(), -1, -1);\n    if (kind != 4) { if (kind < 5) { if (kind == 2) { s0 = 0xD; } } } else { s0 = 0xE; }",
     "the completion fade noted in the log"),

    # Movies (docs/VIDEO.md). The game sends the video stream to the IPU by starting a DMA transfer
    # on its channel; the port has no IPU, and takes the bytes there for its own decoder.
    ("src/main/movie_player.cpp",
     "    *(volatile u32*)0x1000B400 = 0x101;\n    *(volatile u32*)0x1000F590 = old;\n    return n;",
     "    *(volatile u32*)0x1000B400 = 0x101;\n    *(volatile u32*)0x1000F590 = old;\n    port::movie::VideoBytes(buf, n);\n    return n;",
     "the video stream's bytes taken where the game sends them to the IPU"),
    # Widescreen: a movie's picture is 4:3 with its own black bands above and below (60 lines of
    # 480 in 14 of the 17: a 16:9 picture inside). In the port's 16:9 picture it is drawn larger by
    # port::movie::Zoom() (4/3) about the middle, which fills the screen and leaves those bands
    # off it. The picture's last column and row are sampled at their own middles, not half a
    # texel past them, where the GS's memory has something else (a line of colours at the edge).
    ("src/main/movie_player.cpp",
     "    AddGifXyzfEntry(&buf, 0x6C00, 0x7900, 0, 0);\n    AddGifUvEntry(&buf, (a1 << 4) | 8, (a2 << 4) | 8);\n    AddGifXyzfEntry(&buf, 0x9400, 0x8700, 0, 0);",
     "    AddGifXyzfEntry(&buf, 0x8000 - (s32)(0x1400 * port::movie::Zoom() / port::WideFactor()), 0x8000 - (s32)(0x700 * port::movie::Zoom()), 0, 0);\n"
     "    AddGifUvEntry(&buf, (a1 << 4) - 8, (a2 << 4) - 8);\n"
     "    AddGifXyzfEntry(&buf, 0x8000 + (s32)(0x1400 * port::movie::Zoom() / port::WideFactor()), 0x8000 + (s32)(0x700 * port::movie::Zoom()), 0, 0);",
     "a movie's picture filling the 16:9 screen"),
    # A movie's picture goes to the GS in one transfer of its rows, where the game sends 1,200 of 16
    # by 16 pixels (the IPU's blocks): the port's decoder writes rows (port/video/movie.cpp), and
    # one transfer costs the port's GS a small part of what 1,200 do.
    ("src/main/movie_player.cpp",
     "    AddGifRegisterWrite(&f, 0x52, ((u64)0x10 << 32) | 0x10);\n"
     "    for (y = 0; y < ny; y++) {\n"
     "        for (x = 0; x < nx; x++) {\n"
     "            BeginGifTag(&f, 0, 1, 0xE);\n"
     "            AddGifRegisterWrite(&f, 0x51, ((u64)(y << 4) << 32) | ((u64)(x << 4) << 48));\n"
     "            AddGifRegisterWrite(&f, 0x53, 0);\n"
     "            AddGifImageTag(&f, 0x40);\n"
     "            AddDmaRefTag(&f, 0x40, DMA_PHYSICAL_ADDRESS(addr));\n"
     "            addr += 0x400;\n"
     "        }\n"
     "    }\n",
     "    AddGifRegisterWrite(&f, 0x52, ((u64)h << 32) | (u64)w);\n"
     "    (void)ny; (void)nx; (void)x;\n"
     "    if (port::movie::Direct()) {   // the renderer has the picture already: the packet names it (port/gs.h)\n"
     "        BeginGifTag(&f, 0, 1, 0xE);\n"
     "        AddGifRegisterWrite(&f, 0x7F, ((u64)0x1E00 << 32) | port::movie::PictureNumber());\n"
     "        h = 0;\n"
     "    } else {\n"
     "        BeginGifTag(&f, 0, 1, 0xE);\n"
     "        AddGifRegisterWrite(&f, 0x51, 0);\n"
     "        AddGifRegisterWrite(&f, 0x53, 0);\n"
     "    }\n"
     "    for (y = 0; y < h; y += 192) {   // (an image tag counts up to 0x7FFF quadwords: 192 rows of 640 pixels)\n"
     "        s32 port_rows = h - y < 192 ? h - y : 192;\n"
     "        s32 port_qw = port_rows * w / 4;\n"
     "        AddGifImageTag(&f, port_qw);\n"
     "        AddDmaRefTag(&f, port_qw, DMA_PHYSICAL_ADDRESS(addr));\n"
     "        addr += port_qw * 16;\n"
     "    }\n",
     "a movie's picture sent to the GS in one transfer"),
    # A line in the log when an animation's event starts a fade to a level or queues a movie: which
    # animation, and where in it.
    # SLY1_SCRIPT_TRACE=N: the script machine's calls (built-in or host function, arguments, result)
    # kept in a ring, printed when animation N starts: what a level's script did before it.
    ("src/main/script_machine.cpp",
     "    CopyConstructScriptValue(ret, &b);\n    p = arr + 32;\n    while (arr != p) {",
     "    port::NoteScriptCall(a.tag, a.tag == 0x1D ? (void*)ent->fn : a.tag == 0x21 ? (void*)host->fn : (void*)0, count, (const int*)arr, (const int*)&b);\n"
     "    CopyConstructScriptValue(ret, &b);\n    p = arr + 32;\n    while (arr != p) {",
     "a script call noted for the trace"),
    # SLY1_TICK_TRACE=N: object N's script timer at each of its tick events (0: every object's running timer).
    ("src/main/base_object.cpp",
     "        f32 f = ((PlacedObject*)self)->eventTimer;\n",
     "        f32 f = ((PlacedObject*)self)->eventTimer;\n        port::NoteTick(self->id, f, *(f32*)p[0]);\n",
     "a script timer's countdown noted for the trace"),
    # SLY1_EVENT_TRACE=N: each script event raised on object N (its key, where from).
    ("src/main/base_object.cpp",
     "    if (SuppressScriptEvent(self, idx, flag, out) == 0) {",
     "    port::NoteScriptEvent(self->id, idx, (long)((char*)__builtin_return_address(0) - (char*)&TriggerScriptEvent));\n"
     "    if (SuppressScriptEvent(self, idx, flag, out) == 0) {",
     "a script event noted for the trace"),
    # SLY1_FREEZE_TRACE=N: object N frozen or let go (the host prints the call stack).
    ("src/main/game_progress.cpp",
     "void SetPlacedObjectFrozen(PlacedObject* self, s32 flag)\n{\n",
     "void SetPlacedObjectFrozen(PlacedObject* self, s32 flag)\n{\n    port::NoteFreeze(self->id, flag);\n",
     "an object's freezing noted for the trace"),
    # SLY1_CAMERA_TRACE=A,B: the camera's position and area each frame of the world, frames A to B.
    ("src/main/game_progress.cpp",
     "void UpdateGroupsNearCamera(World* self)\n{\n",
     "void UpdateGroupsNearCamera(World* self)\n{\n    port::NoteCamera((const float*)((Camera*)g_camera)->position, ((Camera*)g_camera)->areaMask);\n",
     "the camera's position noted for the trace"),
    # The camera's push out of geometry kept in the world. ComputeConstrainedPushOut solves a weight
    # for each of up to 64 hit points; from a place inside solid geometry the system is degenerate
    # and the weights run to the largest float, so the camera is put some 1e21 units away and takes
    # ten seconds to come back. Retail's code does the same sums; it is reached when a level's
    # opening fly-over ends on its last pose in the very step a script cuts it (A Stealthy
    # Approach: the camera ends inside the van), which a step of 1/60 s never does and the port's
    # longer steps often do. The level then freezes the van (it is out of the camera's range),
    # the van's script timer is cleared, and its script runs again and leaves for the hideout:
    # the blue screen after Sly comes out of the van. A push that lands beyond any level's size
    # is not taken: the camera stays where it was going.
    ("src/main/camera.cpp",
     "    Vu0::Q(*out) = Vu0::MulAddX(Vu0::Q(*origin), Vu0::Q(hit), Vu0::ScalarAt(clock->step));\n    PopBackMemoryMark();",
     "    {\n"
     "        Vec4 pushed;\n"
     "        Vu0::Q(pushed) = Vu0::MulAddX(Vu0::Q(*origin), Vu0::Q(hit), Vu0::ScalarAt(clock->step));\n"
     "        if (port::CameraPushInWorld((const float*)&pushed, (const float*)dest))\n"
     "            *out = pushed;\n"
     "        else\n"
     "            *out = *(Vec4*)dest;\n"
     "    }\n"
     "    PopBackMemoryMark();",
     "a camera push beyond the world not taken"),
    # SLY1_COLOUR_TRACE=1: an object's tree given another colour (SetObjectTreeRecordValue).
    ("src/main/placed_object.cpp",
     "    PlacedObject* child = (PlacedObject*)object->children.head;\n    object->recordValue = value;\n    object->recordFlags |= 2;",
     "    PlacedObject* child = (PlacedObject*)object->children.head;\n    port::NoteObjectColour(object->id, value);\n    object->recordValue = value;\n    object->recordFlags |= 2;",
     "an object's colour noted for the trace"),
    # SLY1_COLOUR_TRACE=1: a light's colour as its shape is rebuilt (RefreshConeVolumeShape).
    ("src/main/base_object.cpp",
     "    PacketWriter w;\n    InitPacketWriter(&w);\n    NoteConeVolumeLightingChange(self);",
     "    PacketWriter w;\n    InitPacketWriter(&w);\n    port::NoteLightColour(self->id, (const float*)&self->hsv, self->mode);\n    NoteConeVolumeLightingChange(self);",
     "a light's colour noted for the trace"),
    # SLY1_PROP_TRACE=N: each read of property N by a script: the object, its class, the descriptor.
    ("src/main/object_properties.cpp",
     "        PropertyDescriptor* r = FindPropertyDescriptor(p, n);\n        ReadPropertyValue(self, p, r);",
     "        PropertyDescriptor* r = FindPropertyDescriptor(p, n);\n        ReadPropertyValue(self, p, r);\n"
     "        port::NotePropertyRead(n, p->id, p->desc->id, (const int*)r, (const int*)self);",
     "a script's read of a property noted for the trace"),
    # Every animation started, kept in a short list the fade event's line prints: which animations
    # started before a fade to a level, and from where in the game's code.
    ("src/main/placed_object.cpp",
     "    obj = NewAnimationPlayer(world);\n    if (out != 0) *out = obj;\n    obj->time = fa;",
     "    obj = NewAnimationPlayer(world);\n"
     "    port::NoteAnimStart(self->id, (long)((char*)__builtin_return_address(0) - (char*)&CreateAnimationPlayer), self->length, flags);\n"
     "    if (out != 0) *out = obj;\n    obj->time = fa;",
     "an animation's start noted for the log"),
    ("src/main/placed_object.cpp",
     "    case 25:\n        StartScreenFade(&g_screenFade, (FadeRequest*)msg->aC.i, msg->a8.h.lo);",
     "    case 25:\n        port::NoteAnimEvent(25, owner->id, self->object ? ((BaseObject*)self->object)->id : -1, self->time, owner->length, self->speed);\n        StartScreenFade(&g_screenFade, (FadeRequest*)msg->aC.i, msg->a8.h.lo);",
     "an animation's fade event noted in the log"),
    ("src/main/placed_object.cpp",
     "    case 51:\n        QueueMovie((MoviePlayer*)g_moviePlayer, (void*)msg->a8.i);",
     "    case 51:\n        port::NoteAnimEvent(51, owner->id, self->object ? ((BaseObject*)self->object)->id : -1, self->time, owner->length, self->speed);\n        QueueMovie((MoviePlayer*)g_moviePlayer, (void*)msg->a8.i);",
     "an animation's movie event noted in the log"),

    # A line in the log for each level load: the two arrival keys and the flags of the request
    # (what the level it was asked from gave), for reproducing a way into a level on the host
    # (SLY1_ARRIVAL); and the level's record, for a report of a fault (port/switch/crashlog.cpp).
    ("src/main/on_off_object.cpp",
     "    OwnedStream sub;\n    SetScreenOwner(2);\n    if (self->key != 0) {",
     "    OwnedStream sub;\n    SetScreenOwner(2);\n    port::NoteWorldLoad(self->arrivalKeyA, self->arrivalKeyB, self->flags, (u32)(self->key));\n    if (self->key != 0) {",
     "each level load noted in the log"),

    # Widescreen (docs/PS3_PARITY.md): the camera's horizontal extent over the vertical is 4/3 wider
    # for the 16:9 picture (Hor+: the vertical field of view kept). The projection, the frustum's
    # sides and what is culled by them all come from this one number.
    ("src/main/camera.cpp",
     "    f32 k = 1.29870129f;\n    f32 x = g_cameraDefaultFogNear;",
     "    f32 k = 1.29870129f * port::WideFactor();\n    f32 x = g_cameraDefaultFogNear;",
     "the camera's aspect for the port's 16:9 picture"),

    # Widescreen: the map page is laid before the camera to fill the 4:3 picture; it is made 4/3
    # wider (the second row of its matrix: the camera's axis across the screen) to fill the 16:9 one, as the PS3
    # release stretches it.
    ("src/main/warp_menu.cpp",
     "    BuildMatrixFromRotationAndTranslation((const Vec4*)((Camera*)s)->position, (const Mat3*)&((Camera*)s)->rot80, (Mat4A*)tmp);\n    ApplyPlacedObjectTransform(obj, (Camera*)s, (ScaledTransform*)tmp);",
     "    BuildMatrixFromRotationAndTranslation((const Vec4*)((Camera*)s)->position, (const Mat3*)&((Camera*)s)->rot80, (Mat4A*)tmp);\n    for (int port_k = 0; port_k < 3; port_k++) ((f32*)tmp)[1 * 4 + port_k] *= port::WideFactor();\n    ApplyPlacedObjectTransform(obj, (Camera*)s, (ScaledTransform*)tmp);",
     "the map page as wide as the 16:9 picture"),

    # Widescreen: the 2D elements (the counters, the text, the menus) are laid out for the 4:3
    # picture, which the port shows 4/3 wider. Each screen element is drawn 3/4 as wide about the
    # side of the screen it rests at (port::Wide2D, by the middle of where it rests), so that it
    # has its shape and keeps its place; a vertex at the screen's very edge stays there (a fill of
    # the whole screen). Every 2D vertex goes through these four functions.
    ("src/main/game_progress.cpp",
     "void AddGifXyzEntry(PacketWriter* self, s32 a1, s32 a2, u64 a3)\n{\n",
     "void AddGifXyzEntry(PacketWriter* self, s32 a1, s32 a2, u64 a3)\n{\n    a1 = port::WideX(a1);\n",
     "a 2D vertex's x for the 16:9 picture (port::WideX)"),
    ("src/main/game_progress.cpp",
     "void AddGifXyzfEntry(PacketWriter* self, u32 a1, u32 a2, u32 a3, u32 a4)\n{\n",
     "void AddGifXyzfEntry(PacketWriter* self, u32 a1, u32 a2, u32 a3, u32 a4)\n{\n    a1 = port::WideX(a1);\n",
     "a 2D vertex's x for the 16:9 picture (port::WideX)"),
    ("src/main/game_progress.cpp",
     "void AddGifXyzfNoDrawEntry(PacketWriter* self, u32 a1, u32 a2, u32 a3, u32 a4)\n{\n",
     "void AddGifXyzfNoDrawEntry(PacketWriter* self, u32 a1, u32 a2, u32 a3, u32 a4)\n{\n    a1 = port::WideX(a1);\n",
     "a 2D vertex's x for the 16:9 picture (port::WideX)"),
    ("src/main/game_progress.cpp",
     "void AddGifXyzfRegListEntry(PacketWriter* self, s32 a1, s32 a2, u32 a3, u32 a4)\n{\n",
     "void AddGifXyzfRegListEntry(PacketWriter* self, s32 a1, s32 a2, u32 a3, u32 a4)\n{\n    a1 = port::WideX(a1);\n",
     "a 2D vertex's x for the 16:9 picture (port::WideX)"),
    ("src/main/object_pool.cpp",
     "        if (p->mode != 0) {\n            p->vt->m09(p);\n        }",
     "        if (p->mode != 0) {\n            port::Wide2D(p->shownX + p->w * 0.5f, i, p->w);\n            p->vt->m09(p);\n            port::Wide2D(-1.0f);\n        }",
     "each screen element drawn about the side of the screen it rests at"),
    # The caption box (element 1) is as wide as the screen: its lens and the lens's frame (the
    # Binoc-u-com) are stretched with the picture, as the PS3 release has them, so that the frame
    # stays on the black it borders (port::Wide2D leaves a full-width element as it is). What it
    # draws after them, the text, the box behind it and the button's glyph, is narrowed about the
    # middle as before.
    ("src/main/text_panel.cpp",
     "    f32 scale = 1.0f;\n    if (self->state == 4) {\n        scale = 0.8f;\n    }",
     "    port::Wide2D(320.0f, 1, 280.0f);\n    f32 scale = 1.0f;\n    if (self->state == 4) {\n        scale = 0.8f;\n    }",
     "the caption box's text narrowed, its lens stretched"),
    # The Binoc-u-com's portraits (the arc elements): the box is narrowed about its side of the
    # screen by its draw (slot 9), but the character is a model placed in the world where the box
    # shows it, and the box's black and its scissor are drawn in requests queued for later, out of
    # the element's draw: all three at the box's 4:3 place, so the character stood out of the
    # narrowed box, over a black wider than it. The character's place and both requests are
    # narrowed as the box is.
    ("src/main/arc_element.cpp",
     "        f32 sx = ((1.0f - held->u) * self->x + held->u * (self->x + self->w)) * 0.003125f - 1.0f;",
     "        f32 sx = port::WideAt((1.0f - held->u) * self->x + held->u * (self->x + self->w), self->shownX + self->w * 0.5f) * 0.003125f - 1.0f;",
     "the portrait's character where the narrowed box shows it"),
    ("src/main/arc_element.cpp",
     "    PushBackMemoryMark();\n    InitGifPacket(&buf);\n    AllocateTempPacketBuffer(&buf, 0x100);",
     "    port::WideScope wideScope(self->shownX + self->w * 0.5f, self->index, self->w);\n    PushBackMemoryMark();\n    InitGifPacket(&buf);\n    AllocateTempPacketBuffer(&buf, 0x100);",
     "the portrait's black and outline narrowed as its box"),
    ("src/main/arc_element.cpp",
     "    s32 px = (s32)x0;\n    s32 pw = (s32)x1 - 1;",
     "    s32 px = (s32)port::WideAt(x0, self->shownX + self->w * 0.5f);\n    s32 pw = (s32)port::WideAt(x1, self->shownX + self->w * 0.5f) - 1;",
     "the portrait's scissor narrowed as its box"),
    ("src/main/arc_element.cpp",
     "    PushBackMemoryMark();\n    InitGifPacket(&buf);\n    AllocateTempPacketBuffer(&buf, 0x200);",
     "    port::WideScope wideScope(self->shownX + self->w * 0.5f, self->index, self->w);\n    PushBackMemoryMark();\n    InitGifPacket(&buf);\n    AllocateTempPacketBuffer(&buf, 0x200);",
     "the portrait's ticks narrowed as its box"),

    # The camera's turn by the right stick, and a setting for it (as the PS Vita release has under
    # Controls: "Camera: Normal" or "Camera: Inverted"). Measured against retail on a PS2 by the
    # player: retail turns the other way from what this build did, so Normal (the default, bit
    # 0x10000 of the save's option flags clear) turns that way and Inverted the way it was. The
    # page's items are copied to a list of the port's with item 27 before the last ("Back"); its
    # label is the port's, and it has no set of alternative labels to measure.
    ("src/main/on_off_object.cpp",
     "                f32 x = pad->secondStickX;\n                f32 lim = g_onOffCameraStepRate[0] * step;",
     "                f32 x = (g_saveData->optionFlags & 0x10000) ? pad->secondStickX : -pad->secondStickX;\n                f32 lim = g_onOffCameraStepRate[0] * step;",
     "the camera's turn: Normal or Inverted"),
    ("src/main/pause_menu.cpp",
     "char* GetMenuChoiceLabel(s32 index)\n{\n    return g_menuChoiceLabels[index];",
     "char* GetMenuChoiceLabel(s32 index)\n{\n    if (index == 27) {\n        static char port_normal[] = \"Camera: Normal\", port_inverted[] = \"Camera: Inverted\";\n        return (g_saveData->optionFlags & 0x10000) ? port_inverted : port_normal;\n    }\n    return g_menuChoiceLabels[index];",
     "the Camera setting's label"),
    ("src/main/pause_menu.cpp",
     "        ChoiceLabels* cl = &g_choiceTextSets[row->items[i]];\n        f32 w = 0.0f;",
     "        static ChoiceLabels port_none;\n        ChoiceLabels* cl = row->items[i] == 27 ? &port_none : &g_choiceTextSets[row->items[i]];\n        f32 w = 0.0f;",
     "the Camera setting has no set of labels of the game's"),
    ("src/main/pause_menu.cpp",
     "    s32 page = self->page;\n    MenuPage* row = &g_pauseMenuPages[page];\n    switch (page) {\n    case 0:\n        row->count = 0;",
     "    s32 page = self->page;\n    MenuPage* row = &g_pauseMenuPages[page];\n    if (page == 31) {\n        static s32 port_items[8];\n        if (row->items != port_items && row->count >= 1 && row->count < 7) {\n            s32 n = row->count;\n            for (s32 k = 0; k < n; k++) port_items[k] = row->items[k];\n            port_items[n] = port_items[n - 1];\n            port_items[n - 1] = 27;\n            row->items = port_items;\n            row->count = n + 1;\n        }\n    }\n    switch (page) {\n    case 0:\n        row->count = 0;",
     "the Controls page with the Camera setting before Back"),
    ("src/main/pause_menu.cpp",
     "    case 31:\n        switch (sel) {\n        case 0x18: {",
     "    case 31:\n        switch (sel) {\n        case 27: {\n            g_saveData->u19EC ^= 0x10000;\n            s32 keep = self->cursor;\n            BuildPauseMenuPage(self);\n            self->cursor = keep;\n            return;\n        }\n        case 0x18: {",
     "choosing the Camera setting turns it over"),

    # "Picture: Normal" or "Picture: Vivid" on the Options page, before Back (the user, 2026-11-24):
    # item 28, bit 0x20000 of the save's option flags, which the renderer reads as it presents a
    # frame (port::VividFactor: Vivid moves each colour away from its grey, an approximation of the
    # PS3 release's more saturated picture; Normal is the PS2's). As the Camera setting: a label
    # of the port's, no set of alternative labels, and choosing it turns it over.
    ("src/main/pause_menu.cpp",
     "char* GetMenuChoiceLabel(s32 index)\n{\n    if (index == 27) {",
     "char* GetMenuChoiceLabel(s32 index)\n{\n    if (index == 28) {\n        static char port_plain[] = \"Picture: Normal\", port_vivid[] = \"Picture: Vivid\";\n        return (g_saveData->optionFlags & 0x20000) ? port_vivid : port_plain;\n    }\n    if (index == 27) {",
     "the Picture setting's label"),
    ("src/main/pause_menu.cpp",
     "ChoiceLabels* cl = row->items[i] == 27 ? &port_none : &g_choiceTextSets[row->items[i]];",
     "ChoiceLabels* cl = (row->items[i] == 27 || row->items[i] == 28) ? &port_none : &g_choiceTextSets[row->items[i]];",
     "the Picture setting has no set of labels of the game's"),
    ("src/main/pause_menu.cpp",
     "            if (show != 0) {\n                row->items[row->count++] = item;\n            }\n        }\n        break;\n    }\n    for (s32 i = 0; i < 27; i++) {",
     "            if (show != 0) {\n                row->items[row->count++] = item;\n            }\n        }\n        if (row->count >= 1) {\n            row->items[row->count] = row->items[row->count - 1];\n            row->items[row->count - 1] = 28;\n            row->count++;\n        }\n        break;\n    }\n    for (s32 i = 0; i < 27; i++) {",
     "the Options page with the Picture setting before Back"),
    ("src/main/pause_menu.cpp",
     "        case 14:\n            SetPauseMenuLayerPage(self, 0, 0x18);\n            return;\n        case 23:\n            SetPauseMenuLayerPage(self, 0, 0x1F);\n            return;",
     "        case 28: {\n            g_saveData->u19EC ^= 0x20000;\n            s32 keep = self->cursor;\n            BuildPauseMenuPage(self);\n            self->cursor = keep;\n            return;\n        }\n        case 14:\n            SetPauseMenuLayerPage(self, 0, 0x18);\n            return;\n        case 23:\n            SetPauseMenuLayerPage(self, 0, 0x1F);\n            return;",
     "choosing the Picture setting turns it over"),

    # "Show FPS: Off" or "Show FPS: On" on the Options page, after Picture (the user, 2026-11-24):
    # item 29, bit 0x40000 of the save's option flags; the renderer draws the frames a second in a
    # corner as it presents a frame (port::ShowFps, gpu.cpp's DrawFps). As the Picture setting.
    ("src/main/pause_menu.cpp",
     "char* GetMenuChoiceLabel(s32 index)\n{\n    if (index == 28) {",
     "char* GetMenuChoiceLabel(s32 index)\n{\n    if (index == 29) {\n        static char port_fps_off[] = \"Show FPS: Off\", port_fps_on[] = \"Show FPS: On\";\n        return (g_saveData->optionFlags & 0x40000) ? port_fps_on : port_fps_off;\n    }\n    if (index == 28) {",
     "the Show FPS setting's label"),
    ("src/main/pause_menu.cpp",
     "ChoiceLabels* cl = (row->items[i] == 27 || row->items[i] == 28) ? &port_none",
     "ChoiceLabels* cl = (row->items[i] >= 27 && row->items[i] <= 29) ? &port_none",
     "the Show FPS setting has no set of labels of the game's"),
    ("src/main/pause_menu.cpp",
     "            row->items[row->count - 1] = 28;\n            row->count++;\n        }",
     "            row->items[row->count - 1] = 28;\n            row->count++;\n            row->items[row->count] = row->items[row->count - 1];\n            row->items[row->count - 1] = 29;\n            row->count++;\n        }",
     "the Options page with the Show FPS setting before Back"),
    ("src/main/pause_menu.cpp",
     "        case 28: {\n            g_saveData->u19EC ^= 0x20000;",
     "        case 29: {\n            g_saveData->u19EC ^= 0x40000;\n            s32 keep = self->cursor;\n            BuildPauseMenuPage(self);\n            self->cursor = keep;\n            return;\n        }\n        case 28: {\n            g_saveData->u19EC ^= 0x20000;",
     "choosing the Show FPS setting turns it over"),

    # "Difficulty: Normal" or "Difficulty: Easy" on the Options page, after Show FPS (the user,
    # 2026-11-25; docs/DIFFICULTY.md): item 30, bit 0x80000 of the save's option flags. As the
    # Picture setting. Normal is retail's game. Easy changes four levels and no other
    # (port::EnemyPace, port::ExtraEnemyClass, kernel.cpp): their enemies move slower, and Piranha
    # Lake has twice its piranhas.
    ("src/main/pause_menu.cpp",
     "char* GetMenuChoiceLabel(s32 index)\n{\n    if (index == 29) {",
     "char* GetMenuChoiceLabel(s32 index)\n{\n    if (index == 30) {\n        static char port_normal_game[] = \"Difficulty: Normal\", port_easy_game[] = \"Difficulty: Easy\";\n        return (g_saveData->optionFlags & 0x80000) ? port_easy_game : port_normal_game;\n    }\n    if (index == 29) {",
     "the Difficulty setting's label"),
    ("src/main/pause_menu.cpp",
     "ChoiceLabels* cl = (row->items[i] >= 27 && row->items[i] <= 29) ? &port_none",
     "ChoiceLabels* cl = (row->items[i] >= 27 && row->items[i] <= 30) ? &port_none",
     "the Difficulty setting has no set of labels of the game's"),
    ("src/main/pause_menu.cpp",
     "            row->items[row->count - 1] = 29;\n            row->count++;\n        }",
     "            row->items[row->count - 1] = 29;\n            row->count++;\n            row->items[row->count] = row->items[row->count - 1];\n            row->items[row->count - 1] = 30;\n            row->count++;\n        }",
     "the Options page with the Difficulty setting before Back"),
    ("src/main/pause_menu.cpp",
     "        case 29: {\n            g_saveData->u19EC ^= 0x40000;",
     "        case 30: {\n            g_saveData->u19EC ^= 0x80000;\n            s32 keep = self->cursor;\n            BuildPauseMenuPage(self);\n            self->cursor = keep;\n            return;\n        }\n        case 29: {\n            g_saveData->u19EC ^= 0x40000;",
     "choosing the Difficulty setting turns it over"),
    # Easy's slower enemies. An agent (class 8 and the classes made from it: the piranhas, the
    # chickens, the fire slugs) moves at the velocity its state's animation gives, taken each frame
    # in UpdateAgent and, while it follows the clip's own motion, in StepAgent: both are scaled.
    ("src/main/agent.cpp",
     "            Vu0::Q(self->headingVelocity) = Vu0::MulX(Vu0::Q(self->headingVelocity), neg);\n            Vu0::Q(push) = neg;",
     "            Vu0::Q(self->headingVelocity) = Vu0::MulX(Vu0::Q(self->headingVelocity), neg);\n            Vu0::Q(push) = neg;\n            Vu0::Q(self->headingVelocity) = Vu0::MulX(Vu0::Q(self->headingVelocity), Vu0::FromFloat(port::EnemyPace()));",
     "an agent's velocity, as the Difficulty setting makes it"),
    ("src/main/agent.cpp",
     "        (**(AgentMotionFn**)src)(src, self, 0, 0, &motion, ap->time, ap->speed);\n",
     "        (**(AgentMotionFn**)src)(src, self, 0, 0, &motion, ap->time, ap->speed);\n        Vu0::Q(motion) = Vu0::MulX(Vu0::Q(motion), Vu0::FromFloat(port::EnemyPace()));\n",
     "an agent's motion along its clip, as the Difficulty setting makes it"),
    # The crabs of Treasure in the Depths are paired objects (class 0x42): they steer at up to
    # their top speed, before they hold a chest and after.
    ("src/main/pair_controller.cpp",
     "        f32 speed = MinF(Vu0::ToFloatMoved(Vu0::Length(Vu0::Q(delta))) / g_gameClock[2], self->maxSpeed);",
     "        f32 speed = MinF(Vu0::ToFloatMoved(Vu0::Length(Vu0::Q(delta))) / g_gameClock[2], self->maxSpeed * port::EnemyPace());",
     "a crab's top speed, as the Difficulty setting makes it"),
    # Easy's second piranha for each one of Piranha Lake: a copy of each object of the class
    # port::ExtraEnemyClass names (none: 0), made as the level's load ends and before its objects
    # are started, as SpawnListedObjectCopies makes the game's own copies.
    ("src/main/world.cpp",
     "    SpawnListedObjectCopies(self);\n    RunQueuedCalls(self);\n    for (BaseObject* o = (BaseObject*)self->list48.head; o != 0; o = o->next) {",
     "    SpawnListedObjectCopies(self);\n"
     "    if (s32 port_cls = port::ExtraEnemyClass()) {\n"
     "        BaseObject* port_found[64];\n"
     "        s32 port_n = 0;\n"
     "        for (BaseObject* o = (BaseObject*)self->list48.head; o != 0 && port_n < 64; o = o->next) {\n"
     "            if (o->desc->id == port_cls) port_found[port_n++] = o;\n"
     "        }\n"
     "        for (s32 k = 0; k < port_n; k++) CreateObjectCopy(port_found[k], self, port_found[k]->parent);\n"
     "    }\n"
     "    RunQueuedCalls(self);\n    for (BaseObject* o = (BaseObject*)self->list48.head; o != 0; o = o->next) {",
     "the Difficulty setting's extra piranhas"),

    # The plucked note of a sneaking step, louder (the user, 2026-11-25; docs/AUDIO.md, "Sneaking").
    # The game starts sound 0x77 at half volume; on the Switch's speakers it was faint, as the user
    # also finds it on a PS Vita. port::SneakStepGain (kernel.cpp) is 3: the note at 1.5.
    ("src/main/sound_system.cpp",
     "        StartSoundEffect(0x77, 0, 0, 0, 0.0f, 0.0f, 0.5f, scale * 0.083333336f, 0.0f, 0, 0);",
     "        StartSoundEffect(0x77, 0, 0, 0, 0.0f, 0.0f, 0.5f * port::SneakStepGain(), scale * 0.083333336f, 0.0f, 0, 0);",
     "the sneaking step's plucked note, louder"),

    # The Options page without "Center Screen...": the PS2's picture could be moved on a television
    # that showed it off centre; the Switch's screen and its dock's picture are where they are
    # (the user, 2026-11-24). The item is left out of the page as it is built; the game's own
    # adjustment page and its code stay, unreached.
    ("src/main/pause_menu.cpp",
     "            case 8:\n                show = g_elementStack.resumeState != 1;\n                break;\n            }",
     "            case 8:\n                show = g_elementStack.resumeState != 1;\n                break;\n            case 14:\n                show = 0;\n                break;\n            }",
     "the Options page without Center Screen"),

    # The Options page without "Vibration: ..." and "Speakers: ..." either (the user, 2026-11-24).
    # What they set stays as it works here with nothing to choose: the sound is stereo (the port's
    # mixer does not act on the IOP's speaker option), and the vibration is on whatever a save
    # holds, for a console that has motors (a save with it off could not turn it on again).
    ("src/main/pause_menu.cpp",
     "            case 14:\n                show = 0;\n                break;\n            }",
     "            case 13:\n            case 14:\n            case 16:\n                show = 0;\n                break;\n            }",
     "the Options page without Vibration and Speakers"),
    ("src/main/pair_controller.cpp",
     "s32 IsPadPatternEnabled(GameProgress* ctx)\n{\n    s32 v = g_padPatternSetting;",
     "s32 IsPadPatternEnabled(GameProgress* ctx)\n{\n    return 1;   /* port: no Vibration setting to choose, on (tools/patches.py) */\n    s32 v = g_padPatternSetting;",
     "the vibration on, with no setting for it"),

    # Frame layout: g++ 2.95 put t right after arr[32] in the frame, and the cleanup walks back from
    # &t; a modern compiler places locals as it likes.
    ("src/main/pose_source.cpp",
     "        e = rp;\n        while (sc.buf != e) {",
     "        e = sc.buf + 32;\n        while (sc.buf != e) {",
     "the setter's argument array is destroyed from its end, which is &ret only in retail's frame"),

    # The one stored pointer declared together with its type (`struct Tri {...} *tris;`), which the
    # pointer rewrite does not reach: the type on its own, so the field becomes a game pointer and
    # the record keeps retail's 12 bytes (it is allocated as 0xC and read as VertexBlend03).
    ("src/main/collider.cpp",
     "    struct Tri { s32 i[3]; } *tris;",
     "    struct Tri { s32 i[3]; };\n    Tri* tris;",
     "the mesh motion record's face triples are a stored pointer, 4 bytes as in retail"),

    # Instrumentation, no change to the game: the level stream's state when the load closes it,
    # for --verify-levels's check that a load consumes the bytes docs/DISC.md gives.
    ("src/main/on_off_object.cpp",
     "            g_currentWorld->desc->m0A(g_currentWorld, &sub);\n            CloseBufferedStream(&sub);",
     "            g_currentWorld->desc->m0A(g_currentWorld, &sub);\n"
     "            port::NoteLevelStreamEnd(sub.remaining, sub.bytesToRead, sub.nextCount, sub.unpackCarry);\n"
     "            CloseBufferedStream(&sub);",
     "records where the level's load left its stream (instrumentation only)"),
    # Instrumentation, no change to the game: the level stream before its first read, under
    # SLY1_STREAM_TRACE, to compare platforms.
    ("src/main/on_off_object.cpp",
     "            g_currentWorld = 0;\n            ReadBufferedBlockHeader(ne, &sub);",
     "            g_currentWorld = 0;\n"
     "            port::TraceBytes(\"level stream\", &sub, 0x60);\n"
     "            ReadBufferedBlockHeader(ne, &sub);",
     "shows the level stream before its first read (instrumentation only)"),
    ("src/main/player.cpp",
     "#define AT(T, p, o) (*(T*)((u8*)(p) + (o)))",
     "#define AT(T, p, o) (*(typename port::FieldOf<T>::type*)((u8*)port::AsAddress(p) + (o)))",
     "the raw-offset macro is given words holding game addresses as well as pointers, and a pointer "
     "field it reads is a 32-bit game address (port::FieldOf views it as a Ptr32); a cast in a "
     "macro's body is not rewritten"),

    # The one integer-to-pointer cast the generator cannot reach, inside a macro's argument.
    ("src/main/placed_object.cpp",
     "F32(((EventObjectPair*)e->a8.i)->f8)",
     "F32((port::FromGameAddr<EventObjectPair>((u32)e->a8.i))->f8)",
     "a cast of a stored word to a pointer inside F32's argument: the word is a game address"),

    # A queued call's third argument is a word (QueuedCall's k3, RunQueuedCalls calls fn(arg, k2,
    # k3)); RunScriptEventHandler, the only queued function that takes a pointer there, is queued
    # with its parameter list's address in it. It takes the word and makes it the pointer.
    ("src/main/base_object.cpp",
     "void RunScriptEventHandler(BaseObject* self, s32 key, R11B008Node* list)\n{\n",
     "void RunScriptEventHandler(BaseObject* self, s32 key, s32 port_list)\n{\n"
     "    R11B008Node* list = port::FromGameAddr<R11B008Node>((u32)port_list);\n",
     "the queued call's third word is the list's game address"),
    ("include/functions/base_object.h",
     "void RunScriptEventHandler(BaseObject* self, s32 key, R11B008Node* list);",
     "void RunScriptEventHandler(BaseObject* self, s32 key, s32 port_list);",
     "the queued call's third word is the list's game address"),
    ("src/main/base_object.cpp",
     "(void*)(void (*)(void*, s32, void*))RunScriptEventHandler",
     "(void*)(void (*)(void*, s32, s32))RunScriptEventHandler",
     "the queued call's third word is the list's game address"),

    # A hook for the port's renderer (the user's decision, 2026-11-17: named hooks in the game's
    # mesh-drawing code, through this file). Where the game draws a model part from its light
    # cache it sends the part's header and then calls the cache's packet, which holds the part's
    # meshes with their colours as they were last lit. The port is told which packet: one VIF NOP
    # with the packet's address in its unused bits, in the word the game pads the header's tag
    # with (the tag is no longer). The renderer may then draw the part from what it kept of the
    # packet the last time, with this frame's header (port/platform/dmac.cpp, PartBegin).
    ("src/main/placed_object_model.cpp",
     "            VifPacketWriter::UnpackV4(&g_vifPacketWriter, &b, 0xB, 0);\n"
     "            VifPacketWriter::Pad(&g_vifPacketWriter);\n"
     "            VifPacketWriter::Close(&g_vifPacketWriter, tag);\n"
     "            VifPacketWriter::Call(&g_vifPacketWriter, cache->refs[half]);",
     "            VifPacketWriter::UnpackV4(&g_vifPacketWriter, &b, 0xB, 0);\n"
     "            VifPacketWriter::Word(&g_vifPacketWriter, 0x00800000 | (half << 22) | (((u32)cache->refs[half] >> 4) & 0x1FFFFF));\n"
     "            VifPacketWriter::Pad(&g_vifPacketWriter);\n"
     "            VifPacketWriter::Close(&g_vifPacketWriter, tag);\n"
     "            VifPacketWriter::Call(&g_vifPacketWriter, cache->refs[half]);",
     "the renderer told which cached packet a model part is drawn from"),

    # PS3 parity (docs/PS3_PARITY.md): no memory card questions. The port's card is always there,
    # formatted and roomy (port/platform/memory_card.cpp); where the game would ask whether to
    # create its save file (page 7), it creates it, as the answer "Yes" does.
    ("src/main/memory_card.cpp",
     "            case 3: SetPauseMenuLayerPage(&g_pauseMenuElement, 1, 7); break;",
     "            case 3: { s32 CreateMemoryCardSaveFiles(MemoryCard* self);\n"
     "                StartBackgroundJob(&g_backgroundJob, (void*)CreateMemoryCardSaveFiles, self); break; }",
     "the save file is created without asking, as the PS3 release has no memory card questions"),

    # A second hook in the part's drawing (docs/RENDERER.md, "The parts drawn the long way"): the
    # packets of a part's meshes written as a chain of their own, called from the list (a NOP
    # pair naming the part's light cache, a call tag, a next tag over the chain, and a ret tag at
    # its end). The GS sees the same words in the same order; the port may leave the call out
    # and draw the part from what it kept of it.
    ("src/main/placed_object_model.cpp",
     "slow:\n    for (i = 0; i < part->nmeshes; i++) {\n        s32 k;",
     "slow:\n"
     "    u32* port_span = 0;\n"
     "    if (cache != 0 && part->nmeshes > 0) {\n"
     "        tag = VifPacketWriter::Open(&g_vifPacketWriter);\n"
     "        VifPacketWriter::Word(&g_vifPacketWriter, 0x000051A0);\n"
     "        VifPacketWriter::Word(&g_vifPacketWriter, 0x00800000 | (((u32)cache >> 4) & 0x1FFFFF));\n"
     "        VifPacketWriter::Pad(&g_vifPacketWriter);\n"
     "        VifPacketWriter::Close(&g_vifPacketWriter, tag);\n"
     "        VifPacketWriter::Call(&g_vifPacketWriter, (void*)(g_vifPacketWriter.cur + 32));\n"
     "        port_span = (u32*)g_vifPacketWriter.cur;\n"
     "        port_span[0] = 0x20000000; port_span[1] = 0; port_span[2] = 0; port_span[3] = 0;\n"
     "        g_vifPacketWriter.cur += 16;\n"
     "    }\n"
     "    for (i = 0; i < part->nmeshes; i++) {\n        s32 k;",
     "the port's hook: a part drawn the long way is a call of its own, so that it can be drawn again from what was kept of it (docs/RENDERER.md)"),
    ("src/main/placed_object_model.cpp",
     "                QueueRangeRecordPacket((RangeRecord*)node);\n            }\n        }\n    }\n\n    if (skin != 0 && (skin->flags & 1)) {",
     "                QueueRangeRecordPacket((RangeRecord*)node);\n            }\n        }\n    }\n"
     "    if (port_span != 0) {\n"
     "        u32* port_ret = (u32*)g_vifPacketWriter.cur;\n"
     "        port_ret[0] = 0x60000000; port_ret[1] = 0; port_ret[2] = 0; port_ret[3] = 0;\n"
     "        g_vifPacketWriter.cur += 16;\n"
     "        port_span[1] = (u32)g_vifPacketWriter.cur;\n"
     "    }\n\n    if (skin != 0 && (skin->flags & 1)) {",
     "the port's hook: a part drawn the long way is a call of its own, so that it can be drawn again from what was kept of it (docs/RENDERER.md)"),

    # The card is the port's save folder and cannot be taken out: once it is known and its save
    # files are read or made (state 5), the game no longer asks for it three times a second on a
    # thread started for each asking. Until then, and in every other state, it asks as before.
    ("src/main/memory_card.cpp",
     "        if (self->nextPollTime <= g_elapsedTime[0])\n",
     "        if (self->state != 5 && self->nextPollTime <= g_elapsedTime[0])\n",
     "the port's card cannot be removed: no poll of it once its save files are in (the user's decision, 2026-11-20)"),
    # The decompilation's macro for the address a DMA tag takes masks the pointer to its low 28
    # bits, which on the PS2 makes a segment address physical. Here a pointer is the host's: the
    # tag's writer (AddDmaRefTag) makes it the game address it stores, and a game address has no
    # segment bits to take off. The pointer goes through as it is.
    ("include/packet_writer.h",
     "#define DMA_PHYSICAL_ADDRESS(p) ((void*)((uptr)(p) & 0xFFFFFFF))",
     "#define DMA_PHYSICAL_ADDRESS(p) ((void*)(p))",
     "a DMA tag's address: the pointer itself, which the tag's writer turns into the game address"),
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
