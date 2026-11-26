// SPDX-License-Identifier: AGPL-3.0-or-later
// The game's globals, at their retail addresses in the mirrored main memory (port/ptr32.h).
//
// tools/gen.py turns every declaration of a game global (D_XXXXXXXX, whatever readable name it is
// spelled with) into a macro of the same name over RetailRef, with that declaration's own type,
// and every definition into its initial value (port_init_D_XXXXXXXX), which start-up copies to
// the global's place. Start-up first fills main memory from the player's executable
// (LoadRetailImage), so every byte the decompilation does not write in C has its retail value.
// An address used only as a number (a VU program's, a checksum range's end) is simply that
// address.
#ifndef PORT_RETAIL_DATA_H
#define PORT_RETAIL_DATA_H

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#include "port/ptr32.h"

namespace port {

template <class T> inline T& RetailRef(std::uint32_t addr) { return *reinterpret_cast<T*>(Native(addr)); }

// Copies a global's initial value to its retail address (written by tools/gen.py, for each
// definition).
inline void RetailInit(std::uint32_t addr, const void* value, std::size_t size)
{
    std::memcpy(Native(addr), value, size);
}

// Fills main memory with the executable's loadable segments (port/iso.h); false without one.
bool LoadRetailImage();

// The audit of that room (a build with PORT_ROOM_AUDIT and ASan's recover mode): the bytes added
// past the local's own size are poisoned while it lives, so any code that reaches them, which
// retail's frame let reach a neighbour, is reported with its function and the variable.
#ifdef PORT_ROOM_AUDIT
}  // namespace port
#include <sanitizer/asan_interface.h>
namespace port {
struct RoomAudit {
    char* at;
    std::size_t size;
    RoomAudit(void* v, std::size_t own, std::size_t whole) : at((char*)v + own), size(whole - own)
    {
        // ASan poisons to the end of an 8-byte granule: up to the last granule end in the room
        std::uintptr_t end = ((std::uintptr_t)at + size) & ~std::uintptr_t(7);
        size = end > (std::uintptr_t)at ? end - (std::uintptr_t)at : 0;
        if (size)
            __asan_poison_memory_region(at, size);
    }
    ~RoomAudit()
    {
        if (size)
            __asan_unpoison_memory_region(at, size);
    }
};
#define PORT_ROOM_AUDIT_NAME2(l) port_room_audit_##l
#define PORT_ROOM_AUDIT_NAME(l) PORT_ROOM_AUDIT_NAME2(l)
#define PORT_ROOM_AUDIT(v, own) port::RoomAudit PORT_ROOM_AUDIT_NAME(__LINE__)((void*)&(v), own, sizeof(v))
#else
#define PORT_ROOM_AUDIT(v, own) ((void)0)
#endif

// The PS2 FPU's division (div.s): by zero, the largest float with the quotient's sign (0 / 0
// too), where IEEE gives infinity or NaN; tools/gen.py writes it for every float division of the
// game's code. An overflowing quotient already saturates (game threads round toward zero).
inline float FDiv(float a, float b)
{
    if (b == 0.0f)
        return std::signbit(a) != std::signbit(b) ? -3.4028235e38f : 3.4028235e38f;
    return a / b;
}
inline float& FDivAssign(float& a, float b) { return a = FDiv(a, b); }

// The level stream's state when a load closes it (instrumentation for --verify-levels): its
// unread decompressed bytes, compressed bytes not yet read, read but not yet unpacked, carried.
// The state of each level load's stream when it closes (milestone 1's byte check,
// tools/level_bytes.py): the last 16 closes, by count, with the texture block's state.
struct LevelStreamEnd {
    int remaining, bytesToRead, nextCount, carry, count, blockSize, blockLoad;
};
extern LevelStreamEnd g_levelStreamEnds[16];
extern std::atomic<int> g_levelStreamCloses;
void NoteLevelStreamEnd(int remaining, int bytesToRead, int nextCount, int carry);
// Under SLY1_STREAM_TRACE: the bytes of a game structure, as words, to compare platforms.
void TraceBytes(const char* what, const void* p, int n);

// A base address as the game's raw-offset macros take it (player.cpp's AT): a pointer as it is, a
// stored pointer, or a word holding a game address.
template <class T> inline void* AsAddress(T* p) { return (void*)p; }
template <class T> inline void* AsAddress(const Ptr32<T>& p) { return (void*)p.get(); }
template <class T, class = std::enable_if_t<std::is_integral<T>::value>>
inline void* AsAddress(T word) { return Native((std::uint32_t)word); }

// Preemption points (port/platform/kernel.cpp). tools/gen.py puts Preempt() in every loop test of
// the game's code: when an interrupt has readied a thread of higher priority, the running thread
// gives way there, as the PS2 preempts it. Spin() is an empty wait loop's body: the same, and a
// moment for the host's other threads.
extern std::atomic<bool> g_preemptPending;
void PreemptSlow();
// The picture's width over the PS2's 4:3 one: 4/3 for the 16:9 screen (the camera's horizontal
// extent is that much wider, the vertical kept; tools/patches.py's camera patch), 1 with
// SLY1_WIDE=0.
float WideFactor();
namespace movie {
// The video stream's bytes on their way to the IPU (port/video/movie.cpp; tools/patches.py).
void VideoBytes(const void* bytes, int size);
// How much larger than the 4:3 screen a movie's picture is drawn: 4/3 in the 16:9 picture (its own
// black bands go off the screen), 1 with SLY1_WIDE=0.
float Zoom();
// Whether a movie's picture is handed to the renderer as it is, and the number of the last decoded.
bool Direct();
unsigned PictureNumber();
// The decoding thread, started with the program (port/host/main.cpp); it does not return.
void WorkerMain();
// Its helper, for half of each picture's work; started the same way.
void HelperMain();
}  // namespace movie
// A level load's request, written to the log (tools/patches.py).
void NoteWorldLoad(int arrivalA, int arrivalB, int flags, unsigned level);
// The record (a game address in the level table) of the level last loaded, or 0.
unsigned LastLevelLoaded();
// A fade that is to load a level: where StartScreenFade was called from (bytes from its own start;
// -1 for the completion fade, whose two values are then its kind and the completion mask).
void NoteFade(long from, int kind, int value, int count, int mode);
// An animation's event 25 (a fade to a level) or 51 (a movie queued): the animation, its object, its time.
void NoteAnimEvent(int event, int animation, int object, float time, float length, float speed);
// An animation started (its id, where from as an offset from CreateAnimationPlayer, its length):
// kept, and printed with the next fade event.
void NoteAnimStart(int animation, long from, float length, int flags);
// SLY1_SCRIPT_TRACE=N: a call the script machine made (the callee's tag and function, the
// arguments' and the result's tag and word); the last ones are printed when animation N starts.
void NoteScriptCall(int tag, void* fn, int count, const int* args, const int* result);
// SLY1_PROP_TRACE=N: a script read property N of an object (its id, its class's, the descriptor's
// words, the value read).
// SLY1_TICK_TRACE=N: object N's script timer at a tick event (its value, the frame's time), or with
// 0 every object's running timer.
void NoteTick(int object, float timer, float step);
// SLY1_EVENT_TRACE=N: a script event raised on object N (its key, the caller as an offset from
// TriggerScriptEvent).
void NoteScriptEvent(int object, int key, long from);
// How much louder than retail the plucked note of a sneaking step is started (tools/patches.py): 3,
// or SLY1_PLUCK_GAIN in hundredths (100 is retail's).
float SneakStepGain();
// The Difficulty setting (docs/DIFFICULTY.md; kernel.cpp; tools/patches.py). EnemyPace: how fast the
// level's enemies move against retail, 1 on Normal and in every level Easy leaves alone.
// ExtraEnemyClass: the class whose objects a level's load makes a second copy of, or 0.
float EnemyPace();
int ExtraEnemyClass();
// SLY1_FREEZE_TRACE=N: object N frozen (flag non-zero) or let go.
void NoteFreeze(int object, int flag);
// SLY1_CAMERA_TRACE=A,B: the camera's position and area mask, each world frame from shown frame A to B.
void NoteCamera(const float* position, int areaMask);
// Whether the place the camera's push out of geometry gives is one to take: finite, and within
// 10,000 units of where the camera was going (tools/patches.py says why it can be neither).
bool CameraPushInWorld(const float* pushed, const float* dest);
// SLY1_COLOUR_TRACE=1: an object given a colour other than the one it was last given.
void NoteObjectColour(int object, unsigned value);
// SLY1_COLOUR_TRACE=1: a light (a cone volume) rebuilt with another hue or value than it had.
void NoteLightColour(int object, const float* hsv, int mode);
void NotePropertyRead(int property, int object, int cls, const int* descriptor, const int* value);
// The 2D elements in the 16:9 picture (tools/patches.py). Wide2D(middle): what is drawn from now
// is one element whose middle rests at that x of the 640 (it is drawn 3/4 as wide about the
// left edge, the middle or the right edge of the screen, by the third the middle is in);
// Wide2D(-1): no element. WideX: a vertex's x (12.4, with the game's offset of 1,728) as the
// element's place makes it; one at the screen's very edge stays.
void Wide2D(float middle, int element = -1, float width = 0.0f);
int WideX(int x);
// An x of the 640 as an element whose middle rests at that x is narrowed (one at the very edge
// stays), for what the element places outside its draw (the portraits' characters, scissors).
float WideAt(float x, float middle);
// Wide2D for a scope: for what an element draws outside its own draw (a request queued for later).
// The letterbox bars drawn (kernel.cpp; tools/patches.py, DrawLetterboxElement)
void LetterboxNoted(float blend);
// SLY1_SND_BEAT (kernel.cpp): the third world's boss's beat clock, restarted (kind 0) or a beat on (1)
void BossBeat(int kind, float gameTime, float beatLength, int beat);
// SLY1_VAN_LOG (kernel.cpp)
void VanLog(float stick, float top, float target, float vx, float vy, float vz, float pace, float zone, int kind, int boosts, int checksum);

struct WideScope {
    WideScope(float middle, int element, float width) { Wide2D(middle, element, width); }
    ~WideScope() { Wide2D(-1.0f); }
    WideScope(const WideScope&) = delete;
    WideScope& operator=(const WideScope&) = delete;
};
// fn called once on a game thread, at its next point of letting others in (the host's diagnosis).
void CallOnGameThread(void (*fn)());
#ifdef PORT_SENTINEL
// A software watchpoint (cmake -DSLY1_SENTINEL=ON): a zero word the kernel places right after its
// thread stacks, checked at every preemption point; a change is reported with the caller's frames.
extern volatile std::uint32_t g_stackEndSentinel[4];
[[noreturn]] void SentinelTripped();
#endif
inline void Preempt()
{
#ifdef PORT_SENTINEL
    if (g_stackEndSentinel[0] != 0)
        SentinelTripped();
#endif
    if (g_preemptPending.load(std::memory_order_relaxed))
        PreemptSlow();
}
void Spin();

// A local the game views through a larger type (its address cast to a Mat3* and written whole):
// the local with N bytes of room, its own type and fields as they were (tools/gen_pointers.py,
// roomy_cast). Retail's frame gave it that room in its neighbours and padding.
template <class T, std::size_t N> struct alignas(16) Room : T {
    char port_room[N - sizeof(T)];
    // Initialised as the local was: from a value of its type, from a braced list of its fields,
    // or not at all (zeroed here).
    template <class... A> Room(A... a) : T{a...}, port_room{} {}
    Room& operator=(const T& v)
    {
        static_cast<T&>(*this) = v;
        return *this;
    }
};

}  // namespace port

#endif
