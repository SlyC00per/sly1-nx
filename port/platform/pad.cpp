// SPDX-License-Identifier: AGPL-3.0-or-later
// The pad library the game calls (sly1-decomp src/lib's pad.c; the game's side is
// src/main/pad_input.cpp): a DualShock 2 in port 0, slot 0, that takes the modes the game asks for
// (analog, then the pressure-sensitive buttons), and nothing in the other ports. Its frames come from
// the platform: the Switch's controller (player 1, or the console in handheld), or on the host a
// pad with nothing pressed.
//
// A frame is the DualShock 2's: byte 1 the mode (0x79: analog with pressure, nine halfwords),
// bytes 2 and 3 the buttons, a bit clear when pressed (byte 2: Select, L3, R3, Start, Up, Right,
// Down, Left; byte 3: L2, R2, L1, R1, Triangle, Circle, Cross, Square), bytes 4 to 7 the right and
// left sticks (0 to 255, 128 centred, down and right high), and bytes 8 to 19 the pressures (Right,
// Left, Up, Down, Triangle, Circle, Cross, Square, L1, R1, L2, R2), 0 to 255.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "common.h"
#include "port/system.h"

#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace {

// The buttons as the game's word holds them (byte 2 high, byte 3 low), set when pressed.
enum : std::uint16_t {
    kL2 = 0x0001, kR2 = 0x0002, kL1 = 0x0004, kR1 = 0x0008,
    kTriangle = 0x0010, kCircle = 0x0020, kCross = 0x0040, kSquare = 0x0080,
    kSelect = 0x0100, kL3 = 0x0200, kR3 = 0x0400, kStart = 0x0800,
    kUp = 0x1000, kRight = 0x2000, kDown = 0x4000, kLeft = 0x8000,
};

struct PadFrame {
    std::uint16_t buttons = 0;
    std::uint8_t rx = 0x80, ry = 0x80, lx = 0x80, ly = 0x80;
};

#if defined(__SWITCH__)

std::mutex g_padLock;
PadState g_pad;
HidVibrationDeviceHandle g_vibration[2][2];   // handheld, player 1: left and right
bool g_padReady;

