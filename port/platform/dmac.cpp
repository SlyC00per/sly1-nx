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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <mutex>
#include <optional>
#include <condition_variable>
#include <deque>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>

#include "port/ptr32.h"
#include "port/system.h"
#include "port/vu0_micro.h"
#include "port/vu1.h"
#include "port/gs.h"

namespace port {

namespace {

constexpr std::uint32_t kVif0 = 0x10008000, kVif1 = 0x10009000, kGif = 0x1000A000;
constexpr std::uint32_t kFromSpr = 0x1000D000, kToSpr = 0x1000D400;
constexpr std::uint32_t kGsCsr = 0x12001000;
constexpr int kCauseGs = 0, kCauseVif0 = 4, kCauseVif1 = 5;

struct Counts {
    std::atomic<unsigned> chains{0}, tags{0}, vifInterrupts{0}, finishes{0}, unknownVifcodes{0}, sprChains{0};
    std::atomic<std::uint64_t> vu1Instructions{0}, path1Packets{0}, vu1Nanoseconds{0}, dmaNanoseconds{0};
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

    // Quadwords still owed to the current tag's data (0: the next quadword is a tag).
    std::uint32_t Left() const { return left_; }

    void Qword(const std::uint32_t* q)
    {
        if (left_ == 0) {
            Tag(q);
            return;
        }
        if (flg_ == 0) {   // PACKED: one register a quadword, A+D carries its register's address
            unsigned reg = (unsigned)(regs_ >> (4 * slot_)) & 0xF;
            if (++slot_ == nreg_)
                slot_ = 0;
            if (reg == 0xE) {
                gs::Write(q[2] & 0xFF, q[0] | (std::uint64_t)q[1] << 32);
                if ((q[2] & 0xFF) == 0x61)
                    Finish();
            } else {
                gs::WritePacked(reg, q);
            }
            if (survey_)
                SurveyPacked(reg, q);
            index_++;
        } else if (flg_ == 1) {   // REGLIST: two registers a quadword, each 64 bits as it is
            for (int h = 0; h < 2 && index_ < regTotal_; h++, index_++) {
                unsigned reg = (unsigned)(regs_ >> (4 * (index_ % nreg_))) & 0xF;
                if (reg != 0xE && reg != 0xF)
                    gs::Write(reg, q[h * 2] | (std::uint64_t)q[h * 2 + 1] << 32);
            }
        } else {                  // IMAGE
            gs::Image(q);
        }
        left_--;
    }

