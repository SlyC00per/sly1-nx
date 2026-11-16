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
    ("src/main/pose_source.cpp",
     "        e = rp;\n        while (sc.buf != e) {",
     "        e = sc.buf + 32;\n        while (sc.buf != e) {",
     "the setter's argument array is destroyed from its end, which is &ret only in retail's frame"),
    ("src/main/script_machine.cpp",
     "    q = &D;",
     "    q = C + 2;",
     "the pair is destroyed from its end, which is &D only in retail's frame"),
    ("src/main/script_machine.cpp",
     "    ScriptValue* p2 = &c;",
     "    ScriptValue* p2 = b + 2;",
     "the pair is destroyed from its end, which is &c only in retail's frame"),

    # A spline path's slot 2 (SampleSplinePathAtLength) is called with the position and tangent
    # outputs (path_follower.cpp); the first is declared s32 and handed on as one.
    ("src/main/path_curve.cpp",
     "typedef void (*CurveSampleAtFn)(void* self, f32 t, s32 a1, void* a2);",
     "typedef void (*CurveSampleAtFn)(void* self, f32 t, void* a1, void* a2);",
     "the spline's sample position is a pointer the slot's callers pass, which an s32 cannot hold here"),
    ("src/main/path_curve.cpp",
     "void SampleSplinePathAtLength(SplinePathCurve* self, s32 a1, void* a2)",
     "void SampleSplinePathAtLength(SplinePathCurve* self, void* a1, void* a2)",
     "the spline's sample position is a pointer the slot's callers pass, which an s32 cannot hold here"),
    ("include/functions/path_curve.h",
     "void SampleSplinePathAtLength(SplinePathCurve* self, s32 a1, void* a2);",
     "void SampleSplinePathAtLength(SplinePathCurve* self, void* a1, void* a2);",
     "the spline's sample position is a pointer the slot's callers pass, which an s32 cannot hold here"),

    # The one stored pointer declared together with its type (`struct Tri {...} *tris;`), which the
    # pointer rewrite does not reach: the type on its own, so the field becomes a game pointer and
    # the record keeps retail's 12 bytes (it is allocated as 0xC and read as VertexBlend03).
    ("src/main/collider.cpp",
     "    struct Tri { s32 i[3]; } *tris;",
     "    struct Tri { s32 i[3]; };\n    Tri* tris;",
     "the mesh motion record's face triples are a stored pointer, 4 bytes as in retail"),

    # Slot 27 of the switched colliders shows the typed-text caption box for an object it is
    # handed (a pointer, through the method table); the value was declared u32 and stored as a word.
    ("src/main/switched_collider.cpp",
     "void ShowTypedTextCaptionBox(SwitchedCollider* self, u32 value)\n{\n    CaptionBox* block = &g_typedTextCaptionBox;\n    ((T217D_Word*)&block->child)->v = value;",
     "void ShowTypedTextCaptionBox(SwitchedCollider* self, void* value)\n{\n    CaptionBox* block = &g_typedTextCaptionBox;\n    block->child = value;",
     "the caption's object comes through a method slot as a pointer, which a u32 cannot hold here"),
    ("include/functions/switched_collider.h",
     "void ShowTypedTextCaptionBox(SwitchedCollider* self, u32 value);",
     "void ShowTypedTextCaptionBox(SwitchedCollider* self, void* value);",
     "the caption's object comes through a method slot as a pointer, which a u32 cannot hold here"),

    # The spline path's nearest-point functions hand their point and output pointers on through
    # the curve's method slots (slot 11, slot 1) as s32 words.
    ('src/main/path_curve.cpp',
     'FindSplinePathNearestTime(self, (s32)pos, (s32)anchor, a3, a4, a5, a6, self->times[best]);',
     'FindSplinePathNearestTime(self, pos, anchor, a3, a4, a5, a6, self->times[best]);',
     "a spline path's point and outputs are pointers its callers pass through the method slots"),
    ('include/functions/path_curve.h',
     'void FindSplinePathNearestPoint(SplinePathCurve* self, Vec4* pos, void* anchor, s32 a3, s32 a4, f32* a5, f32* a6);',
     'void FindSplinePathNearestPoint(SplinePathCurve* self, Vec4* pos, void* anchor, void* a3, void* a4, f32* a5, f32* a6);',
     "a spline path's point and outputs are pointers its callers pass through the method slots"),
    ('include/functions/path_curve.h',
     'void FindSplinePathNearestTime(SplinePathCurve* self, s32 a1, s32 a2, s32 a3, s32 a4, f32* a5, f32* a6, f32 f12);',
     'void FindSplinePathNearestTime(SplinePathCurve* self, void* a1, void* a2, void* a3, void* a4, f32* a5, f32* a6, f32 f12);',
     "a spline path's point and outputs are pointers its callers pass through the method slots"),
    ('include/functions/path_curve.h',
     'void ProjectOntoSplinePathNearLength(SplinePathCurve* self, s32 p1, s32 p2, s32 p3, s32 p4, s32 p5, s32 p6);',
     'void ProjectOntoSplinePathNearLength(SplinePathCurve* self, void* p1, void* p2, void* p3, void* p4, void* p5, void* p6);',
     "a spline path's point and outputs are pointers its callers pass through the method slots"),
    ('src/main/path_curve.cpp',
     'typedef void (*SplineSlot11Fn)(void* self, s32 p1, f32 x, s32 p2, s32 p3, s32 p4, s32 p5, s32 p6);',
     'typedef void (*SplineSlot11Fn)(void* self, void* p1, f32 x, void* p2, void* p3, void* p4, void* p5, void* p6);',
     "a spline path's point and outputs are pointers its callers pass through the method slots"),
    ('src/main/path_curve.cpp',
     'void FindSplinePathNearestPoint(SplinePathCurve* self, Vec4* pos, void* anchor, s32 a3, s32 a4, f32* a5, f32* a6)\n',
     'void FindSplinePathNearestPoint(SplinePathCurve* self, Vec4* pos, void* anchor, void* a3, void* a4, f32* a5, f32* a6)\n',
     "a spline path's point and outputs are pointers its callers pass through the method slots"),
    ('src/main/path_curve.cpp',
     'void FindSplinePathNearestTime(SplinePathCurve* self, s32 a1, s32 a2, s32 a3, s32 a4, f32* a5, f32* a6, f32 f12)\n',
     'void FindSplinePathNearestTime(SplinePathCurve* self, void* a1, void* a2, void* a3, void* a4, f32* a5, f32* a6, f32 f12)\n',
     "a spline path's point and outputs are pointers its callers pass through the method slots"),
    ('src/main/path_curve.cpp',
     '((void (*)(PathCurve*, s32, s32, f32))fn)(self, a3, a4, out.time);',
     '((void (*)(PathCurve*, void*, void*, f32))fn)(self, a3, a4, out.time);',
     "a spline path's point and outputs are pointers its callers pass through the method slots"),
    ('src/main/path_curve.cpp',
     'void ProjectOntoSplinePathNearLength(SplinePathCurve* self, s32 p1, s32 p2, s32 p3, s32 p4, s32 p5, s32 p6)\n',
     'void ProjectOntoSplinePathNearLength(SplinePathCurve* self, void* p1, void* p2, void* p3, void* p4, void* p5, void* p6)\n',
     "a spline path's point and outputs are pointers its callers pass through the method slots"),
    ('src/main/path_curve.cpp',
     '    s32 q1 = p1;',
     '    void* q1 = p1;',
     "a spline path's point and outputs are pointers its callers pass through the method slots"),
    ('src/main/path_curve.cpp',
     '    s32 q2 = p2;',
     '    void* q2 = p2;',
     "a spline path's point and outputs are pointers its callers pass through the method slots"),
    ('src/main/path_curve.cpp',
     '    s32 q3 = p3;',
     '    void* q3 = p3;',
     "a spline path's point and outputs are pointers its callers pass through the method slots"),
    ('src/main/path_curve.cpp',
     '    s32 q4 = p4;',
     '    void* q4 = p4;',
     "a spline path's point and outputs are pointers its callers pass through the method slots"),
    ('src/main/path_curve.cpp',
     '    s32 q5 = p5;',
     '    void* q5 = p5;',
     "a spline path's point and outputs are pointers its callers pass through the method slots"),
    ('src/main/path_curve.cpp',
     '    s32 q6 = p6;',
     '    void* q6 = p6;',
     "a spline path's point and outputs are pointers its callers pass through the method slots"),

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

    # The property putter calls each setter with the value's word (fn(p, val->i)); for a vector, a
    # matrix or an object (types 2, 4, 9, 10, 11 and the 0x2000 kinds, object classes) the word is
    # the payload's game address and the setter takes the pointer. The 0x1000 kinds are
    # enumerations (4096 the anchor presets, 4110 modes): an integer, as it was.
    ("src/main/object_properties.cpp",
     '        case 2:\n            fn(p, val->i);\n            break;\n        case 4:\n            fn(p, val->i);\n            break;\n        case 9:\n            fn(p, val->i);\n            break;\n        case 10:\n            fn(p, val->i);\n            break;\n        case 11:\n            fn(p, val->i);\n            break;\n        case 13:\n            break;\n        default:\n            if (d->type & 0x1000) {\n                fn(p, val->i);\n            } else if (d->type & 0x2000) {\n                fn(p, val->i);\n            }\n            break;',
     '        case 2:\n            ((void (*)(void*, void*))fn)(p, port::FromGameAddr<void>((u32)val->i));\n            break;\n        case 4:\n            ((void (*)(void*, void*))fn)(p, port::FromGameAddr<void>((u32)val->i));\n            break;\n        case 9:\n            ((void (*)(void*, void*))fn)(p, port::FromGameAddr<void>((u32)val->i));\n            break;\n        case 10:\n            ((void (*)(void*, void*))fn)(p, port::FromGameAddr<void>((u32)val->i));\n            break;\n        case 11:\n            ((void (*)(void*, void*))fn)(p, port::FromGameAddr<void>((u32)val->i));\n            break;\n        case 13:\n            break;\n        default:\n            if (d->type & 0x1000) {\n                fn(p, val->i);\n            } else if (d->type & 0x2000) {\n                ((void (*)(void*, void*))fn)(p, port::FromGameAddr<void>((u32)val->i));\n            }\n            break;',
     "a setter of a vector, matrix or object property takes the payload's pointer, not its word"),

    # A camera controller's activation (slot 0 of its method table, called with a pointer: the
    # table type CameraControllerVt) keeps that pointer in the controller's word at 0x8, which
    # UpdateValueCameraController reads as its object.
    ("src/main/camera_controller.cpp",
     "void ActivateValueCameraController(ValueCameraController* obj, s32 value) { obj->value = value; }",
     "void ActivateValueCameraController(ValueCameraController* obj, void* value) { obj->ptr = value; }",
     "the controller is activated with its object's pointer, which a word cannot hold here"),
    ("include/functions/camera_controller.h",
     "void ActivateValueCameraController(ValueCameraController* obj, s32 value);",
     "void ActivateValueCameraController(ValueCameraController* obj, void* value);",
     "the controller is activated with its object's pointer, which a word cannot hold here"),

    # Slot 23 of a world-listed object (CopyWorldListedObject) takes the object it copies; the
    # effect spawner calls it with the owner it keeps as a word.
    ("src/main/effect_spawner.cpp",
     "                typedef void (*F)(WorldListedObject*, s32);\n                ((F)set->desc->m17)(set, h->ownerArgument);",
     "                typedef void (*F)(WorldListedObject*, void*);\n"
     "                ((F)set->desc->m17)(set, port::FromGameAddr<void>((u32)h->ownerArgument));",
     "the copied object's pointer, from the word that keeps its game address"),

    # ConvertDoubleToFloat takes a double; two variadic functions declare it on the double's bits
    # (u64), the same 64-bit register on the PS2, a general register here where the function reads
    # a floating-point one. They convert through the bits.
    ('src/main/curve_segment.cpp',
     'extern "C" f32 ConvertDoubleToFloat(u64 bits);',
     'extern "C" f32 ConvertDoubleToFloat(double a);\nstatic inline f32 port_DoubleBitsToFloat(u64 bits)\n{\n    double d;\n    __builtin_memcpy(&d, &bits, 8);\n    return ConvertDoubleToFloat(d);\n}',
     "a double's bits, as a word slot holds them, made the double the conversion takes in its FP register"),
    ('src/main/curve_segment.cpp',
     'ConvertDoubleToFloat(*(u64*)((ap += 8) - 8))',
     'port_DoubleBitsToFloat(*(u64*)((ap += 8) - 8))',
     "a double's bits, as a word slot holds them, made the double the conversion takes in its FP register"),
    ('src/main/player_request.cpp',
     'extern "C" f32 ConvertDoubleToFloat(u64 bits);',
     'extern "C" f32 ConvertDoubleToFloat(double a);\nstatic inline f32 port_DoubleBitsToFloat(u64 bits)\n{\n    double d;\n    __builtin_memcpy(&d, &bits, 8);\n    return ConvertDoubleToFloat(d);\n}',
     "a double's bits, as a word slot holds them, made the double the conversion takes in its FP register"),
    ('src/main/player_request.cpp',
     'rows[i] = ConvertDoubleToFloat(ap[-1]);',
     'rows[i] = port_DoubleBitsToFloat(ap[-1]);',
     "a double's bits, as a word slot holds them, made the double the conversion takes in its FP register"),
    ('src/main/player_request.cpp',
     'cols[j] = ConvertDoubleToFloat(ap[-1]);',
     'cols[j] = port_DoubleBitsToFloat(ap[-1]);',
     "a double's bits, as a word slot holds them, made the double the conversion takes in its FP register"),

    # Four hand-written functions declared without parameters, only for their addresses in the
    # method tables; the port defines them in C++ (port/game_asm), so they need their parameters.
    ("src/main/class_descriptors.cpp", "void SetSwitchedColliderMode();",
     "void SetSwitchedColliderMode(void* self, s32 mode);",
     "a hand-written function's parameters, for the port's definition"),
    ("src/main/class_descriptors.cpp", "void ComputeObjectGathererContactValues();",
     "s32 ComputeObjectGathererContactValues(void* self, void* contact);",
     "a hand-written function's parameters, for the port's definition"),
    ("src/main/class_descriptors.cpp", "void ResolveObjectGathererContact();",
     "void ResolveObjectGathererContact(void* self, void* contact, s32 side);",
     "a hand-written function's parameters, for the port's definition"),
    ("src/main/class_descriptors.cpp", "void ComputePathFollowerContactValue();",
     "void ComputePathFollowerContactValue(void* self, void* contacts, s32 side);",
     "a hand-written function's parameters, for the port's definition"),

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
