// SPDX-License-Identifier: AGPL-3.0-or-later
// The native handlers (port/render/vu1_handlers.h). Each is written from the entry's microcode
// and the builder that packs its input, as docs/RENDERER.md describes, and checked against the
// interpreter with SLY1_VU1_CHECK before it is relied on.
#include "vu1_handlers.h"

namespace port {
namespace vu1 {

const HandlerEntry kHandlers[] = {
    {0, 0, nullptr, nullptr},   // (none yet)
};
const std::size_t kHandlerCount = 0;

}  // namespace vu1
}  // namespace port
