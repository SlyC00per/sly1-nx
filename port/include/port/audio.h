// SPDX-License-Identifier: AGPL-3.0-or-later
// The sound engine behind the commands the game sends to the IOP's sound driver
// (port/audio/audio.cpp; docs/DESIGN.md, section 8). The game's side is
// sly1-decomp's src/main/iop_commands.cpp, whose comments say what each command asks.
#ifndef PORT_AUDIO_H
#define PORT_AUDIO_H

#include <cstdint>

namespace port {
namespace audio {

// One command of the sound service (0x123456): its number, its data, and the word the driver
// answers with.
std::uint32_t Command(std::int32_t cmd, const std::uint8_t* data, std::int32_t size);

// A sound bank loaded from its first sector (the second service, 0x123457): its handle.
std::uint32_t LoadBank(std::uint32_t sector);

// A movie's sound copied into the ring the driver plays it from (a transfer to the IOP's memory at
// the address command 0x3B answered with).
void MovieRingWrite(std::uint32_t iopAddress, const std::uint8_t* from, std::uint32_t size);

// The mixer: 48 kHz stereo to the Switch's audio output, or on the host to the WAV file
// SLY1_AUDIO_WAV names. Wanted() says whether there is anywhere for sound to go; MixerMain is
// the mixer's thread, which the program starts (port/host/main.cpp) and which does not return.
bool Wanted();
void MixerMain();

// On the host: the WAV file's header written with what was mixed (at the program's end).
void Finish();

}  // namespace audio
}  // namespace port

#endif
