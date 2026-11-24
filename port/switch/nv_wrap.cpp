// SPDX-License-Identifier: AGPL-3.0-or-later
// Diagnosis only (SLY1_DEKO3D_DEBUG): libnx's GPU channel kickoff wrapped at link time, its
// failures reported with the number of entries it carried and its result.
#include <switch.h>
#include <cstdio>

extern "C" {
Result __real_nvGpuChannelKickoff(NvGpuChannel* c);
Result __wrap_nvGpuChannelKickoff(NvGpuChannel* c)
{
    static unsigned most = 0;
    const unsigned n = c->num_entries;
    if (n > most) {
        most = n;
        std::fprintf(stderr, "nvwrap: kickoff of %u entries (most yet)\n", n);
    }
    const Result r = __real_nvGpuChannelKickoff(c);
    if (R_FAILED(r))
        std::fprintf(stderr, "nvwrap: kickoff of %u entries failed: result 0x%x (module %u, description %u)\n", n, r, R_MODULE(r), R_DESCRIPTION(r));
    return r;
}
}
