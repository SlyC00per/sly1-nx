// SPDX-License-Identifier: AGPL-3.0-or-later
// Main memory's start-up contents: the executable's segments from the player's disc
// (port/retail_data.h).
#include "port/retail_data.h"

#include <vector>

#include "port/iso.h"

namespace port {

bool LoadRetailImage() { return LoadExecutableSegments(); }

}  // namespace port
