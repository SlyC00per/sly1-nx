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
#include <cstdarg>
#include <mutex>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>

#include "port/ptr32.h"
#include "port/system.h"
#include "port/vu0_micro.h"

namespace port {

namespace {

constexpr std::uint32_t kVif0 = 0x10008000, kVif1 = 0x10009000, kGif = 0x1000A000;
constexpr std::uint32_t kFromSpr = 0x1000D000, kToSpr = 0x1000D400;
constexpr std::uint32_t kGsCsr = 0x12001000;
constexpr int kCauseGs = 0, kCauseVif0 = 4, kCauseVif1 = 5;

struct Counts {
    std::atomic<unsigned> chains{0}, tags{0}, vifInterrupts{0}, finishes{0}, unknownVifcodes{0}, sprChains{0};
} g_counts;

// SLY1_RENDER_SURVEY: what the renderer must handle (docs/RENDERER.md), printed by DmacDump.
struct Survey {
    // read on first use: --env sets it after static initialisation
    bool On()
    {
        static const bool on = std::getenv("SLY1_RENDER_SURVEY") != nullptr;
        return on;
    }
    std::mutex lock;
    std::map<std::string, unsigned long> counts;          // "what" -> times
    std::map<std::uint32_t, std::uint32_t> programAt;     // VU1 micro address (qwords) -> hash of its upload
    std::map<std::uint32_t, std::uint32_t> programSize;   // hash -> 64-bit instructions
    void Count(const char* fmt, ...) __attribute__((format(printf, 2, 3)))
    {
        char buf[128];
        va_list va;
        va_start(va, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, va);
        va_end(va);
        std::lock_guard<std::mutex> lk(lock);
        counts[buf]++;
    }
} g_survey;

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
    explicit Gif(int path) : path_(path) {}

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
            if (g_survey.On())
                SurveyPacked(reg, q);
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
        if (g_survey.On()) {
            static const char* kFlg[] = {"PACKED", "REGLIST", "IMAGE", "IMAGE"};
            g_survey.Count("gif path %d: %s packets", path_, kFlg[flg_]);
            if ((q[1] >> 14) & 1)   // PRE: the tag sets PRIM
                g_survey.Count("gif path %d: primitive %s (from a tag)", path_, Prim(q[1] >> 15));
            if (flg_ == 2)
                g_survey.Count("gif path %d: image uploads, format 0x%02X", path_, dpsm_);
        }
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

    static const char* Prim(std::uint32_t prim)
    {
        static const char* kNames[] = {"point", "line", "line strip", "triangle", "triangle strip",
                                       "triangle fan", "sprite", "(7)"};
        return kNames[prim & 7];
    }

    // A PACKED register write: the primitive kinds, texture formats and upload formats seen.
    void SurveyPacked(unsigned reg, const std::uint32_t* q)
    {
        unsigned addr = reg == 0xE ? (q[2] & 0xFF) : reg;
        std::uint64_t data = q[0] | (std::uint64_t)q[1] << 32;
        switch (addr) {
        case 0x00:   // PRIM
            g_survey.Count("gif path %d: primitive %s%s%s", path_, Prim((std::uint32_t)data),
                           (data >> 4) & 1 ? ", textured" : "", (data >> 6) & 1 ? ", blended" : "");
            break;
        case 0x06: case 0x07:   // TEX0_1, TEX0_2: the texture's format
            g_survey.Count("gif path %d: texture format 0x%02X", path_, (unsigned)(data >> 20) & 0x3F);
            break;
        case 0x50:   // BITBLTBUF: an upload's format
            dpsm_ = (unsigned)(data >> 56) & 0x3F;
            break;
        default:
            if (reg == 0xE)
                g_survey.Count("gif path %d: A+D register 0x%02X", path_, addr);
        }
    }

