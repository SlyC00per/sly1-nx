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

}  // namespace gpu
}  // namespace port

#endif