void InitSwitchPad()
{
    if (g_padReady)
        return;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    hidInitializeVibrationDevices(g_vibration[0], 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
    hidInitializeVibrationDevices(g_vibration[1], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);
    hidInitializeTouchScreen();
    g_padReady = true;
}

std::uint8_t StickByte(s32 v)   // -32767..32767, up positive, to 0..255, 128 centred, down high
{
    s32 b = 128 + v / 256;
    return (std::uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
}

PadFrame ReadFrame()
{
    std::lock_guard<std::mutex> lk(g_padLock);
    InitSwitchPad();
    padUpdate(&g_pad);
    u64 held = padGetButtons(&g_pad);
    // By position, as the PlayStation's: the bottom face button is Cross.
    static const struct { u64 from; std::uint16_t to; } kMap[] = {
        {HidNpadButton_B, kCross}, {HidNpadButton_A, kCircle}, {HidNpadButton_Y, kSquare},
        {HidNpadButton_X, kTriangle}, {HidNpadButton_L, kL1}, {HidNpadButton_R, kR1},
        {HidNpadButton_ZL, kL2}, {HidNpadButton_ZR, kR2}, {HidNpadButton_Plus, kStart},
        {HidNpadButton_Minus, kSelect}, {HidNpadButton_StickL, kL3}, {HidNpadButton_StickR, kR3},
        {HidNpadButton_Up, kUp}, {HidNpadButton_Down, kDown}, {HidNpadButton_Left, kLeft},
        {HidNpadButton_Right, kRight},
    };
    PadFrame f;
    for (const auto& m : kMap)
        if (held & m.from)
            f.buttons |= m.to;
    {   // The screen touched anywhere is L1, as the PS Vita release has it for the Binocucom (the
        // user, 2026-11-24): held while a finger is on the screen, and for four reads at least, so
        // that the shortest tap is a press the game sees. (No touch screen when docked: none read.)
        static int held4;
        HidTouchScreenState touch = {};
        const bool touching = hidGetTouchScreenStates(&touch, 1) > 0 && touch.count > 0;
        if (touching)
            held4 = 4;
        if (touching || held4 > 0) {
            f.buttons |= kL1;
            if (!touching)
                held4--;
        }
    }
    HidAnalogStickState l = padGetStickPos(&g_pad, 0), r = padGetStickPos(&g_pad, 1);
    f.lx = StickByte(l.x);
    f.ly = StickByte(-l.y);
    f.rx = StickByte(r.x);
    f.ry = StickByte(-r.y);
    return f;
}

// The DualShock 2's motors on HD rumble: the small one (on or off) as the high band, the large one
// (0 to 255) as the low band.
void Rumble(std::uint8_t small, std::uint8_t large)
{
    std::lock_guard<std::mutex> lk(g_padLock);
    InitSwitchPad();
    HidVibrationValue v[2];
    for (auto& x : v) {
        x.amp_low = large / 255.0f;
        x.freq_low = 160.0f;
        x.amp_high = small ? 0.5f : 0.0f;
        x.freq_high = 320.0f;
    }
    int set = padIsHandheld(&g_pad) ? 0 : 1;
    hidSendVibrationValues(g_vibration[set], v, 2);
}

#else

PadFrame ReadFrame() { return PadFrame(); }   // the host's pad: nothing pressed
void Rumble(std::uint8_t, std::uint8_t) {}

#endif

bool Connected(s32 port, s32 slot) { return port == 0 && slot == 0; }

}  // namespace

// The buttons the last read gave the game, as its word holds them (the host's SLY1_GOTO watches it)
std::atomic<std::uint16_t> port::g_padHeld{0};

extern "C" {

s32 InitPadLibrary(s32) { return 1; }
s32 OpenPadPort(s32, s32, u8*) { return 1; }

// The slot's state: 6, stable, for the pad; 0, disconnected, elsewhere.
s32 GetPadSlotState(s32 port, s32 slot) { return Connected(port, slot) ? 6 : 0; }

// Its mode information: the current and extended ids (7, a DualShock in analog mode).
s32 GetPadReplyListValue(s32 port, s32 slot, s32 kind, s32)
{
    if (!Connected(port, slot))
        return 0;
    return kind == 1 || kind == 2 ? 7 : 0;
}

// Setting the main mode (analog, locked) is accepted.
s32 SendPadTwoWordRequest(s32 port, s32 slot, s32, s32) { return Connected(port, slot) ? 1 : 0; }

// Its actuators: two; the small one (on or off) and the large one (a byte), as a DualShock 2
// describes them.
s32 GetPadReplyTableByte(s32 port, s32 slot, s32 index, s32 kind)
{
    if (!Connected(port, slot))
        return 0;
    if (index == -1)
        return 2;
    static const s32 kActuators[2][4] = {{0, 1, 2, 0}, {0, 1, 1, 1}};
    return index >= 0 && index < 2 && kind >= 0 && kind < 4 ? kActuators[index][kind] : 0;
}

// The actuators' alignment is accepted.
s32 SendPadSixByteRequest(s32 port, s32 slot, u8*) { return Connected(port, slot) ? 1 : 0; }

// Pressure-sensitive buttons: available, and entered.
s32 HasFullPadMask(s32 port, s32 slot) { return Connected(port, slot) ? 1 : 0; }
s32 SendFullPadMask(s32 port, s32 slot) { return Connected(port, slot) ? 1 : 0; }

// Every request completes at once.
s32 GetPadRequestMode(s32, s32) { return 0; }

// A frame (see the top of this file); nonzero when one was read.
s32 ReadPadFrame(s32 port, s32 slot, u8* out)
{
    if (!Connected(port, slot))
        return 0;
    PadFrame f = ReadFrame();
    {   // SLY1_PAD_WALK=N: from the Nth read on, the left stick held forward and the camera turned
        // slowly to the right, whatever the pad says: the player walks a circle, the same each run,
        // for timing a moving scene with no one at the controls (docs/RENDERER.md).
        static const long from = std::getenv("SLY1_PAD_WALK") ? std::atol(std::getenv("SLY1_PAD_WALK")) : -1;
        static long reads;
        // SLY1_PAD_TURN=N: the camera turned only, the player left standing (walking blind, it dies)
        static const long turnFrom = std::getenv("SLY1_PAD_TURN") ? std::atol(std::getenv("SLY1_PAD_TURN")) : -1;
        ++reads;
        if (from >= 0 && reads >= from) {
            f.ly = 0x00;
            f.lx = 0x80;
            f.rx = 0xB0;
            f.ry = 0x80;
        }
        // SLY1_PAD_START=N: START held for the Nth read and the five after (the pause menu, unattended)
        static const long startAt = std::getenv("SLY1_PAD_START") ? std::atol(std::getenv("SLY1_PAD_START")) : -1;
        if (startAt >= 0 && reads >= startAt && reads < startAt + 6)
            f.buttons |= 0x0800;
        // SLY1_PAD_TAP=N: the cross button tapped every N reads (a level's opening talk passed, unattended)
        static const long tapEvery = std::getenv("SLY1_PAD_TAP") ? std::atol(std::getenv("SLY1_PAD_TAP")) : 0;
        if (tapEvery > 0 && reads % tapEvery < 4)
            f.buttons |= kCross;
        // SLY1_PAD_SCRIPT=N:MASK,N:MASK,...: the buttons of MASK (hexadecimal, the PS2's bits) held for
        // the Nth read and the five after (a menu walked, unattended)
        static const char* const script = std::getenv("SLY1_PAD_SCRIPT");
        for (const char* at = script; at && *at;) {
            char* end = nullptr;
            const long n = std::strtol(at, &end, 10);
            if (*end != ':')
                break;
            const unsigned long mask = std::strtoul(end + 1, &end, 16);
            if (reads >= n && reads < n + 6)
                f.buttons |= (std::uint16_t)mask;
            at = *end == ',' ? end + 1 : end;
            if (*end != ',')
                break;
        }
        // SLY1_PAD_AIM=N: from the Nth read on, the right stick held fully up (a vehicle's gun fired)
        static const long aimFrom = std::getenv("SLY1_PAD_AIM") ? std::atol(std::getenv("SLY1_PAD_AIM")) : -1;
        if (aimFrom >= 0 && reads >= aimFrom && reads % 240 < 200) {
            f.rx = 0x80;
            f.ry = 0x00;
        }
        if (turnFrom >= 0 && reads >= turnFrom) {
            f.rx = 0xC0;
            f.ry = 0x80;
        }
    }
    port::g_padHeld = f.buttons;
    std::memset(out, 0, 32);
    out[1] = 0x79;
    std::uint16_t released = (std::uint16_t)~f.buttons;
    out[2] = (u8)(released >> 8);
    out[3] = (u8)released;
    out[4] = f.rx;
    out[5] = f.ry;
    out[6] = f.lx;
    out[7] = f.ly;
    // The pressures: a digital button pressed is a full press.
    static const std::uint16_t kPressureOrder[12] = {kRight, kLeft, kUp, kDown, kTriangle, kCircle,
                                                     kCross, kSquare, kL1, kR1, kL2, kR2};
    for (int i = 0; i < 12; i++)
        out[8 + i] = (f.buttons & kPressureOrder[i]) ? 0xFF : 0;
    return 1;
}

// The motors: byte 0 the small one (on when nonzero), byte 1 the large one.
s32 SetPadActuatorBytes(s32 port, s32 slot, u8* bytes)
{
    if (!Connected(port, slot) || !bytes)
        return 0;
    Rumble(bytes[0], bytes[1]);
    return 1;
}

}  // extern "C"