    int path_;
    unsigned dpsm_ = 0;
    std::uint32_t left_ = 0, nreg_ = 1, flg_ = 0, index_ = 0;
    std::uint64_t regs_ = 0;
};

// A VIF's stream of 32-bit words: its VIFcodes and their data.
class Vif {
public:
    // VIF0 feeds VU0 (port/vu0_micro.h): its MPG, UNPACK and microprogram starts take effect.
    Vif(int cause, bool vu0) : cause_(cause), vu0_(vu0) {}

    void Word(std::uint32_t w)
    {
        if (left_ == 0) {
            Code(w);
            return;
        }
        if (keep_)
            data_.push_back(w);
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
        cmd_ = cmd;
        imm_ = imm;
        num_ = num;
        data_.clear();
        keep_ = vu0_ && (cmd == 0x4A || (cmd & 0x60) == 0x60);
        if (!vu0_ && g_survey.On()) {
            keep_ = cmd == 0x4A;   // VIF1's microcode, to name the programs by a hash of it
            if ((cmd & 0x60) == 0x60)
                g_survey.Count("vif1 UNPACK 0x%02X", cmd & 0x7F);
            if (cmd == 0x14 || cmd == 0x15 || cmd == 0x17)
                SurveyStart(cmd == 0x17 ? 0xFFFF : (imm & 0x3FF));
        }
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
            case 0x10: case 0x11: case 0x13:
                left_ = 0;   // NOP, OFFSET .. MARK, the flushes
                break;
            case 0x14: case 0x15: case 0x17:   // MSCAL, MSCALF, MSCNT
                left_ = 0;
                if (vu0_)
                    vu0::Run(cmd == 0x17 ? vu0::Vu0().pc * 8 : (imm & 0x1FF) * 8);
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
        if (keep_) {
            keep_ = false;
            if (!vu0_)
                SurveyMicrocode();
            else if (cmd_ == 0x4A)
                LoadMicrocode();
            else
                Unpack();
        }
        if (irq_) {
            irq_ = false;
            g_counts.vifInterrupts++;
            RaiseInterrupt(cause_);
        }
    }

    // VIF1's MPG: the program's instructions hashed, its addresses noted.
    void SurveyMicrocode()
    {
        std::uint32_t h = 2166136261u;
        for (std::uint32_t w : data_)
            h = (h ^ w) * 16777619u;
        std::lock_guard<std::mutex> lk(g_survey.lock);
        for (std::uint32_t k = 0; k < data_.size() / 2; k++)
            g_survey.programAt[(imm_ + k) & 0x7FF] = h;
        g_survey.programSize[h] = (std::uint32_t)data_.size() / 2;
        // SLY1_VU1_DUMP=FOLDER: each upload written there as <address>-<hash>.bin (the player's own
        // game data: build folders only, never committed)
        if (const char* dir = std::getenv("SLY1_VU1_DUMP")) {
            char path[512];
            std::snprintf(path, sizeof path, "%s/%03X-%08X.bin", dir, imm_ & 0x7FF, h);
            if (std::FILE* f = std::fopen(path, "wb")) {
                std::fwrite(data_.data(), 4, data_.size(), f);
                std::fclose(f);
            }
        }
    }

    // A microprogram start: the program (its upload's hash) and the entry, in 64-bit instructions.
    void SurveyStart(std::uint32_t entry)
    {
        std::uint32_t h = 0;
        {
            std::lock_guard<std::mutex> lk(g_survey.lock);
            if (entry != 0xFFFF) {
                auto it = g_survey.programAt.find(entry);
                h = it == g_survey.programAt.end() ? 0 : it->second;
            }
        }
        if (entry == 0xFFFF)
            g_survey.Count("vif1 MSCNT (continue)");
        else
            g_survey.Count("vif1 MSCAL entry 0x%03X of program %08X", entry, h);
    }

    // MPG: the instructions to VU0's micro memory at imm (in 64-bit instructions).
    void LoadMicrocode()
    {
        vu0::State& s = vu0::Vu0();
        for (std::size_t k = 0; k < data_.size(); k++)
            s.micro[(imm_ * 2 + k) & 1023] = data_[k];
    }

