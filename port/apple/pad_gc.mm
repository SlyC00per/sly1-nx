// SPDX-License-Identifier: AGPL-3.0-or-later
// A game controller through Apple's GameController framework (the Mac's and the iPhone's alike):
// the first one with the full set of controls (two sticks, four face buttons, shoulders and
// triggers), read as the PS2's pad by the buttons' places: the bottom face button is Cross.
#include "port/apple.h"

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include <cmath>
#include <cstdio>

namespace port {
namespace apple {

void WatchControllers()
{
    static bool watching;
    if (watching)
        return;
    watching = true;
    NSNotificationCenter* centre = [NSNotificationCenter defaultCenter];
    [centre addObserverForName:GCControllerDidConnectNotification object:nil queue:nil usingBlock:^(NSNotification* note) {
        GCController* c = note.object;
        std::fprintf(stderr, "pad: a controller connected: %s%s\n", c.vendorName ? c.vendorName.UTF8String : "?",
                     c.extendedGamepad ? "" : " (without the full set of controls: not used)");
    }];
    [centre addObserverForName:GCControllerDidDisconnectNotification object:nil queue:nil usingBlock:^(NSNotification*) {
        std::fprintf(stderr, "pad: a controller disconnected\n");
    }];
    // (the Mac gives a program its controllers only while it is the active one, unless asked)
    GCController.shouldMonitorBackgroundEvents = NO;
}

bool ControllerPad(std::uint64_t* pad)
{
    GCExtendedGamepad* g = nil;
    for (GCController* c in GCController.controllers)
        if (c.extendedGamepad) {
            g = c.extendedGamepad;
            break;
        }
    if (!g)
        return false;
    std::uint16_t buttons = 0;
    auto held = [&](GCControllerButtonInput* b, std::uint16_t bit) {
        if (b && b.pressed)
            buttons |= bit;
    };
    held(g.buttonA, 0x0040);          // bottom: Cross
    held(g.buttonX, 0x0080);          // left: Square
    held(g.buttonB, 0x0020);          // right: Circle
    held(g.buttonY, 0x0010);          // top: Triangle
    held(g.leftShoulder, 0x0004);     // L1
    held(g.rightShoulder, 0x0008);    // R1
    held(g.leftTrigger, 0x0001);      // L2
    held(g.rightTrigger, 0x0002);     // R2
    held(g.buttonMenu, 0x0800);       // Start
    held(g.buttonOptions, 0x0100);    // Select
    held(g.leftThumbstickButton, 0x0200);
    held(g.rightThumbstickButton, 0x0400);
    held(g.dpad.up, 0x1000);
    held(g.dpad.right, 0x2000);
    held(g.dpad.down, 0x4000);
    held(g.dpad.left, 0x8000);
    // a stick's axis, -1 to 1 (up and right positive), as the pad's byte (down and right high)
    auto axis = [](float v, bool flip) {
        const float a = flip ? -v : v;
        const long n = std::lround(128.0f + a * 127.0f);
        return (std::uint64_t)(n < 0 ? 0 : n > 255 ? 255 : n);
    };
    const std::uint64_t rx = axis(g.rightThumbstick.xAxis.value, false), ry = axis(g.rightThumbstick.yAxis.value, true),
                        lx = axis(g.leftThumbstick.xAxis.value, false), ly = axis(g.leftThumbstick.yAxis.value, true);
    *pad = buttons | rx << 16 | ry << 24 | lx << 32 | ly << 40;
    return true;
}

}  // namespace apple
}  // namespace port
