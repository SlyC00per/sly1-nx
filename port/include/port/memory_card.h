// SPDX-License-Identifier: AGPL-3.0-or-later
// Saves without a memory card (port/platform/memory_card.cpp): the folder that stands for the card.
#ifndef PORT_MEMORY_CARD_H
#define PORT_MEMORY_CARD_H

#include <string>

namespace port {

// The folder the game's card files go in ("save" in the working folder unless set).
void SetSaveFolder(const std::string& path);
// A save file (0x1A00 bytes) put in slot 1 to 3, the one there backed up first (memory_card.cpp).
bool InstallSave(int slot, const unsigned char* data, std::size_t size);

}  // namespace port

#endif
