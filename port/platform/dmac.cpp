// SPDX-License-Identifier: AGPL-3.0-or-later
// The DMA controller, VIF and GIF as far as the game's waits need them (port/dmac.h).
//
// A channel the game starts in source-chain mode is walked at once, tag by tag (refe, cnt, next,
// ref, refs, call, ret, end, with the two-level address stack), reading main memory or the
// scratchpad as the transfer reaches it: an interrupt handler run in the middle of a transfer
// (VIF1's, which packs colours into packets further on) writes data the chain then reads, as on
// the PS2. VIF0 and VIF1 decode their VIFcodes to keep their place in the stream; a VIFcode with
// its interrupt bit raises cause 4 or 5. GIF packets (VIF1's DIRECT, the GIF channel) are read
// tag by tag, and a write to the GS's FINISH register sets CSR's FINISH flag and raises cause 0,
// the GS interrupt the render thread waits on. Drawing itself is the renderer's (later); VU1's
// microprograms are not run, so a FINISH a microprogram would send is not seen here.
#include "port/dmac.h"

#include <atomic>
#include <cstdlib>
#include <map>
#include <string>
#include <cstdio>
#include <cstring>

#include "port/ptr32.h"
#include "port/system.h"

namespace port {

namespace {

constexpr std::uint32_t kVif0 = 0x10008000, kVif1 = 0x10009000, kGif = 0x1000A000;
constexpr std::uint32_t kFromSpr = 0x1000D000, kToSpr = 0x1000D400;
constexpr std::uint32_t kGsCsr = 0x12001000;
constexpr int kCauseGs = 0, kCauseVif0 = 4, kCauseVif1 = 5;

struct Counts {
    std::atomic<unsigned> chains{0}, tags{0}, vifInterrupts{0}, finishes{0}, unknownVifcodes{0}, sprChains{0};
} g_counts;

// Main memory or the scratchpad at a tag's address (bit 31 of the address field: the scratchpad).
// The rest is a game address as the port has them: main memory, its uncached alias, or the port's
// image (a packet the game builds on a thread's stack).
const std::uint32_t* At(std::uint32_t addr, bool spr)
{
    if (spr)
        return reinterpret_cast<const std::uint32_t*>(Scratchpad() + (addr & 0x3FF0));
    return reinterpret_cast<const std::uint32_t*>(Native(addr & 0x7FFFFFF0));
}

// A GIF packet stream, a quadword at a time.
class Gif {
public:
    void Qword(const std::uint32_t* q)
    {
        if (left_ == 0) {
            Tag(q);
            return;
        }
        if (flg_ == 0) {   // PACKED: one register a quadword, A+D carries its register's address
            unsigned reg = (unsigned)(regs_ >> (4 * (index_ % nreg_))) & 0xF;
            if (reg == 0xE && (q[2] & 0xFF) == 0x61)
                Finish();
            index_++;
        }
        left_--;
    }

private:
    void Tag(const std::uint32_t* q)
    {
        std::uint32_t nloop = q[0] & 0x7FFF;
        flg_ = (q[1] >> 26) & 3;
        nreg_ = (q[1] >> 28) & 0xF;
        if (nreg_ == 0)
            nreg_ = 16;
        regs_ = q[2] | (std::uint64_t)q[3] << 32;
        index_ = 0;
        if (flg_ == 0)
            left_ = nloop * nreg_;
        else if (flg_ == 1)
            left_ = (nloop * nreg_ + 1) / 2;
        else
            left_ = nloop;
    }

    // The GS has drawn everything before the FINISH write: CSR's FINISH flag and its interrupt
    // (the game unmasks it before it waits).
    static void Finish()
    {
        *static_cast<volatile std::uint64_t*>(Native(kGsCsr)) |= 2;
        g_counts.finishes++;
        RaiseInterrupt(kCauseGs);
    }

    std::uint32_t left_ = 0, nreg_ = 1, flg_ = 0, index_ = 0;
    std::uint64_t regs_ = 0;
};

// A VIF's stream of 32-bit words: its VIFcodes and their data.
class Vif {
public:
    explicit Vif(int cause) : cause_(cause) {}

    void Word(std::uint32_t w)
    {
        if (left_ == 0) {
            Code(w);
            return;
        }
        if (direct_) {
            qword_[filled_++] = w;
            if (filled_ == 4) {
                gif_.Qword(qword_);
                filled_ = 0;
            }
        }
        if (--left_ == 0)
            End();
    }

