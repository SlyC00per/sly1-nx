// SPDX-License-Identifier: AGPL-3.0-or-later
// What Apple's systems give the port, the Mac's and the iPhone's alike (port/apple/; docs/IOS.md):
// the sound output and a game controller.
#ifndef PORT_APPLE_H
#define PORT_APPLE_H

#include <cstdint>

namespace port {
namespace apple {

// The sound output started (an audio queue, 48 kHz stereo) and given to the mixer
// (port::audio::SetOutput); false when the system gives none.
bool StartAudio();
void AudioReport();
// The output paused or going again (the app left the screen); the mixer waits meanwhile.
void PauseAudio(bool paused);   // for the run's report: how often the output ran out of sound

// Controllers watched for from here on (once, on the thread that runs the program's events).
void WatchControllers();
// The controller's state as port::g_hostPad holds a pad (buttons in bits 0 to 15, then the right
// stick's x and y and the left's); false when no controller is connected.
bool ControllerPad(std::uint64_t* pad);

}  // namespace apple
}  // namespace port

#endif