    // UNPACK to VU0's data memory: num vectors of vn+1 fields of 32 >> vl bits (V4-5: 16-bit
    // colours) from address imm, cl of every wl quadwords written (STCYCL); fields zero- or
    // sign-extended (usn), a scalar to all four fields.
    void Unpack()
    {
        std::uint32_t vn = (cmd_ >> 2) & 3, vl = cmd_ & 3, n = num_ ? num_ : 256;
        if (cmd_ & 0x10) {
            std::fprintf(stderr, "dmac: a masked UNPACK to VU0 (0x%02X), not done\n", cmd_);
            return;
        }
        if (wl_ > cl_) {
            std::fprintf(stderr, "dmac: a filling UNPACK to VU0 (cl %u, wl %u), not done\n", cl_, wl_);
            return;
        }
        bool usn = (imm_ >> 14) & 1, v45 = vn == 3 && vl == 3;
        std::uint32_t bits = v45 ? 16 : 32u >> vl, fields = v45 ? 1 : vn + 1;
        std::uint64_t pos = 0;   // bit position in the data
        auto take = [&](std::uint32_t nb) {
            std::uint32_t v = 0;
            for (std::uint32_t b = 0; b < nb; b++, pos++) {
                std::size_t word = pos / 32;
                if (word < data_.size() && (data_[word] >> (pos % 32)) & 1)
                    v |= 1u << b;
            }
            return v;
        };
        vu0::State& s = vu0::Vu0();
        for (std::uint32_t i = 0; i < n; i++) {
            std::uint32_t q = ((imm_ & 0x3FF) + (i / wl_) * cl_ + (i % wl_)) & 0xFF;
            std::uint32_t* out = reinterpret_cast<std::uint32_t*>(s.data + q * 16);
            std::uint32_t v[4];
            for (std::uint32_t f = 0; f < fields; f++) {
                std::uint32_t x = take(bits);
                if (!usn && bits < 32 && (x >> (bits - 1)) & 1)
                    x |= ~0u << bits;
                v[f] = x;
            }
            if (v45) {
                std::uint32_t c = v[0];
                out[0] = (c & 0x1F) << 3;
                out[1] = ((c >> 5) & 0x1F) << 3;
                out[2] = ((c >> 10) & 0x1F) << 3;
                out[3] = ((c >> 15) & 1) << 7;
            } else if (fields == 1) {
                out[0] = out[1] = out[2] = out[3] = v[0];
            } else {
                for (std::uint32_t f = 0; f < fields; f++)
                    out[f] = v[f];
            }
        }
    }

    int cause_;
    bool vu0_ = false, keep_ = false;
    std::uint32_t cmd_ = 0, imm_ = 0, num_ = 0;
    std::vector<std::uint32_t> data_;
    std::uint32_t left_ = 0, cl_ = 1, wl_ = 1, filled_ = 0;
    std::uint32_t qword_[4] = {};
    bool irq_ = false, direct_ = false;
    Gif gif_{2};
    std::map<std::uint32_t, unsigned> seen_;
};

Vif g_vif0(kCauseVif0, true), g_vif1(kCauseVif1, false);
Gif g_gif{3};

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
    // SLY1_DMAC_TRACE=2: each tag too
    static const bool traceTags = trace && std::getenv("SLY1_DMAC_TRACE")[0] == '2';
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
        if (traceTags)
            std::fprintf(stderr, "dmac:   tag at 0x%08X: %08X %08X %08X %08X (id %u, qwc %u, addr 0x%08X)\n", tadr,
                         tag[0], tag[1], tag[2], tag[3], id, qwc, addr);
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
    if (g_survey.On()) {
        std::lock_guard<std::mutex> lk(g_survey.lock);
        for (auto& kv : g_survey.counts)
            std::fprintf(stderr, "survey: %10lu  %s\n", kv.second, kv.first.c_str());
        for (auto& kv : g_survey.programSize)
            std::fprintf(stderr, "survey: program %08X: %u instructions\n", kv.first, kv.second);
    }
}

}  // namespace port