    // Where the stream stands, and the VIFcodes seen since the last call, for the trace.
    std::string State()
    {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s, %u words owed", left_ ? "inside a VIFcode's data" : "between VIFcodes", left_);
        std::string s = buf;
        for (auto& kv : seen_) {
            std::snprintf(buf, sizeof buf, " %02X:%u", kv.first, kv.second);
            s += buf;
        }
        seen_.clear();
        return s;
    }

private:
    void Code(std::uint32_t w)
    {
        std::uint32_t cmd = (w >> 24) & 0x7F, imm = w & 0xFFFF, num = (w >> 16) & 0xFF;
        seen_[(cmd & 0x60) == 0x60 ? 0x60 : cmd]++;
        irq_ = (w >> 31) != 0;
        direct_ = false;
        if ((cmd & 0x60) == 0x60) {   // UNPACK: num vectors of vn+1 elements of 32 >> vl bits
            std::uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3, n = num ? num : 256;
            std::uint32_t vectors = n;
            if (wl_ > cl_)   // filling: only cl of every wl written vectors come from the stream
                vectors = cl_ * (n / wl_) + (n % wl_ < cl_ ? n % wl_ : cl_);
            std::uint32_t bits = (vn == 3 && vl == 3) ? 16 : (32u >> vl) * (vn + 1);
            left_ = (vectors * bits + 31) / 32;
        } else {
            switch (cmd) {
            case 0x00: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
            case 0x10: case 0x11: case 0x13: case 0x14: case 0x15: case 0x17:
                left_ = 0;   // NOP, OFFSET .. MARK, the flushes, the microprogram starts
                break;
            case 0x01:   // STCYCL
                cl_ = imm & 0xFF;
                wl_ = (imm >> 8) & 0xFF;
                if (wl_ == 0)
                    wl_ = 256;
                left_ = 0;
                break;
            case 0x20:   // STMASK
                left_ = 1;
                break;
            case 0x30: case 0x31:   // STROW, STCOL
                left_ = 4;
                break;
            case 0x4A:   // MPG: num 64-bit instructions
                left_ = (num ? num : 256) * 2;
                break;
            case 0x50: case 0x51:   // DIRECT, DIRECTHL: imm quadwords for the GIF
                left_ = (imm ? imm : 65536) * 4;
                direct_ = true;
                filled_ = 0;
                break;
            default:
                if (g_counts.unknownVifcodes++ < 8)
                    std::fprintf(stderr, "dmac: VIFcode 0x%08X not understood\n", w);
                left_ = 0;
            }
        }
        if (left_ == 0)
            End();
    }

    // A VIFcode done; with its interrupt bit, the VIF stops and interrupts, and the handler lets
    // it go on (FBRST's STC).
    void End()
    {
        direct_ = false;
        if (irq_) {
            irq_ = false;
            g_counts.vifInterrupts++;
            RaiseInterrupt(cause_);
        }
    }

