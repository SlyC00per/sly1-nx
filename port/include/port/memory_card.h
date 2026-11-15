// SPDX-License-Identifier: AGPL-3.0-or-later
// Saves without a memory card (port/platform/memory_card.cpp): the folder that stands for the card.
#ifndef PORT_MEMORY_CARD_H
#define PORT_MEMORY_CARD_H

#include <string>

namespace port {

// The folder the game's card files go in ("save" in the working folder unless set).
void SetSaveFolder(const std::string& path);

}  // namespace port

#endif
