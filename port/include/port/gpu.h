// SPDX-License-Identifier: AGPL-3.0-or-later
// The Switch's GPU renderer (port/switch/gpu.cpp): the GS's primitives drawn with deko3d.
#ifndef PORT_GPU_H
#define PORT_GPU_H

namespace port {
namespace gpu {

// Creates the device, the display and the renderer and gives it to the GS; false without one.
bool Start();

// What the renderer did, for the run's report.
void Report();
// Everything of the GPU's given back, with the GS thread parked (port::ParkGsThread): before the
// program hands this process to another (port/host/main.cpp's HandOver).
void Shutdown();
bool Started();   // the GPU is held by this program

#if defined(SLY1_METAL)
// The Mac's window (port/apple/gpu_metal.mm; SLY1_METAL=window): whether one is asked for, and
// its event loop, which the program's first thread runs until the window is closed.
bool WindowWanted();
void RunWindow();
// The screen's layer (a CAMetalLayer, the iPhone app's view's: port/ios/app.mm), which each frame
// shown is put on; the frame buffers' size, once the renderer is made (0 before).
void SetScreenLayer(void* layer);
void FrameSize(unsigned* width, unsigned* height);
// The program left the screen, or is back (the iPhone's: nothing may be drawn while it is away):
// while suspended the frame's end waits, and the game with it.
void Suspend(bool suspended);
#endif

}  // namespace gpu
}  // namespace port

#endif