    int cause_;
    std::uint32_t left_ = 0, cl_ = 1, wl_ = 1, filled_ = 0;
    std::uint32_t qword_[4] = {};
    bool irq_ = false, direct_ = false;
    Gif gif_;
    std::map<std::uint32_t, unsigned> seen_;
};

Vif g_vif0(kCauseVif0), g_vif1(kCauseVif1);
Gif g_gif;

// A quadword of the scratchpad, by SADR's offset (it wraps at 16 KiB).
std::uint32_t* Spr(std::uint32_t sadr)
{
    return reinterpret_cast<std::uint32_t*>(Scratchpad() + (sadr & 0x3FF0));
}

}  // namespace

void RunFromSprChain(std::uint32_t regs)
{
    // A destination chain: each tag is read from the scratchpad at SADR (cnt, cnts, end), and the
    // quadwords after it go to the tag's address in main memory.
    volatile std::uint32_t* r = static_cast<volatile std::uint32_t*>(Native(regs));
    g_counts.sprChains++;
    std::uint32_t sadr = r[0x80 / 4];
    for (int guard = 0; guard < 1 << 16; guard++) {
        const std::uint32_t* tag = Spr(sadr);
        std::uint32_t qwc = tag[0] & 0xFFFF, id = (tag[0] >> 28) & 7, addr = tag[1] & 0x7FFFFFF0;
        sadr += 16;
        g_counts.tags++;
        for (std::uint32_t i = 0; i < qwc; i++, sadr += 16)
            std::memcpy(Native(addr + i * 16), Spr(sadr), 16);
        if (id == 7)
            break;
    }
    r[0x80 / 4] = sadr;
    r[0x20 / 4] = 0;
    r[0] &= ~0x100u;
}

void RunDmaChain(std::uint32_t regs, std::uint32_t tadr)
{
    volatile std::uint32_t* r = static_cast<volatile std::uint32_t*>(Native(regs));
    Vif* vif = regs == kVif0 ? &g_vif0 : regs == kVif1 ? &g_vif1 : nullptr;
    Gif* gif = regs == kGif ? &g_gif : nullptr;
    bool toSpr = regs == kToSpr;   // main memory into the scratchpad from SADR, tags too with TTE
    std::uint32_t sadr = toSpr ? r[0x80 / 4] : 0;
    if (!vif && !gif && !toSpr) {
        std::fprintf(stderr, "dmac: a chain on channel 0x%08X, which the port does not run\n", regs);
        r[0] &= ~0x100u;
        return;
    }
    g_counts.chains++;
    static const bool trace = std::getenv("SLY1_DMAC_TRACE") != nullptr;
    unsigned finishesBefore = g_counts.finishes, tagsBefore = g_counts.tags;
    std::uint32_t start = tadr;
    bool tte = (r[0] & 0x40) != 0, tie = (r[0] & 0x80) != 0;
    std::uint32_t stack[2];
    int depth = 0;
    for (int guard = 0; guard < 1 << 20; guard++) {
        bool tagSpr = (tadr & 0x80000000u) != 0;
        const std::uint32_t* tag = At(tadr, tagSpr);
        std::uint32_t qwc = tag[0] & 0xFFFF, id = (tag[0] >> 28) & 7, addr = tag[1] & 0x7FFFFFF0;
        bool irq = (tag[0] >> 31) != 0, spr = (tag[1] >> 31) != 0;
        g_counts.tags++;
        if (vif && tte) {
            vif->Word(tag[2]);
            vif->Word(tag[3]);
        }
        if (toSpr && tte) {
            std::memcpy(Spr(sadr), tag, 16);
            sadr += 16;
        }
        std::uint32_t data = tadr + 16, next = 0;
        bool dataSpr = tagSpr, end = false;
        switch (id) {
        case 0: data = addr; dataSpr = spr; end = true; break;           // refe
        case 1: next = data + qwc * 16; break;                           // cnt
        case 2: next = addr | (spr ? 0x80000000u : 0); break;            // next
        case 3: case 4: data = addr; dataSpr = spr; next = tadr + 16; break;   // ref, refs
        case 5:                                                          // call
            if (depth < 2)
                stack[depth++] = data + qwc * 16;
            next = addr | (spr ? 0x80000000u : 0);
            break;
        case 6:                                                          // ret
            if (depth > 0)
                next = stack[--depth];
            else
                end = true;
            break;
        case 7: end = true; break;                                       // end
        }
        for (std::uint32_t i = 0; i < qwc; i++) {
            const std::uint32_t* q = At(data + i * 16, dataSpr);
            if (toSpr) {
                std::memcpy(Spr(sadr), q, 16);
                sadr += 16;
            } else if (vif) {
                for (int k = 0; k < 4; k++)
                    vif->Word(q[k]);
            } else {
                gif->Qword(q);
            }
        }
        if (end || (irq && tie))
            break;
        tadr = next;
    }
    if (trace)
        std::fprintf(stderr, "dmac: chain on 0x%08X from 0x%08X%s: %u tags, %u FINISH, VIF %s\n", regs, start,
                     tte ? " (TTE)" : "", g_counts.tags - tagsBefore, g_counts.finishes - finishesBefore,
                     vif ? vif->State().c_str() : "-");
    r[0x30 / 4] = tadr;
    if (toSpr)
        r[0x80 / 4] = sadr;
    r[0x20 / 4] = 0;
    r[0] &= ~0x100u;   // STR: the transfer is over
}

void DmacDump()
{
    std::fprintf(stderr, "dmac: %u chains (%u out of the scratchpad), %u tags, %u VIF interrupts, %u GS FINISH, "
                 "%u VIFcodes not understood\n", g_counts.chains.load(), g_counts.sprChains.load(),
                 g_counts.tags.load(), g_counts.vifInterrupts.load(), g_counts.finishes.load(),
                 g_counts.unknownVifcodes.load());
}

}  // namespace port