    // A run of PACKED data in one piece of memory: its whole loops handed to the GS at once
    // (gs::PackedRun). Returns the quadwords taken, 0 when the data must go a quadword at a time.
    std::uint32_t Run(const std::uint32_t* q, std::uint32_t avail)
    {
        if (flg_ >= 2 && left_ != 0) {   // IMAGE: the data as it is
            std::uint32_t n = std::min(left_, avail);
            gs::ImageRun(q, n);
            left_ -= n;
            return n;
        }
        if (!plain_ || slot_ != 0 || left_ < nreg_)
            return 0;
        std::uint32_t loops = std::min(left_, avail) / nreg_;
        if (loops == 0)
            return 0;
        gs::PackedRun(regs_, nreg_, q, loops);
        left_ -= loops * nreg_;
        index_ += loops * nreg_;
        return loops * nreg_;
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
        slot_ = 0;
        survey_ = g_survey.On();
        regTotal_ = nloop * nreg_;
        if ((q[1] >> 14) & 1)   // PRE: the tag sets PRIM
            gs::Write(0x00, (q[1] >> 15) & 0x7FF);
        if (g_survey.On()) {
            static const char* kFlg[] = {"PACKED", "REGLIST", "IMAGE", "IMAGE"};
            g_survey.Count("gif path %d: %s packets", path_, kFlg[flg_]);
            if ((q[1] >> 14) & 1)   // PRE: the tag sets PRIM
                g_survey.Count("gif path %d: primitive %s (from a tag)", path_, Prim(q[1] >> 15));
            if (flg_ == 2)
                g_survey.Count("gif path %d: image uploads, format 0x%02X", path_, dpsm_);
        }
        plain_ = false;
        if (flg_ == 0 && !survey_) {   // PACKED without A+D: nothing the GIF itself acts on
            plain_ = true;
            for (std::uint32_t k = 0; k < nreg_; k++)
                if (((regs_ >> (4 * k)) & 0xF) == 0xE)
                    plain_ = false;
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
    std::uint32_t left_ = 0, nreg_ = 1, flg_ = 0, index_ = 0, regTotal_ = 0, slot_ = 0;
    bool survey_ = false, plain_ = false;
    std::uint64_t regs_ = 0;
};

// GIF path 1: what VU1's XGKICK sends, from its data memory.
Gif g_path1{1};
// Path 2 (VIF1's DIRECT) as the GS thread parses it.
Gif g_path2{2};

// --- The GS thread: the GS's work (the three paths' packets and the display's calls) on a core
// of its own, after the render thread's VIF1 and VU1 (core 1 on the Switch, beside the clock).
// A path-1 packet is copied out of VU1's data memory when it is kicked (the microcode writes the
// next one over it), VIF1's DIRECT data and the GIF channel's items are passed on as they come,
// all in one queue, in order, so the GS sees what it would have seen.
struct GsItem {
    std::uint8_t path = 0;   // 1-3, or 0: a call
    std::vector<std::uint32_t> words;
    void (*fn)(std::uint64_t, std::uint64_t) = nullptr;
    std::uint64_t a = 0, b = 0;
};
std::mutex g_gsLock;
std::condition_variable g_gsWake, g_gsIdle;
std::deque<GsItem> g_gsQueue;
std::vector<std::vector<std::uint32_t>> g_gsSpare;
bool g_gsBusy, g_gsWaiting;
std::atomic<bool> g_gsStarted;
// The render thread's items for the GS, passed on together at the end of each of its own items
// (a wake of the GS thread is a system call on the Switch: one a packet cost more than the GS's
// work), and its own stock of emptied buffers.
std::vector<GsItem> g_gsPending;
std::vector<std::vector<std::uint32_t>> g_gsLocalSpare;

// Only with the render thread: without it, the paths reach the GS from the game's thread in turn.
bool GsAsync()
{
    static const bool sync = std::getenv("SLY1_RENDER_SYNC") != nullptr;
    return !sync && g_gsStarted.load(std::memory_order_acquire);
}

std::vector<std::uint32_t> GsSpare()
{
    if (g_gsLocalSpare.empty()) {
        std::lock_guard<std::mutex> lk(g_gsLock);
        for (int k = 0; k < 64 && !g_gsSpare.empty(); k++) {
            g_gsLocalSpare.push_back(std::move(g_gsSpare.back()));
            g_gsSpare.pop_back();
        }
        if (g_gsLocalSpare.empty())
            return {};
    }
    std::vector<std::uint32_t> v = std::move(g_gsLocalSpare.back());
    g_gsLocalSpare.pop_back();
    v.clear();
    return v;
}

// (on the render thread)
void GsEnqueue(GsItem&& item) { g_gsPending.push_back(std::move(item)); }

void GsPassOn()
{
    if (g_gsPending.empty())
        return;
    std::lock_guard<std::mutex> lk(g_gsLock);
    for (GsItem& item : g_gsPending)
        g_gsQueue.push_back(std::move(item));
    g_gsPending.clear();
    if (g_gsWaiting)
        g_gsWake.notify_one();
}

// XGKICK: the GIF packet at quadword qaddr of VU1's data memory, tag by tag until a tag with
// EOP has had its data (the data memory wraps at 1,024 quadwords).
void KickPath1(std::uint32_t qaddr, void*)
{
    const std::uint8_t* d = vu1::Data();
    g_counts.path1Packets++;
    if (GsAsync()) {   // the packet copied, tag by tag to the one with EOP
        GsItem item;
        item.path = 1;
        item.words = GsSpare();
        for (int tags = 0; tags < 1024; tags++) {
            const std::uint32_t* tag = reinterpret_cast<const std::uint32_t*>(d + (qaddr & 0x3FF) * 16);
            std::uint32_t nloop = tag[0] & 0x7FFF, flg = (tag[1] >> 26) & 3, nreg = (tag[1] >> 28) & 0xF;
            if (nreg == 0)
                nreg = 16;
            std::uint32_t n = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
            for (std::uint32_t k = 0; k <= n && k < 1024; k++) {
                const std::uint32_t* q = reinterpret_cast<const std::uint32_t*>(d + ((qaddr + k) & 0x3FF) * 16);
                item.words.insert(item.words.end(), q, q + 4);
            }
            qaddr += std::min<std::uint32_t>(n, 1023) + 1;
            if ((tag[0] >> 15) & 1)
                break;
        }
        GsEnqueue(std::move(item));
        return;
    }
    gs::Hold hold;
    {   // SLY1_KICK_TRACE=entry: the GIF tags of that program's kicks (the first few), for diagnosis
        static const long traced = std::getenv("SLY1_KICK_TRACE") ? std::strtol(std::getenv("SLY1_KICK_TRACE"), nullptr, 16) : -1;
        static int printed;
        std::uint32_t hash, entry;
        vu1::CurrentProgram(&hash, &entry);
        if (traced >= 0 && (long)entry == traced && printed < 12) {
            printed++;
            std::uint32_t at = qaddr;
            for (int tags = 0; tags < 8; tags++) {
                const std::uint32_t* t = reinterpret_cast<const std::uint32_t*>(d + (at & 0x3FF) * 16);
                std::uint32_t nloop = t[0] & 0x7FFF, flg = (t[1] >> 26) & 3, nreg = (t[1] >> 28) & 0xF;
                std::fprintf(stderr, "kick 0x%03X: tag at %u NLOOP %u EOP %u PRE %u PRIM %03X FLG %u NREG %u REGS %08X%08X\n", entry, at & 0x3FF,
                             nloop, (t[0] >> 15) & 1, (t[1] >> 14) & 1, (t[1] >> 15) & 0x7FF, flg, nreg ? nreg : 16, t[3], t[2]);
                std::uint32_t n = flg == 0 ? nloop * (nreg ? nreg : 16) : flg == 1 ? (nloop * (nreg ? nreg : 16) + 1) / 2 : nloop;
                at += n + 1;
                if ((t[0] >> 15) & 1)
                    break;
            }
        }
    }
    for (int tags = 0; tags < 1024; tags++) {
        const std::uint32_t* tag = reinterpret_cast<const std::uint32_t*>(d + (qaddr & 0x3FF) * 16);
        bool eop = (tag[0] >> 15) & 1;
        g_path1.Qword(tag);
        qaddr++;
        for (std::uint32_t guard = 0; g_path1.Left() > 0 && guard < 1024; guard++) {
            const std::uint32_t* q = reinterpret_cast<const std::uint32_t*>(d + (qaddr & 0x3FF) * 16);
            std::uint32_t taken = g_path1.Run(q, 1024 - (qaddr & 0x3FF));   // up to the memory's wrap
            if (taken == 0) {
                g_path1.Qword(q);
                taken = 1;
            }
            qaddr += taken;
        }
        if (eop)
            break;
    }
}

// A VIF's stream of 32-bit words: its VIFcodes and their data.
class Vif {
public:
    // VIF0 feeds VU0 (port/vu0_micro.h): its MPG, UNPACK and microprogram starts take effect.
    Vif(int cause, bool vu0) : cause_(cause), vu0_(vu0) {}

    // A quadword's words: the data of a kept VIFcode (an unpack, microcode) appended at once.
    void Words(const std::uint32_t* w, int n)
    {
        int k = 0;
        while (k < n) {
            if (left_ != 0 && direct_ && !keep_ && filled_ == 0 && n - k >= 4 && !(!vu0_ && GsAsync())) {
                // DIRECT's data a quadword at a time from where it is (whole runs of a GIF tag's loops at
                // once), not gathered a word at a time
                std::uint32_t quads = std::min<std::uint32_t>(left_ / 4, (std::uint32_t)(n - k) / 4);
                if (quads > 0) {
                    std::uint32_t done = 0;
                    while (done < quads) {
                        const std::uint32_t* q = w + k + done * 4;
                        std::uint32_t taken = gif_.Left() ? gif_.Run(q, quads - done) : 0;
                        if (taken == 0) {
                            gif_.Qword(q);
                            taken = 1;
                        }
                        done += taken;
                    }
                    k += (int)(quads * 4);
                    left_ -= quads * 4;
                    if (left_ == 0)
                        End();
                    continue;
                }
            }
            if (left_ != 0 && keep_ && !direct_) {
                if (data_.empty() && (cmd_ & 0x60) == 0x60 && (std::uint32_t)(n - k) >= left_) {
                    // an unpack whose data is all here: unpacked from where it is, not copied first
                    src_ = w + k;
                    srcWords_ = left_;
                    k += (int)left_;
                    left_ = 0;
                    End();
                    src_ = nullptr;
                    continue;
                }
                int take = (int)std::min<std::uint32_t>(left_, (std::uint32_t)(n - k));
                data_.insert(data_.end(), w + k, w + k + take);
                k += take;
                left_ -= (std::uint32_t)take;
                if (left_ == 0)
                    End();
            } else {
                Word(w[k++]);
            }
        }
    }

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
                if (!vu0_ && GsAsync())
                    directData_.insert(directData_.end(), qword_, qword_ + 4);
                else
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
        for (std::uint32_t c = 0; c < 128; c++) {
            if (!seen_[c])
                continue;
            std::snprintf(buf, sizeof buf, " %02X:%u", c, seen_[c]);
            s += buf;
            seen_[c] = 0;
        }
        return s;
    }

private:
    void Code(std::uint32_t w)
    {
        std::uint32_t cmd = (w >> 24) & 0x7F, imm = w & 0xFFFF, num = (w >> 16) & 0xFF;
        seen_[(cmd & 0x60) == 0x60 ? 0x60 : cmd]++;   // an array: a map here cost a tree lookup a code
        irq_ = (w >> 31) != 0;
        direct_ = false;
        cmd_ = cmd;
        imm_ = imm;
        num_ = num;
        data_.clear();
        // the VIFcodes whose data the VIF keeps: microcode, unpacks, and its mask, row and column
        keep_ = cmd == 0x4A || (cmd & 0x60) == 0x60 || cmd == 0x20 || cmd == 0x30 || cmd == 0x31;
        if (!vu0_ && g_survey.On()) {
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
            case 0x00: case 0x06: case 0x07: case 0x10: case 0x11: case 0x13:
                left_ = 0;   // NOP, MSKPATH3, MARK, the flushes (the port's transfers are done when they end)
                break;
            case 0x02:   // OFFSET: the double buffer's second half, and back to the first
                ofst_ = imm & 0x3FF;
                dbf_ = false;
                tops_ = base_;
                left_ = 0;
                break;
            case 0x03: base_ = imm & 0x3FF; left_ = 0; break;    // BASE
            case 0x04: itops_ = imm & 0x3FF; left_ = 0; break;   // ITOP
            case 0x05: mode_ = imm & 3; left_ = 0; break;        // STMOD
            case 0x14: case 0x15: case 0x17:   // MSCAL, MSCALF, MSCNT
                left_ = 0;
                if (vu0_) {
                    vu0::Run(cmd == 0x17 ? vu0::Vu0().pc * 8 : (imm & 0x1FF) * 8);
                } else {
                    // the double buffer swaps: the program gets TOPS as TOP, ITOPS as ITOP
                    vu1::Context c;
                    c.top = tops_;
                    c.itop = itops_;
                    c.kick = KickPath1;
                    dbf_ = !dbf_;
                    tops_ = (base_ + (dbf_ ? ofst_ : 0)) & 0x3FF;
                    // VU1's time (SLY1_VU1_TIMES, or with SLY1_VU1_PROFILE): two reads of the clock a
                    // program, which in an emulator may each leave the guest
                    static const bool timed = std::getenv("SLY1_VU1_TIMES") || std::getenv("SLY1_VU1_PROFILE");
                    std::uint64_t t0 = timed ? NowNs() : 0;
                    std::uint64_t n = vu1::Run(cmd == 0x17 ? vu1::Vu1().pc : (imm & 0x7FF), c);
                    g_counts.vu1Instructions += n;
                    if (timed)
                        g_counts.vu1Nanoseconds += NowNs() - t0;
                }
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
        if (direct_ && !directData_.empty()) {   // a DIRECT's data, to the GS thread
            GsItem item;
            item.path = 2;
            item.words = std::move(directData_);
            directData_ = GsSpare();
            GsEnqueue(std::move(item));
        }
        direct_ = false;
        if (keep_) {
            keep_ = false;
            if (cmd_ == 0x4A) {
                if (!vu0_ && g_survey.On())
                    SurveyMicrocode();
                LoadMicrocode();
            } else if (cmd_ == 0x20) {
                mask_ = data_.empty() ? 0 : data_[0];
            } else if (cmd_ == 0x30 || cmd_ == 0x31) {
                for (std::size_t k = 0; k < 4 && k < data_.size(); k++)
                    (cmd_ == 0x30 ? row_ : col_)[k] = data_[k];
            } else {
                Unpack();
            }
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

    // MPG: the instructions to the VU's micro memory at imm (in 64-bit instructions).
    void LoadMicrocode()
    {
        std::uint32_t* micro = vu0_ ? vu0::Vu0().micro : vu1::Micro();
        std::uint32_t words = vu0_ ? 1024 : 4096;
        for (std::size_t k = 0; k < data_.size(); k++)
            micro[(imm_ * 2 + k) & (words - 1)] = data_[k];
        if (!vu0_)
            vu1::MicrocodeLoaded(imm_ & 0x7FF, (std::uint32_t)data_.size() / 2);
    }

    // UNPACK to the VU's data memory: num vectors of vn+1 fields of 32 >> vl bits (V4-5: 16-bit
    // colours) from address imm (plus TOPS with the FLG bit, on VIF1), fields zero- or
    // sign-extended (usn), a scalar to all four fields. STCYCL's cl and wl: with wl <= cl, wl of
    // every cl quadwords are written; with wl > cl, wl in a row, cl of them from the data and the
    // rest from the mask's row or column. The mask (STMASK, with the m bit) picks per field and
    // cycle the data, the row, the column or nothing; the mode (STMOD) adds the row to the data
    // (offset) or accumulates the data into the row (difference).
    // Unpack's plain case: each vector's fields (8, 16 or 32 bits, sign-extended unless usn) into
    // its quadword; a single field fills all four, a field the format lacks keeps what was there.
    template <std::uint32_t Fields, std::uint32_t Bits>
    void UnpackFields(std::uint8_t* mem, std::uint32_t qmask, std::uint32_t addr, std::uint32_t n, bool usn,
                      const std::uint8_t* bytes)
    {
        const std::uint32_t wl = wl_, cl = cl_;
        std::uint32_t cyc = 0, base = addr;
        for (std::uint32_t i = 0; i < n; i++) {
            std::uint32_t* out = reinterpret_cast<std::uint32_t*>(mem + ((base + cyc) & qmask) * 16);
            std::uint32_t v[4];
            for (std::uint32_t f = 0; f < Fields; f++) {
                const std::uint8_t* b = bytes + (i * Fields + f) * (Bits / 8);
                if (Bits == 32) {
                    std::memcpy(&v[f], b, 4);
                } else if (Bits == 16) {
                    std::uint16_t h;
                    std::memcpy(&h, b, 2);
                    v[f] = usn ? h : (std::uint32_t)(std::int32_t)(std::int16_t)h;
                } else {
                    v[f] = usn ? b[0] : (std::uint32_t)(std::int32_t)(std::int8_t)b[0];
                }
            }
            if (Fields == 1) {
                out[0] = out[1] = out[2] = out[3] = v[0];
            } else {
                for (std::uint32_t f = 0; f < Fields; f++)
                    out[f] = v[f];
            }
            if (++cyc == wl) {
                cyc = 0;
                base += cl;
            }
        }
    }

    // The same unpack a quadword at a time, field type and sign fixed: a vertex's fields widened
    // four at a time, the destination walked straight when no cycle skips and the memory does not
    // wrap (most unpacks).
    template <std::uint32_t Fields, std::uint32_t Bits, bool Usn>
    static void UnpackFast(std::uint8_t* mem, std::uint32_t qmask, std::uint32_t addr, std::uint32_t n, std::uint32_t wl,
                           std::uint32_t cl, const std::uint8_t* bytes)
    {
        typedef std::uint32_t W4 __attribute__((vector_size(16)));
        typedef std::int32_t I4v __attribute__((vector_size(16)));
        typedef std::uint8_t B4 __attribute__((vector_size(4)));
        typedef std::int8_t S4 __attribute__((vector_size(4)));
        typedef std::uint16_t H4 __attribute__((vector_size(8)));
        typedef std::int16_t T4 __attribute__((vector_size(8)));
        constexpr std::uint32_t kIn = Fields * (Bits / 8);
        auto one = [&](std::uint32_t* out, const std::uint8_t* in) {
            if (Fields == 4 && Bits == 32) {
                std::memcpy(out, in, 16);
            } else if (Fields == 4 && Bits == 8) {
                W4 v;
                if (Usn) {
                    B4 b;
                    std::memcpy(&b, in, 4);
                    v = __builtin_convertvector(b, W4);
                } else {
                    S4 b;
                    std::memcpy(&b, in, 4);
                    v = (W4)__builtin_convertvector(b, I4v);
                }
                std::memcpy(out, &v, 16);
            } else if (Fields == 4 && Bits == 16) {
                W4 v;
                if (Usn) {
                    H4 h;
                    std::memcpy(&h, in, 8);
                    v = __builtin_convertvector(h, W4);
                } else {
                    T4 h;
                    std::memcpy(&h, in, 8);
                    v = (W4)__builtin_convertvector(h, I4v);
                }
                std::memcpy(out, &v, 16);
            } else if (Bits == 32) {
                if (Fields == 1) {
                    std::uint32_t x;
                    std::memcpy(&x, in, 4);
                    out[0] = out[1] = out[2] = out[3] = x;
                } else {
                    std::memcpy(out, in, Fields * 4);
                }
            } else {
                std::uint32_t v[4];
                for (std::uint32_t f = 0; f < Fields; f++) {
                    const std::uint8_t* b = in + f * (Bits / 8);
                    if (Bits == 16) {
                        std::uint16_t h;
                        std::memcpy(&h, b, 2);
                        v[f] = Usn ? h : (std::uint32_t)(std::int32_t)(std::int16_t)h;
                    } else {
                        v[f] = Usn ? b[0] : (std::uint32_t)(std::int32_t)(std::int8_t)b[0];
                    }
                }
                if (Fields == 1)
                    out[0] = out[1] = out[2] = out[3] = v[0];
                else
                    for (std::uint32_t f = 0; f < Fields; f++)
                        out[f] = v[f];
            }
        };
        if ((wl >= cl || wl == 0) && (addr & qmask) + n <= qmask + 1) {
            std::uint8_t* out = mem + (addr & qmask) * 16;
            if (Fields == 4 && Bits == 32) {   // the quadwords as they are
                std::memcpy(out, bytes, (std::size_t)n * 16);
                return;
            }
            std::uint32_t i = 0;
            if (Fields == 4 && Bits == 8) {   // four quadwords from each sixteen bytes
                typedef std::uint8_t B16 __attribute__((vector_size(16)));
                typedef std::int8_t S16 __attribute__((vector_size(16)));
                for (; i + 4 <= n; i += 4, out += 64, bytes += 16) {
                    if (Usn) {
                        B16 b;
                        std::memcpy(&b, bytes, 16);
                        W4 v0 = __builtin_convertvector(__builtin_shufflevector(b, b, 0, 1, 2, 3), W4);
                        W4 v1 = __builtin_convertvector(__builtin_shufflevector(b, b, 4, 5, 6, 7), W4);
                        W4 v2 = __builtin_convertvector(__builtin_shufflevector(b, b, 8, 9, 10, 11), W4);
                        W4 v3 = __builtin_convertvector(__builtin_shufflevector(b, b, 12, 13, 14, 15), W4);
                        std::memcpy(out, &v0, 16);
                        std::memcpy(out + 16, &v1, 16);
                        std::memcpy(out + 32, &v2, 16);
                        std::memcpy(out + 48, &v3, 16);
                    } else {
                        S16 b;
                        std::memcpy(&b, bytes, 16);
                        I4v v0 = __builtin_convertvector(__builtin_shufflevector(b, b, 0, 1, 2, 3), I4v);
                        I4v v1 = __builtin_convertvector(__builtin_shufflevector(b, b, 4, 5, 6, 7), I4v);
                        I4v v2 = __builtin_convertvector(__builtin_shufflevector(b, b, 8, 9, 10, 11), I4v);
                        I4v v3 = __builtin_convertvector(__builtin_shufflevector(b, b, 12, 13, 14, 15), I4v);
                        std::memcpy(out, &v0, 16);
                        std::memcpy(out + 16, &v1, 16);
                        std::memcpy(out + 32, &v2, 16);
                        std::memcpy(out + 48, &v3, 16);
                    }
                }
            }
            for (; i < n; i++, out += 16, bytes += kIn)
                one(reinterpret_cast<std::uint32_t*>(out), bytes);
            return;
        }
        // by cycles: each block of wl quadwords written where it goes (a block wraps rarely)
        std::uint32_t base = addr, i = 0;
        while (i < n) {
            std::uint32_t block = std::min(wl ? wl : n, n - i);
            std::uint32_t at = base & qmask;
            if (at + block <= qmask + 1) {
                std::uint8_t* out = mem + at * 16;
                for (std::uint32_t c = 0; c < block; c++, out += 16, bytes += kIn)
                    one(reinterpret_cast<std::uint32_t*>(out), bytes);
            } else {
                for (std::uint32_t c = 0; c < block; c++, bytes += kIn)
                    one(reinterpret_cast<std::uint32_t*>(mem + ((base + c) & qmask) * 16), bytes);
            }
            i += block;
            base += cl;
        }
    }

    template <std::uint32_t Fields, std::uint32_t Bits>
    void UnpackSign(std::uint8_t* mem, std::uint32_t qmask, std::uint32_t addr, std::uint32_t n, bool usn, const std::uint8_t* bytes)
    {
        if (usn || Bits == 32)
            UnpackFast<Fields, Bits, true>(mem, qmask, addr, n, wl_, cl_, bytes);
        else
            UnpackFast<Fields, Bits, false>(mem, qmask, addr, n, wl_, cl_, bytes);
    }

    void UnpackPlain(std::uint8_t* mem, std::uint32_t qmask, std::uint32_t addr, std::uint32_t n, std::uint32_t fields,
                     std::uint32_t bits, bool usn, const std::uint8_t* bytes)
    {
        static const bool check = std::getenv("SLY1_UNPACK_CHECK") != nullptr;
        static thread_local std::vector<std::uint8_t> before, fast;
        std::size_t size = (std::size_t)(qmask + 1) * 16;
        if (check) {
            before.assign(mem, mem + size);
        }
        switch (fields * 100 + bits) {
        case 132: UnpackSign<1, 32>(mem, qmask, addr, n, usn, bytes); break;
        case 116: UnpackSign<1, 16>(mem, qmask, addr, n, usn, bytes); break;
        case 108: UnpackSign<1, 8>(mem, qmask, addr, n, usn, bytes); break;
        case 232: UnpackSign<2, 32>(mem, qmask, addr, n, usn, bytes); break;
        case 216: UnpackSign<2, 16>(mem, qmask, addr, n, usn, bytes); break;
        case 208: UnpackSign<2, 8>(mem, qmask, addr, n, usn, bytes); break;
        case 332: UnpackSign<3, 32>(mem, qmask, addr, n, usn, bytes); break;
        case 316: UnpackSign<3, 16>(mem, qmask, addr, n, usn, bytes); break;
        case 308: UnpackSign<3, 8>(mem, qmask, addr, n, usn, bytes); break;
        case 432: UnpackSign<4, 32>(mem, qmask, addr, n, usn, bytes); break;
        case 416: UnpackSign<4, 16>(mem, qmask, addr, n, usn, bytes); break;
        default: UnpackSign<4, 8>(mem, qmask, addr, n, usn, bytes); break;
        }
        if (check) {   // SLY1_UNPACK_CHECK: the same unpack the field-by-field way, on the memory as it was
            fast.assign(mem, mem + size);
            std::memcpy(mem, before.data(), size);
            switch (fields * 100 + bits) {
            case 132: UnpackFields<1, 32>(mem, qmask, addr, n, usn, bytes); break;
            case 116: UnpackFields<1, 16>(mem, qmask, addr, n, usn, bytes); break;
            case 108: UnpackFields<1, 8>(mem, qmask, addr, n, usn, bytes); break;
            case 232: UnpackFields<2, 32>(mem, qmask, addr, n, usn, bytes); break;
            case 216: UnpackFields<2, 16>(mem, qmask, addr, n, usn, bytes); break;
            case 208: UnpackFields<2, 8>(mem, qmask, addr, n, usn, bytes); break;
            case 332: UnpackFields<3, 32>(mem, qmask, addr, n, usn, bytes); break;
            case 316: UnpackFields<3, 16>(mem, qmask, addr, n, usn, bytes); break;
            case 308: UnpackFields<3, 8>(mem, qmask, addr, n, usn, bytes); break;
            case 432: UnpackFields<4, 32>(mem, qmask, addr, n, usn, bytes); break;
            case 416: UnpackFields<4, 16>(mem, qmask, addr, n, usn, bytes); break;
            default: UnpackFields<4, 8>(mem, qmask, addr, n, usn, bytes); break;
            }
            static std::atomic<std::uint64_t> unpacks, differed;
            std::uint64_t u = ++unpacks;
            if (std::memcmp(mem, fast.data(), size) != 0 && differed++ < 8)
                std::fprintf(stderr, "unpack check: V%u-%u (%u at %u, cycle %u/%u, usn %d) differs\n", fields, bits, n, addr, cl_, wl_, (int)usn);
            if ((u & (u - 1)) == 0)
                std::fprintf(stderr, "unpack check: %llu unpacks, %llu differed\n", (unsigned long long)u, (unsigned long long)differed.load());
        }
    }

    void Unpack()
    {
        std::uint8_t* mem = vu0_ ? vu0::Vu0().data : vu1::Data();
        std::uint32_t qmask = vu0_ ? 0xFF : 0x3FF;
        std::uint32_t vn = (cmd_ >> 2) & 3, vl = cmd_ & 3, n = num_ ? num_ : 256;
        bool masked = (cmd_ & 0x10) != 0, usn = (imm_ >> 14) & 1, v45 = vn == 3 && vl == 3;
        std::uint32_t bits = v45 ? 16 : 32u >> vl, fields = v45 ? 1 : vn + 1;
        std::uint32_t addr = imm_ & 0x3FF;
        if (!vu0_ && ((imm_ >> 15) & 1))
            addr += tops_;
        if (!vu0_)   // the quadwords written (a cycle longer than its writes skips some)
            vu1::DataWritten(addr & 0x3FF, n, cl_, wl_);
        // every format's fields are 8, 16 or 32 bits (V4-5 a 16-bit colour): read them as such
        const std::uint8_t* bytes = reinterpret_cast<const std::uint8_t*>(src_ ? src_ : data_.data());
        std::size_t have = (src_ ? srcWords_ : data_.size()) * 4, at = 0;
        auto take = [&](std::uint32_t nb) -> std::uint32_t {
            std::uint32_t v = 0;
            std::size_t n = nb / 8;
            if (at + n <= have) {
                if (n == 4)
                    std::memcpy(&v, bytes + at, 4);
                else if (n == 2) {
                    std::uint16_t h;
                    std::memcpy(&h, bytes + at, 2);
                    v = h;
                } else
                    v = bytes[at];
            }
            at += n;
            return v;
        };
        bool filling = wl_ > cl_;
        // the common case (no mask, no addition, no filling, not V4-5): the fields copied straight
        {   // SLY1_UNPACK_SURVEY: the quadwords unpacked by format (diagnosis)
            static const bool survey = std::getenv("SLY1_UNPACK_SURVEY") != nullptr;
            if (survey) {
                static std::map<std::uint32_t, std::uint64_t> counts;
                static std::uint64_t all;
                std::uint32_t key = fields << 16 | bits << 8 | (masked ? 1 : 0) | (mode_ ? 2 : 0) | (filling ? 4 : 0) |
                                    (wl_ != cl_ ? 8 : 0) | (v45 ? 16 : 0);
                counts[key] += n;
                if (((all += n) & 0xFFFFF) < n) {
                    std::fprintf(stderr, "unpack survey: %llu quadwords;", (unsigned long long)all);
                    for (auto& kv : counts)
                        std::fprintf(stderr, " V%u-%u%s%s%s%s%s %llu", kv.first >> 16, (kv.first >> 8) & 0xFF, kv.first & 1 ? " masked" : "",
                                     kv.first & 2 ? " mode" : "", kv.first & 4 ? " fill" : "", kv.first & 8 ? " cycle" : "",
                                     kv.first & 16 ? " (5551)" : "", (unsigned long long)kv.second);
                    std::fprintf(stderr, "\n");
                }
            }
        }
        if (!masked && mode_ == 0 && !filling && !v45 && have >= (std::size_t)n * fields * (bits / 8)) {
            UnpackPlain(mem, qmask, addr, n, fields, bits, usn, bytes);
            return;
        }
        for (std::uint32_t i = 0; i < n; i++) {
            std::uint32_t cyc = i % wl_;
            std::uint32_t q = (filling ? addr + i : addr + (i / wl_) * cl_ + cyc) & qmask;
            std::uint32_t* out = reinterpret_cast<std::uint32_t*>(mem + q * 16);
            bool fromData = !filling || cyc < cl_;
            std::uint32_t v[4] = {0, 0, 0, 0};
            bool have[4] = {false, false, false, false};
            if (fromData) {
                std::uint32_t raw[4];
                for (std::uint32_t f = 0; f < fields; f++) {
                    std::uint32_t x = take(bits);
                    if (!usn && bits < 32 && (x >> (bits - 1)) & 1)
                        x |= ~0u << bits;
                    raw[f] = x;
                }
                if (v45) {
                    std::uint32_t c = raw[0];
                    v[0] = (c & 0x1F) << 3;
                    v[1] = ((c >> 5) & 0x1F) << 3;
                    v[2] = ((c >> 10) & 0x1F) << 3;
                    v[3] = ((c >> 15) & 1) << 7;
                    have[0] = have[1] = have[2] = have[3] = true;
                } else if (fields == 1) {
                    v[0] = v[1] = v[2] = v[3] = raw[0];
                    have[0] = have[1] = have[2] = have[3] = true;
                } else {
                    for (std::uint32_t f = 0; f < fields; f++) {
                        v[f] = raw[f];
                        have[f] = true;
                    }
                }
            }
            std::uint32_t c4 = cyc < 3 ? cyc : 3;
            for (std::uint32_t f = 0; f < 4; f++) {
                std::uint32_t m = masked ? (mask_ >> ((c4 * 4 + f) * 2)) & 3 : (fromData ? 0 : 1);
                switch (m) {
                case 0:
                    if (!have[f])
                        break;   // a field the format does not carry keeps what was there
                    if (mode_ == 1)
                        out[f] = v[f] + row_[f];
                    else if (mode_ == 2)
                        out[f] = row_[f] = row_[f] + v[f];
                    else
                        out[f] = v[f];
                    break;
                case 1: out[f] = row_[f]; break;
                case 2: out[f] = col_[c4]; break;
                default: break;   // write-protected
                }
            }
        }
    }

    int cause_;
    bool vu0_ = false, keep_ = false;
    // VIF1's registers (VIF0 keeps the unpacking ones too)
    std::uint32_t row_[4] = {}, col_[4] = {}, mask_ = 0, mode_ = 0;
    std::uint32_t base_ = 0, ofst_ = 0, tops_ = 0, itops_ = 0;
    bool dbf_ = false;
    std::uint32_t cmd_ = 0, imm_ = 0, num_ = 0;
    std::vector<std::uint32_t> data_;
    const std::uint32_t* src_ = nullptr;   // an unpack's data where it came, in place of data_ (Words)
    std::size_t srcWords_ = 0;
    std::uint32_t left_ = 0, cl_ = 1, wl_ = 1, filled_ = 0;
    std::uint32_t qword_[4] = {};
    bool irq_ = false, direct_ = false;
    std::vector<std::uint32_t> directData_;   // a DIRECT's quadwords for the GS thread
    Gif gif_{2};
    unsigned seen_[128] = {};   // the VIFcodes seen since the trace's last call, by command
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

// --- The render thread. VIF1 and the GIF (and so VU1 and the GS) run on a thread of their own,
// on another core, as the PS2's run beside the EE: a VIF1 or GIF chain is walked on the game's
// thread and its data copied into an item of the render queue (so the game may reuse its
// buffers at once, as after a finished transfer), and the render thread passes the items on in
// order, raising the VIF's and the GS's interrupts as it meets them. The display's registers and
// the buffer swap go through the queue too. SLY1_RENDER_SYNC runs everything on the game's
// thread, as before.
struct RenderItem {
    enum Kind : std::uint8_t { Vif1, Gif, Call } kind = Vif1;
    std::vector<std::uint32_t> words;
    void (*fn)(std::uint64_t, std::uint64_t) = nullptr;
    std::uint64_t a = 0, b = 0;
};
std::mutex g_renderLock;
std::condition_variable g_renderWake, g_renderIdle;
std::deque<RenderItem> g_renderQueue;
std::vector<std::vector<std::uint32_t>> g_renderSpare;   // emptied items' buffers, for reuse
bool g_renderBusy, g_renderWaiting;
std::atomic<bool> g_renderStarted;
std::atomic<std::uint64_t> g_renderQueuedWords, g_renderMaxQueued;

bool RenderAsync()
{
    static const bool sync = std::getenv("SLY1_RENDER_SYNC") != nullptr;
    return !sync && g_renderStarted.load(std::memory_order_acquire);
}

std::vector<std::uint32_t> SpareBuffer()
{
    std::lock_guard<std::mutex> lk(g_renderLock);
    if (g_renderSpare.empty())
        return {};
    std::vector<std::uint32_t> v = std::move(g_renderSpare.back());
    g_renderSpare.pop_back();
    v.clear();
    return v;
}

void Enqueue(RenderItem&& item)
{
    std::lock_guard<std::mutex> lk(g_renderLock);
    std::uint64_t q = g_renderQueuedWords += item.words.size();
    if (q > g_renderMaxQueued)
        g_renderMaxQueued = q;
    g_renderQueue.push_back(std::move(item));
    if (g_renderWaiting)
        g_renderWake.notify_one();
}

// The sinks: straight into VIF and GIF, or into an item.
struct DirectSink {
    Vif* vif;
    Gif* gif;
    std::optional<gs::Hold> hold;   // the chain holds the GS throughout (VIF1's DIRECT reaches it too)
    // VIF1 (its DIRECT) and the GIF reach the GS; VIF0 does not, and must not wait for it (the
    // render thread can hold the GS while it waits for an interrupt to be serviced)
    DirectSink(Vif* v, Gif* g) : vif(v), gif(g)
    {
        if (g || v == &g_vif1)
            hold.emplace();
    }
    void OnVif(const std::uint32_t* w, int n) { vif->Words(w, n); }
    void OnGif(const std::uint32_t* q) { gif->Qword(q); }
    bool Direct() const { return true; }
};
struct CaptureSink {
    std::vector<std::uint32_t>& out;
    void OnVif(const std::uint32_t* w, int n) { out.insert(out.end(), w, w + n); }
    void OnGif(const std::uint32_t* q) { out.insert(out.end(), q, q + 4); }
    bool Direct() const { return false; }
};

// A chain walked as the DMAC walks it, its data handed to sink: OnVif(words, n) for VIF0 and VIF1
// (the tags' upper halves with TTE, then each quadword), OnGif(quadword) for the GIF channel.
template <class Sink>
void WalkChain(std::uint32_t regs, std::uint32_t tadr, Sink& sink)
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
        if (vif && tte)
            sink.OnVif(tag + 2, 2);
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
                sink.OnVif(q, 4);
            } else {
                sink.OnGif(q);
            }
        }
        if (end || (irq && tie))
            break;
        tadr = next;
    }
    if (trace)
        std::fprintf(stderr, "dmac: chain on 0x%08X from 0x%08X%s: %u tags, %u FINISH, VIF %s\n", regs, start,
                     tte ? " (TTE)" : "", g_counts.tags - tagsBefore, g_counts.finishes - finishesBefore,
                     vif && sink.Direct() ? vif->State().c_str() : "-");
    r[0x30 / 4] = tadr;
    if (toSpr)
        r[0x80 / 4] = sadr;
    r[0x20 / 4] = 0;
    r[0] &= ~0x100u;   // STR: the transfer is over
}

void RunDmaChain(std::uint32_t regs, std::uint32_t tadr)
{
    std::uint64_t t0 = NowNs();
    if ((regs == kVif1 || regs == kGif) && RenderAsync()) {
        RenderItem item;
        item.kind = regs == kVif1 ? RenderItem::Vif1 : RenderItem::Gif;
        item.words = SpareBuffer();
        CaptureSink sink{item.words};
        WalkChain(regs, tadr, sink);
        Enqueue(std::move(item));
    } else {
        DirectSink sink(regs == kVif0 ? &g_vif0 : regs == kVif1 ? &g_vif1 : nullptr, regs == kGif ? &g_gif : nullptr);
        WalkChain(regs, tadr, sink);
    }
    g_counts.dmaNanoseconds += NowNs() - t0;
}

void SendGifNormal(std::uint32_t madr, std::uint32_t qwc)
{
    bool spr = (madr & 0x80000000u) != 0;
    if (RenderAsync()) {
        RenderItem item;
        item.kind = RenderItem::Gif;
        item.words = SpareBuffer();
        for (std::uint32_t i = 0; i < qwc; i++) {
            const std::uint32_t* q = At((madr & 0x7FFFFFFFu) + i * 16, spr);
            item.words.insert(item.words.end(), q, q + 4);
        }
        Enqueue(std::move(item));
        return;
    }
    gs::Hold hold;
    for (std::uint32_t i = 0; i < qwc; i++)
        g_gif.Qword(At((madr & 0x7FFFFFFFu) + i * 16, spr));
}

void RenderCall(void (*fn)(std::uint64_t, std::uint64_t), std::uint64_t a, std::uint64_t b)
{
    if (!RenderAsync()) {
        fn(a, b);
        return;
    }
    RenderItem item;
    item.kind = RenderItem::Call;
    item.fn = fn;
    item.a = a;
    item.b = b;
    Enqueue(std::move(item));
}

void RenderFlush()
{
    {
        std::unique_lock<std::mutex> lk(g_renderLock);
        g_renderIdle.wait_for(lk, std::chrono::seconds(5), [] { return g_renderQueue.empty() && !g_renderBusy; });
    }
    std::unique_lock<std::mutex> lk(g_gsLock);
    g_gsIdle.wait_for(lk, std::chrono::seconds(5), [] { return g_gsQueue.empty() && !g_gsBusy; });
}

void GsThreadMain()
{
    g_gsStarted.store(true, std::memory_order_release);
    for (;;) {
        GsItem item;
        {
            std::unique_lock<std::mutex> lk(g_gsLock);
            g_gsBusy = false;
            if (g_gsQueue.empty()) {
                g_gsIdle.notify_all();
                g_gsWaiting = true;
                g_gsWake.wait(lk, [] { return !g_gsQueue.empty(); });
                g_gsWaiting = false;
            }
            item = std::move(g_gsQueue.front());
            g_gsQueue.pop_front();
            g_gsBusy = true;
        }
        if (item.path == 0) {
            item.fn(item.a, item.b);
        } else {
            Gif& gif = item.path == 1 ? g_path1 : item.path == 2 ? g_path2 : g_gif;
            gs::Hold hold;
            for (std::size_t k = 0; k + 4 <= item.words.size(); k += 4)
                gif.Qword(&item.words[k]);
        }
        if (item.words.capacity()) {
            std::lock_guard<std::mutex> lk(g_gsLock);
            if (g_gsSpare.size() < 256)
                g_gsSpare.push_back(std::move(item.words));
        }
    }
}

// The render thread's time in each kind of item (VIF1 data, GIF data, calls), for the report.
std::atomic<std::uint64_t> g_renderNs[3], g_renderItems[3];

void RenderThreadMain()
{
    g_renderStarted.store(true, std::memory_order_release);
    for (;;) {
        RenderItem item;
        {
            std::unique_lock<std::mutex> lk(g_renderLock);
            g_renderBusy = false;
            if (g_renderQueue.empty()) {
                g_renderIdle.notify_all();
                g_renderWaiting = true;
                g_renderWake.wait(lk, [] { return !g_renderQueue.empty(); });
                g_renderWaiting = false;
            }
            item = std::move(g_renderQueue.front());
            g_renderQueue.pop_front();
            g_renderQueuedWords -= item.words.size();
            g_renderBusy = true;
        }
        const bool gsThread = GsAsync();
        const std::uint64_t started = NowNs();
        const int kind = (int)item.kind;
        switch (item.kind) {
        case RenderItem::Vif1: {
            std::optional<gs::Hold> hold;
            if (!gsThread)
                hold.emplace();
            g_vif1.Words(item.words.data(), (int)item.words.size());
            break;
        }
        case RenderItem::Gif:
            if (gsThread) {
                GsItem g;
                g.path = 3;
                g.words = std::move(item.words);
                GsEnqueue(std::move(g));
            } else {
                gs::Hold hold;
                for (std::size_t k = 0; k + 4 <= item.words.size();) {
                    std::uint32_t taken = g_gif.Left() ? g_gif.Run(&item.words[k], (std::uint32_t)((item.words.size() - k) / 4)) : 0;
                    if (taken == 0) {
                        g_gif.Qword(&item.words[k]);
                        taken = 1;
                    }
                    k += (std::size_t)taken * 4;
                }
            }
            break;
        case RenderItem::Call:
            if (gsThread) {
                GsItem g;
                g.fn = item.fn;
                g.a = item.a;
                g.b = item.b;
                GsEnqueue(std::move(g));
            } else {
                item.fn(item.a, item.b);
            }
            break;
        }
        GsPassOn();
        g_renderNs[kind < 3 ? kind : 0] += NowNs() - started;
        g_renderItems[kind < 3 ? kind : 0]++;
        if (item.words.capacity()) {
            std::lock_guard<std::mutex> lk(g_renderLock);
            if (g_renderSpare.size() < 64)
                g_renderSpare.push_back(std::move(item.words));
        }
    }
}


void DmacDump()
{
    std::fprintf(stderr, "dmac: %u chains (%u out of the scratchpad), %u tags, %u VIF interrupts, %u GS FINISH, "
                 "%u VIFcodes not understood\n", g_counts.chains.load(), g_counts.sprChains.load(),
                 g_counts.tags.load(), g_counts.vifInterrupts.load(), g_counts.finishes.load(),
                 g_counts.unknownVifcodes.load());
    std::fprintf(stderr, "dmac: VU1 ran %llu instructions in %.3f s (%.1f million a second, %llu translated); %llu packets on path 1\n",
                 (unsigned long long)g_counts.vu1Instructions.load(), g_counts.vu1Nanoseconds.load() / 1e9,
                 g_counts.vu1Nanoseconds.load() ? g_counts.vu1Instructions.load() * 1e3 / g_counts.vu1Nanoseconds.load() : 0.0,
                 (unsigned long long)vu1::TranslatedInstructions(), (unsigned long long)g_counts.path1Packets.load());
    std::fprintf(stderr, "dmac: chains took %.2f s on the game's thread%s; the render queue held at most %.1f MB\n",
                 g_counts.dmaNanoseconds.load() / 1e9, g_renderStarted ? " (VIF1 and GIF chains copied for the render thread)" : "",
                 g_renderMaxQueued.load() * 4 / 1e6);
    if (g_renderStarted)
        std::fprintf(stderr, "dmac: the render thread took %.2f s for VIF1's data (VU1 and its kicks among it), %.2f s for the "
                     "GIF's, %.2f s for calls (the swaps); %llu, %llu and %llu items\n", g_renderNs[0].load() / 1e9, g_renderNs[1].load() / 1e9,
                     g_renderNs[2].load() / 1e9, (unsigned long long)g_renderItems[0].load(), (unsigned long long)g_renderItems[1].load(),
                     (unsigned long long)g_renderItems[2].load());
    vu1::ReportLive();
    std::uint64_t failures = 0, checked = vu1::HandlerChecks(&failures);
    if (checked)
        std::fprintf(stderr, "dmac: VU1 handlers checked %llu times, %llu differed\n", (unsigned long long)checked,
                     (unsigned long long)failures);
    if (g_survey.On()) {
        std::lock_guard<std::mutex> lk(g_survey.lock);
        for (auto& kv : g_survey.counts)
            std::fprintf(stderr, "survey: %10lu  %s\n", kv.second, kv.first.c_str());
        for (auto& kv : g_survey.programSize)
            std::fprintf(stderr, "survey: program %08X: %u instructions\n", kv.first, kv.second);
    }
}

}  // namespace port
