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

#if defined(__SWITCH__)
#include <switch.h>
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <optional>
#include <condition_variable>
#include "port/thread.h"
#include <thread>
#include <deque>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>
#include <set>
#include <cstdio>
#include <cstring>

#include "port/ptr32.h"
#include "port/system.h"
#include "port/vu0_micro.h"
#include "port/vu1.h"
#include "port/gs.h"
#include "port/gs_backend.h"

namespace port {

// SLY1_TIMELINE=N: from the Nth frame shown, 400 events (or SLY1_TIMELINE_EVENTS) with their times in milliseconds: what the
// game's thread queues, what the render thread starts and ends, FINISH, and what else calls
// TimelineEvent (the vertical blank, the buffer swap).
static bool Timeline()
{
    static const long from = std::getenv("SLY1_TIMELINE") ? std::atol(std::getenv("SLY1_TIMELINE")) : -1;
    static std::atomic<int> left{std::getenv("SLY1_TIMELINE_EVENTS") ? std::atoi(std::getenv("SLY1_TIMELINE_EVENTS")) : 400};
    return from >= 0 && (long)gs::FramesShown() >= from && left.fetch_sub(1) > 0;
}
#define TIMELINE(...) do { if (Timeline()) { char line[160]; int n = std::snprintf(line, sizeof line, "timeline: %9.3f ", (NowNs() % 100000000000ull) / 1e6); \
    n += std::snprintf(line + n, sizeof line - (std::size_t)n, __VA_ARGS__); std::fprintf(stderr, "%s\n", line); } } while (0)

void TimelineEvent(const char* what) { TIMELINE("%s", what); }

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

// (SLY1_FINISH_LAG=0: off.) The game is told a list is finished
// when the render thread has done with it, not when the GS thread has drawn it. The game's own
// render thread waits for FINISH after each of a frame's three packets before it waits for the
// vertical blank, so a frame cost the handing over, the render thread's work, what the GS thread
// had left and the two packets after, one after the other: with each of the three threads at
// 12 ms a frame took two blanks. Now the GS thread may run a frame behind (no further: the render
// thread does not start a list while the GS thread still owes more than one frame's FINISHes),
// and a frame fits a blank when each thread's own work does.
// g_finishAhead: FINISH writes the game has been told of that the GS thread has yet to reach.
std::atomic<int> g_finishAhead{0};
std::atomic<std::uint64_t> g_finishesEarly{0}, g_finishesLate{0};
// (the GS thread's) the item being run ends with a FINISH the game has been told of already
bool g_itemFinishTold;
bool FinishLag()
{
    static const bool on = !std::getenv("SLY1_FINISH_LAG") || std::getenv("SLY1_FINISH_LAG")[0] != '0';
    return on;
}

// A GIF packet stream, a quadword at a time.
class Gif {
public:
    explicit Gif(int path) : path_(path) {}

    // Quadwords still owed to the current tag's data (0: the next quadword is a tag).
    std::uint32_t Left() const { return left_; }
    // (FinishEarly, on the render thread)
    static void RaiseFinish() { Raise(); }

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
                {   // (SLY1_GIF_SLOW_LOG=2: the A+D writes inside vertex packets, the first of each kind)
                    static const bool log = std::getenv("SLY1_GIF_SLOW_LOG") && std::getenv("SLY1_GIF_SLOW_LOG")[0] == '2';
                    static int said[2];
                    const int kind = nreg_ == 3 ? 0 : nreg_ == 10 ? 1 : -1;
                    if (log && kind >= 0 && said[kind] < 24) {
                        said[kind]++;
                        std::fprintf(stderr, "gifad: nreg %u: register %02X = %08X%08X\n", nreg_, q[2] & 0xFF, q[1], q[0]);
                    }
                }
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
        {   // SLY1_GIF_SLOW_LOG=1: the PACKED tags that go a quadword at a time (an A+D among their
            // registers), by their registers, with the quadwords each kind had; printed every 400,000 tags
            static const bool log = std::getenv("SLY1_GIF_SLOW_LOG") != nullptr;
            if (log) {
                struct Slow {
                    std::mutex lock;
                    std::map<std::pair<std::uint64_t, std::uint32_t>, std::pair<std::uint64_t, std::uint64_t>> by;
                    std::uint64_t fast = 0;
                    std::uint64_t tags = 0;
                    void Say()
                    {
                        std::vector<std::pair<std::uint64_t, std::pair<std::uint64_t, std::uint32_t>>> top;
                        for (auto& kv : by)
                            top.push_back({kv.second.first, kv.first});
                        std::sort(top.rbegin(), top.rend());
                        std::fprintf(stderr, "gifslow: %llu quadwords in plain PACKED runs\n", (unsigned long long)fast);
                        for (std::size_t k = 0; k < top.size() && k < 16; k++)
                            std::fprintf(stderr, "gifslow: %llu quadwords in %llu tags: nreg %u regs %016llX\n", (unsigned long long)top[k].first,
                                         (unsigned long long)by[top[k].second].second, top[k].second.second, (unsigned long long)top[k].second.first);
                    }
                };
                static Slow slow;
                std::lock_guard<std::mutex> lk(slow.lock);
                if (++slow.tags % 400000 == 0)
                    slow.Say();
                if (flg_ == 0 && !plain_) {
                    auto& e = slow.by[{regs_ & (nreg_ == 16 ? ~0ull : (1ull << (4 * nreg_)) - 1), nreg_}];
                    e.first += (std::uint64_t)nloop * nreg_;
                    e.second++;
                } else if (flg_ == 0) {
                    slow.fast += (std::uint64_t)nloop * nreg_;
                }
            }
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
        // one the game was told of when the render thread passed it (FinishEarly): only counted off
        if (g_itemFinishTold) {
            g_itemFinishTold = false;
            g_finishAhead.fetch_sub(1, std::memory_order_acq_rel);
            return;
        }
        g_finishesLate++;
        Raise();
    }
    static void Raise()
    {
        TIMELINE("FINISH raised");
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
// A packet or a batch kept of a model part (PartReplay): a path's words, or for path 4 a batch's
// tag word, record and vertices as QueueBatch lays them out.
struct PartKept {
    std::uint8_t path;
    std::shared_ptr<const std::vector<std::uint32_t>> words;
    // the words themselves, set when the part is kept: the GS thread goes through a part's items
    // without following each one's pointers first (it asks for the next one's memory ahead)
    const std::uint32_t* data = nullptr;
    std::uint32_t size = 0;
};

// The render thread's time in each kind of item (VIF1 data, GIF data, calls), for the report.
std::atomic<std::uint64_t> g_renderNs[3], g_renderItems[3];
struct GsItem {
    std::uint8_t path = 0;   // 1-3, 4 a batch, 5 a part drawn again, or 0: a call
    std::vector<std::uint32_t> words;
    // a model part drawn again (PartReplay): the words kept from the first time, for a path's
    // packet all of them, for a batch its vertices (words has its tag word and record, this frame's)
    std::shared_ptr<const std::vector<std::uint32_t>> kept;
    // path 5, a whole part drawn again: everything kept of it (words has this frame's matrix, the
    // viewport's scale and offset, the first id for its batches and the first id they were kept with)
    std::shared_ptr<const std::vector<PartKept>> part;
    void (*fn)(std::uint64_t, std::uint64_t) = nullptr;
    std::uint64_t a = 0, b = 0;
    // path 6, a part run beside the render thread (PartJob): its items come in this one's place
    std::shared_ptr<struct PartJob> job;
    // path 5: what the part's batches are drawn with this frame, made ahead by the helper (PrepTask)
    std::shared_ptr<struct PrepTask> prep;
    // paths 2 and 3: its last tag is the game's FINISH, and the game has been told (FinishEarly)
    bool finishTold = false;    // a call: when the list of VIF1's before it reached the render thread (g_listAt)
    std::uint64_t listAt = 0;
};

// What a kept batch is drawn with this frame (a part drawn again): its vertices skinned by this
// frame's bones and moved by its sway, its lighting's parameters for this frame (b.litNow, in sc).
// iw: the item's words (PartDecide's), whose layout ReplayLayout reads. Run by the GS thread, or
// by the helper ahead of it for the whole part (PrepTask).
struct ReplayLayout {
    std::uint32_t stCount = 0, swayCount = 0, boneCount = 0;
    bool hasLit = false;
    std::size_t litAt = 0, swayAt = 0, boneAt = 0, projAt = 0, fix = 0;
    explicit ReplayLayout(const std::uint32_t* iw)
    {
        stCount = iw[27] & 0xFFFF;
        hasLit = (iw[27] & 0x10000u) != 0;
        swayCount = (iw[27] >> 20) & 0xF;
        boneCount = (iw[27] >> 24) & 0xFF;
        litAt = 28 + 2 * (std::size_t)stCount;
        swayAt = litAt + (hasLit ? gs::kLitParamWords : 0);
        boneAt = swayAt + 12 * (std::size_t)swayCount;
        projAt = boneAt + 16 * (std::size_t)boneCount;
        fix = projAt + 24 * (std::size_t)swayCount;
    }
};
struct BatchScratch {
    std::vector<std::uint32_t> skinned, litSkinned, moved;
    const void* skinnedPart = nullptr;
    std::uint32_t skinnedId = 0;
    bool skinnedLit = false;
    gs::LitParams litNow;
};
void BatchNow(const std::uint32_t* iw, const ReplayLayout& L, const void* partKey, const std::uint32_t* w, gs::Batch& b, BatchScratch& sc);

// A part drawn again whose batches are skinned, swayed or lit anew each frame: that work made by
// the helper when the part is decided, so that the GS thread, which limits the frame, only takes
// what it made (measured on the Switch: skinning there cost it 6 microseconds a part, and the
// helper had the time). The GS thread makes it itself if the helper has not begun, and does the
// work its own way if the helper is set aside in the middle of it. Of a pool, made once.
struct PrepTask {
    std::atomic<int> state{0};      // 0 waiting, 1 being made, 2 made
    std::atomic<int> runners{0};
    std::shared_ptr<const std::vector<PartKept>> part;
    std::vector<std::uint32_t> words;                          // the item's words (its parameters)
    std::vector<std::vector<std::uint32_t>> quads, colours;    // by the part's item: moved vertices, a kick's colours
    std::vector<std::uint8_t> moved;                           // bit 0 skinned, bit 1 swayed
    std::vector<std::uint32_t> all3;
    BatchScratch sc;
    std::uint64_t begunAt = 0;
};
std::vector<PrepTask*> g_prepFree;
std::mutex g_prepFreeLock;
constexpr std::uint32_t kListPreps = 2048;
std::shared_ptr<PrepTask> g_preps[kListPreps];
std::atomic<std::uint32_t> g_prepCount{0}, g_prepNext{0};
std::mutex g_prepLock;   // (the queue's slots: the render thread fills and empties them, the helper takes)
std::atomic<std::uint64_t> g_prepsMade[4], g_prepWaitNs{0};   // by the helper, the GS thread, done the GS thread's own way, the render thread
// SLY1_PART_PREP_RENDER=0: the render thread makes none of them when it has nothing else
bool PrepRenderOn()
{
    static const bool off = std::getenv("SLY1_PART_PREP_RENDER") && std::getenv("SLY1_PART_PREP_RENDER")[0] == '0';
    return !off;
}
bool PrepOn()
{
    static const bool off = std::getenv("SLY1_PART_PREP") && std::getenv("SLY1_PART_PREP")[0] == '0';
    return !off;
}
std::shared_ptr<PrepTask> PrepNew()
{
    PrepTask* t = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_prepFreeLock);
        for (std::size_t k = g_prepFree.size(); k-- > 0 && !t;)
            if (g_prepFree[k]->runners.load(std::memory_order_acquire) == 0) {
                t = g_prepFree[k];
                g_prepFree.erase(g_prepFree.begin() + (std::ptrdiff_t)k);
            }
    }
    if (!t)
        t = new PrepTask;
    t->state.store(0, std::memory_order_relaxed);
    t->begunAt = 0;
    return std::shared_ptr<PrepTask>(t, [](PrepTask* x) {
        x->part.reset();
        std::lock_guard<std::mutex> lk(g_prepFreeLock);
        g_prepFree.push_back(x);
    });
}
// The work, on this thread (who: 0 the helper, 1 the GS thread): true when this thread made it.
bool PrepRun(PrepTask& t, int who)
{
    struct Runner {
        PrepTask& t;
        explicit Runner(PrepTask& x) : t(x) { t.runners.fetch_add(1, std::memory_order_acq_rel); }
        ~Runner() { t.runners.fetch_sub(1, std::memory_order_acq_rel); }
    } runner(t);
    int waiting = 0;
    if (!t.state.compare_exchange_strong(waiting, 1, std::memory_order_acq_rel))
        return false;
    t.begunAt = NowNs();
    const std::vector<PartKept>& part = *t.part;
    const ReplayLayout L(t.words.data());
    const std::size_t head = 1 + (sizeof(gs::Batch) + 3) / 4;
    t.quads.resize(part.size());
    t.colours.resize(part.size());
    t.moved.assign(part.size(), 0);
    std::uint32_t lastId = 0, lastCount = 0;
    for (std::size_t i = 0; i < part.size(); i++) {
        t.quads[i].clear();
        t.colours[i].clear();
        const PartKept& kept = part[i];
        if (kept.path != 4)
            continue;
        gs::Batch b;
        std::memcpy(&b, &kept.data[1], sizeof b);
        b.quads = kept.data + head;
        BatchNow(t.words.data(), L, &part, kept.data, b, t.sc);
        if (b.skinMoved || b.swayMoved) {
            t.quads[i].assign(b.quads, b.quads + (std::size_t)b.count * b.stride * 4);
            t.moved[i] = (b.skinMoved ? 1 : 0) | (b.swayMoved ? 2 : 0);
        }
        if (b.lit && b.litNow) {   // the three kicks' colours made at the first, the others taken from them
            if (!(b.id != 0 && b.id == lastId && b.count == lastCount)) {
                t.all3.resize((std::size_t)b.count * 12);
                gs::LitColoursAll(b, t.all3.data());
                lastId = b.id;
                lastCount = b.count;
            }
            const std::size_t at = (std::size_t)(b.litKick < 3 ? b.litKick : 0) * b.count * 4;
            t.colours[i].assign(t.all3.begin() + (std::ptrdiff_t)at, t.all3.begin() + (std::ptrdiff_t)(at + (std::size_t)b.count * 4));
        }
    }
    t.state.store(2, std::memory_order_release);
    g_prepsMade[who]++;
    return true;
}
bool PrepWaiting()
{
    return g_prepNext.load(std::memory_order_acquire) < g_prepCount.load(std::memory_order_acquire);
}
std::shared_ptr<PrepTask> PrepTake()
{
    std::lock_guard<std::mutex> lk(g_prepLock);
    const std::uint32_t next = g_prepNext.load(std::memory_order_acquire);
    if (next >= g_prepCount.load(std::memory_order_acquire))
        return nullptr;
    g_prepNext.store(next + 1, std::memory_order_release);
    return g_preps[next];
}

void BatchNow(const std::uint32_t* iw, const ReplayLayout& L, const void* partKey, const std::uint32_t* w, gs::Batch& b, BatchScratch& sc)
{
    const std::size_t head = 1 + (sizeof(gs::Batch) + 3) / 4;
    const bool hasLit = L.hasLit;
    const std::uint32_t swayCount = L.swayCount, boneCount = L.boneCount;
    const std::size_t litAt = L.litAt, swayAt = L.swayAt, boneAt = L.boneAt;
    (void)head;
        b.lit = b.litWords ? b.quads + (std::size_t)b.count * b.stride * 4 : nullptr;
        b.litNow = nullptr;
        b.sway = b.swayWords ? b.quads + (std::size_t)b.count * b.stride * 4 + b.litWords : nullptr;
        b.swayMoved = false;
        b.skin = b.skinWords ? b.quads + (std::size_t)b.count * b.stride * 4 + b.litWords + b.swayWords : nullptr;
        b.skinMoved = false;
        std::vector<std::uint32_t>& skinned = sc.skinned, & litSkinned = sc.litSkinned;
        // (a batch's kicks come one after another, each with its own copy of the vertices:
        // skinned once, for the first; the next take what it gave)
        const void*& skinnedPart = sc.skinnedPart;
        std::uint32_t& skinnedId = sc.skinnedId;
        bool& skinnedLit = sc.skinnedLit;
        // (only a lit one's: the kicks' own colours are in their vertices otherwise, and those
        // of the first would be drawn for all)
        const bool again = b.skin && boneCount != 0 && b.id != 0 && skinnedPart == partKey && skinnedId == b.id && skinnedLit && b.lit &&
                           skinned.size() == (std::size_t)b.count * b.stride * 4;
        if (again) {
            b.quads = skinned.data();
            b.skinMoved = true;
            if (litSkinned.size() == b.litWords)
                b.lit = litSkinned.data();
        } else if (b.skin && boneCount != 0) {
            skinnedPart = partKey;
            skinnedId = b.id;
            skinnedLit = false;
            // its vertices skinned by this frame's bones, and its normals for its lighting (a
            // matrix with no bone known is one no vertex weighs: the keeping said so)
            float mats[64] = {};
            for (int j = 0; j < 4; j++)
                if (b.boneSource[j] < boneCount)
                    std::memcpy(mats + 16 * j, &iw[boneAt + 16 * (std::size_t)b.boneSource[j]], 64);
            skinned.resize((std::size_t)b.count * b.stride * 4);
            float* normals = nullptr;
            if (b.lit && b.skin[1]) {
                litSkinned.assign(b.lit, b.lit + b.litWords);
                normals = reinterpret_cast<float*>(litSkinned.data() + gs::kLitHead);
            }
            gs::SkinQuads(b, mats, skinned.data(), normals);
            skinnedLit = normals != nullptr;
            {   // SLY1_SKIN_CHECK=4: this frame's bones against those it was kept with, and the kept
                // vertices skinned anew by the latter against themselves
                static const bool check = std::getenv("SLY1_SKIN_CHECK") && std::getenv("SLY1_SKIN_CHECK")[0] == '4';
                if (check) {
                    static std::vector<std::uint32_t> again;
                    again.resize(skinned.size());
                    gs::SkinQuads(b, reinterpret_cast<const float*>(b.skin + 2), again.data(), nullptr);
                    double self = 0, moved = 0;
                    for (std::uint32_t k = 0; k < b.count; k++)
                        for (int c = 0; c < 3; c++) {
                            const std::size_t at = (k * b.stride + b.positionAt) * 4 + c;
                            const float* kept = reinterpret_cast<const float*>(w + head);
                            self = std::max<double>(self, std::fabs(reinterpret_cast<const float*>(again.data())[at] - kept[at]));
                            moved = std::max<double>(moved, std::fabs(reinterpret_cast<const float*>(skinned.data())[at] - kept[at]));
                        }
                    double bones = 0;
                    for (int q = 0; q < 64; q++)
                        bones = std::max<double>(bones, std::fabs(mats[q] - reinterpret_cast<const float*>(b.skin + 2)[q]));
                    static int said;
                    if (said++ < 30)
                        std::fprintf(stderr, "skin check 4: batch of %u: kept vertices skinned anew by their bones %g apart; by this frame's %g; the bones %g apart\n", b.count, self, moved, bones);
                }
            }
            b.quads = skinned.data();
            b.skinMoved = true;
            if (normals)
                b.lit = litSkinned.data();
        }
        if (b.sway && b.swaySource < swayCount) {   // its vertices moved by this frame's sway
            std::vector<std::uint32_t>& moved = sc.moved;
            moved.resize((std::size_t)b.count * b.stride * 4);
            gs::SwayQuads(b, reinterpret_cast<const float*>(&iw[swayAt + 12 * (std::size_t)b.swaySource]), moved.data());
            b.quads = moved.data();
            b.swayMoved = true;
        }
        gs::LitParams& litNow = sc.litNow;
        if (hasLit && b.lit) {   // this frame's lighting, with the batch's own start and vf01
            std::memcpy(&litNow, &iw[litAt], sizeof litNow);
            gs::LitParams was;
            std::memcpy(&was, b.lit, sizeof was);
            float add;
            std::memcpy(&add, b.lit + gs::kLitParamWords, 4);
            litNow.start[1] += add;
            std::memcpy(litNow.one, was.one, sizeof litNow.one);
            if (litNow.lights == was.lights)
                b.litNow = &litNow;
        }
}

// On a thread running a part's job: where what it queues for the GS goes. The items stay where
// they are from job to job, and each one's buffer is used again for the item that comes in its
// place: a job's thread neither allocates nor takes a lock (the helper may be set aside by the
// game's threads at any point, and what it held would be held until it runs again).
struct Collect {
    std::vector<GsItem> items;
    std::size_t used = 0;
    bool trouble = false;   // a batch of the run was not handed over as unpacked (vu1::PartTrouble)
};
thread_local Collect* t_collect = nullptr;
// (a job's) counted up with each item it gives: a thread waiting for the job sees whether the one
// running it is getting on or has been set aside (PartJobNeed)
thread_local std::atomic<std::uint32_t>* t_jobBeat = nullptr;
// SLY1_SKIN_VERIFY=1 (with SLY1_PART_JOBS=0): a skinned part that would be drawn again is run as
// ever, and each batch it gives is compared with the one its keeping would have drawn.
struct SkinVerify {
    bool on = false;
    std::vector<std::vector<std::uint32_t>> predicted;   // positions, count*3 floats as words, by batch
    std::vector<std::vector<std::uint32_t>> colours;     // a lit one's kick's colours, count*4 (empty: none)
    std::vector<std::vector<float>> normals;             // its normals skinned anew, count*3
    std::size_t next = 0;
};
SkinVerify g_skinVerify; // (SLY1_PART_DIFF and the other censuses: PartDiffReport prints them.) Counted by the render
// thread and the threads that run jobs, printed and cleared by the host's main thread at a
// level's end: a lock (without it a run of the 46 levels faulted in the map at level 11).
std::map<std::string, std::uint64_t> g_diffCounts;
std::mutex g_diffLock;
void DiffCount(const std::string& name)
{
    std::lock_guard<std::mutex> lk(g_diffLock);
    g_diffCounts[name]++;
}
// (a job's thread) its VIF's count of texture-offset unpacks, and the count when the job began:
// which of the packet's offsets a batch's is (gs::Batch::stSource), as PartRecordItem says it
thread_local const std::uint32_t* t_jobStCount = nullptr;
thread_local std::uint32_t t_jobStBase = 0;
thread_local const std::uint32_t* t_jobSwayCount = nullptr;
thread_local std::uint32_t t_jobSwayBase = 0;
class Vif;
thread_local const Vif* t_jobVif = nullptr;   // (its bones: gs::Batch::boneSource)
thread_local std::uint32_t t_jobBoneBase = 0;
void BoneSources(gs::Batch& b, const std::vector<std::uint32_t>& words, const Vif& vif, std::uint32_t base);
std::atomic<std::uint64_t> g_jobStaleCodes{0};   // interrupts and microcode loads a job's stale run came to, and left alone
std::mutex g_gsLock;
std::condition_variable g_gsWake, g_gsIdle;
std::vector<GsItem> g_gsQueue;   // (a deque allocated a block every few items, and the GS thread freed it)
std::vector<std::vector<std::uint32_t>> g_gsSpare;
constexpr std::size_t kGsSpareMost = 2048;   // (SLY1_PERF_LOG's perfjobs line says how often the stock was empty)
std::atomic<std::uint64_t> g_gsSpareNone{0}, g_gsSpareHad{0}, g_gsSpareGrown{0};
constexpr std::size_t kGsSpareWords = 4096 + 64;
bool g_gsBusy, g_gsWaiting;
std::atomic<bool> g_gsStarted;
// The render thread's items for the GS, passed on together at the end of each of its own items
// (a wake of the GS thread is a system call on the Switch: one a packet cost more than the GS's
// work), and its own stock of emptied buffers.
std::vector<GsItem> g_gsPending;
// (each thread that queues for the GS has a stock of its own: the render thread, and those running a part's job)
thread_local std::vector<std::vector<std::uint32_t>>* t_gsLocalSpare = nullptr;

// Only with the render thread: without it, the paths reach the GS from the game's thread in turn.
bool GsAsync()
{
    static const bool sync = std::getenv("SLY1_RENDER_SYNC") != nullptr;
    return !sync && g_gsStarted.load(std::memory_order_acquire);
}

std::vector<std::uint32_t> GsSpare()
{
    if (Collect* c = t_collect) {   // the buffer of the item that was in the next one's place
        if (c->used >= c->items.size())
            return {};
        std::vector<std::uint32_t> v = std::move(c->items[c->used].words);
        v.clear();
        return v;
    }
    if (!t_gsLocalSpare)
        t_gsLocalSpare = new std::vector<std::vector<std::uint32_t>>;
    std::vector<std::vector<std::uint32_t>>& g_gsLocalSpare = *t_gsLocalSpare;
    if (g_gsLocalSpare.empty()) {
        std::lock_guard<std::mutex> lk(g_gsLock);
        for (int k = 0; k < 256 && !g_gsSpare.empty(); k++) {
            g_gsLocalSpare.push_back(std::move(g_gsSpare.back()));
            g_gsSpare.pop_back();
        }
        if (g_gsLocalSpare.empty()) {
            g_gsSpareNone.fetch_add(1, std::memory_order_relaxed);
            std::vector<std::uint32_t> fresh;
            fresh.reserve(kGsSpareWords);
            return fresh;
        }
    }
    g_gsSpareHad.fetch_add(1, std::memory_order_relaxed);
    std::vector<std::uint32_t> v = std::move(g_gsLocalSpare.back());
    g_gsLocalSpare.pop_back();
    v.clear();
    // Room for the largest packet or batch (VU1's data memory is 1,024 quadwords): a buffer from
    // the stock that was too small for what it was taken for grew, an allocation, a copy and a
    // free each time, a sixth of the render thread's work walking on the Switch.
    if (v.capacity() < kGsSpareWords) {
        g_gsSpareGrown.fetch_add(1, std::memory_order_relaxed);
        v.reserve(kGsSpareWords);
    }
    return v;
}

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

// (on the render thread) Passed on every 24 items, so that the GS thread draws a frame's first
// batches while the render thread is still at its later ones (a frame is one item of VIF1's).
struct PartRecord;
PartRecord* g_partRecording;   // the model part being recorded (PartReplay, below)
extern PartRecord* g_partObserving;
extern bool g_observeBad;
void PartRecordItem(const GsItem& item);
bool PartCall(std::uint32_t marker, std::uint32_t target, bool slow);
void PartDone();
struct PartRecord;

// FINISH writes among what the render thread has queued for the GS thread in its current item
// (the game ends each of a frame's packets with one: a tag of one A+D register and the write).
int g_finishSeen;
bool EndsWithFinish(const std::vector<std::uint32_t>& w)
{
    if (w.size() < 8 || (w.size() & 3))
        return false;
    // the tags walked from the first (a packet starts at one): the last must be the FINISH's and
    // its one quadword of data the item's last
    const std::size_t quads = w.size() / 4;
    std::size_t at = 0;
    for (int tags = 0; tags < 4096; tags++) {
        const std::uint32_t* tag = &w[at * 4];
        const std::uint32_t nloop = tag[0] & 0x7FFF, flg = (tag[1] >> 26) & 3;
        std::uint32_t nreg = (tag[1] >> 28) & 0xF;
        if (nreg == 0)
            nreg = 16;
        const std::size_t n = flg == 0 ? (std::size_t)nloop * nreg : flg == 1 ? ((std::size_t)nloop * nreg + 1) / 2 : nloop;
        const std::size_t next = at + 1 + n;
        if (next > quads)
            return false;
        if (next == quads)
            return nloop == 1 && flg == 0 && nreg == 1 && (tag[2] & 0xF) == 0xE && (tag[6] & 0xFF) == 0x61;
        at = next;
    }
    return false;
}
// At the end of an item of the render thread's: the game told of the FINISHes it held, now that
// everything before them has been through the render thread (its VU1 work and the interrupts of
// its list among it). The GS thread counts each off when it comes to it (Gif::Finish).
void GsPassOn();
void FinishEarly()
{
    if (g_finishSeen == 0)
        return;
    GsPassOn();
    for (; g_finishSeen > 0; g_finishSeen--) {
        g_finishesEarly++;
        Gif::RaiseFinish();
    }
}
// Before a list of VIF1's is begun: the GS thread no more than a frame behind (a frame has three
// FINISHes). The wait has no limit of time: with one, the GS thread fell 36 frames behind on the
// Switch and the controls answered a second late. The GS thread has all it was given by now
// (GsPassOn before FinishEarly) and needs nothing more of this thread to come to its FINISHes. A
// wait of half a second is said (a GS thread that has stopped is then seen, not silent).
std::atomic<std::uint64_t> g_lagWaitNs{0};
void FinishLagWait()
{
    if (g_finishAhead.load(std::memory_order_acquire) <= 3)
        return;
    const std::uint64_t from = NowNs();
    bool said = false;
    while (g_finishAhead.load(std::memory_order_acquire) > 3) {
        if (PrepRenderOn() && PrepWaiting()) {   // (as when its queue is empty: RenderThreadMain)
            if (std::shared_ptr<PrepTask> p = PrepTake())
                PrepRun(*p, 3);
            continue;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        if (!said && NowNs() - from > 500000000ull) {
            said = true;
            std::fprintf(stderr, "dmac: the render thread has waited half a second for the GS thread (%d FINISHes owed)\n", g_finishAhead.load());
        }
    }
    g_lagWaitNs += NowNs() - from;
}

// SLY1_PERF_LOG: from a list of VIF1's reaching the render thread to the frame drawn from it being
// given to the display (the swap after it, run by the GS thread): how long the port holds a frame.
std::atomic<std::uint64_t> g_listAt{0}, g_heldNs{0}, g_heldMaxNs{0}, g_heldCount{0};

// One of VU1's packets after another joins it in its item (SLY1_PATH1_JOIN=0: an item each). The
// outlines' program kicks a packet for each edge it draws, 620 a frame in A Stealthy Approach
// and over half of the GS thread's items: each cost it a buffer, its place in the queue, and
// the taking and timing of an item, 2.7 ms a frame on the Switch for 1,100 packets.
// (the item to join, where KickPath1 is to put the packet, or none)
GsItem* Path1Last();

void GsEnqueue(GsItem&& item)
{
    if (Collect* c = t_collect) {
        if (item.path == 4 && t_jobStCount && item.words.size() > 1 + (sizeof(gs::Batch) + 3) / 4) {
            gs::Batch b;
            std::memcpy(&b, &item.words[1], sizeof b);
            if (b.stApplied) {
                const std::uint32_t seen = *t_jobStCount - t_jobStBase;
                b.stSource = seen >= 1 && seen <= 64 ? (std::uint8_t)(seen - 1) : 0xFF;
            }
            if ((b.swayWords != 0 || b.projected) && t_jobSwayCount) {
                const std::uint32_t seen = *t_jobSwayCount - t_jobSwayBase;
                const std::uint8_t source = seen >= 1 && seen <= 64 ? (std::uint8_t)(seen - 1) : 0xFF;
                if (b.swayWords != 0)
                    b.swaySource = source;
                if (b.projected)
                    b.projSource = source;
            }
            if (b.skinWords != 0 && t_jobVif)
                BoneSources(b, item.words, *t_jobVif, t_jobBoneBase);
            std::memcpy(&item.words[1], &b, sizeof b);
        }
        if (c->used < c->items.size())
            c->items[c->used] = std::move(item);
        else
            c->items.push_back(std::move(item));
        c->used++;
        if (t_jobBeat)
            t_jobBeat->fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (g_skinVerify.on && item.path == 4) {
        gs::Batch b;
        std::memcpy(&b, &item.words[1], sizeof b);
        const std::uint32_t* q = &item.words[1 + (sizeof(gs::Batch) + 3) / 4];
        static std::uint64_t batches, wrong, said;
        batches++;
        double worst = 0;
        if (g_skinVerify.next < g_skinVerify.predicted.size()) {
            const std::vector<std::uint32_t>& p = g_skinVerify.predicted[g_skinVerify.next];
            if (p.size() == (std::size_t)b.count * 3)
                for (std::uint32_t k = 0; k < b.count; k++)
                    for (int c = 0; c < 3; c++) {
                        float x, y;
                        std::memcpy(&x, &p[k * 3 + c], 4);
                        std::memcpy(&y, &q[(k * b.stride + b.positionAt) * 4 + c], 4);
                        worst = std::max<double>(worst, std::fabs(x - y));
                    }
            else
                worst = 1e9;
        } else {
            worst = 1e9;
        }
        {   // the normals the program lit by (the item's lighting, as the handlers noted it) against those skinned anew
            const std::size_t at = g_skinVerify.next;
            static int said;
            if (b.litWords && at < g_skinVerify.normals.size() && g_skinVerify.normals[at].size() == (std::size_t)b.count * 3 && said < 6) {
                const float* got = reinterpret_cast<const float*>(q + (std::size_t)b.count * b.stride * 4 + gs::kLitHead);
                double far = 0;
                int farK = 0;
                for (std::uint32_t k = 0; k < b.count; k++)
                    for (int c = 0; c < 3; c++)
                        if (std::fabs(got[k * 3 + c] - g_skinVerify.normals[at][k * 3 + c]) > far) {
                            far = std::fabs(got[k * 3 + c] - g_skinVerify.normals[at][k * 3 + c]);
                            farK = (int)k;
                        }
                if (far > 0.01) {
                    said++;
                    const float* a = &g_skinVerify.normals[at][farK * 3];
                    std::fprintf(stderr, "skin verify: batch %zu's normals: furthest %g, vertex %d: the program's %g %g %g, anew %g %g %g\n", at + 1, far, farK,
                                 got[farK * 3], got[farK * 3 + 1], got[farK * 3 + 2], a[0], a[1], a[2]);
                }
            }
        }
        {   // the colours
            static std::uint64_t lit, litWrong, litSaid;
            const std::size_t at = g_skinVerify.next;
            if (at < g_skinVerify.colours.size() && g_skinVerify.colours[at].size() == (std::size_t)b.count * 4) {
                lit++;
                int far = 0;
                for (std::uint32_t k = 0; k < b.count; k++)
                    for (int c = 0; c < 3; c++)
                        far = std::max(far, std::abs((int)q[(k * b.stride + b.colourAt) * 4 + c] - (int)g_skinVerify.colours[at][k * 4 + c]));
                if (far > 1) {
                    litWrong++;
                    if (litSaid++ < 8)
                        std::fprintf(stderr, "skin verify: batch %zu's colours: furthest %d from what the keeping would draw (kick %u)\n", at + 1, far, b.litKick);
                }
                if ((lit & (lit - 1)) == 0)
                    std::fprintf(stderr, "skin verify: %llu lit batches, %llu with colours further than 1\n", (unsigned long long)lit, (unsigned long long)litWrong);
            }
        }
        g_skinVerify.next++;
        if (worst > 0.05) {
            wrong++;
            if (said++ < 10)
                std::fprintf(stderr, "skin verify: batch %zu of %zu (%u vertices, skinned %d): furthest %g from what the keeping would draw\n", g_skinVerify.next,
                             g_skinVerify.predicted.size(), b.count, (int)(b.skinWords != 0), worst);
        }
        if ((batches & (batches - 1)) == 0)
            std::fprintf(stderr, "skin verify: %llu batches, %llu other than the keeping would draw\n", (unsigned long long)batches, (unsigned long long)wrong);
    }
    if (g_partRecording)
        PartRecordItem(item);
    if (g_partObserving && (item.path == 0 || item.path == 3 || item.path == 5))
        g_observeBad = true;
    if ((item.path == 2 || item.path == 3) && FinishLag() && EndsWithFinish(item.words)) {
        item.finishTold = true;
        g_finishAhead.fetch_add(1, std::memory_order_acq_rel);
        g_finishSeen++;
    }
    g_gsPending.push_back(std::move(item));
    if (g_gsPending.size() >= 24)
        GsPassOn();
}

// A batch from VU1's handlers for the GS thread (gs::SetBatchSink): the tag's word, the batch's
// record, and its vertices copied (VU1's data memory is written over by the next batch).
std::atomic<std::uint64_t> g_batchesQueued, g_batchesRefused, g_gsBusyNs;
// The GS thread's time and items by kind (an item's path: 0 a call (the swap among them), 1 VU1's
// packets, 2 VIF1's DIRECT data, 3 the GIF channel's (uploads), 4 a batch, 5 a part drawn again,
// 6 a job's items), for SLY1_PERF_LOG's perfgs line.
std::atomic<std::uint64_t> g_gsNsByPath[8], g_gsItemsByPath[8];
// A counter only one thread adds to: no atomic read-modify-write (each was a loop with barriers on
// the Switch's cores, on the GS thread a few for every item and batch); readers still see whole values.
inline void Bump(std::atomic<std::uint64_t>& a, std::uint64_t v) { a.store(a.load(std::memory_order_relaxed) + v, std::memory_order_relaxed); }
// (perfgs) a part drawn again's time, by what in it: its batches (drawn, with their per-frame work),
// its packets kept as register writes, its packets through the GIF
std::atomic<std::uint64_t> g_replayNs[5];   // a part drawn again: batches, register writes, packets, its prep, before it
std::atomic<std::uint64_t> g_path1Joined{0};   // VU1's packets that joined the one before in its item
bool QueueBatch(std::uint32_t tagWord1, const gs::Batch& b)
{
    GsItem item;
    item.path = 4;
    item.words = GsSpare();
    const std::size_t head = 1 + (sizeof(gs::Batch) + 3) / 4, quads = (std::size_t)b.count * b.stride * 4;
    std::uint32_t record[1 + (sizeof(gs::Batch) + 3) / 4] = {tagWord1};
    std::memcpy(&record[1], &b, sizeof b);
    item.words.reserve(head + quads + (b.lit ? b.litWords : 0) + (b.sway ? b.swayWords : 0) + (b.skin ? b.skinWords : 0));
    item.words.insert(item.words.end(), record, record + head);
    item.words.insert(item.words.end(), b.quads, b.quads + quads);
    if (b.lit)   // (its lighting after its vertices: gs::Batch::lit)
        item.words.insert(item.words.end(), b.lit, b.lit + b.litWords);
    if (b.sway)   // (and its sway after that: gs::Batch::sway)
        item.words.insert(item.words.end(), b.sway, b.sway + b.swayWords);
    if (b.skin)   // (and its skinning: gs::Batch::skin)
        item.words.insert(item.words.end(), b.skin, b.skin + b.skinWords);
    GsEnqueue(std::move(item));
    g_batchesQueued++;
    return true;
}

// The outlines' edges put together. The outlines' program (0x26D) kicks a packet for each edge: a
// tag with one register write and the edge's two vertices (a line), then the same again with
// another write (the line once more for its depth alone, which the GPU renderer leaves out: the
// first writes its depth everywhere). Some 620 edges a frame in A Stealthy Approach, each two
// tags, two register writes and four vertices for the GS to take one by one. As the packets join
// in one item (Path1Last), a run of edges whose tags and writes are the same becomes: the first
// edge's first half as it is, one tag with all the other edges' vertices (lines, two vertices a
// loop), and the last edge's second half: the same lines drawn by the first write's state, and
// the registers and the vertex queue left as the last edge left them.
// SLY1_OUTLINE_JOIN=0: not. (The software drawing draws every edge's second line where the GPU
// renderer leaves it out; under lockstep its pictures are the same with the edges put together.)
struct OutlineRun {
    std::size_t end = 0;     // the item's words when the run was last added to (0: no run)
    std::size_t run = 0;     // where the tag of the lines is (0: only one edge yet)
    std::size_t tail = 0;    // where the last edge's second half is
    std::uint32_t head[8];   // an edge's first tag and write
    std::uint32_t second[8]; // its second
};
thread_local OutlineRun t_outline;
std::atomic<std::uint64_t> g_outlinesJoined{0};

void OutlineJoin(GsItem& item, std::size_t at)
{
    static const bool on = !(std::getenv("SLY1_OUTLINE_JOIN") && std::getenv("SLY1_OUTLINE_JOIN")[0] == '0');
    OutlineRun& o = t_outline;
    std::vector<std::uint32_t>& w = item.words;
    const auto half = [&](std::size_t q, bool eop) {   // a tag of one loop of an A+D write and two vertices, the primitive a line set by the tag
        return (w[q] & 0x7FFF) == 1 && ((w[q] >> 15) & 1) == (eop ? 1u : 0u) && ((w[q + 1] >> 26) & 3) == 0 && (w[q + 1] >> 28) == 3 && (w[q + 2] & 0xFFF) == 0x44E &&
               ((w[q + 1] >> 14) & 1) == 1 && ((w[q + 1] >> 15) & 7) == 1;
    };
    const bool edge = on && w.size() == at + 32 && half(at, false) && half(at + 16, true);
    if (!edge) {
        o.end = 0;
        return;
    }
    const bool follows = o.end == at && o.end != 0 && std::memcmp(o.head, &w[at], 32) == 0 && std::memcmp(o.second, &w[at + 16], 32) == 0 &&
                         std::memcmp(o.second, &w[o.tail], 32) == 0;
    if (!follows) {
        std::memcpy(o.head, &w[at], 32);
        std::memcpy(o.second, &w[at + 16], 32);
        o.run = 0;
        o.tail = at + 16;
        o.end = w.size();
        return;
    }
    // the new edge's two vertices (its first half's), and its second half whole
    std::uint32_t verts[8], tail[16];
    std::memcpy(verts, &w[at + 8], 32);
    std::memcpy(tail, &w[at + 16], 64);
    std::size_t put = o.tail;
    if (o.run == 0) {   // the second edge of a run: the tag of the lines in the first edge's second half's place
        o.run = put;
        w[put] = 0;
        w[put + 1] = 2u << 28;
        w[put + 2] = 0x44;
        w[put + 3] = 0;
        put += 4;
    }
    w.resize(put + 8 + 16);
    std::memcpy(&w[put], verts, 32);
    std::memcpy(&w[put + 8], tail, 64);
    w[o.run]++;   // (a loop more: under 0x7FFF, an item being 4,096 words at most)
    o.tail = put + 8;
    o.end = w.size();
    g_outlinesJoined++;
}

GsItem* Path1Last()
{
    static const bool off = std::getenv("SLY1_PATH1_JOIN") && std::getenv("SLY1_PATH1_JOIN")[0] == '0';
    if (off)
        return nullptr;
    GsItem* last = nullptr;
    if (Collect* c = t_collect)
        last = c->used != 0 ? &c->items[c->used - 1] : nullptr;
    else if (!g_partRecording && !g_gsPending.empty())
        last = &g_gsPending.back();
    if (!last || last->path != 1 || last->kept || last->finishTold || last->words.size() > 4096)
        return nullptr;
    return last;
}

// XGKICK: the GIF packet at quadword qaddr of VU1's data memory, tag by tag until a tag with
// EOP has had its data (the data memory wraps at 1,024 quadwords).
void KickPath1(std::uint32_t qaddr, void*)
{
    const std::uint8_t* d = vu1::Data();
    g_counts.path1Packets++;
    {   // SLY1_PATH1_CENSUS=1: VU1's packets by their first tag and the program that kicked them (partdiff:)
        static const bool census = std::getenv("SLY1_PATH1_CENSUS") != nullptr;
        if (census && !t_collect) {
            const std::uint32_t* tag = reinterpret_cast<const std::uint32_t*>(d + (qaddr & 0x3FF) * 16);
            std::uint32_t hash, entry;
            vu1::CurrentProgram(&hash, &entry);
            char name[112];
            std::snprintf(name, sizeof name, "path1: program %08X 0x%03X: nloop %u eop %u flg %u nreg %u regs %X%s", hash, entry, tag[0] & 0x7FFF, (tag[0] >> 15) & 1, (tag[1] >> 26) & 3,
                          (tag[1] >> 28) & 0xF, tag[2], g_partRecording ? " (a part being kept)" : "");
            DiffCount(name);
        }
    }
    if (GsAsync()) {   // the packet copied, tag by tag to the one with EOP
        GsItem fresh;
        GsItem* const last = Path1Last();
        if (!last) {
            fresh.path = 1;
            fresh.words = GsSpare();
        } else {
            g_path1Joined++;
        }
        GsItem& item = last ? *last : fresh;
        const std::size_t packetAt = item.words.size();
        for (int tags = 0; tags < 1024; tags++) {
            const std::uint32_t* tag = reinterpret_cast<const std::uint32_t*>(d + (qaddr & 0x3FF) * 16);
            std::uint32_t nloop = tag[0] & 0x7FFF, flg = (tag[1] >> 26) & 3, nreg = (tag[1] >> 28) & 0xF;
            if (nreg == 0)
                nreg = 16;
            std::uint32_t n = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
            // the tag and its data, in one piece or two (the data memory wraps)
            const std::uint32_t total = std::min<std::uint32_t>(n, 1023) + 1, from = qaddr & 0x3FF;
            const std::uint32_t first = std::min(total, 1024 - from);
            const std::uint32_t* q = reinterpret_cast<const std::uint32_t*>(d + from * 16);
            item.words.insert(item.words.end(), q, q + first * 4);
            if (total > first) {
                const std::uint32_t* q0 = reinterpret_cast<const std::uint32_t*>(d);
                item.words.insert(item.words.end(), q0, q0 + (total - first) * 4);
            }
            qaddr += std::min<std::uint32_t>(n, 1023) + 1;
            if ((tag[0] >> 15) & 1)
                break;
        }
        OutlineJoin(item, packetAt);
        if (!last)
            GsEnqueue(std::move(fresh));
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
// The unpack of a mesh's texture offset: one vector of two words to 22 from TOPS, which the
// program at 0x4F8 adds to every vertex's S and T (a texture that slides along a surface). In a
// part drawn from its cache the game writes the two words anew each frame and nothing else of the
// packet changes: they are left out of the part's hash and given to the renderer as numbers
// (docs/RENDERER.md, "A part's numbers left to the renderer"). SLY1_PART_PARAMS=0: not.
constexpr std::uint32_t kStUnpackMask = 0x7FFF83FFu, kStUnpack = 0x64018016u;
// The unpack of a mesh's sway: six quadwords to 22 (rows 22 to 25 the four displacements the sway
// program, 0x754 of chunk 51A89CD8, moves each vertex by with its weights; 26 and 27 what else it
// reads). The game writes them anew each frame in the mesh's data; they are left out of the part's
// hash and the renderer moves the kept vertices by how much they changed (gs::SwayQuads).
constexpr std::uint32_t kSwayUnpack = 0x6C068016u;
// The unpack of a bone's matrix for a skinned mesh (DrawModelPart: four quadwords, V4-32, to the
// mesh's bone slot from TOPS): the game writes the bones anew each frame in the part's own chain.
// They are left out of the part's hash, found again by their place in the chain, and the renderer
// skins the kept vertices by them (gs::SkinQuads).
constexpr std::uint32_t kBoneMask = 0x7FFFC000u, kBoneUnpack = 0x6C04C000u;
bool SkinOn()
{
    // Off unless SLY1_SKIN=1 (2026-11-22): measured on the Switch in one play of A Stealthy Approach
    // with SLY1_SKIN=ab (on and off in turn, 600 frames each), the skinning anew took the helper
    // from 9.1 to 3.8 ms a frame but cost the GS thread, which limits the frame, 26 to 32
    // microseconds for each part drawn again: windows at 60 frames a second 71% on, 83% off. It is
    // for when the skinning is done off the GS thread.
    static const char* const e = std::getenv("SLY1_SKIN");
    if (e && e[0] == 'a')
        return gs::FramesShown() / 600 % 2 == 0;
    return e && e[0] == '1';
}
// A place in a part's packet that the hash leaves out: in memory that stays (a cached packet, a
// REF of a mesh's data), or, for a part drawn the long way, in its own chain, which is written
// anew each frame at another place: then as words from the chain's start.
struct MaskAt {
    const std::uint32_t* p = nullptr;
    std::ptrdiff_t fromChain = -1;   // words from the chain's start, or -1: p
    // the VIFcode before the words (an unpack's, whose data they are): a place in this frame's
    // chain is taken only when the same code is before it (the verdict that a part is as it was
    // may be the helper's, of the list before, and a chain laid out otherwise has other words there)
    std::uint32_t code = 0;
    const std::uint32_t* At(const std::uint32_t* chain) const { return fromChain < 0 ? p : chain + fromChain; }
};
bool SwayOn()
{
    static const bool off = std::getenv("SLY1_SWAY") && std::getenv("SLY1_SWAY")[0] == '0';
    return !off;
}
// The unpacks of a light's packet (RefreshConeVolumeShape): its colour to 22 (four words), its
// scale and base to 23 and 24 (eight), then a directional light's direction to 25, or a point light's attenuation to 25
// and place to 26. A part lit by VU1 (mode 4) whose light moves has only these words other from
// frame to frame; for such a part they are left out of its hash, and its colours are made from
// what they hold each frame (docs/RENDERER.md, "A lit part's colours left to the renderer").
inline bool LightUnpack(std::uint32_t code)
{
    const std::uint32_t c = code & kStUnpackMask;
    // (and a spot light's: its cone's two ramps to 25 and 26, its direction to 27, its view and
    // projection to 28 to 31)
    return c == 0x6C018016u || c == 0x6C028017u || c == 0x68018019u || c == 0x64018019u || c == 0x6801801Au || c == 0x6C028019u ||
           c == 0x6801801Bu || c == 0x6C04801Cu;
}
struct LitAt {
    std::uint32_t addr, words;
    const std::uint32_t* p;
};
// SLY1_LIT_REPLAY=0: a part lit by VU1 is kept and drawn again only while its lights and its
// header are what they were, as before.
bool LitReplayOn()
{
    static const bool off = std::getenv("SLY1_LIT_REPLAY") && std::getenv("SLY1_LIT_REPLAY")[0] == '0';
    return !off;
}
bool PartParamsOn()
{
    static const bool off = std::getenv("SLY1_PART_PARAMS") && std::getenv("SLY1_PART_PARAMS")[0] == '0';
    return !off;
}

class Vif {
public:
    // VIF0 feeds VU0 (port/vu0_micro.h): its MPG, UNPACK and microprogram starts take effect.
    Vif(int cause, bool vu0) : cause_(cause), vu0_(vu0) {}

    // A quadword's words: the data of a kept VIFcode (an unpack, microcode) appended at once.
    // The hook's NOP seen since the last call (LiveSink), 0 if none: the part is open from here to
    // PartEnd.
    // Whether the hook's NOP was the pair of a part drawn the long way (its meshes' packets as a
    // chain of their own); read before TakePartBegin, which forgets it.
    bool PartSlow() const { return partSlow_; }
    std::uint32_t LastTop() const { return lastTop_; }
    std::uint32_t TakePartBegin()
    {
        std::uint32_t w = partBegin_;
        partSlow_ = false;
        partBegin_ = 0;
        if (w)
            gs::PartOpen(true, (w & 0x1FFFFF) << 4, (w >> 22) & 1);
        return w;
    }
    void PartEnd() { gs::PartOpen(false, 0, 0); }

    // The registers a part's packet finds and leaves (PartReplay).
    struct Regs {
        std::uint32_t cl, wl, row[4], col[4], mask, mode, base, ofst, tops, itops, dbf;
        bool operator==(const Regs& o) const { return std::memcmp(this, &o, sizeof o) == 0; }
    };
    Regs GetRegs() const
    {
        Regs r;
        std::memset(&r, 0, sizeof r);
        r.cl = cl_; r.wl = wl_; r.mask = mask_; r.mode = mode_; r.base = base_; r.ofst = ofst_; r.tops = tops_; r.itops = itops_;
        r.dbf = dbf_ ? 1 : 0;
        std::memcpy(r.row, row_, sizeof r.row);
        std::memcpy(r.col, col_, sizeof r.col);
        return r;
    }
    void SetRegs(const Regs& r)
    {
        cl_ = r.cl; wl_ = r.wl; mask_ = r.mask; mode_ = r.mode; base_ = r.base; ofst_ = r.ofst; tops_ = r.tops; itops_ = r.itops;
        dbf_ = r.dbf != 0;
        std::memcpy(row_, r.row, sizeof r.row);
        std::memcpy(col_, r.col, sizeof r.col);
    }
    bool BetweenCodes() const { return left_ == 0; }
    // the unpacks so far of a mesh's texture offset (kStUnpack)
    std::uint32_t StUnpacks() const { return stUnpacks_; }
    const std::uint32_t* StUnpacksAt() const { return &stUnpacks_; }
    std::uint32_t SwayUnpacks() const { return swayUnpacks_; }
    // the bone unpacks so far, and the last one to each place (0: none)
    std::uint32_t BoneUnpacks() const { return boneUnpacks_; }
    std::uint32_t LastBoneAt(std::uint32_t addr) const { return lastBone_[addr & 0x3FF]; }
    const std::uint32_t* SwayUnpacksAt() const { return &swayUnpacks_; }

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
            if (left_ != 0 && direct_ && !keep_ && filled_ == 0 && n - k >= 4 && !vu0_ && GsAsync()) {
                // DIRECT's data for the GS thread: whole quadwords copied at once, not gathered a
                // word at a time (one copy a quadword was most of what the list's reading cost)
                const std::uint32_t take = std::min<std::uint32_t>(left_, (std::uint32_t)(n - k)) & ~3u;
                if (take > 0) {
                    directData_.insert(directData_.end(), w + k, w + k + take);
                    k += (int)take;
                    left_ -= take;
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
            if ((w & kStUnpackMask) == kStUnpack)
                stUnpacks_++;
            if ((w & kStUnpackMask) == kSwayUnpack)
                swayUnpacks_++;
            if ((w & kBoneMask) == kBoneUnpack)
                lastBone_[w & 0x3FF] = ++boneUnpacks_;
            if (wl_ > cl_)   // filling: only cl of every wl written vectors come from the stream
                vectors = cl_ * (n / wl_) + (n % wl_ < cl_ ? n % wl_ : cl_);
            std::uint32_t bits = (vn == 3 && vl == 3) ? 16 : (32u >> vl) * (vn + 1);
            left_ = (vectors * bits + 31) / 32;
        } else {
            switch (cmd) {
            case 0x00: case 0x06: case 0x07: case 0x10: case 0x11: case 0x13:
                left_ = 0;   // NOP, MSKPATH3, MARK, the flushes (the port's transfers are done when they end)
                if (cmd == 0x00 && (w & 0x00800000) && !vu0_)   // the hook's NOP (tools/patches.py)
                    partBegin_ = w;
                else if (cmd == 0x00 && (w & 0x00FFFFFF) == 0x51A0 && !vu0_)   // before it: a part drawn the long way
                    partSlow_ = true;
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
                if (t_jobBeat)   // (a job's run getting on: each program it starts, as each item it gives)
                    t_jobBeat->fetch_add(1, std::memory_order_relaxed);
                if (vu0_) {
                    vu0::Run(cmd == 0x17 ? vu0::Vu0().pc * 8 : (imm & 0x1FF) * 8);
                } else {
                    // the double buffer swaps: the program gets TOPS as TOP, ITOPS as ITOP
                    vu1::Context c;
                    c.top = tops_;
                    lastTop_ = tops_;
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
                if (t_collect)   // (as the interrupt below: never from a job)
                    g_jobStaleCodes.fetch_add(1, std::memory_order_relaxed);
                else
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
            // Not from a part's job. A job's packet has no interrupt (its shape says so), but a thread
            // set aside in the middle of a job goes on with it after the render thread has run the job
            // itself and the game has been told the list is finished: the packet's memory is then the
            // next frame's, and what the stale run reads there may be any VIFcode. Its items are
            // dropped; an interrupt it raised ran the game's handler on the helper thread, a fault on
            // the Switch (2026-11-20, with the blank's slack on, which lets the game write sooner).
            if (t_collect) {
                g_jobStaleCodes.fetch_add(1, std::memory_order_relaxed);
                return;
            }
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
    std::uint32_t stUnpacks_ = 0, swayUnpacks_ = 0, boneUnpacks_ = 0;
    std::uint32_t lastBone_[1024] = {};
    bool dbf_ = false;
    std::uint32_t cmd_ = 0, imm_ = 0, num_ = 0;
    std::vector<std::uint32_t> data_;
    const std::uint32_t* src_ = nullptr;   // an unpack's data where it came, in place of data_ (Words)
    std::size_t srcWords_ = 0;
    std::uint32_t partBegin_ = 0, lastTop_ = 0;
    bool partSlow_ = false;
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
    enum Kind : std::uint8_t { Vif1, Gif, Call, Vif1Live } kind = Vif1;
    std::uint32_t regs = 0, tadr = 0;   // Vif1Live: the chain, walked where it is
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
    TIMELINE("game queues %s, %zu words", item.kind == RenderItem::Vif1 ? "VIF1 (copy)" : item.kind == RenderItem::Vif1Live ? "VIF1 (in place)" : item.kind == RenderItem::Gif ? "GIF" : "a call", item.words.size());
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
    void OnTagDone(std::uint32_t) {}
    bool SkipData(std::uint32_t) { return false; }
    bool SkipCall(std::uint32_t) { return false; }
    bool Direct() const { return true; }
};
// A list's length, its tags walked and nothing else.
struct CountSink {
    std::size_t words = 0;
    void OnVif(const std::uint32_t*, int n) { words += (std::size_t)n; }
    void OnGif(const std::uint32_t*) { words += 4; }
    void OnTagDone(std::uint32_t) {}
    bool SkipData(std::uint32_t qwc) { words += (std::size_t)qwc * 4; return true; }   // counted, not looked at
    bool SkipCall(std::uint32_t) { return false; }
    bool Direct() const { return false; }
};
constexpr std::size_t kLiveListWords = 16384;   // 64 KiB: a frame's list is ten times that and more

// VIF1's list walked by the render thread where it lies (a RenderItem of kind Vif1Live).
// --- A model part drawn again from what was kept of its packet (with the GS thread;
// SLY1_PART_REPLAY=0: not).
//
// A part whose lighting has not changed is drawn by a call of its light cache's packet
// (tools/patches.py's hook says which): the part's meshes, unpacked into VU1's memory, with VU1's
// programs that set them up and kick them. What comes of that for the GS is the same each frame
// but for the matrix: packets that set registers, and batches (their vertices as they were
// unpacked, for the renderer to transform). So the first time, what the part queues for the GS
// thread is kept. After that, while the packet's own words and the part's header are what they
// were (the header's first four quadwords, its matrix, apart), the call is not followed: the
// kept packets and batches are queued again, the batches with the matrix made of this frame's
// header and VU1's matrix at 4, and VIF1's registers are set as the packet left them. Neither
// the VIF's unpacks nor VU1's programs run for it.
//
// A part is not kept (and is run as ever) when anything but that came of it: a batch not handed
// over as unpacked, a batch whose matrix is not the header's times VU1's, a packet with vertices
// in it, an interrupt, a FINISH.
namespace {
struct PartRecord {
    bool ok = false;
    std::uint64_t hash = 0, hashWhole = 0;   // PartHash, and with every piece whole
    std::uint32_t verified = 0;          // the times it has been drawn again with the whole hash the same
    std::uint64_t usedFrame = 0;         // the frame it was last kept or drawn again in (the ones unused longest are let go for room)
    bool moving = false;                 // its vertices change where the hash does not look: not drawn again
    // ...until movingUntil, when it is kept anew: one whose vertices changed once (its cache's
    // colours written again when a light changed) is then drawn again as before. Left alone for
    // 120 frames the first time and twice as long each time after (to 3,840).
    std::uint64_t movingUntil = 0;
    std::uint32_t movingWait = 0;
    std::uint32_t header[44] = {};       // the part's header, 11 quadwords at TOPS
    Vif::Regs before, after;
    std::vector<PartKept> items;                          // while it is recorded
    std::shared_ptr<const std::vector<PartKept>> kept;    // once it is kept: what the GS thread is given
    std::uint64_t words = 0;             // what its items hold, counted in g_partWords
    std::uint64_t keyFirst = 0, keys = 0;   // its batches' keys (gs::Batch::keep)
    std::uint32_t stale = 0;             // the times running it was found changed
    std::uint64_t notBefore = 0;         // a part that keeps changing is not kept again before this frame
    // How long it is then left alone: 30 frames, and twice as long each time after (to 960),
    // until it has been drawn again 60 times from one keeping. Keeping a part costs several
    // times running it, and a part that fades for seconds was kept anew three times a second.
    std::uint32_t lockout = 0, replays = 0;
    // what the part was when it was last found other than kept (or new): it is kept anew only when
    // the next list has it the same (PartCall)
    std::uint64_t seenHash = 0, seenList = 0;
    std::uint32_t seenHeader[44] = {};
    Vif::Regs seenRegs = {};
    std::uint32_t ids = 0;               // the batches' ids run from firstId for this many
    std::uint32_t firstId = 0;
    // while it is recorded
    std::uint64_t troubleWas = 0, interruptsWas = 0, finishesWas = 0;
    bool bad = false;
    // the hash a frame late (PartHashLate): what the helper last found, and whether it is asked
    std::uint32_t serial = 0;            // which keeping of the part this is (a result for another is dropped)
    bool pending = false;                // a job is running it for this keeping (PartJobsKeep)
    std::uint64_t seen = 0, seenWhole = 0;
    bool wholeArrived = false, asked = false;
    // A part found changed is hashed before each decision, as it was, until this frame: one that
    // changes every frame or two would otherwise be drawn as it was every other frame for good.
    std::uint64_t watchUntil = 0;
    // a part drawn the long way: its header as its first batch found it, and where
    bool slow = false, headerSet = false;
    std::uint32_t headerAt = 0;
    std::uint32_t verdictList = 0;   // the list in which the helper's last good hash of it came
    // The TEX0 writes left out of the hash: where they are in the packet, what they were when the
    // part was kept, and where each is in the kept packets (item, word, which write). texStrict:
    // a kept TEX0 that is none of them, or too many: the values must be what they were.
    struct TexFix {
        std::uint32_t item, word, which;
    };
    std::vector<const std::uint32_t*> texAt;
    std::vector<std::uint64_t> texWas;
    std::vector<TexFix> texFix;
    bool texStrict = false;
    // A part's numbers left to the renderer. alphaStrict: a batch kept of it has the alpha's scale
    // in its colours, so the header's 9.x must be what it was. stAt, stWas: where the texture
    // offsets left out of the hash are in the packet (called at stTarget) and what they were when
    // the part was kept; each kept batch says which is its own (gs::Batch::stSource, counted from
    // stBase of VIF1's count). stStrict: an offset no kept batch answers for, or too many: the
    // values must be what they were.
    bool alphaStrict = false, stStrict = false;
    bool maskRead = true, maskSet = false;   // (PartHasher's, of the packet it was kept from)
    // A part lit by VU1 (mode 4). litAt: its lights' words left out of the hash, in the packet's
    // order (every mesh refers to the same lights' packets); litWas: what they held when the part
    // was kept; lights: the part's lights, made of them (LitSource), the same for each mesh.
    // litStrict: a lit batch kept of it has no lighting noted, or the lights are not ones that are
    // followed: the words and the header must be what they were.
    struct LitSource {
        const float* colour = nullptr, * scale = nullptr, * base = nullptr, * dir = nullptr, * atten = nullptr, * place = nullptr;
        const float* ramp = nullptr, * view = nullptr;   // a spot light's (its direction in dir)
    };
    bool litStrict = false;
    std::vector<LitAt> litAt;
    std::vector<std::uint32_t> litWas;
    std::vector<LitSource> lights;
    struct St {
        float s, t;
    };
    std::vector<const std::uint32_t*> stAt;
    std::vector<St> stWas;
    std::uint32_t stBase = 0, stTarget = 0;
    // The same for a mesh's sway (kSwayUnpack): where its rows lie, what they were, and VIF1's
    // count of such unpacks when the part was kept (gs::Batch::swaySource counts from it).
    // swayStrict: a sway no kept batch answers for, or a kept batch moved by none: the rows must be
    // what they were.
    std::vector<const std::uint32_t*> swayAt;
    std::vector<std::uint32_t> swayWas;
    std::uint32_t swayBase = 0;
    bool swayStrict = false;
    // A skinned part's bones (kBoneUnpack): where each lies (MaskAt), what it was, VIF1's count of
    // bone unpacks when the part was kept (gs::Batch::boneSource counts from it). boneStrict: a
    // bone no kept batch answers for, a skinned batch without its skinning noted, or one not
    // left out: the bones must be what they were.
    std::vector<MaskAt> boneAt;
    std::vector<std::uint32_t> boneWas;
    std::uint32_t boneBase = 0;
    bool boneStrict = false;
    // For a job (PartJob): its packet's shape when it was last run here, and the registers it was
    // run from and left (two pairs: the game turns VIF1's buffers about from part to part, and a
    // part comes with either), with nothing a job must not have (an interrupt, a FINISH, a call).
    bool jobOk = false;
    Vif::Regs jobBefore2, jobAfter2;
    bool jobTwo = false;
    std::uint64_t shape = 0;
    Vif::Regs jobBefore, jobAfter;
};
std::unordered_map<std::uint32_t, PartRecord> g_parts;
// What a part kept before it was found changed: let go when the render thread has nothing queued,
// not in the list's walk (a part's packets freed one by one were 3% of the walk in slow frames on
// the Switch, the allocator's lock shared with the GS thread).
std::vector<std::shared_ptr<const std::vector<PartKept>>> g_partGrave;
std::vector<std::vector<PartKept>> g_partGraveItems;

// --- The parts' hashes a frame late, on a helper thread.
//
// Hashing each part's packet before deciding to draw it again was about 2 ms a frame of the render
// thread on the Switch, most of it the wait for memory. A part that is kept is now drawn again on
// what the helper found the last time it was asked (the frame before, as a rule), and the helper
// is asked again: a part whose packet has changed (a texture's next picture, new colours from the
// lights) is drawn once more as it was and kept anew the frame after. The registers and the header
// are compared at once as before. SLY1_PART_HASH_NOW=1: every hash before the decision, as before.
struct HashJob {
    std::uint32_t key, serial, target, cl, wl;
    bool tte, whole, slow;
    std::uint32_t list;   // the list it came in (g_listSerial)
    bool lit = false;     // (PartHash's: a part lit by VU1, its lights' words left out)
};
struct HashResult {
    std::uint32_t key, serial;
    std::uint64_t hash, hashWhole;
    bool whole, valid;
};
// The lists the render thread has begun. The chain of a part drawn the long way lies in its
// frame's packet memory, which the game starts to write over when it has handed the next frame's
// list to be drawn: a hash the helper makes of it is good only if no later list had begun when the
// hash was done.
std::uint32_t g_listSerial;
std::atomic<std::uint32_t> g_listsBegun;
std::atomic<std::uint64_t> g_hashTooLate;
std::mutex g_hashLock;
std::condition_variable g_hashWake;
// The parts' jobs (PartJob, below) waiting for a thread, with g_hashLock as the helper's other work.
// A list's jobs in their order: written by the render thread alone and counted when written,
// taken by number (the helper, and the render thread when its list is walked). No lock: the
// helper runs below the game's threads and may be set aside while it holds one.
constexpr std::uint32_t kListJobs = 4096;
std::shared_ptr<PartJob> g_listJobs[kListJobs];
std::atomic<std::uint32_t> g_listJobCount, g_listJobNext;
bool g_jobHelperWaiting;
std::atomic<bool> g_hashQueued;   // hash work has been passed on since the helper last took it
// While the render thread walks a list the helper does not sleep between jobs: it asks again
// (waking it for each job was a system call a part on the render thread).
std::atomic<bool> g_listWalked;
PartJob* PartJobTake();
bool PartJobRun(PartJob& j, int who, bool again);
std::vector<HashJob> g_hashJobs, g_hashPending;      // for the helper; the render thread's, not passed on yet
std::vector<HashResult> g_hashResults;
std::atomic<bool> g_hashAnyResult, g_hashStarted;
std::atomic<std::uint64_t> g_hashHelperNs, g_hashHelped;
std::uint32_t g_partSerial;
std::uint64_t g_partsLate;   // drawn again on a hash a frame late
// The times each part has come in the list being walked: a table of fixed size, emptied by a new
// stamp (a map's node for each part of each list was most of the render thread's allocations).
struct PartTurns {
    struct Slot {
        std::uint32_t key, stamp;
        std::uint8_t turn;
    };
    static constexpr std::uint32_t kSlots = 8192;
    Slot slots[kSlots] = {};
    std::uint32_t stamp = 1, used = 0;
    void clear()
    {
        stamp++;
        used = 0;
    }
    std::uint8_t& operator[](std::uint32_t key)
    {
        static std::uint8_t full;   // (a list of over 6,000 parts: the rest all the first turn)
        for (std::uint32_t at = (key * 0x9E3779B1u) >> 19;; at = (at + 1) & (kSlots - 1)) {
            Slot& s = slots[at];
            if (s.stamp == stamp && s.key == key)
                return s.turn;
            if (s.stamp != stamp) {
                if (used >= kSlots * 3 / 4) {
                    full = 0;
                    return full;
                }
                used++;
                s = {key, stamp, 0};
                return s.turn;
            }
        }
    }
} g_partTurns;
std::uint64_t g_partsSlowReplayed, g_partsSlowKept, g_partsSlowRefused;
std::uint64_t g_partsAlphaMoved, g_partsStMoved;
std::uint64_t g_partsLitNow;
std::uint64_t g_partsSwayed;
std::uint64_t g_partsSkinned;
std::uint64_t g_bonesMislaid;
  // parts whose bones were not at their places in this frame's chain   // parts drawn again with their vertices skinned by this frame's bones   // parts drawn again with their vertices moved by this frame's sway   // parts lit by VU1 drawn again with this frame's lighting
// (SLY1_PERF_LOG's perfkeep: why a part is kept anew: 0 never kept, 1 to 3 kept and found other by
// its hash, registers, header, 4 its moving mark ended, 5 known but not kept (refused, let go);
// and why one is run here and watched: 0 never run here cleanly, 1 no registers known for its
// shape, 2 met with other registers than known)
std::uint64_t g_keepWhy[6], g_watchWhy[3];
std::uint64_t g_partsLetGo, g_partsLetGoRecent;
std::uint64_t g_fullNsBy[7], g_fullBy[7];
std::uint64_t g_partsKeepPutOff;   // parts not kept in the list they could have been (no more than a few a list)
std::uint64_t g_keptByJob, g_keptByJobRefused;   // parts kept from a job's run of them, and those a job's run did not give as a part's
void PartKeepFinish(PartRecord* r, bool ok, const float* matrix, const Vif::Regs& after, const char* jobWhy);
std::uint64_t g_jobsByShape;       // parts made a job at first sight by another part's registers for the same shape   // kept parts let go for room, and those of them drawn in the last 120 frames   // drawn again with another alpha's scale, another texture offset

std::uint64_t PartHash(std::uint32_t target, bool whole, std::uint32_t cl, std::uint32_t wl, bool tte, bool slow = false,
                       std::vector<const std::uint32_t*>* tex = nullptr, bool shape = false, std::vector<const std::uint32_t*>* st = nullptr,
                       std::uint8_t* maskUse = nullptr, bool lit = false, std::vector<LitAt>* litAt = nullptr,
                       std::vector<const std::uint32_t*>* swayAt = nullptr, std::vector<MaskAt>* boneAt = nullptr);

}  // namespace

// Above the game's threads on their core (SLY1_HELPER_PRIORITY), the helper stands aside between
// two jobs while the game needs the core at once (GameUrgent, port/platform/kernel.cpp).
bool GameUrgent();
std::atomic<bool> g_helperAbove{false};
std::atomic<std::uint64_t> g_helperGaveWayNs{0};
inline void HelperGiveWay()
{
#if defined(__SWITCH__)
    if (!g_helperAbove.load(std::memory_order_relaxed) || !GameUrgent())
        return;
    const std::uint64_t t0 = NowNs();
    for (int k = 0; k < 400 && GameUrgent(); k++)
        svcSleepThread(50000);
    g_helperGaveWayNs += NowNs() - t0;
#endif
}

void HashHelperMain()
{
    PlaceThread(ThreadRole::Helper);
    ProfileThisThread("helper");   // (SLY1_PROFILE=helper)
    g_hashStarted.store(true, std::memory_order_release);
    std::vector<HashJob> jobs;
    std::vector<HashResult> results;
    for (;;) {
#if defined(__SWITCH__)
        {   // SLY1_HELPER_PRIORITY=ab: below the game's threads (0x2D) and above them (0x2B) in turn,
            // 600 frames each, to compare in one played run
            static const bool turns = std::getenv("SLY1_HELPER_PRIORITY") && std::getenv("SLY1_HELPER_PRIORITY")[0] == 'a';
            static int was = -1;
            const int now = (int)(gs::FramesShown() / 600 % 2);
            if (turns && now != was) {
                was = now;
                svcSetThreadPriority(CUR_THREAD_HANDLE, now ? 0x2B : 0x2D);
                g_helperAbove.store(now != 0, std::memory_order_relaxed);
            }
        }
#endif
        {
            std::unique_lock<std::mutex> lk(g_hashLock);
            for (const HashResult& r : results)
                g_hashResults.push_back(r);
            if (!results.empty())
                g_hashAnyResult.store(true, std::memory_order_release);
            results.clear();
            g_jobHelperWaiting = true;
            g_hashWake.wait(lk, [] { return !g_hashJobs.empty() || g_listWalked.load(std::memory_order_acquire); });
            g_jobHelperWaiting = false;
            g_hashQueued.store(false, std::memory_order_release);
            jobs.swap(g_hashJobs);
        }
        for (;;) {   // the parts' jobs first: the GS thread waits for them
            HelperGiveWay();
            if (PartJob* j = PartJobTake())
                PartJobRun(*j, 0, false);
            else if (std::shared_ptr<PrepTask> p = PrepTake())
                PrepRun(*p, 0);
            else if (g_listWalked.load(std::memory_order_acquire) && jobs.empty() && !g_hashQueued.load(std::memory_order_acquire))
#if defined(__SWITCH__)
                // (asleep a moment, not a yield: above the game's threads a yield gives them nothing,
                // and the list's walk waits for the game's interrupt handler: the screen stayed black)
                svcSleepThread(30000);
#else
                std::this_thread::yield();
#endif
            else
                break;
        }
        const std::uint64_t t0 = NowNs();
        for (std::size_t n = 0; n < jobs.size(); n++) {
            HelperGiveWay();
            while (PartJob* pj = PartJobTake()) {
                PartJobRun(*pj, 0, false);
                HelperGiveWay();
            }
            const HashJob& j = jobs[n];
            HashResult r = {j.key, j.serial, 0, 0, j.whole, true};
            if (!(j.slow && g_listsBegun.load(std::memory_order_acquire) != j.list)) {
                r.hash = PartHash(j.target, false, j.cl, j.wl, j.tte, j.slow, nullptr, false, nullptr, nullptr, j.lit);
                if (j.whole)
                    r.hashWhole = PartHash(j.target, true, j.cl, j.wl, j.tte, j.slow, nullptr, false, nullptr, nullptr, j.lit);
            }
            if (j.slow && g_listsBegun.load(std::memory_order_acquire) != j.list) {
                r.valid = false;
                g_hashTooLate++;
            }
            results.push_back(r);
        }
        g_hashHelped += jobs.size();
        jobs.clear();
        g_hashHelperNs += NowNs() - t0;
    }
}

namespace {
bool PartHashLate()
{
    static const bool now = std::getenv("SLY1_PART_HASH_NOW") != nullptr;
    if (now)
        return false;
    return g_hashStarted.load(std::memory_order_acquire);   // the helper is running (the program starts it)
}

// (on the render thread) The jobs asked for since the last time handed to the helper.
void HashPassOn()
{
    if (g_hashPending.empty())
        return;
    {
        // only if the lock is free: the helper may be set aside while it holds it (it runs below
        // the game's threads), and the render thread does not wait for that
        std::unique_lock<std::mutex> lk(g_hashLock, std::try_to_lock);
        if (!lk.owns_lock())
            return;
        g_hashJobs.insert(g_hashJobs.end(), g_hashPending.begin(), g_hashPending.end());
        g_hashQueued.store(true, std::memory_order_release);
    }
    g_hashPending.clear();
    g_hashWake.notify_one();
}

// (on the render thread) What the helper has found, put in the parts' records.
void HashTakeResults()
{
    if (!g_hashAnyResult.load(std::memory_order_acquire))
        return;
    static std::vector<HashResult> results;
    {
        std::unique_lock<std::mutex> lk(g_hashLock, std::try_to_lock);   // as HashPassOn
        if (!lk.owns_lock())
            return;
        g_hashAnyResult.store(false, std::memory_order_release);
        results.swap(g_hashResults);
    }
    for (const HashResult& h : results) {
        auto it = g_parts.find(h.key);
        if (it == g_parts.end() || it->second.serial != h.serial)
            continue;
        PartRecord& r = it->second;
        r.asked = false;
        if (!h.valid)
            continue;
        r.verdictList = g_listSerial;
        r.seen = h.hash;
        if (h.whole) {
            r.seenWhole = h.hashWhole;
            r.wholeArrived = true;
        }
    }
    results.clear();
}   // by the hook's packet and half
std::uint64_t g_partTimes[2];   // ns: parts drawn again, parts run in full
std::uint64_t g_partsTexMoved, g_partsReplayed, g_partsRecorded, g_partsRefused, g_partsStale, g_partItemsReplayed, g_partsMoving, g_partsStaleBy[3], g_partHashNs, g_partWords, g_partsCleared, g_partKey, g_partsFickle, g_partsWaited, g_waitWhy[12];
constexpr std::size_t kBatchHead = 1 + (sizeof(gs::Batch) + 3) / 4;

bool PartReplayOn()
{
    // on with the GS thread (the Switch); SLY1_PART_REPLAY=0: every part run as ever
    static const bool off = std::getenv("SLY1_PART_REPLAY") && std::getenv("SLY1_PART_REPLAY")[0] == '0';
    return !off && GsAsync();
}

void PartHeader(std::uint32_t tops, std::uint32_t out[44]);
// Which of the packet's bone unpacks (counted from base) each of a skinned batch's four matrices
// was: the last one to its place (the batch's skin[0], four quadwords a matrix) before the batch.
void BoneSources(gs::Batch& b, const std::vector<std::uint32_t>& words, const Vif& vif, std::uint32_t base)
{
    // (the batch's skin, as QueueBatch laid it out after its vertices, lighting and sway)
    const std::size_t at = 1 + (sizeof(gs::Batch) + 3) / 4 + (std::size_t)b.count * b.stride * 4 + b.litWords + b.swayWords;
    const bool has = b.skinWords != 0 && at < words.size();
    const std::uint32_t mats = has ? words[at] : 0;
    for (int j = 0; j < 4; j++) {
        const std::uint32_t last = has ? vif.LastBoneAt(mats + 4 * (std::uint32_t)j) : 0;
        b.boneSource[j] = last > base && last - base <= 254 ? (std::uint8_t)(last - base - 1) : 0xFF;
        {
            static const bool log = std::getenv("SLY1_SKIN_LOG") != nullptr;
            static std::atomic<int> said{0};
            if (log && has && b.boneSource[j] == 0xFF && said++ < 16)
                std::fprintf(stderr, "skinsrc: matrix %d at %u: last bone there %u, base %u, bones so far %u\n", j, mats + 4 * j, last, base, vif.BoneUnpacks());
        }
    }
}

void PartRecordItem(const GsItem& item)
{
    PartRecord& r = *g_partRecording;
    if (item.path == 0 || item.path == 3 || item.path == 5 || item.kept) {   // a call, the GIF channel's: not a part's
        if (std::getenv("SLY1_PART_DIFF")) {
            char name[64];
            std::snprintf(name, sizeof name, "refusal: an item of path %d%s while %s mode %d was kept", item.path, item.kept ? " (kept)" : "", r.slow ? "long" : "cached", (int)r.header[37]);
            DiffCount(name);
        }
        r.bad = true;
        return;
    }
    if (item.path == 4) {   // a batch: a key of its own, for the renderer to keep its vertices by
        std::vector<std::uint32_t> words = item.words;
        gs::Batch b;
        std::memcpy(&b, &words[1], sizeof b);
        b.keep = ++g_partKey;
        if (b.stApplied) {   // which of the packet's texture offsets is in its S and T: the last VIF1 unpacked
            const std::uint32_t seen = g_vif1.StUnpacks() - r.stBase;
            b.stSource = seen >= 1 && seen <= 64 ? (std::uint8_t)(seen - 1) : 0xFF;
        }
        if (b.swayWords != 0 || b.projected) {   // which of the packet's unpacks to 22 moved or laid it: likewise
            const std::uint32_t seen = g_vif1.SwayUnpacks() - r.swayBase;
            const std::uint8_t source = seen >= 1 && seen <= 64 ? (std::uint8_t)(seen - 1) : 0xFF;
            if (b.swayWords != 0)
                b.swaySource = source;
            if (b.projected)
                b.projSource = source;
        }
        if (b.skinWords != 0)   // which of the packet's bones its four matrices were
            BoneSources(b, words, g_vif1, r.boneBase);
        std::memcpy(&words[1], &b, sizeof b);
        r.items.push_back({item.path, std::make_shared<const std::vector<std::uint32_t>>(std::move(words))});
    } else {
        r.items.push_back({item.path, std::make_shared<const std::vector<std::uint32_t>>(item.words)});
    }
    r.words += item.words.size();
    g_partWords += item.words.size();
}

// A hash of the packet called at target, to the tag that returns: its tags and, as VIF1 would
// take them, the words that follow a tag or that a tag refers to: every VIFcode, and every code's
// data whole, but for an unpack of more than 64 words (a mesh's vertices), of which one quadword in
// 32 is taken unless whole is set. So a register's value set in a mesh's data (a texture's
// place, which the game writes anew when it moves the texture) always counts, and vertices that
// move (a flag in the wind) are found by the whole hash: a part is hashed whole the first times
// it is drawn again and now and then after, and one found changed where those quadwords were not is
// marked as moving and run as ever from then on.
struct PartHasher {
    std::uint64_t h = 0xCBF29CE484222325ull;
    std::uint32_t left = 0, cl = 1, wl = 1, at = 0;
    bool whole = false, sampled = false;
    // a part drawn the long way: the data of its header's unpack (the chain's first unpack of 11
    // whole quadwords) is left out, as the header of a part drawn from its cache is compared apart
    bool skipHeader = false, ignored = false;
    // SLY1_PART_DIFF: a hash for each VIFcode's data and each tag, to say what changed in a part
    std::vector<std::pair<std::uint32_t, std::uint64_t>>* segs = nullptr;
    // A part drawn from its cache: the value of a TEX0 write among the registers its packet
    // unpacks for VU1 to kick is left out (the game places a texture anew in the GS's memory from
    // frame to frame, and nothing else of the part changes with it); where the writes are is
    // noted when asked for (tex), for PartCall to read them as they are now.
    bool shape = false, shapeBad = false, shapeData = false;
    bool texFree = false, adWatch = false;
    std::uint32_t adLeft = 0;
    std::vector<const std::uint32_t*>* tex = nullptr;
    // The same for a mesh's texture offset (kStUnpack): its two words left out, and where they are
    // noted when asked for (st), in the order VIF1 meets them.
    bool stFree = false, stSkip = false;
    std::vector<const std::uint32_t*>* st = nullptr;
    // A mesh's sway (kSwayUnpack): its 24 words left out, where they lie in memory that stays (a
    // cached packet, or a REF of a mesh's data: not a part's chain, in the frame's memory).
    bool swayFree = false, swaySkip = false, stable = true;
    std::vector<const std::uint32_t*>* sway = nullptr;
    // A skinned mesh's bones (kBoneUnpack): their 16 words left out, where they lie noted (chain:
    // the part's chain's start, for those in it).
    bool bonesFree = false, boneSkip = false;
    std::vector<MaskAt>* bones = nullptr;
    const std::uint32_t* chain = nullptr;
    std::uint32_t boneCode = 0;
    // Whether the packet unpacks through the mask as it comes (a masked unpack before any STMASK
    // of its own), and whether it sets the mask: a part that does not read the mask it is met
    // with is the same part whatever that is.
    bool maskRead = false, maskSet = false;
    // A part lit by VU1: its lights' words (LightUnpack) left out, and noted when asked for.
    bool litFree = false, litSkip = false;
    std::vector<LitAt>* lit = nullptr;
    std::uint32_t litAddr = 0;
    void Mix(std::uint32_t w)
    {
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    std::uint64_t laneA = 0, laneB = 0;   // (Feed: an unpack's words in two lanes, SLY1_HASH_LANES)
    std::uint32_t lanePart = 0;
    bool laneHalf = false;
    void Feed(const std::uint32_t* w, std::size_t n)
    {
        for (std::size_t k = 0; k < n;) {
            if (left == 0) {
                std::uint32_t code = w[k++], cmd = (code >> 24) & 0x7F, imm = code & 0xFFFF, num = (code >> 16) & 0xFF;
                Mix(code);
                shapeData = cmd == 0x20 || cmd == 0x30 || cmd == 0x31;
                shapeBad = shapeBad || (code >> 31) != 0 || cmd == 0x4A;
                if (segs)
                    segs->push_back({code, 0});
                sampled = false;
                at = 0;
                stSkip = stFree && (code & kStUnpackMask) == kStUnpack && wl <= cl;
                if (stFree && (code & kStUnpackMask) == kStUnpack && !(stSkip && stable) && st)
                    st->push_back(nullptr);   // (one not left out: its place kept, the part compared by its words)
                stSkip = stSkip && stable;
                swaySkip = swayFree && stable && (code & kStUnpackMask) == kSwayUnpack && wl <= cl;
                boneSkip = bonesFree && (code & kBoneMask) == kBoneUnpack && wl <= cl && (stable || chain);
                boneCode = code;
                if (swayFree && !swaySkip && (code & kStUnpackMask) == kSwayUnpack && sway)
                    sway->push_back(nullptr);   // (one not left out: its place kept, the part compared by its words)
                litSkip = litFree && LightUnpack(code) && wl <= cl;
                litAddr = code & 0x3FF;
                if (cmd == 0x20)
                    maskSet = true;
                else if ((cmd & 0x70) == 0x70 && !maskSet)
                    maskRead = true;
                if ((cmd & 0x60) == 0x60) {   // UNPACK, as Vif::Code has it
                    std::uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3, count = num ? num : 256;
                    std::uint32_t vectors = count;
                    if (wl > cl)
                        vectors = cl * (count / wl) + (count % wl < cl ? count % wl : cl);
                    std::uint32_t bits = (vn == 3 && vl == 3) ? 16 : (32u >> vl) * (vn + 1);
                    left = (vectors * bits + 31) / 32;
                    sampled = !whole && left > 64;
                    ignored = false;
                    adWatch = texFree && vn == 3 && vl == 0 && left <= 64;
                    adLeft = 0;
                    if (skipHeader && num == 0x0B && vn == 3 && vl == 0) {
                        skipHeader = false;
                        ignored = true;
                        sampled = false;
                    }
                } else if (cmd == 0x01) {
                    cl = imm & 0xFF;
                    wl = (imm >> 8) & 0xFF;
                    if (wl == 0)
                        wl = 256;
                } else if (cmd == 0x20) {
                    left = 1;
                } else if (cmd == 0x30 || cmd == 0x31) {
                    left = 4;
                } else if (cmd == 0x4A) {
                    left = (num ? num : 256) * 2;
                } else if (cmd == 0x50 || cmd == 0x51) {
                    left = (imm ? imm : 65536) * 4;
                }
                continue;
            }
            const std::size_t take = std::min<std::size_t>(left, n - k);
            if (litSkip) {
                litSkip = false;
                if (take == left) {   // (in one piece)
                    if (lit)
                        lit->push_back({litAddr, left, w + k});
                    k += take;
                    at += (std::uint32_t)take;
                    left = 0;
                    continue;
                }
            }
            if (boneSkip) {
                boneSkip = false;
                if (take == 16 && left == 16) {   // (in one piece)
                    if (bones) {
                        MaskAt m;
                        m.p = w + k;
                        m.code = boneCode;
                        if (!stable)
                            m.fromChain = w + k - chain;
                        bones->push_back(m);
                    }
                    k += 16;
                    at += 16;
                    left = 0;
                    continue;
                }
                if (bones)
                    bones->push_back(MaskAt());   // (not left out: in the hash)
            }
            if (swaySkip) {
                swaySkip = false;
                if (take == 24 && left == 24 && stable) {   // (in one piece)
                    if (sway)
                        sway->push_back(w + k);
                    k += 24;
                    at += 24;
                    left = 0;
                    continue;
                }
                if (sway)
                    sway->push_back(nullptr);
            }
            if (stSkip) {
                stSkip = false;
                if (take == 2 && left == 2) {   // (in one piece, as the game writes it)
                    if (st)
                        st->push_back(w + k);
                    k += 2;
                    at += 2;
                    left = 0;
                    continue;
                }
                if (st)
                    st->push_back(nullptr);
            }
            if (segs && !ignored && !segs->empty())
                for (std::size_t i = 0; i < take; i++)
                    segs->back().second = (segs->back().second ^ w[k + i]) * 0x9E3779B97F4A7C15ull + 1;
            if (ignored || (shape && !shapeData)) {
                k += take;
                at += (std::uint32_t)take;
                left -= (std::uint32_t)take;
                continue;
            }
            if (adWatch && (at & 3) == 0 && (take & 3) == 0) {
                for (std::size_t i = 0; i < take; i += 4) {
                    const std::uint32_t* q = w + k + i;
                    std::uint64_t v[2];
                    std::memcpy(v, q, 16);
                    if (adLeft != 0) {
                        adLeft--;
                        if ((q[2] & 0xFF) == 0x06 && q[3] == 0) {
                            v[0] = 0;
                            if (tex)
                                tex->push_back(q);
                        }
                    } else if (q[2] == 0xE && q[3] == 0 && (q[1] >> 28) == 1 && ((q[1] >> 26) & 3) == 0) {
                        adLeft = q[0] & 0x7FFF;
                    }
                    h = (h ^ v[0]) * 0x9E3779B97F4A7C15ull;
                    h ^= h >> 29;
                    h = (h ^ v[1]) * 0x9E3779B97F4A7C15ull;
                    h ^= h >> 29;
                }
            } else if (!sampled) {
                adWatch = false;
                std::size_t i = 0;
                // the words in two lanes by their place in the unpack (the even pairs and the odd ones),
                // joined when the unpack ends, so that the same words give the same hash however
                // they come in pieces: one chain of multiplies was the helper's most sampled line on
                // the Switch after the codes' own reads. SLY1_HASH_LANES=0: one chain, as before.
                static const bool lanes = !std::getenv("SLY1_HASH_LANES") || std::getenv("SLY1_HASH_LANES")[0] != '0';
                if (lanes) {
                    if (at == 0) {
                        laneA = h;
                        laneB = h ^ 0x632BE59BD9B4E019ull;
                        laneHalf = false;
                    }
                    if (!laneHalf) {   // (whole pairs from here: four words at a time, the lanes side by side)
                        const bool odd = ((at + (std::uint32_t)i) >> 1) & 1;
                        std::uint64_t& first = odd ? laneB : laneA;
                        std::uint64_t& second = odd ? laneA : laneB;
                        for (; i + 4 <= take; i += 4) {
                            std::uint64_t v[2];
                            std::memcpy(v, w + k + i, 16);
                            first = (first ^ v[0]) * 0x9E3779B97F4A7C15ull;
                            second = (second ^ v[1]) * 0x9E3779B97F4A7C15ull;
                            first ^= first >> 29;
                            second ^= second >> 29;
                        }
                    }
                    for (; i < take; i++) {
                        const std::uint32_t word = w[k + i];
                        if (!laneHalf) {
                            lanePart = word;
                            laneHalf = true;
                            continue;
                        }
                        laneHalf = false;
                        const std::uint64_t v = lanePart | (std::uint64_t)word << 32;
                        std::uint64_t& lane = (((at + (std::uint32_t)i) >> 1) & 1) ? laneB : laneA;
                        lane = (lane ^ v) * 0x9E3779B97F4A7C15ull;
                        lane ^= lane >> 29;
                    }
                    if (take == left) {   // (the unpack's end: the lanes joined, and a word left over)
                        h = (laneA ^ (laneB * 0xC2B2AE3D27D4EB4Full)) * 0x9E3779B97F4A7C15ull;
                        h ^= h >> 29;
                        if (laneHalf)
                            Mix(lanePart);
                        laneHalf = false;
                    }
                }
                for (; i + 2 <= take; i += 2) {   // two words at a time
                    std::uint64_t v;
                    std::memcpy(&v, w + k + i, 8);
                    h = (h ^ v) * 0x9E3779B97F4A7C15ull;
                    h ^= h >> 29;
                }
                for (; i < take; i++)
                    Mix(w[k + i]);
            } else {
                for (std::size_t i = 0; i < take;) {   // four words of every 128
                    const std::uint32_t in = (at + (std::uint32_t)i) & 127;
                    if (in < 4) {
                        Mix(w[k + i]);
                        i++;
                    } else {
                        i += std::min<std::size_t>(take - i, 128 - in);
                    }
                }
            }
            k += take;
            at += (std::uint32_t)take;
            left -= (std::uint32_t)take;
        }
    }
};

std::vector<std::pair<std::uint32_t, std::uint64_t>>* g_diffSegs;
bool PartTte() { return (static_cast<volatile std::uint32_t*>(Native(kVif1))[0] & 0x40) != 0; }

// shape: the packet's VIFcodes and tags alone (and the data of the codes that set VIF1's
// registers), 0 when it has what a part's job must not (microcode loaded, an interrupt): two
// packets of one shape leave VIF1's registers the same.
std::uint64_t PartHash(std::uint32_t target, bool whole, std::uint32_t cl, std::uint32_t wl, bool tte, bool slow,
                       std::vector<const std::uint32_t*>* tex, bool shape, std::vector<const std::uint32_t*>* st, std::uint8_t* maskUse,
                       bool lit, std::vector<LitAt>* litAt, std::vector<const std::uint32_t*>* swayAt, std::vector<MaskAt>* boneAt)
{
    PartHasher hasher;
    hasher.bonesFree = !shape && lit && PartParamsOn() && SkinOn();   // (a part lit by VU1: the skinned ones)
    hasher.bones = boneAt;
    if (slow && !(target & 0x80000000u))
        hasher.chain = At(target, false);
    hasher.swayFree = !shape && PartParamsOn() && SwayOn();
    hasher.sway = swayAt;
    hasher.litFree = lit && !shape;
    hasher.lit = litAt;
    struct MaskOut {   // (bit 0: read as met; bit 1: set)
        PartHasher& h;
        std::uint8_t* out;
        ~MaskOut()
        {
            if (out)
                *out = (std::uint8_t)((h.maskRead ? 1 : 0) | (h.maskSet ? 2 : 0));
        }
    } maskOut{hasher, maskUse};
    // (a part drawn the long way too: its texture offsets left out where they lie in memory that
    // stays, a REF of a mesh's data; 53,000 changes over the 46 levels' tour were these)
    static const bool stLong = !(std::getenv("SLY1_ST_LONG") && std::getenv("SLY1_ST_LONG")[0] == '0');
    hasher.stFree = !shape && PartParamsOn() && (stLong || !slow);
    hasher.st = st;
    hasher.skipHeader = slow && !shape;
    hasher.shape = shape;
    hasher.texFree = !slow && !shape;   // (a part drawn the long way has its packets in the frame's memory: no place to read again)
    hasher.tex = tex;
    hasher.segs = g_diffSegs;
    hasher.whole = whole;
    hasher.cl = cl;
    hasher.wl = wl;
    // An address the walk reads must lie in main memory (or its uncached alias): the chain of a
    // part drawn the long way is in the frame's own packet memory, which the game writes over two
    // frames on, and the helper may come to it after that and find anything in its tags.
    bool wild = false;
    auto inMemory = [&](std::uint32_t addr, std::uint32_t quads) {
        const std::uint32_t a = addr & 0x0FFFFFF0;
        const bool ok = (addr & 0x50000000u) == 0 && a >= 0x10 && a < kRamSize && quads <= (kRamSize - a) / 16;
        wild = wild || !ok;
        return ok;
    };
    auto data = [&](std::uint32_t addr, bool spr, std::uint32_t qwc) {
        if (!spr) {
            if (!inMemory(addr, qwc))
                return;
            hasher.Feed(At(addr, false), (std::size_t)qwc * 4);
        } else {
            for (std::uint32_t i = 0; i < qwc; i++)
                hasher.Feed(At(addr + i * 16, true), 4);
        }
    };
    std::uint32_t tadr = target, stack[2];
    int depth = 0;
    for (int guard = 0; guard < 4096; guard++) {
        const bool tagSpr = (tadr & 0x80000000u) != 0;
        if (wild || (!tagSpr && !inMemory(tadr & 0x7FFFFFFFu, 2)))
            return 0;
        const std::uint32_t* tag = At(tadr & 0x7FFFFFFFu, tagSpr);
        std::uint32_t qwc = tag[0] & 0xFFFF, id = (tag[0] >> 28) & 7, addr = tag[1] & 0x7FFFFFF0;
        bool spr = (tag[1] >> 31) != 0;
        hasher.Mix(tag[0]);
        if (!shape)
            hasher.Mix(tag[1]);
        else if (tag[0] >> 31)
            return 0;
        if (g_diffSegs)
            g_diffSegs->push_back({0xFF000000u | id, tag[0] | (std::uint64_t)tag[1] << 32});
        if (tte)
            hasher.Feed(tag + 2, 2);
        std::uint32_t after = (tadr & 0x7FFFFFFFu) + 16;
        // (a part drawn the long way: its chain's own data is in the frame's memory, written anew
        // each frame at other places; what a REF names stays)
        hasher.stable = !slow || id == 3 || id == 4;
        switch (id) {
        case 1:
            data(after, tagSpr, qwc);
            tadr = (after + qwc * 16) | (tagSpr ? 0x80000000u : 0);
            break;
        case 2: tadr = addr | (spr ? 0x80000000u : 0); break;
        case 3: case 4:
            data(addr, spr, qwc);
            tadr = after | (tagSpr ? 0x80000000u : 0);
            break;
        case 5:
            if (depth >= 2)
                return 0;
            data(after, tagSpr, qwc);
            stack[depth++] = (after + qwc * 16) | (tagSpr ? 0x80000000u : 0);
            tadr = addr | (spr ? 0x80000000u : 0);
            break;
        case 6:
            data(after, tagSpr, qwc);
            if (depth == 0)
                return hasher.left == 0 && !hasher.shapeBad ? (hasher.h | 1) : 0;   // it must end between VIFcodes
            tadr = stack[--depth];
            break;
        default: return 0;   // refe, end: not a called packet's
        }
    }
    return 0;
}

// The part's header as it is in VU1's memory now.
void PartHeader(std::uint32_t tops, std::uint32_t out[44])
{
    const std::uint8_t* d = vu1::Data();
    for (int k = 0; k < 11; k++)
        std::memcpy(out + k * 4, d + ((tops + (std::uint32_t)k) & 0x3FF) * 16, 16);
}

// The renderer told, in turn with what is queued, that a record's batches will not come again.
void PartForget(PartRecord& r)
{
    if (r.keys == 0)
        return;
    GsItem item;
    item.path = 0;
    item.fn = [](std::uint64_t first, std::uint64_t count) { gs::ForgetKept(first, count); };
    item.a = r.keyFirst;
    item.b = r.keys;
    g_gsPending.push_back(std::move(item));
    r.keys = 0;
}

// Whether two headers are the same in what the packet's programs take from them, the matrix
// apart: quadwords 8 (the fog) and 9 (the alpha's scale, the lighting's mode and count, the cull
// flag) and 10's w (the colours' scale). A part drawn from its cache is unlit or lit by the cache
// alone, so the inverse matrix (4 to 7) is not read, and 10's x y z, the centre for a light, are
// what was on the game's stack.
// A part drawn the long way is lit by VU1 from the header: its colours are right only while the
// inverse matrix and the centre are the same too (4 to 10 whole).
bool KeepTwice()
{
    static const bool twice = !std::getenv("SLY1_PART_KEEP_TWICE") || std::getenv("SLY1_PART_KEEP_TWICE")[0] != '0';
    return twice;
}
bool g_headerNearAsked;   // (perfwait's count with SLY1_PART_HDR_EXACT: what the tolerance would have passed)
bool PartHeaderSame(const std::uint32_t a[44], const std::uint32_t b[44], bool slow, bool alphaFree, bool litFree = false)
{
    // (litFree: a part lit by VU1 whose colours the renderer makes from this frame's header and
    // lights: the inverse matrix, the centre and the colours' scale may be any; the fog and the
    // mode and count must be what they were)
    if (litFree)
        return std::memcmp(a + 32, b + 32, 16) == 0 && std::memcmp(a + 37, b + 37, 12) == 0 && (alphaFree || a[36] == b[36]);
    // (alphaFree: the alpha's scale, 9.x, is the renderer's to apply: any will do)
    // (its mode, 9.y: -1 unlit and 0 lit by its cache alone read neither, as a part drawn from its
    // cache; the others are lit by VU1 from them)
    if (slow && !((a[37] == 0xFFFFFFFFu || a[37] == 0) && a[37] == b[37])) {
        if (std::memcmp(a + 16, b + 16, 20 * 4) == 0 && (alphaFree || a[36] == b[36]) && std::memcmp(a + 37, b + 37, 7 * 4) == 0)
            return true;
        // The game makes the inverse matrix and the centre anew each frame, and while the camera
        // moves they come out other in their last digits for an object that stands still (seen
        // walking through A Stealthy Approach: 20 to 45 parts a frame run again for that alone,
        // most of what the render thread did there). Within 1/1024 in the rotation (the cosines a
        // light is weighed by: an eighth of a step of a colour) and a quarter of a unit in the
        // translation and the centre, the part is what it was kept as; the fog, the mode and
        // count and the colours' scale exactly, the alpha's scale as below.
        // SLY1_PART_HDR_EXACT=1: every word the same.
        // Off unless SLY1_PART_HDR_EXACT=0: on the Switch, played, it passed under one part a frame (the
        // parts whose headers are other there do change), so the headers are compared exactly.
        static const bool exactAll = !(std::getenv("SLY1_PART_HDR_EXACT") && std::getenv("SLY1_PART_HDR_EXACT")[0] == '0') || std::getenv("SLY1_PART_HASH_NOW") != nullptr;
        // (SLY1_PART_HDR_EXACT=ab: exact and near in turn, 600 frames each, to compare in one run)
        static const bool turns = std::getenv("SLY1_PART_HDR_EXACT") && std::getenv("SLY1_PART_HDR_EXACT")[0] == 'a';
        const bool exactNow = turns ? gs::FramesShown() / 600 % 2 == 0 : exactAll;
        if ((exactNow && !g_headerNearAsked) || std::memcmp(a + 32, b + 32, 16) != 0 || std::memcmp(a + 37, b + 37, 12) != 0 || a[43] != b[43])
            return false;
        // SLY1_PART_HDR_NEAR=rotation,position[,inverse]: how far (default 1/1024 and a quarter of a unit).
        // A part of something that sways or turns a little each frame is then drawn with the
        // lighting it was kept with until it has turned that far: its vertices are placed by this
        // frame's matrix as ever, only the colours VU1 lit them with are a few frames old.
        static const struct Near {
            // (inverse: the inverse matrix's fourth row, which the routines that light a model's
            // part do not read, and which moves far when an object far from the origin turns a little)
            float rotation = 1.0f / 1024, position = 0.25f, inverse = 0.25f;
            Near()
            {
                if (const char* e = std::getenv("SLY1_PART_HDR_NEAR"))
                    if (std::sscanf(e, "%f,%f,%f", &rotation, &position, &inverse) < 3)
                        inverse = position;
            }
        } tolerance;
        const auto near = [&](int k, float by) {
            if (a[k] == b[k])
                return true;
            float x, y;
            std::memcpy(&x, a + k, 4);
            std::memcpy(&y, b + k, 4);
            return x - y < by && y - x < by;
        };
        for (int k = 16; k < 28; k++)
            if (!near(k, tolerance.rotation))
                return false;
        for (int k = 28; k < 32; k++)
            if (!near(k, tolerance.inverse))
                return false;
        for (int k = 40; k < 43; k++)
            if (!near(k, tolerance.position))
                return false;
        return alphaFree || near(36, 1.0f / 512);
    }
    if (std::memcmp(a + 32, b + 32, 16) != 0 || std::memcmp(a + 37, b + 37, 12) != 0 || a[43] != b[43])
        return false;
    if (a[36] == b[36] || alphaFree)
        return true;
    // 9.x, the scale of the vertices' alpha: a part that fades with its distance has it other in
    // its last digits each frame while nothing moves. Within 1/512 of what the part was kept with
    // (a quarter of a step of the GS's alpha) it is drawn as kept; SLY1_PART_HASH_NOW=1: never.
    static const bool exact = std::getenv("SLY1_PART_HASH_NOW") != nullptr;
    float x, y;
    std::memcpy(&x, a + 36, 4);
    std::memcpy(&y, b + 36, 4);
    return !exact && x - y < 1.0f / 512 && y - x < 1.0f / 512;
}

// SLY1_PART_DIFF=1 (with SLY1_PART_HASH_NOW=1: the helper must not hash meanwhile): what changes in
// the parts that change, and why a part is not kept. Each call's packet is hashed a VIFcode
// and a tag at a time and compared with the same part's the list before; what differs is counted
// by kind (PartDiffReport, with partcounts).
struct DiffSeen {
    std::vector<std::pair<std::uint32_t, std::uint64_t>> segs;
    std::uint32_t header[44];
};
std::unordered_map<std::uint32_t, DiffSeen> g_diffSeen;
std::atomic<bool> g_diffSeenClear{false};
void PartDiff(std::uint32_t key, std::uint32_t target, std::uint32_t cl, std::uint32_t wl, bool tte, bool slow, const std::uint32_t* header)
{
    DiffSeen now;
    g_diffSegs = &now.segs;
    PartHash(target, true, cl, wl, tte, slow);
    g_diffSegs = nullptr;
    std::memcpy(now.header, header, sizeof now.header);
    if (g_diffSeenClear.exchange(false))
        g_diffSeen.clear();
    auto it = g_diffSeen.find(key);
    char kindName[24];
    std::snprintf(kindName, sizeof kindName, slow ? "long %d" : "cached %d", (int)header[37]);   // (with its lighting's mode)
    const char* kind = kindName;
    DiffCount(std::string(kind) + " calls");
    {
        char name[64];
        std::snprintf(name, sizeof name, "%s calls of mode %d", kind, (int)header[37]);
        DiffCount(name);
    }
    if (it != g_diffSeen.end()) {
        const DiffSeen& was = it->second;
        char name[96];
        bool any = false;
        if (was.segs.size() != now.segs.size()) {
            DiffCount(std::string(kind) + " other shape");
            any = true;
        } else {
            std::set<std::string> kinds;
            for (std::size_t k = 0; k < now.segs.size(); k++) {
                const std::uint32_t c = now.segs[k].first;
                if (was.segs[k].first != c) {
                    std::snprintf(name, sizeof name, "%s code %08X", kind, (c >> 24) == 0xFF ? c : c & 0xFF000000u);
                    kinds.insert(name);
                } else if (was.segs[k].second != now.segs[k].second) {
                    if ((c >> 24) == 0xFF)
                        std::snprintf(name, sizeof name, "%s tag %u", kind, c & 7);
                    else if (((c >> 24) & 0x60) == 0x60)
                        std::snprintf(name, sizeof name, "%s unpack %02X to %03X%s num %u", kind, (c >> 24) & 0x6F, c & 0x3FF, (c & 0x8000) ? "+" : "",
                                      (c >> 16) & 0xFF);
                    else
                        std::snprintf(name, sizeof name, "%s data of %02X", kind, (c >> 24) & 0x7F);
                    kinds.insert(name);
                }
            }
            for (const std::string& n : kinds)
                DiffCount(n);
            any = !kinds.empty();
        }
        for (int q = 4; q < 11; q++)
            if (std::memcmp(was.header + q * 4, now.header + q * 4, 16) != 0) {
                int mask = 0;
                for (int w = 0; w < 4; w++)
                    mask |= (was.header[q * 4 + w] != now.header[q * 4 + w]) << w;
                std::snprintf(name, sizeof name, "%s header %d words %X%s", kind, q, q >= 8 ? mask : 0, any ? " (packet too)" : " (packet same)");
                DiffCount(name);
            }
        if (any)
            DiffCount(std::string(kind) + " packet changed");
    } else {
        DiffCount(std::string(kind) + " first seen");
    }
    g_diffSeen[key] = std::move(now);
}
void PartDiffReport()
{
    std::lock_guard<std::mutex> lk(g_diffLock);
    for (const auto& kv : g_diffCounts)
        std::fprintf(stderr, "partdiff: %-44s %llu\n", kv.first.c_str(), (unsigned long long)kv.second);
    g_diffCounts.clear();
    g_diffSeenClear = true;   // (the render thread's: cleared there, at its next part)
}

// The hook's NOP and then the call of target: true when the part has been drawn from what was
// kept, and the call is not to be followed.
// The lighting of a part lit by VU1, for this frame: each of its lights as the programs make it
// before their loop (0x142 for a point light: the direction from the object's centre, the scale
// and base faded by the distance there; then 0x0FE: the direction turned into the part's own
// space by the header's inverse matrix and made unit length). start is 3 of VU1's data memory
// as it is; a batch adds its own to the second (gs::Batch::lit), and has vf01.
bool PartLitNow(const PartRecord& r, const std::uint32_t* header, gs::LitParams* out)
{
    if (r.lights.empty() || r.lights.size() > 4)
        return false;
    const float* h = reinterpret_cast<const float*>(header);
    const float* rows = h + 16;      // quadwords 4, 5, 6
    const float* centre = h + 40;    // quadword 10
    gs::LitParams p;
    p.lights = (std::uint32_t)r.lights.size();
    for (std::size_t k = 0; k < r.lights.size(); k++) {
        const PartRecord::LitSource& s = r.lights[k];
        gs::LitLight& l = p.light[k];
        float dir[3], fade = 1.0f;
        const float* scale = s.scale, * base = s.base;
        static const float kOutside[3] = {23.0f, 23.0f, 23.0f};
        if (s.view) {
            // a spot light (0x154): the object's centre through its view and projection; outside
            // the view the program goes on as a directional light from 25 (the first ramp) with a
            // scale of 23 (0x179, as the microcode has it); inside, the cone's factor at the centre
            // scales the light, its direction the spot's (27)
            const float* m = s.view;
            float c[4];
            for (int i = 0; i < 4; i++)
                c[i] = m[i] * centre[0] + m[4 + i] * centre[1] + m[8 + i] * centre[2] + m[12 + i];
            const bool outside = !(c[3] > 0.0f) || std::fabs(c[0]) > c[3] || std::fabs(c[1]) > c[3] || std::fabs(c[2]) > c[3];
            if (outside) {
                std::memcpy(dir, s.ramp, 12);
                scale = kOutside;
            } else {
                const float q = 1.0f / c[3];
                const float x = std::fabs(c[0] * q), y = std::fabs(c[1] * q);
                const float p[4] = {x, y, x * x + y * y, c[3]};
                float f[4];
                for (int i = 0; i < 4; i++) {
                    f[i] = s.ramp[4 + i] * p[i] + s.ramp[i];
                    f[i] = f[i] < 1.0f ? f[i] : 1.0f;
                    f[i] = f[i] > 0.0f ? f[i] : 0.0f;
                }
                fade = f[0] * f[3] * f[2] * f[1];
                std::memcpy(dir, s.dir, 12);
            }
        } else if (s.place) {
            float v[3] = {s.place[0] - centre[0], s.place[1] - centre[1], s.place[2] - centre[2]};
            const float len2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
            const float over = len2 == 0.0f ? 3.4028235e38f : 1.0f / std::sqrt(len2);
            fade = s.atten[0] + over * s.atten[1];
            fade = fade > 0.0f ? (fade < 1.0f ? fade : 1.0f) : 0.0f;
            for (int i = 0; i < 3; i++)
                dir[i] = v[i] * over;
        } else {
            std::memcpy(dir, s.dir, 12);
        }
        float turned[3];
        for (int i = 0; i < 3; i++)
            turned[i] = rows[i] * dir[0] + rows[4 + i] * dir[1] + rows[8 + i] * dir[2];
        const float len2 = turned[0] * turned[0] + turned[1] * turned[1] + turned[2] * turned[2];
        const float over = len2 == 0.0f ? 3.4028235e38f : 1.0f / std::sqrt(len2);
        for (int i = 0; i < 3; i++) {
            l.dir[i] = turned[i] * over;
            l.scale[i] = scale[i] * fade;
            l.base[i] = base[i] * fade;
            l.colour[i] = s.colour[i];
        }
    }
    std::memcpy(p.start, vu1::Data() + 3 * 16, 8);
    p.one[0] = p.one[1] = p.one[2] = 1.0f;   // (the batch's own: PartReplay's draw)
    p.colourScale = h[43];
    *out = p;
    return true;
}

// What PartDecide found of the call, for PartCall to make a job of it.
std::uint32_t g_callKey, g_callHeader[44], g_callHeaderAt;
Vif::Regs g_callRegs;
bool g_callTte;

// SLY1_PART_CEILING=1 (=ab: in turn with the usual, 600 frames each): a part once kept is drawn
// again as it was kept whatever has changed in it since, and one seen in the list before is kept
// whatever it was then. The picture is wrong (stale lighting, poses and fades); the work is what
// a renderer that never ran a part's packet again would leave: the ceiling of that, measured
// before it is built.
bool PartCeiling()
{
    static const char* const e = std::getenv("SLY1_PART_CEILING");
    return e && (e[0] != 'a' || gs::FramesShown() / 600 % 2 == 1);
}

bool PartDecide(std::uint32_t marker, std::uint32_t target, bool slow)
{
    const bool ceiling = PartCeiling();
    int keepWhy = 0;
    const auto alphaFree = [](const PartRecord& r) { return PartParamsOn() && !r.alphaStrict; };
    if (!PartReplayOn() || !g_vif1.BetweenCodes())
        return false;
    // A part drawn the long way (the game's own packets for its meshes, lit by VU1 each frame; the
    // second hook makes them a chain called from the list): kept and drawn again as a part drawn
    // from its cache is, with two differences. Its header is unpacked inside the chain, so the
    // header is not compared apart (the hash covers it) and is taken from VU1's memory when the
    // part's first batch is recorded; and when the chain is left out the header is put where the
    // chain would have put it, for what the game draws of the part afterwards (its strips).
    // SLY1_PART_SLOW=0: such parts run as ever.
    static const bool noSlow = std::getenv("SLY1_PART_SLOW") && std::getenv("SLY1_PART_SLOW")[0] == '0';
    if (slow && noSlow)
        return false;
    // A part drawn more than once in a list under one name (the same light cache for two
    // requests) is a part of its own each time, by its turn in the list: under one key the
    // drawings took each other's place and each was found changed every frame.
    std::uint32_t key = (marker & 0x7FFFFF) | (slow ? 0x800000u : 0u);
    {
        std::uint8_t& turn = g_partTurns[key];
        key |= (std::uint32_t)std::min<std::uint8_t>(turn, 127) << 24;
        if (turn < 255)
            turn++;
    }
    const Vif::Regs regs = g_vif1.GetRegs();
    // (a part drawn the long way begins by setting BASE and OFFSET, checked below: what those
    // registers, TOPS and the double buffer's flag were before it is nothing to it. The flag is
    // other by how many programs the part before it ran.)
    // (maskFree: the part's packet does not unpack through the mask it is met with)
    const auto sameRegs = [&](const Vif::Regs& was, bool maskFree = false) {
        Vif::Regs x = was;
        if (maskFree)
            x.mask = regs.mask;
        if (slow) {
            x.base = regs.base;
            x.ofst = regs.ofst;
            x.tops = regs.tops;
            x.dbf = regs.dbf;
        }
        return x == regs;
    };
    const auto regsCensus = [&](const Vif::Regs& a) {   // SLY1_PART_REGS_LOG: which registers are other (partdiff:)
        static const bool regsLog = std::getenv("SLY1_PART_REGS_LOG") != nullptr;
        if (regsLog && !sameRegs(a)) {
            char name[96];
            std::snprintf(name, sizeof name, "regs other (%s):%s%s%s%s%s%s%s%s%s%s", slow ? "long" : "cached", a.cl != regs.cl || a.wl != regs.wl ? " cycle" : "",
                          a.mask != regs.mask ? " mask" : "", a.mode != regs.mode ? " mode" : "", a.base != regs.base ? " base" : "", a.ofst != regs.ofst ? " ofst" : "",
                          a.tops != regs.tops ? " tops" : "", a.itops != regs.itops ? " itops" : "", a.dbf != regs.dbf ? " dbf" : "",
                          std::memcmp(a.row, regs.row, sizeof a.row) ? " row" : "", std::memcmp(a.col, regs.col, sizeof a.col) ? " col" : "");
            DiffCount(name);
        }
    };
    // The game turns VIF1's two buffers about from mesh to mesh, so a part comes in either half
    // by what was drawn before it (its packet is another one then, and the registers it leaves):
    // a part of its own for each half. Under one key a part was kept anew whenever something
    // drawn before it came into view or left it. SLY1_PART_HALVES=0: one.
    {
        static const bool one = std::getenv("SLY1_PART_HALVES") && std::getenv("SLY1_PART_HALVES")[0] == '0';
        if (!one && regs.base != 0x28)
            key |= 0x80000000u;
    }
    std::uint32_t header[44], headerAt = 0;
    if (slow) {
        // the header where the chain has it: its first tag's data begins BASE, OFFSET, and the
        // unpack of the header's 11 quadwords at TOPS (which OFFSET makes BASE)
        if (target & 0x80000000u)
            return false;
        const std::uint32_t* tag = At(target, false);
        const std::uint32_t* d = tag + 4;
        if (((tag[0] >> 28) & 7) != 1 || (tag[0] & 0xFFFF) < 12 || (d[0] >> 24) != 0x03 || (d[1] >> 24) != 0x02 ||
            ((d[2] >> 24) & 0x6F) != 0x6C || ((d[2] >> 16) & 0xFF) != 0x0B)
            return false;
        headerAt = ((d[2] & 0x8000) ? (d[0] & 0x3FF) : 0) + (d[2] & 0x3FF);
        std::memcpy(header, d + 3, sizeof header);
    } else {
        PartHeader(regs.tops, header);
    }
    const bool tte = PartTte();
    g_callKey = key;
    std::memcpy(g_callHeader, header, sizeof header);
    g_callHeaderAt = headerAt;
    g_callRegs = regs;
    g_callTte = tte;
    // (a part lit by VU1: its lights' words are left out of its hash, PartHasher::litFree)
    const bool litMode = slow && header[37] == 4 && PartParamsOn() && LitReplayOn();
    const auto litFree = [&](const PartRecord& r) { return litMode && !r.litStrict; };
    static const bool diffOn = std::getenv("SLY1_PART_DIFF") != nullptr && std::getenv("SLY1_PART_HASH_NOW") != nullptr;
    if (diffOn)
        PartDiff(key, target, regs.cl, regs.wl, tte, slow, header);
    auto it = g_parts.find(key);
    std::uint64_t hash = 0;
    bool late = false;   // the hash is what the helper last found, not this frame's
    if (PartHashLate() && it != g_parts.end()) {
        HashTakeResults();
        PartRecord& r = it->second;
        if (!ceiling && !r.ok && !r.moving && r.notBefore > gs::FramesShown()) {
            g_partsFickle++;
            return false;   // one that keeps changing, left alone for now: no hash needed
        }
        // (a part drawn the long way whose hash the helper has not made in time for four lists is
        // hashed here: it is not drawn again unseen for longer)
        if ((r.ok || r.moving) && gs::FramesShown() >= r.watchUntil && !(slow && g_listSerial - r.verdictList > 4)) {
            late = true;
            hash = r.seen ? r.seen : r.hash;
            if (!r.asked) {
                r.asked = true;
                const bool whole = r.ok && (r.verified < 8 || ((gs::FramesShown() + key) & 31) == 0);
                g_hashPending.push_back({key, r.serial, target, regs.cl, regs.wl, tte, whole, slow, g_listSerial, litMode});
                if (g_hashPending.size() >= 16)
                    HashPassOn();
            }
        }
    }
    auto hashNow = [&] {
        const std::uint64_t h0 = NowNs();
        const std::uint64_t h = PartHash(target, false, regs.cl, regs.wl, tte, slow, nullptr, false, nullptr, nullptr, litMode);
        g_partHashNs += NowNs() - h0;
        return h;
    };
    // A part whose header or registers are neither what it was kept with nor what the list before
    // had is run as ever without being hashed: it cannot be drawn again, and it is not kept until
    // two lists running have it the same (below). Hashing such parts here was a tenth of the render
    // thread's work walking on the Switch (the helper's hashes come a list late for them).
    if (!late && it != g_parts.end() && !it->second.moving && KeepTwice()) {
        PartRecord& r = it->second;
        const bool asKept = r.ok && (ceiling || (sameRegs(r.before, !r.maskRead) && PartHeaderSame(r.header, header, slow, alphaFree(r), litFree(r))));
        const bool asSeen = r.seenList != 0 && r.seenList + 1 >= g_listSerial && (ceiling || (sameRegs(r.seenRegs, r.ok && !r.maskRead) && PartHeaderSame(r.seenHeader, header, slow, alphaFree(r), litFree(r))));
        if (!asKept && !asSeen) {
            if (r.ok) {
                g_partsStale++;
                g_partsStaleBy[sameRegs(r.before, !r.maskRead) ? 2 : 1]++;
                regsCensus(r.before);
            }
            {   // (SLY1_PERF_LOG's perfwait: why it is not what the list before had)
                const std::uint32_t* a = r.seenHeader;
                const bool seen = r.seenList != 0 && r.seenList + 1 >= g_listSerial;
                g_waitWhy[slow ? 1 : 0]++;
                if (!seen) {
                    g_waitWhy[2]++;
                } else {
                    if (!(sameRegs(r.seenRegs, r.ok && !r.maskRead)))
                        g_waitWhy[3]++;
                    if (!PartHeaderSame(a, header, slow, alphaFree(r), litFree(r))) {
                        g_waitWhy[4]++;
                        g_headerNearAsked = true;
                        if (PartHeaderSame(a, header, slow, alphaFree(r), litFree(r)))
                            g_waitWhy[11]++;
                        g_headerNearAsked = false;
                        {   // SLY1_PART_HDR_LOG=frame: what is other in the headers, from that frame on (the first 60)
                            static const long from = std::getenv("SLY1_PART_HDR_LOG") ? std::atol(std::getenv("SLY1_PART_HDR_LOG")) : -1;
                            static int said;
                            if (from >= 0 && (long)gs::FramesShown() >= from && said < 60) {
                                said++;
                                float x[44], y[44];
                                std::memcpy(x, a, sizeof x);
                                std::memcpy(y, header, sizeof y);
                                std::fprintf(stderr, "hdr: frame %llu part %08X slow %d mode %d:", (unsigned long long)gs::FramesShown(), marker, (int)slow, (int)header[37]);
                                for (int k = 16; k < 44; k++)
                                    if (a[k] != header[k])
                                        std::fprintf(stderr, " [%d] %.6g -> %.6g", k, x[k], y[k]);
                                std::fprintf(stderr, "\n");
                            }
                        }
                        if (std::memcmp(a + 16, header + 16, 64) != 0) g_waitWhy[5]++;
                        if (std::memcmp(a + 32, header + 32, 16) != 0) g_waitWhy[6]++;
                        if (a[36] != header[36]) g_waitWhy[7]++;
                        if (std::memcmp(a + 37, header + 37, 12) != 0) g_waitWhy[8]++;
                        if (std::memcmp(a + 40, header + 40, 12) != 0) g_waitWhy[9]++;
                        if (a[43] != header[43]) g_waitWhy[10]++;
                    }
                }
            }
            r.seenHash = 0;
            r.seenList = g_listSerial;
            r.seenRegs = regs;
            std::memcpy(r.seenHeader, header, sizeof r.seenHeader);
            g_partsWaited++;
            return false;
        }
    }
    if (!late)
        hash = hashNow();
    if (it != g_parts.end() && it->second.moving && it->second.hash == hash) {
        PartRecord& r = it->second;
        if (gs::FramesShown() < r.movingUntil) {
            g_partsMoving++;
            return false;   // run as ever, and not kept
        }
        keepWhy = 4;
        r.moving = false;   // tried again: kept anew below
        r.ok = false;
    }
    if (it != g_parts.end() && it->second.ok) {
        PartRecord& r = it->second;
        bool same = hash != 0 && r.hash == hash && sameRegs(r.before, !r.maskRead) && PartHeaderSame(r.header, header, slow, alphaFree(r), litFree(r));
        std::uint64_t texNow[4];
        bool texMoved = false;
        for (std::size_t k = 0; same && k < r.texAt.size(); k++) {
            const std::uint32_t* q = r.texAt[k];
            std::uint64_t v;
            std::memcpy(&v, q, 8);
            same = (q[2] & 0xFF) == 0x06 && q[3] == 0 && (!r.texStrict || v == r.texWas[k]);
            if (!r.texStrict) {
                texNow[k] = v;
                texMoved = texMoved || v != r.texWas[k];
                for (std::size_t j = 0; j < k; j++)   // two that were one texture are still one
                    same = same && (r.texWas[j] != r.texWas[k] || texNow[j] == v);
            }
        }
        // the texture offsets left out of the hash, as the packet has them now
        PartRecord::St stNow[8];
        std::uint32_t stCount = 0;
        if (same && !r.stAt.empty()) {
            // (a cached packet's offsets are read where they were found; a part drawn the long way
            // has a chain of its own each frame, its offsets in REFs of its meshes' data, which the
            // hash's tags say are the same)
            same = r.slow || r.stTarget == target;
            for (std::size_t k = 0; same && k < r.stAt.size(); k++) {
                PartRecord::St v;
                if (!r.stAt[k]) {   // (one in the hash: as it was)
                    if (!r.stStrict)
                        stNow[k] = r.stWas[k];
                    continue;
                }
                std::memcpy(&v, r.stAt[k], 8);
                if (r.stStrict)
                    same = std::memcmp(&v, &r.stWas[k], 8) == 0;
                else
                    stNow[k] = v;
            }
            if (same && !r.stStrict)
                stCount = (std::uint32_t)r.stAt.size();
        }
        // a mesh's sway: its rows as the packet has them now, turned into the part's space by the
        // header's 4 to 7 as 0x22B turns them, or the words as they were (swayStrict)
        float swayNow[16][12];
        std::uint32_t projNow[16][24];   // each unpack to 22 as 0x076 makes of it: its matrix, colour and limits
        std::uint32_t swayCount = 0;
        if (same && !r.swayAt.empty()) {
            if (r.swayStrict) {
                for (std::size_t k = 0; same && k < r.swayAt.size(); k++)
                    same = !r.swayAt[k] || std::memcmp(r.swayAt[k], &r.swayWas[k * 24], 96) == 0;   // (a null one is in the hash)
            } else {
                const float* h = reinterpret_cast<const float*>(header);
                const float* g = reinterpret_cast<const float*>(vu1::Data() + 8 * 16);   // (0x076's: data memory 8 to 11)
                for (std::size_t k = 0; k < r.swayAt.size() && k < 16; k++) {
                    const float* rows = reinterpret_cast<const float*>(r.swayAt[k]);
                    float m[16];
                    for (int q = 0; q < 4; q++)   // 0x22B: result row q the rows weighted by g's row q
                        for (int c = 0; c < 4; c++)
                            m[q * 4 + c] = rows[c] * g[q * 4] + rows[4 + c] * g[q * 4 + 1] + rows[8 + c] * g[q * 4 + 2] + rows[12 + c] * g[q * 4 + 3];
                    std::memcpy(projNow[k], m, 64);
                    std::memcpy(projNow[k] + 16, r.swayAt[k] + 16, 32);
                    for (int i = 0; i < 4; i++)
                        for (int c = 0; c < 3; c++)
                            swayNow[k][i * 3 + c] = rows[i * 4] * h[16 + c] + rows[i * 4 + 1] * h[20 + c] + rows[i * 4 + 2] * h[24 + c] + rows[i * 4 + 3] * h[28 + c];
                }
                swayCount = (std::uint32_t)std::min<std::size_t>(r.swayAt.size(), 15);
            }
        }
        // a skinned part's bones: where they are in this frame's chain, as they are now, or as they
        // were (boneStrict)
        static std::vector<const std::uint32_t*> boneNow;   // (the render thread's)
        std::uint32_t boneCount = 0;
        if (same && !r.boneAt.empty()) {
            const std::uint32_t* chain = slow && !(target & 0x80000000u) ? At(target, false) : nullptr;
            const bool reach = r.boneAt.size() <= 255;
            boneNow.resize(r.boneAt.size());
            for (std::size_t k = 0; same && k < r.boneAt.size(); k++) {
                const MaskAt& m = r.boneAt[k];
                const std::uint32_t* now = m.fromChain >= 0 ? (chain ? chain + m.fromChain : nullptr) : m.p;
                if (!m.p) {   // (one in the hash)
                    now = nullptr;
                } else if (!now || now[-1] != m.code) {   // (the chain laid out otherwise: run as ever)
                    same = false;
                    g_bonesMislaid++;
                    break;
                }
                if (r.boneStrict)
                    same = !now || std::memcmp(now, &r.boneWas[k * 16], 64) == 0;
                else if (reach)
                    boneNow[k] = now ? now : &r.boneWas[k * 16];
            }
            if (same && !r.boneStrict && reach)
                boneCount = (std::uint32_t)r.boneAt.size();
            {   // SLY1_SKIN_CHECK=5: the bones found by their place in the chain against a walk of it now
                static const bool check = std::getenv("SLY1_SKIN_CHECK") && std::getenv("SLY1_SKIN_CHECK")[0] == '5';
                if (check && boneCount) {
                    std::vector<MaskAt> walked;
                    PartHash(target, false, regs.cl, regs.wl, tte, slow, nullptr, false, nullptr, nullptr, litMode, nullptr, nullptr, &walked);
                    static std::uint64_t parts, wrong, said;
                    parts++;
                    bool bad = walked.size() != r.boneAt.size();
                    for (std::size_t k = 0; !bad && k < walked.size(); k++)
                        bad = walked[k].p != boneNow[k];
                    if (bad) {
                        wrong++;
                        if (said++ < 6)
                            std::fprintf(stderr, "skin check 5: %zu bones walked, %zu kept; first other at %zd\n", walked.size(), r.boneAt.size(),
                                         walked.size() == r.boneAt.size() ? std::distance(walked.begin(), std::mismatch(walked.begin(), walked.end(), boneNow.begin(), [](const MaskAt& m, const std::uint32_t* p) { return m.p == p; }).first) : (std::ptrdiff_t)-1);
                    }
                    if ((parts & (parts - 1)) == 0)
                        std::fprintf(stderr, "skin check 5: %llu parts, %llu with bones elsewhere than their places say\n", (unsigned long long)parts, (unsigned long long)wrong);
                }
            }
        }
        // a part lit by VU1: its lights' words as the packet has them now (the same packets' words:
        // the hash covers the tags), or the lighting's parameters made of them for this frame
        gs::LitParams litNow;
        bool hasLit = false;
        if (same && !r.litAt.empty()) {
            if (r.litStrict || !litMode) {
                std::size_t at = 0;
                for (const LitAt& l : r.litAt) {
                    same = same && std::memcmp(l.p, &r.litWas[at], l.words * 4) == 0;
                    at += l.words;
                }
            } else {
                hasLit = PartLitNow(r, header, &litNow);
                same = hasLit;
                // SLY1_LIT_NOW_CHECK=1 (with SLY1_PART_JOBS=0): the part is run here all the same, and
                // the handlers compare what the programs light it with against this (vu1::LitExpect)
                static const bool check = std::getenv("SLY1_LIT_NOW_CHECK") != nullptr;
                if (check && hasLit) {
                    static gs::LitParams expect;
                    expect = litNow;
                    vu1::LitExpect(&expect);
                    return false;
                }
            }
        }
        if (ceiling) {
            same = r.kept != nullptr;   // (a record without its kept items faulted here at the end of the one run)
            texMoved = false;
            stCount = 0;
            hasLit = false;
            swayCount = 0;
            boneCount = 0;
        }
        if (!same)
            regsCensus(r.before);
        if (!same)   // why, for the report: the hash, the registers, or the header
            g_partsStaleBy[keepWhy = (r.hash != hash ? 0 : !(sameRegs(r.before, !r.maskRead)) ? 1 : 2)]++, keepWhy++;
        const bool wholeDue = !ceiling && (late ? r.wholeArrived : (r.verified < 8 || ((gs::FramesShown() + key) & 31) == 0));
        if (same && wholeDue) {
            const std::uint64_t whole = late ? r.seenWhole : PartHash(target, true, regs.cl, regs.wl, tte, slow, nullptr, false, nullptr, nullptr, litMode);
            r.wholeArrived = false;
            if (whole == r.hashWhole) {
                r.verified++;
            } else {
                r.moving = true;
                r.movingWait = std::min<std::uint32_t>(std::max<std::uint32_t>(r.movingWait * 2, 120), 3840);
                r.movingUntil = gs::FramesShown() + r.movingWait;
                r.kept.reset();
                PartForget(r);
                g_partWords -= std::min<std::uint64_t>(g_partWords, r.words);
                r.words = 0;
                g_partsMoving++;
                return false;
            }
        }
        if (same) {
            // one item for the part: the GS thread goes through what was kept (an item a packet and
            // a batch, each with memory of its own, cost the Switch as much as running the part)
            GsItem item;
            item.path = 5;
            item.part = r.kept;
            item.words = GsSpare();
            item.words.resize(28 + 2 * stCount + (hasLit ? gs::kLitParamWords : 0) + 12 * swayCount + 16 * boneCount + 24 * swayCount);
            float matrix[16];
            if (r.slow) {
                vu1::PartMatrixFrom(reinterpret_cast<const float*>(header), matrix);
                std::uint8_t* d = vu1::Data();
                for (int k = 0; k < 11; k++)
                    std::memcpy(d + ((headerAt + (std::uint32_t)k) & 0x3FF) * 16, header + k * 4, 16);
                g_partsSlowReplayed++;
            } else {
                vu1::PartMatrix(regs.tops, matrix);
            }
            std::memcpy(&item.words[0], matrix, sizeof matrix);
            std::memcpy(&item.words[16], vu1::Data(), 32);
            item.words[24] = vu1::TakeBatchIds(r.ids);
            item.words[25] = r.firstId;
            // this frame's numbers for the renderer: the alpha's scale, and how far each texture
            // offset is from what the kept vertices have in them
            item.words[26] = header[36];
            item.words[27] = stCount | (hasLit ? 0x10000u : 0u) | swayCount << 20 | boneCount << 24;
            if (swayCount)   // (after the bones: what 0x076 lays each unpack to 22 on with)
                std::memcpy(&item.words[28 + 2 * stCount + (hasLit ? gs::kLitParamWords : 0) + 12 * swayCount + 16 * boneCount], projNow, 96 * swayCount);
            if (hasLit) {
                std::memcpy(&item.words[28 + 2 * stCount], &litNow, sizeof litNow);
                g_partsLitNow++;
            }
            if (swayCount) {
                std::memcpy(&item.words[28 + 2 * stCount + (hasLit ? gs::kLitParamWords : 0)], swayNow, 48 * swayCount);
                g_partsSwayed++;
            }
            if (boneCount) {
                std::uint32_t* to = &item.words[28 + 2 * stCount + (hasLit ? gs::kLitParamWords : 0) + 12 * swayCount];
                for (std::uint32_t k = 0; k < boneCount; k++)
                    std::memcpy(to + 16 * k, boneNow[k], 64);
                g_partsSkinned++;
                static const bool verify = std::getenv("SLY1_SKIN_VERIFY") != nullptr;
                if (verify) {   // what each kept batch would be drawn with, then the part run as ever
                    g_skinVerify.on = true;
                    g_skinVerify.next = 0;
                    g_skinVerify.predicted.clear();
                    g_skinVerify.colours.clear();
                    g_skinVerify.normals.clear();
                    for (const PartKept& kk : *r.kept) {
                        if (kk.path != 4)
                            continue;
                        gs::Batch b;
                        std::memcpy(&b, &kk.data[1], sizeof b);
                        b.quads = kk.data + kBatchHead;
                        std::vector<std::uint32_t> pos((std::size_t)b.count * 3, 0);
                        if (b.skinWords) {
                            b.skin = b.quads + (std::size_t)b.count * b.stride * 4 + b.litWords + b.swayWords;
                            float mats[64] = {};
                            for (int j = 0; j < 4; j++)
                                if (b.boneSource[j] < boneCount)
                                    std::memcpy(mats + 16 * j, &to[16 * b.boneSource[j]], 64);
                            std::vector<std::uint32_t> q((std::size_t)b.count * b.stride * 4);
                            std::vector<std::uint32_t> lit;
                            float* normals = nullptr;
                            if (b.litWords && b.skin[1]) {
                                const std::uint32_t* l = b.quads + (std::size_t)b.count * b.stride * 4;
                                lit.assign(l, l + b.litWords);
                                normals = reinterpret_cast<float*>(lit.data() + gs::kLitHead);
                            }
                            gs::SkinQuads(b, mats, q.data(), normals);
                            for (std::uint32_t k = 0; k < b.count; k++)
                                std::memcpy(&pos[k * 3], &q[(k * b.stride + b.positionAt) * 4], 12);
                            std::vector<std::uint32_t> col;
                            if (hasLit && !lit.empty()) {   // the kick's colours as the GS thread would make them
                                gs::LitParams now = litNow, was;
                                std::memcpy(&was, lit.data(), sizeof was);
                                float add;
                                std::memcpy(&add, lit.data() + gs::kLitParamWords, 4);
                                now.start[1] += add;
                                std::memcpy(now.one, was.one, sizeof now.one);
                                if (now.lights == was.lights) {
                                    b.lit = lit.data();
                                    b.litNow = &now;
                                    col.resize((std::size_t)b.count * 4);
                                    gs::LitColoursNow(b, col.data());
                                }
                            }
                            g_skinVerify.colours.push_back(std::move(col));
                            g_skinVerify.normals.emplace_back(normals ? normals : nullptr, normals ? normals + (std::size_t)b.count * 3 : nullptr);
                        } else {
                            for (std::uint32_t k = 0; k < b.count; k++)
                                std::memcpy(&pos[k * 3], &b.quads[(k * b.stride + b.positionAt) * 4], 12);
                        }
                        if (g_skinVerify.colours.size() < g_skinVerify.predicted.size() + 1)
                            g_skinVerify.colours.emplace_back();
                        g_skinVerify.predicted.push_back(std::move(pos));
                    }
                    return false;
                }
            }
            for (std::uint32_t k = 0; k < stCount; k++) {
                const float d[2] = {stNow[k].s - r.stWas[k].s, stNow[k].t - r.stWas[k].t};
                std::memcpy(&item.words[28 + 2 * k], d, 8);
                g_partsStMoved += d[0] != 0.0f || d[1] != 0.0f;
            }
            g_partsAlphaMoved += header[36] != r.header[36];
            if (texMoved) {   // the kept packets' TEX0 writes with what the packet has now
                for (const PartRecord::TexFix& f : r.texFix) {
                    item.words.push_back(f.item);
                    item.words.push_back(f.word);
                    item.words.push_back((std::uint32_t)texNow[f.which]);
                    item.words.push_back((std::uint32_t)(texNow[f.which] >> 32));
                }
                g_partsTexMoved++;
            }
            if ((hasLit || swayCount || boneCount) && PrepOn()) {   // the per-frame work made ahead by the helper
                std::shared_ptr<PrepTask> prep = PrepNew();
                prep->part = r.kept;
                prep->words.assign(item.words.begin(), item.words.end());
                std::lock_guard<std::mutex> lk(g_prepLock);
                const std::uint32_t at = g_prepCount.load(std::memory_order_acquire);
                if (at < kListPreps) {
                    g_preps[at] = prep;
                    g_prepCount.store(at + 1, std::memory_order_release);
                }
                item.prep = std::move(prep);
            }
            g_gsPending.push_back(std::move(item));
            if (g_gsPending.size() >= 24)
                GsPassOn();
            {
                Vif::Regs after = r.after;
                if (!r.maskSet)   // (met with another mask than it was kept with, and it leaves the mask alone)
                    after.mask = regs.mask;
                g_vif1.SetRegs(after);
            }
            vu1::PartSkipped();
            r.stale = 0;
            if (r.lockout != 0 && ++r.replays >= 60)
                r.lockout = 0;
            g_partsReplayed++;
            r.usedFrame = gs::FramesShown();
            g_partsLate += late ? 1 : 0;
            g_partItemsReplayed += r.kept->size();
            return true;
        }
        g_partsStale++;
    }
    std::uint64_t watchUntil = it != g_parts.end() ? it->second.watchUntil : 0;
    if (late) {
        if (it->second.hash != hash)
            watchUntil = gs::FramesShown() + 120;
        hash = hashNow();   // kept anew: the hash of what is there now
    }
    if (hash == 0)
        return false;
    // kept from this time on; everything kept is let go first when it has grown past 48 MB (another
    // level's parts stay kept until then)
    // (Until 2026-11-21 everything kept was let go at once, and a whole scene's parts were then
    // kept anew over the next frames: with a record for each half of the double buffer the tour of
    // A Stealthy Approach reached the 48 MB within a minute.) Now the parts not drawn for longest
    // are let go, until an eighth is free. SLY1_PART_KEEP_MB: the limit.
    static const std::uint64_t keepWords = (std::uint64_t)(std::getenv("SLY1_PART_KEEP_MB") ? std::max(8, std::atoi(std::getenv("SLY1_PART_KEEP_MB"))) : 48) << 18;
    if (g_partWords > keepWords) {
        static std::vector<std::pair<std::uint64_t, std::uint32_t>> byUse;
        byUse.clear();
        for (auto& kv : g_parts)
            if (kv.second.words != 0 && kv.second.kept && &kv.second != g_partRecording)
                byUse.push_back({kv.second.usedFrame, kv.first});
        std::sort(byUse.begin(), byUse.end());
        for (std::size_t k = 0; k < byUse.size() && g_partWords > keepWords - keepWords / 8; k++) {
            PartRecord& old = g_parts[byUse[k].second];
            PartForget(old);
            g_partWords -= std::min<std::uint64_t>(g_partWords, old.words);
            old.words = 0;
            old.ok = false;
            g_partGrave.push_back(std::move(old.kept));
            old.kept.reset();
            g_partsLetGo++;
            if (gs::FramesShown() - byUse[k].first < 120)
                g_partsLetGoRecent++;
        }
        g_partsCleared++;
        it = g_parts.find(key);
    }
    if (keepWhy == 0 && it != g_parts.end() && g_parts.count(key))
        keepWhy = 5;
    PartRecord& r = g_parts[key];
    // A part found changed three times running (one that fades, its header's scales other each
    // frame) is run as ever for the next 30 frames: keeping it each frame costs more than it saves.
    const std::uint32_t stale = r.ok ? r.stale + 1 : r.stale;
    if (!ceiling && r.notBefore > gs::FramesShown()) {
        g_partsFickle++;
        return false;
    }
    // Kept only when it is what it was in the list before. A part whose header is other each frame
    // while the camera moves (one VU1 lights from its inverse matrix) was kept anew every time it
    // was met, which costs several times running it: 4 to 5 parts a frame and 6 ms of the render
    // thread on the Switch, walking through A Stealthy Approach. SLY1_PART_KEEP_TWICE=0: at once.
    {
        const bool twice = KeepTwice();
        const bool asSeen = r.seenList != 0 && r.seenList + 1 >= g_listSerial && (ceiling || (r.seenHash == hash && sameRegs(r.seenRegs, r.ok && !r.maskRead) && PartHeaderSame(r.seenHeader, header, slow, alphaFree(r), litFree(r))));
        if (twice && !asSeen) {
            r.seenHash = hash;
            r.seenList = g_listSerial;
            r.seenRegs = regs;
            std::memcpy(r.seenHeader, header, sizeof r.seenHeader);
            g_partsWaited++;
            return false;
        }
        // SLY1_PART_KEEP_US=N: no more parts are kept by the render thread in one list than N
        // microseconds of its time allow. Off: at 2 ms the Switch put off 50,000 keepings in one
        // play, the parts waiting were jobs (92 in a late frame against 47) and twice as many frames
        // came late; parts are kept from their jobs instead (PartJobsKeep).
        // Keeping a part costs several times running it, and where the view
        // turns onto a new place ten parts were kept in one frame, 3 to 6 ms, which made that frame
        // late (the Switch's late: lines, 2026-11-22). The rest wait their turn as jobs. (By their
        // number, three a list, the keeping fell behind: 11,666 put off over the host's tour and a
        // sixth fewer parts drawn again.)
        static const std::uint64_t most = std::getenv("SLY1_PART_KEEP_US") ? (std::uint64_t)std::max(100, std::atoi(std::getenv("SLY1_PART_KEEP_US"))) * 1000 : ~0ull;
        static std::uint64_t list, keepNsAtStart;
        if (list != g_listSerial) {
            list = g_listSerial;
            keepNsAtStart = g_fullNsBy[0];
        }
        if (!ceiling && g_fullNsBy[0] - keepNsAtStart >= most) {
            r.seenHash = hash;
            r.seenList = g_listSerial;
            r.seenRegs = regs;
            std::memcpy(r.seenHeader, header, sizeof r.seenHeader);
            g_partsKeepPutOff++;
            return false;
        }
    }
    if (r.pending)   // (a job of this list is running it for its keeping)
        return false;
    g_partWords -= std::min<std::uint64_t>(g_partWords, r.words);   // what it held before is let go
    PartForget(r);
    {
        const std::uint32_t lockout = r.lockout;
        const bool alphaStrict = r.alphaStrict;
        const std::uint32_t movingWait = r.movingWait;
        const bool jobOk = r.jobOk;
        const std::uint64_t shape = r.shape;
        const Vif::Regs jobBefore = r.jobBefore, jobAfter = r.jobAfter, jobBefore2 = r.jobBefore2, jobAfter2 = r.jobAfter2;
        const bool jobTwo = r.jobTwo;
        if (r.kept)
            g_partGrave.push_back(std::move(r.kept));
        if (!r.items.empty())
            g_partGraveItems.push_back(std::move(r.items));
        r = PartRecord();
        r.lockout = lockout;
        r.alphaStrict = alphaStrict;
        r.movingWait = movingWait;
        r.jobOk = jobOk;
        r.shape = shape;
        r.jobBefore = jobBefore;
        r.jobAfter = jobAfter;
        r.jobBefore2 = jobBefore2;
        r.jobAfter2 = jobAfter2;
        r.jobTwo = jobTwo;
    }
    r.serial = ++g_partSerial;
    r.usedFrame = gs::FramesShown();
    r.verdictList = g_listSerial;
    r.slow = slow;
    r.watchUntil = watchUntil;
    r.stale = stale;
    if (stale >= 3 && !ceiling) {
        r.stale = 0;
        r.lockout = std::min<std::uint32_t>(std::max<std::uint32_t>(r.lockout * 2, 30), 960);
        r.notBefore = gs::FramesShown() + r.lockout;
        g_partsFickle++;
        return false;
    }
    r.keyFirst = g_partKey + 1;
    r.hash = hash;
    {
        const std::uint64_t shape = PartHash(target, false, regs.cl, regs.wl, tte, slow, nullptr, true);
        if (shape != r.shape) {
            r.shape = shape;
            r.jobOk = r.jobTwo = false;
        }
    }
    {
        std::uint8_t maskUse = 1;
        r.hashWhole = PartHash(target, true, regs.cl, regs.wl, tte, slow, &r.texAt, false, &r.stAt, &maskUse, litMode, &r.litAt, &r.swayAt, &r.boneAt);
        r.maskRead = (maskUse & 1) != 0 || r.hashWhole == 0;
        r.maskSet = (maskUse & 2) != 0;
    }
    for (const std::uint32_t* q : r.texAt) {
        std::uint64_t v;
        std::memcpy(&v, q, 8);
        r.texWas.push_back(v);
    }
    for (const std::uint32_t* q : r.stAt) {
        if (!q) {
            r.stWas.push_back({0.0f, 0.0f});
            continue;
        }
        PartRecord::St v;
        std::memcpy(&v, q, 8);
        r.stWas.push_back(v);
    }
    r.stTarget = target;
    r.stBase = g_vif1.StUnpacks();
    for (const std::uint32_t* q : r.swayAt)
        if (q)
            r.swayWas.insert(r.swayWas.end(), q, q + 24);
        else
            r.swayWas.insert(r.swayWas.end(), 24, 0u);
    r.swayBase = g_vif1.SwayUnpacks();
    for (const MaskAt& m : r.boneAt)
        if (m.p)
            r.boneWas.insert(r.boneWas.end(), m.p, m.p + 16);
        else
            r.boneWas.insert(r.boneWas.end(), 16, 0u);
    r.boneBase = g_vif1.BoneUnpacks();
    {   // the part's lights, of the words left out: each begins with its colour (to 22)
        std::vector<PartRecord::LitSource> all;
        bool bad = false;
        for (const LitAt& at : r.litAt) {
            for (std::uint32_t k = 0; k < at.words; k++)
                r.litWas.push_back(at.p[k]);
            const float* f = reinterpret_cast<const float*>(at.p);
            if (at.addr == 0x16) {
                all.emplace_back();
                all.back().colour = f;
            } else if (all.empty()) {
                bad = true;
            } else if (at.addr == 0x17 && at.words == 8) {
                all.back().scale = f;
                all.back().base = f + 4;
            } else if (at.addr == 0x19 && at.words == 3) {
                all.back().dir = f;
            } else if (at.addr == 0x19 && at.words == 2) {
                all.back().atten = f;
            } else if (at.addr == 0x1A) {
                all.back().place = f;
            } else if (at.addr == 0x19 && at.words == 8) {
                all.back().ramp = f;
            } else if (at.addr == 0x1B && at.words == 3) {
                all.back().dir = f;
            } else if (at.addr == 0x1C && at.words == 16) {
                all.back().view = f;
            } else {
                bad = true;
            }
        }
        // (every mesh refers to the same lights' packets: the part's lights are the first mesh's)
        std::size_t each = all.size();
        for (std::size_t k = 1; k < all.size(); k++)
            if (all[k].colour == all[0].colour) {
                each = k;
                break;
            }
        for (std::size_t k = 0; k < all.size(); k++) {
            const PartRecord::LitSource& l = all[k];
            const bool spot = l.ramp || l.view;
            const bool kind = spot ? (l.ramp && l.view && l.dir && !l.atten && !l.place)
                                   : ((l.dir != nullptr) != (l.atten != nullptr) && (l.atten != nullptr) == (l.place != nullptr));
            bad = bad || !l.scale || !kind || l.colour != all[k % each].colour;
        }
        r.litStrict = litMode && (bad || each == 0 || each > 4);
        if (!r.litStrict && litMode)
            r.lights.assign(all.begin(), all.begin() + (std::ptrdiff_t)each);
    }
    r.texStrict = r.texAt.size() > 4;
    std::memcpy(r.header, header, sizeof header);
    r.before = regs;
    r.troubleWas = vu1::PartTrouble();
    r.interruptsWas = g_counts.vifInterrupts;
    r.finishesWas = g_counts.finishes;
    g_partRecording = &r;
    g_keepWhy[keepWhy]++;
    return false;
}

// --- A part run beside the render thread.
//
// A part that is neither drawn again from what was kept nor being kept (one lit by VU1 afresh each
// frame, one whose packets write vertices, one that keeps changing) was run by the render thread
// in its turn: most of that thread's time in a heavy scene. Such a part is now made a job: its
// packet is walked through a VIF1 of the job's own into a VU1 of the job's own (vu1::Lane, begun
// as a copy of the render thread's), and what it queues for the GS is collected in the job. The
// render thread goes on at once from the registers the part left the last time it ran here,
// which the packet's shape answers for (PartHash's shape: the same VIFcodes leave the same
// registers), as it does after a part drawn again. An item in the GS's queue stands for the job:
// the GS thread takes the job's items there, running the job itself if no one has begun it. The
// helper thread takes jobs in their order, and the render thread the rest when its list is
// walked; it does not begin the next list before every job of this one is done (a part's packet
// may lie in the frame's own memory). SLY1_PART_JOBS=0: every part in its turn as before.
// A job's own copy of its part's packet, as VIF1 would be given it (WalkPart into this in place of
// a VIF): the job then reads nothing of the game's memory, and the game may be told its list is
// finished before the job is done. Of a fixed size, made once: a thread set aside in the middle
// of a job may still be reading it when the job is another part's (it reads other words, and what
// it makes of them is dropped). A packet too long for it is read where it lies, as before.
constexpr std::uint32_t kJobWords = 48 * 1024;
struct JobCopy {
    std::uint32_t* words;
    std::uint32_t used = 0;
    bool over = false;
    void Words(const std::uint32_t* p, int n)
    {
        if (over || used + (std::uint32_t)n > kJobWords) {
            over = true;
            return;
        }
        std::memcpy(words + used, p, (std::size_t)n * 4);
        used += (std::uint32_t)n;
    }
};
struct PartJob {
    std::unique_ptr<std::uint32_t[]> words{new std::uint32_t[kJobWords]};
    std::uint32_t wordCount = 0;
    bool copied = false;
    vu1::Lane* lane = vu1::LaneNew();
    std::uint32_t target = 0, key = 0;
    bool tte = false;
    Vif::Regs before, after;
    Collect out;
    std::atomic<int> state{0};   // 0 waiting, 1 begun by a thread, 2 done
    std::atomic<std::uint32_t> beat{0};   // items given by the runs of it so far (t_jobBeat)
    // The lane stays as the job began: a thread runs the job on a copy of its own, and the first
    // to end gives the job its items. So a thread that needs a job another has begun and not
    // ended (the helper, set aside by the game's threads in the middle of one) runs it again
    // itself and does not wait.
    std::atomic<bool> given{false};
    std::uint64_t begunAt = 0;
    // A job is used again for another part once done, while a thread set aside in the middle of
    // it may still hold it. gen is odd while the render thread makes the job anew and other
    // after; a thread counts itself in runners before it reads gen, and gives nothing when gen
    // has changed. The render thread makes gen odd before it reads runners, and leaves a job
    // with a runner alone.
    std::atomic<std::uint32_t> gen{0};
    std::atomic<int> runners{0};
    Collect ref;   // SLY1_PART_JOBS_CHECK: what the job gives when run at once on the render thread
    bool hasRef = false;
    // The part is to be kept from what the job gives (PartJobsKeep): its key and the record's
    // serial when the job was made (0: not), and the matrix its batches must have.
    std::uint32_t keepKey = 0;
    std::uint64_t keepSerial = 0;
    float keepMatrix[16];
    bool right = false;   // the run that gave the job its items left VIF1's registers as foretold
};
std::vector<std::shared_ptr<PartJob>> g_keepJobs;   // (the render thread's) this list's jobs whose parts are to be kept from them
std::vector<PartJob*> g_jobFree;
std::mutex g_jobFreeLock;   // (the render thread and the GS thread: never the helper)
std::vector<std::uint32_t> g_jobWrongKeys;         // parts whose job left other registers than foretold
std::atomic<std::uint32_t> g_jobsOut;              // jobs made and not yet done
std::atomic<std::uint64_t> g_jobsBy[3], g_jobNsBy[3], g_jobsWrong;   // by the helper, the render thread, the GS thread
std::uint64_t g_jobsMade, g_jobsShape, g_jobSerial, g_jobWordsCopied, g_jobsNotCopied, g_drainsLater;
bool g_listJobsLive, g_drainOwed;   // (the render thread's) a job of this list reads the game's memory; the list's jobs are yet to be seen done
std::atomic<std::uint64_t> g_jobsAgain, g_jobsChecked, g_jobsDiffered;   // jobs run a second time by a thread that needed them
std::uint64_t g_jobDrainWaitNs;
std::uint64_t g_jobMakeNs[4];   // making a job: the shape's hash, the job taken, VU1 copied, queued

bool PartJobsOn()
{
    static const bool off = std::getenv("SLY1_PART_JOBS") && std::getenv("SLY1_PART_JOBS")[0] == '0';
    return !off && GsAsync();
}

// The part's packet through vif: the walk PartHash makes, the data given to the VIF.
template <class Sink> bool WalkPart(std::uint32_t target, bool tte, Sink& vif)
{
    bool wild = false;
    auto inMemory = [&](std::uint32_t addr, std::uint32_t quads) {
        const std::uint32_t a = addr & 0x0FFFFFF0;
        const bool ok = (addr & 0x50000000u) == 0 && a >= 0x10 && a < kRamSize && quads <= (kRamSize - a) / 16;
        wild = wild || !ok;
        return ok;
    };
    auto data = [&](std::uint32_t addr, bool spr, std::uint32_t qwc) {
        if (!spr) {
            if (qwc && inMemory(addr, qwc))
                vif.Words(At(addr, false), (int)(qwc * 4));
        } else {
            for (std::uint32_t i = 0; i < qwc; i++)
                vif.Words(At(addr + i * 16, true), 4);
        }
    };
    std::uint32_t tadr = target, stack[2];
    int depth = 0;
    for (int guard = 0; guard < 4096; guard++) {
        const bool tagSpr = (tadr & 0x80000000u) != 0;
        if (wild || (!tagSpr && !inMemory(tadr & 0x7FFFFFFFu, 2)))
            return false;
        const std::uint32_t* tag = At(tadr & 0x7FFFFFFFu, tagSpr);
        const std::uint32_t qwc = tag[0] & 0xFFFF, id = (tag[0] >> 28) & 7, addr = tag[1] & 0x7FFFFFF0;
        const bool spr = (tag[1] >> 31) != 0;
        if (tag[0] >> 31)
            return false;
        if (tte)
            vif.Words(tag + 2, 2);
        const std::uint32_t after = (tadr & 0x7FFFFFFFu) + 16;
        switch (id) {
        case 1:
            data(after, tagSpr, qwc);
            tadr = (after + qwc * 16) | (tagSpr ? 0x80000000u : 0);
            break;
        case 2: tadr = addr | (spr ? 0x80000000u : 0); break;
        case 3: case 4:
            data(addr, spr, qwc);
            tadr = after | (tagSpr ? 0x80000000u : 0);
            break;
        case 5:
            if (depth >= 2)
                return false;
            data(after, tagSpr, qwc);
            stack[depth++] = (after + qwc * 16) | (tagSpr ? 0x80000000u : 0);
            tadr = addr | (spr ? 0x80000000u : 0);
            break;
        case 6:
            data(after, tagSpr, qwc);
            if (depth == 0)
                return !wild;
            tadr = stack[--depth];
            break;
        default:
            return false;
        }
    }
    return false;
}

// The job's work on this thread's own copy of its lane: what it queues for the GS into out.
// False when the packet did not leave VIF1's registers as foretold.
bool PartJobWork(PartJob& j, Collect& out)
{
    static thread_local vu1::Lane* work = nullptr;
    static thread_local Vif* vif = nullptr;   // (the thread's own, kept: its buffers are not made again for each job)
    if (!work)
        work = vu1::LaneNew();
    if (!vif)
        vif = new Vif(kCauseVif1, false);
    vu1::LaneCopy(work, j.lane);
    out.used = 0;
    vu1::LaneEnter(work);
    t_collect = &out;
    t_jobBeat = &j.beat;
    t_jobStCount = vif->StUnpacksAt();
    t_jobStBase = vif->StUnpacks();
    t_jobSwayCount = vif->SwayUnpacksAt();
    t_jobSwayBase = vif->SwayUnpacks();
    t_jobVif = vif;
    t_jobBoneBase = vif->BoneUnpacks();
    const std::uint64_t troubleWas = vu1::PartTrouble();
    vif->SetRegs(j.before);
    bool walked;
    if (j.copied) {
        vif->Words(j.words.get(), (int)std::min<std::uint32_t>(j.wordCount, kJobWords));
        walked = true;
    } else {
        walked = WalkPart(j.target, j.tte, *vif);
    }
    const bool right = walked && vif->BetweenCodes() && vif->GetRegs() == j.after;
    out.trouble = vu1::PartTrouble() != troubleWas;
    t_jobStCount = nullptr;
    t_jobSwayCount = nullptr;
    t_jobVif = nullptr;
    if (!vif->BetweenCodes()) {   // left inside a code: not used again
        delete vif;
        vif = nullptr;
    }
    t_collect = nullptr;
    t_jobBeat = nullptr;
    vu1::LaneEnter(nullptr);
    return right;
}

// The job run by this thread: if no thread has begun it, or (again) when it was begun elsewhere
// and is not done. who: 0 the helper, 1 the render thread, 2 the GS thread.
bool PartJobRun(PartJob& j, int who, bool again = false)
{
    struct Runner {
        PartJob& j;
        explicit Runner(PartJob& job) : j(job) { j.runners.fetch_add(1, std::memory_order_seq_cst); }
        ~Runner() { j.runners.fetch_sub(1, std::memory_order_seq_cst); }
    } runner(j);
    const std::uint32_t gen = j.gen.load(std::memory_order_seq_cst);
    if (gen & 1)
        return false;
    int waiting = 0;
    if (!j.state.compare_exchange_strong(waiting, 1, std::memory_order_acq_rel)) {
        if (!again || waiting == 2)
            return false;
        g_jobsAgain++;
    } else {
        j.begunAt = NowNs();
    }
    const std::uint64_t t0 = NowNs();
    static thread_local Collect* mine = nullptr;
    if (!mine)
        mine = new Collect;
    const bool right = PartJobWork(j, *mine);
    g_jobNsBy[who] += NowNs() - t0;
    if (j.gen.load(std::memory_order_seq_cst) != gen || j.given.exchange(true, std::memory_order_acq_rel))
        return true;   // another thread ended it first (or the job is another part's now): this run's items are dropped
    std::swap(j.out, *mine);
    j.right = right;
    if (!right) {
        g_jobsWrong++;
        std::lock_guard<std::mutex> lk(g_hashLock);
        g_jobWrongKeys.push_back(j.key);
    }
    g_jobsBy[who]++;
    j.state.store(2, std::memory_order_release);
    g_jobsOut.fetch_sub(1, std::memory_order_acq_rel);
    return true;
}

// (the render thread or the GS thread) The job is needed now: run here if not begun, waited for
// a little if begun elsewhere (a job takes a quarter of a millisecond), then run here again.
// (SLY1_JOB_WAIT_US=N: for N microseconds; SLY1_JOB_WAIT_US=ab: 400 and 150 in turn, 600 frames each)
std::uint64_t JobWaitNs()
{
    static const char* const e = std::getenv("SLY1_JOB_WAIT_US");
    static const std::uint64_t fixed = e && e[0] != 'a' ? (std::uint64_t)std::atol(e) * 1000 : 400000;
    if (e && e[0] == 'a')
        return gs::FramesShown() / 600 % 2 ? 150000 : 400000;
    return fixed;
}
// A job begun elsewhere is run here again when the thread running it gives no item for a while (it
// has been set aside: the helper, below the game's threads on their core, waits for them), not after
// a fixed time: on the Switch the GS thread ran 2.7 ms of jobs a late frame again that the helper,
// let go on, ended first (SLY1_JOB_BEAT=0: the fixed time only; =ab: in turn, 600 frames each).
// SLY1_JOB_STALL_US: how long without an item (120).
std::atomic<std::uint64_t> g_jobsTakenOver{0};
void PartJobNeed(PartJob& j, int who)
{
    static const char* const e = std::getenv("SLY1_JOB_BEAT");
    static const std::uint64_t stallNs = std::getenv("SLY1_JOB_STALL_US") ? (std::uint64_t)std::atol(std::getenv("SLY1_JOB_STALL_US")) * 1000 : 120000;
    const bool beat = !e || (e[0] == 'a' ? gs::FramesShown() / 600 % 2 == 0 : e[0] != '0');
    std::uint32_t seen = j.beat.load(std::memory_order_relaxed);
    std::uint64_t seenAt = NowNs();
    while (j.state.load(std::memory_order_acquire) != 2) {
        if (PartJobRun(j, who))
            continue;
        const std::uint64_t now = NowNs();
        if (beat) {
            const std::uint32_t b = j.beat.load(std::memory_order_relaxed);
            if (b != seen) {
                seen = b;
                seenAt = now;
            }
            // (at the latest after the fixed time's four times over, whatever the beat)
            if (now - seenAt > stallNs || now - j.begunAt > 4 * JobWaitNs()) {
                g_jobsTakenOver++;
                PartJobRun(j, who, true);
                continue;
            }
        } else if (now - j.begunAt > JobWaitNs()) {
            PartJobRun(j, who, true);
            continue;
        }
        std::this_thread::yield();
    }
}

// The list's next job that no thread has taken by its number, or none. (The GS thread begins the
// one it has come to without asking here: PartJobRun then finds it begun.)
PartJob* PartJobTake()
{
    for (;;) {
        std::uint32_t next = g_listJobNext.load(std::memory_order_acquire);
        if (next >= g_listJobCount.load(std::memory_order_acquire))
            return nullptr;
        if (g_listJobNext.compare_exchange_weak(next, next + 1, std::memory_order_acq_rel))
            return g_listJobs[next].get();
    }
}

// (the render thread, its list walked) The jobs no thread has begun run here, and those begun
// waited for.
std::uint32_t g_drainAt;   // (the render thread's) PartJobsDrainStep: the jobs before this one are done
void PartJobsDrain()
{
    while (PartJob* j = PartJobTake())
        PartJobRun(*j, 1);
    const std::uint64_t t0 = NowNs();
    const std::uint32_t count = g_listJobCount.load(std::memory_order_acquire);
    for (std::uint32_t k = 0; k < count; k++)
        PartJobNeed(*g_listJobs[k], 1);
    while (g_jobsOut.load(std::memory_order_acquire) != 0)
        std::this_thread::yield();
    g_jobDrainWaitNs += NowNs() - t0;
}

// The same a step at a time, true when every job is done: the render thread takes what the game
// queues between two steps. (The game's two packets after a frame's list came while the list's
// jobs were seen done, an eighth of the render thread's work in slow frames on the Switch, and
// waited with their FINISHes for the end of it.)
bool PartJobsDrainStep()
{
    if (PartJob* j = PartJobTake()) {
        PartJobRun(*j, 1);
        return false;
    }
    const std::uint64_t t0 = NowNs();
    const std::uint32_t count = g_listJobCount.load(std::memory_order_acquire);
    while (g_drainAt < count && g_listJobs[g_drainAt]->state.load(std::memory_order_acquire) == 2)
        g_drainAt++;
    bool done = false;
    if (g_drainAt < count) {
        PartJob& j = *g_listJobs[g_drainAt];
        if (!PartJobRun(j, 1)) {
            if (NowNs() - j.begunAt > JobWaitNs())
                PartJobRun(j, 1, true);
            else
                std::this_thread::yield();
        }
    } else if (g_jobsOut.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    } else {
        done = true;
    }
    g_jobDrainWaitNs += NowNs() - t0;
    return done;
}

// (the render thread, a list begun) The parts whose jobs went wrong are not made jobs again.
// (the render thread, every job of the list done) The parts to be kept from their jobs: what each
// job gave, copied (the job's buffers are another part's next), with a key for each batch, and
// then judged as the render thread's own recording is.
void PartJobsKeep()
{
    for (std::shared_ptr<PartJob>& jp : g_keepJobs) {
        PartJob& j = *jp;
        auto it = g_parts.find(j.keepKey);
        if (it == g_parts.end() || it->second.serial != j.keepSerial || !it->second.pending)
            continue;
        PartRecord& r = it->second;
        r.pending = false;
        const bool done = j.state.load(std::memory_order_acquire) == 2 && j.right && !j.out.trouble && j.out.used != 0;
        bool ok = done;
        r.keyFirst = g_partKey + 1;
        r.items.clear();
        for (std::size_t k = 0; ok && k < j.out.used; k++) {
            const GsItem& item = j.out.items[k];
            if ((item.path != 1 && item.path != 2 && item.path != 4) || item.kept) {
                if (std::getenv("SLY1_PART_DIFF")) {
                    char name[80];
                    std::snprintf(name, sizeof name, "refusal: a job's item of path %d%s (%s mode %d)", item.path, item.kept ? " (kept)" : "", r.slow ? "long" : "cached", (int)r.header[37]);
                    DiffCount(name);
                }
                ok = false;
                break;
            }
            std::vector<std::uint32_t> words = item.words;
            if (item.path == 4) {
                gs::Batch b;
                std::memcpy(&b, &words[1], sizeof b);
                b.keep = ++g_partKey;
                std::memcpy(&words[1], &b, sizeof b);
            }
            r.words += words.size();
            g_partWords += words.size();
            r.items.push_back({item.path, std::make_shared<const std::vector<std::uint32_t>>(std::move(words))});
        }
        Vif::Regs after = j.after;
        PartKeepFinish(&r, ok, j.keepMatrix, after, !done ? "its job did not give a part's items" : "an item not a part's");
        (r.ok ? g_keptByJob : g_keptByJobRefused)++;
    }
    g_keepJobs.clear();
}

void PartJobsBegin()
{
    // (every job of the list before is done: PartJobsDrain)
    PartJobsKeep();
    {   // the preps the helper has not taken are the GS thread's (each item holds its own)
        std::lock_guard<std::mutex> lk(g_prepLock);
        const std::uint32_t had = g_prepCount.load(std::memory_order_acquire);
        for (std::uint32_t k = 0; k < had; k++)
            g_preps[k].reset();
        g_prepCount.store(0, std::memory_order_release);
        g_prepNext.store(0, std::memory_order_release);
    }
    const std::uint32_t had = g_listJobCount.load(std::memory_order_acquire);
    g_listJobNext.store(0, std::memory_order_release);
    g_listJobCount.store(0, std::memory_order_release);
    g_drainAt = 0;
    for (std::uint32_t k = 0; k < had; k++)
        g_listJobs[k].reset();
    g_listJobsLive = false;
    if (g_jobsWrong.load(std::memory_order_acquire) == 0)
        return;
    std::vector<std::uint32_t> keys;
    {
        std::lock_guard<std::mutex> lk(g_hashLock);
        keys.swap(g_jobWrongKeys);
    }
    for (std::uint32_t key : keys) {
        auto it = g_parts.find(key);
        if (it != g_parts.end()) {
            it->second.jobOk = it->second.jobTwo = false;
            it->second.shape = 0;
        }
    }
}

// A part run here from registers its record has no pair for: watched (not kept) to its return,
// for the pair. g_observeBad: it queued what a job must not (a call, the GIF channel's packet).
PartRecord* g_partObserving;
Vif::Regs g_observeBefore;
std::uint64_t g_observeInterrupts, g_observeFinishes;
bool g_observeBad;

// The part has just been run here from before: the pair noted if the run was clean, or the part
// not to be made a job.
// The registers a packet of a shape was met with and left, whichever part's it was: two packets
// of one shape (the same VIFcodes, tags and register-setting data) leave VIF1's registers the same,
// so a part met for the first time is made a job at once by what another part of its shape
// showed, where it was run here and watched before (ten parts in a frame where the view turns
// onto a new place). Two pairs a shape, as a part's record has.
struct ShapeRegs {
    Vif::Regs before[2], after[2];
    int pairs = 0;
    bool maskRead = true, maskSet = false;
};
std::unordered_map<std::uint64_t, ShapeRegs> g_shapeRegs;

void PartJobSeen(PartRecord& r, const Vif::Regs& before, bool clean)
{
    if (!clean) {
        r.jobOk = r.jobTwo = false;
        if (r.shape != 0)
            g_shapeRegs.erase(r.shape);
        r.shape = 0;
        return;
    }
    const Vif::Regs after = g_vif1.GetRegs();
    if (r.shape != 0) {
        if (g_shapeRegs.size() > 8192)
            g_shapeRegs.clear();
        ShapeRegs& sr = g_shapeRegs[r.shape];
        int k = 0;
        while (k < sr.pairs && !(sr.before[k] == before))
            k++;
        if (k == sr.pairs && sr.pairs < 2)
            sr.pairs++;
        if (k < 2) {
            sr.before[k] = before;
            sr.after[k] = after;
        }
        sr.maskRead = r.maskRead;
        sr.maskSet = r.maskSet;
    }
    if (!r.jobOk || r.jobBefore == before) {
        r.jobBefore = before;
        r.jobAfter = after;
        r.jobOk = true;
    } else {
        r.jobBefore2 = before;
        r.jobAfter2 = after;
        r.jobTwo = true;
    }
}

int g_fullWhy;
// (SLY1_PERF_LOG's perfmodes: the parts by how they are drawn (from their cache, the long way), their
// header's lighting mode (9.y: -1, 0, 1 ...) and what became of each: drawn again as kept, made a
// job, run here in full. A census for the renderer that is to draw them itself.)
std::uint64_t g_modeCensus[2][8][3];
bool PartCallInner(std::uint32_t marker, std::uint32_t target, bool slow);
bool PartCall(std::uint32_t marker, std::uint32_t target, bool slow)
{
    const std::uint64_t jobsWas = g_jobsMade;
    const bool skipped = PartCallInner(marker, target, slow);
    {   // (SLY1_VU1_SEQ: where each part begins among the programs, with its lighting's mode and count)
        static const bool seq = std::getenv("SLY1_VU1_SEQ") != nullptr;
        if (seq)
            std::fprintf(stderr, "seqpart frame %llu %s mode %d lights %u key %08X%s\n", (unsigned long long)gs::FramesShown(), slow ? "long" : "cached",
                         (int)g_callHeader[37], g_callHeader[39], g_callKey, skipped ? " (not run here)" : "");
    }
    const std::int32_t mode = (std::int32_t)g_callHeader[37];
    const int m = mode < -1 || mode > 6 ? 7 : mode + 1;
    g_modeCensus[slow ? 1 : 0][m][!skipped ? 2 : g_jobsMade != jobsWas ? 1 : 0]++;
    return skipped;
}
bool PartCallInner(std::uint32_t marker, std::uint32_t target, bool slow)
{
    g_callKey = 0;
    if (PartDecide(marker, target, slow))
        return true;
    // (g_fullWhy: why the part is run here in full, for SLY1_PERF_LOG's perffull: 0 it is being
    // kept, 1 no key (fickle, moving, jobs off), 2 lit by volumes, 3 not known, 4 watched for its
    // registers, 5 its shape cannot be hashed, 6 its shape changed)
    g_fullWhy = g_partRecording ? 0 : 1;
    // A part that is to be kept (PartDecide has begun its record) is made a job all the same when
    // it can be one, and kept from what the job gives (PartJobsKeep): the render thread ran it
    // itself for that before, which cost it several times a part's run, 3 to 6 ms in a frame where
    // the view turned onto a new place. SLY1_PART_KEEP_JOB=0: kept by the render thread's run.
    static const bool keepByJob = !std::getenv("SLY1_PART_KEEP_JOB") || std::getenv("SLY1_PART_KEEP_JOB")[0] != '0';
    PartRecord* const keeping = g_partRecording;
    if ((keeping && (!keepByJob || keeping->slow != slow)) || g_callKey == 0 || !PartJobsOn())
        return false;
    // (a part lit by its cache and by volumes of light, mode 1, reads the volumes' packets where
    // the game writes them anew each frame: run in its turn, before the game has)
    g_fullWhy = 2;
    if (g_callHeader[37] == 1)
        return false;
    // (a part met for the first time has no record yet: it gets one here, so that it can be a job
    // at once by its shape, below. SLY1_PART_SHAPE_REGS=0: run here this once, as before.)
    auto it = g_parts.find(g_callKey);
    g_fullWhy = 3;
    if (it == g_parts.end()) {
        static const bool shared = !std::getenv("SLY1_PART_SHAPE_REGS") || std::getenv("SLY1_PART_SHAPE_REGS")[0] != '0';
        if (!shared)
            return false;
        it = g_parts.emplace(g_callKey, PartRecord()).first;
        it->second.slow = slow;
    }
    PartRecord& r = it->second;
    // The registers a part is met with, but for those it does not read (as PartDecide's sameRegs:
    // the mask, when its packet unpacks through none before setting one; for a part drawn the
    // long way, what its BASE and OFFSET set anew). Compared whole, a part met with another mask
    // or the double buffer's flag the other way was run here and watched each time: 10 to 14 parts
    // a frame and 5 ms of the render thread in the Switch's slowest stretches.
    const auto sameRegs = [&](const Vif::Regs& was) {
        Vif::Regs x = was;
        if (!r.maskRead)
            x.mask = g_callRegs.mask;
        if (slow) {
            x.base = g_callRegs.base;
            x.ofst = g_callRegs.ofst;
            x.tops = g_callRegs.tops;
            x.dbf = g_callRegs.dbf;
        }
        return x == g_callRegs;
    };
    bool first = r.jobOk && sameRegs(r.jobBefore), second = !first && r.jobOk && r.jobTwo && sameRegs(r.jobBefore2);
    if (!first && !second) {
        // (another part's registers for this shape: SLY1_PART_SHAPE_REGS=0: not)
        static const bool shared = !std::getenv("SLY1_PART_SHAPE_REGS") || std::getenv("SLY1_PART_SHAPE_REGS")[0] != '0';
        std::uint8_t use = 1;
        const std::uint64_t shape = shared ? PartHash(target, false, g_callRegs.cl, g_callRegs.wl, g_callTte, slow, nullptr, true, nullptr, &use) : 0;
        auto known = shape != 0 ? g_shapeRegs.find(shape) : g_shapeRegs.end();
        if (known != g_shapeRegs.end()) {
            const ShapeRegs& sr = known->second;
            const bool maskReadWas = r.maskRead;
            r.maskRead = sr.maskRead;
            for (int k = 0; k < sr.pairs && !first; k++)
                if (sameRegs(sr.before[k])) {
                    r.shape = shape;
                    r.jobBefore = sr.before[k];
                    r.jobAfter = sr.after[k];
                    r.jobOk = true;
                    r.jobTwo = false;
                    r.maskSet = sr.maskSet;
                    first = true;
                    g_jobsByShape++;
                }
            if (!first)
                r.maskRead = maskReadWas;
        }
    }
    if (keeping && (&r != keeping || (!first && !second))) {
        g_fullWhy = 0;
        return false;   // kept by this run
    }
    if (!first && !second) {
        // run here and watched, if its shape is the one known (or none is yet)
        std::uint8_t maskUse = 1;
        const std::uint64_t shape = PartHash(target, false, g_callRegs.cl, g_callRegs.wl, g_callTte, slow, nullptr, true, nullptr, &maskUse);
        g_fullWhy = shape != 0 ? 4 : 5;
        g_watchWhy[r.jobOk ? 2 : r.shape == shape && shape != 0 ? 1 : 0]++;
        if (shape != 0) {
            r.maskRead = (maskUse & 1) != 0;
            r.maskSet = (maskUse & 2) != 0;
            if (r.shape != shape) {
                r.shape = shape;
                r.jobOk = r.jobTwo = false;
            }
            g_partObserving = &r;
            g_observeBefore = g_callRegs;
            g_observeInterrupts = g_counts.vifInterrupts;
            g_observeFinishes = g_counts.finishes;
            g_observeBad = false;
        }
        return false;
    }
    Vif::Regs jobAfter = first ? r.jobAfter : r.jobAfter2;
    if (!r.maskSet)   // (it leaves the mask as it finds it)
        jobAfter.mask = g_callRegs.mask;
    {   // SLY1_PART_JOBS_ONLY=slow, cached, or a mode's number: jobs of those parts alone (diagnosis)
        static const char* only = std::getenv("SLY1_PART_JOBS_ONLY");
        if (only && (only[0] == 's' ? !slow : only[0] == 'c' ? slow : (int)g_callHeader[37] != std::atoi(only)))
            return false;
    }
    const std::uint64_t tCall = NowNs();
    if (PartHash(target, false, g_callRegs.cl, g_callRegs.wl, g_callTte, slow, nullptr, true) != r.shape) {
        r.jobOk = r.jobTwo = false;   // (watched when it next comes)
        r.shape = 0;
        g_jobsShape++;
        g_fullWhy = 6;
        return false;
    }
    const std::uint64_t tShape = NowNs();
    std::shared_ptr<PartJob> job;
    const auto back = [](PartJob* j) {
        std::lock_guard<std::mutex> lk2(g_jobFreeLock);
        g_jobFree.push_back(j);
    };
    {
        std::lock_guard<std::mutex> lk(g_jobFreeLock);
        for (std::size_t k = g_jobFree.size(); k-- > 0 && !job;) {
            PartJob* j = g_jobFree[k];
            j->gen.fetch_add(1, std::memory_order_seq_cst);   // odd: being made anew
            if (j->runners.load(std::memory_order_seq_cst) != 0) {
                j->gen.fetch_add(1, std::memory_order_seq_cst);   // a thread still holds it: another
                continue;
            }
            g_jobFree.erase(g_jobFree.begin() + (std::ptrdiff_t)k);
            job.reset(j, back);
        }
    }
    if (!job) {
        job.reset(new PartJob, back);
        job->gen.fetch_add(1, std::memory_order_seq_cst);
    }
    job->keepKey = 0;
    if (keeping) {
        // (from here on nothing sends the part back to the render thread's own run)
        g_partRecording = nullptr;
        keeping->pending = true;
        job->keepKey = g_callKey;
        job->keepSerial = keeping->serial;
        if (slow)
            vu1::PartMatrixFrom(reinterpret_cast<const float*>(g_callHeader), job->keepMatrix);
        else
            vu1::PartMatrix(g_callRegs.tops, job->keepMatrix);
        g_keepJobs.push_back(job);
    }
    job->target = target;
    job->key = g_callKey;
    job->tte = g_callTte;
    job->before = g_callRegs;
    job->after = jobAfter;
    job->out.used = 0;
    job->given.store(false, std::memory_order_release);
    job->begunAt = 0;
    job->state.store(0, std::memory_order_release);
    // (its batches' ids from a run of its own, far from the render thread's)
    const std::uint64_t tCopy = NowNs();
    vu1::LaneCopyMain(job->lane, 0x80000000u | (std::uint32_t)((g_jobSerial++ & 0x7FFFF) << 12) | 1);
    {   // the packet copied for the job (SLY1_JOB_COPY=0: read where it lies, and the list's end waits for the jobs)
        static const bool copy = !std::getenv("SLY1_JOB_COPY") || std::getenv("SLY1_JOB_COPY")[0] != '0';
        job->copied = false;
        if (copy) {
            JobCopy c{job->words.get()};
            const bool ok = WalkPart(job->target, job->tte, c) && !c.over;
            job->wordCount = c.used;
            job->copied = ok;
            g_jobWordsCopied += c.used;
        }
        if (!job->copied) {
            g_listJobsLive = true;
            g_jobsNotCopied++;
        }
    }
    // (after the copy: the job's work reads the copy, and one left from the job's last use hung the check)
    {   // SLY1_PART_JOBS_CHECK=1: the job's work done here too, at once, to compare what a thread gives with
        static const bool check = std::getenv("SLY1_PART_JOBS_CHECK") != nullptr;
        job->hasRef = check;
        if (check)
            PartJobWork(*job, job->ref);
    }
    job->gen.fetch_add(1, std::memory_order_seq_cst);   // made: threads may run it
    const std::uint64_t tQueue = NowNs();
    g_jobsOut.fetch_add(1, std::memory_order_acq_rel);
    GsItem item;
    item.path = 6;
    item.job = job;
    g_gsPending.push_back(std::move(item));
    if (g_gsPending.size() >= 24)
        GsPassOn();
    // SLY1_PART_JOBS=now: each job run here at once (the same work in the same order, for telling
    // a fault of the job's own state from one of its timing)
    static const bool now = std::getenv("SLY1_PART_JOBS") && std::getenv("SLY1_PART_JOBS")[0] == 'n';
    const std::uint32_t at = g_listJobCount.load(std::memory_order_acquire);
    if (now || at >= kListJobs) {
        PartJobRun(*job, 1);
    } else {
        g_listJobs[at] = std::move(job);
        g_listJobCount.store(at + 1, std::memory_order_release);
    }
    if (slow) {   // its header where its packet would have put it, as for one drawn again
        std::uint8_t* d = vu1::Data();
        for (int k = 0; k < 11; k++)
            std::memcpy(d + ((g_callHeaderAt + (std::uint32_t)k) & 0x3FF) * 16, g_callHeader + k * 4, 16);
    }
    g_vif1.SetRegs(jobAfter);
    vu1::PartSkipped();
    g_jobsMade++;
    g_jobMakeNs[0] += tShape - tCall;
    g_jobMakeNs[1] += tCopy - tShape;
    g_jobMakeNs[2] += tQueue - tCopy;
    g_jobMakeNs[3] += NowNs() - tQueue;
    return true;
}

// The return from a part's call: what was recorded is kept if nothing but packets of registers
// and batches of the header's matrix came of it.
void PartDone()
{
    g_skinVerify.on = false;
    if (PartRecord* seen = g_partObserving) {
        g_partObserving = nullptr;
        PartJobSeen(*seen, g_observeBefore, !g_observeBad && g_vif1.BetweenCodes() && g_counts.vifInterrupts == g_observeInterrupts &&
                                                g_counts.finishes == g_observeFinishes);
    }
    vu1::LitExpect(nullptr);
    PartRecord* r = g_partRecording;
    g_partRecording = nullptr;
    if (!r)
        return;
    bool ok = !r->bad && g_vif1.BetweenCodes() && vu1::PartTrouble() == r->troubleWas && g_counts.vifInterrupts == r->interruptsWas &&
              g_counts.finishes == r->finishesWas && !r->items.empty();
    PartJobSeen(*r, r->before, r->shape != 0 && !r->bad && g_vif1.BetweenCodes() && g_counts.vifInterrupts == r->interruptsWas &&
                                   g_counts.finishes == r->finishesWas);
    float matrix[16];
    if (r->slow) {
        vu1::PartMatrixFrom(reinterpret_cast<const float*>(r->header), matrix);
    } else {
        vu1::PartMatrix(r->before.tops, matrix);
    }
    PartKeepFinish(r, ok, matrix, g_vif1.GetRegs(), nullptr);
}

// What was collected of a part (r->items) kept if nothing but packets of registers and batches of
// the header's matrix came of it: by the render thread's own run of the part (PartDone), or by a
// job's (PartJobsKeep).
void PartKeepFinish(PartRecord* r, bool ok, const float* matrix, const Vif::Regs& after, const char* jobWhy)
{
    std::uint32_t minId = ~0u, maxId = 0;
    bool alphaStrict = false, stStrict = r->stAt.size() > 8, litStrict = r->litStrict, swayStrict = r->swayAt.size() > 15;
    for (const std::uint32_t* q : r->swayAt)
        swayStrict = swayStrict || q == nullptr;
    bool boneStrict = r->boneAt.size() > 254;
    std::vector<char> boneUsed(r->boneAt.size(), 0);
    for (const MaskAt& m : r->boneAt)
        boneStrict = boneStrict || m.p == nullptr;
    std::uint64_t stUsed = 0, swayUsed = 0;
    for (std::size_t k = 0; ok && k < r->items.size(); k++) {
        const std::vector<std::uint32_t>& w = *r->items[k].words;
        if (r->items[k].path == 4) {
            gs::Batch b;
            std::memcpy(&b, &w[1], sizeof b);
            alphaStrict = alphaStrict || !b.alphaDeferred;
            if (b.stride == 4 && !r->lights.empty()) {   // a lit batch: its lighting noted, by the part's lights
                std::uint32_t lights = 0;
                if (b.litWords != 0 && w.size() >= kBatchHead + (std::size_t)b.count * b.stride * 4 + b.litWords)
                    lights = w[kBatchHead + (std::size_t)b.count * b.stride * 4];
                if (lights != r->lights.size())
                    litStrict = true;
            }
            if (b.stApplied) {
                if (b.stSource < r->stAt.size())
                    stUsed |= 1ull << b.stSource;
                else
                    stStrict = true;
            }
            if (b.swayWords != 0) {
                if (b.swaySource < r->swayAt.size())
                    swayUsed |= 1ull << b.swaySource;
                else
                    swayStrict = true;
            }
            if (b.projected) {   // a texture laid on it from another view: made anew of its unpack to 22
                if (b.projSource < r->swayAt.size() && r->swayAt[b.projSource])
                    swayUsed |= 1ull << b.projSource;
                else
                    swayStrict = true;
            }
            if (b.skinWords != 0) {   // a skinned batch: each of its matrices a bone of the packet's
                const std::size_t skinAt = kBatchHead + (std::size_t)b.count * b.stride * 4 + b.litWords + b.swayWords;
                const std::uint32_t* skin = skinAt + b.skinWords <= w.size() ? &w[skinAt] : nullptr;
                for (int j = 0; j < 4; j++) {
                    {   // SLY1_SKIN_CHECK=3: the bone taken for matrix j against the matrix the loop used
                        static const bool check = std::getenv("SLY1_SKIN_CHECK") && std::getenv("SLY1_SKIN_CHECK")[0] == '3';
                        if (check && skin && gs::SkinUses(skin, b.count, j)) {
                            static std::uint64_t n3, wrong3, unknown3;
                            n3++;
                            if (b.boneSource[j] >= r->boneAt.size()) {
                                unknown3++;
                            } else if (std::memcmp(&r->boneWas[b.boneSource[j] * 16], skin + 2 + 16 * j, 64) != 0) {
                                wrong3++;
                                if (wrong3 < 6) {
                                    // which bone, if any, has it
                                    int found = -1;
                                    for (std::size_t q = 0; q < r->boneAt.size() && found < 0; q++)
                                        if (std::memcmp(&r->boneWas[q * 16], skin + 2 + 16 * j, 64) == 0)
                                            found = (int)q;
                                    std::fprintf(stderr, "skin check 3: matrix %d at %u: taken bone %u, the one that has it %d (of %zu)\n", j, skin[0] + 4 * j, b.boneSource[j], found, r->boneAt.size());
                                }
                            }
                            if ((n3 & (n3 - 1)) == 0)
                                std::fprintf(stderr, "skin check 3: %llu matrices, %llu taken from another bone, %llu with none\n", (unsigned long long)n3, (unsigned long long)wrong3, (unsigned long long)unknown3);
                        }
                    }
                    if (b.boneSource[j] < r->boneAt.size())
                        boneUsed[b.boneSource[j]] = 1;
                    else if (!skin || gs::SkinUses(skin, b.count, j))
                        boneStrict = true;   // (one no vertex weighs needs no bone)
                }
            } else if (b.stride == 4 && !r->boneAt.empty()) {
                // (a lit batch of a part with bones, its skinning not noted: by the bones as they were)
                boneStrict = true;
            }
            ok = std::memcmp(b.matrix, matrix, sizeof b.matrix) == 0;
            if (!ok && std::getenv("SLY1_PART_DIFF")) {
                std::uint32_t hash, entry;
                vu1::CurrentProgram(&hash, &entry);
                char name[96];
                std::snprintf(name, sizeof name, "refusal: batch %zu of %zu, matrix other (last program %08X 0x%03X)", k, r->items.size(), hash, entry);
                DiffCount(name);
            }
            if (b.id != 0) {
                minId = std::min(minId, b.id);
                maxId = std::max(maxId, b.id);
            }
        } else {
            // a path's packet: tags of A+D writes alone, and none to a vertex's registers.
            // Kept as the writes themselves (path 7: a register, its value's two halves; a tag's
            // PRIM first where it sets one), for the GS thread to make without the GIF's reading
            // of tags and quadwords (a fifth of its time was the registers' way in).
            std::vector<std::uint32_t> flat;
            flat.reserve(w.size());
            for (std::size_t q = 0; ok && q + 4 <= w.size();) {
                std::uint32_t nloop = w[q] & 0x7FFF, flg = (w[q + 1] >> 26) & 3, nreg = (w[q + 1] >> 28) & 0xF;
                if ((w[q + 1] >> 14) & 1) {   // PRE
                    flat.push_back(0x00);
                    flat.push_back((w[q + 1] >> 15) & 0x7FF);
                    flat.push_back(0);
                }
                ok = flg == 0 && nreg == 1 && (w[q + 2] & 0xF) == 0xE && q + 4 + (std::size_t)nloop * 4 <= w.size();
                if (!ok && std::getenv("SLY1_PART_DIFF")) {
                    std::uint32_t hash, entry;
                    vu1::CurrentProgram(&hash, &entry);
                    char name[112];
                    std::snprintf(name, sizeof name, "refusal: packet tag flg %u nreg %u regs %X prim %X pre %u (last program %08X 0x%03X)", flg, nreg, w[q + 2], (w[q + 1] >> 15) & 0x7FF, (w[q+1]>>14)&1, hash, entry);
                    DiffCount(name);
                }
                for (std::uint32_t i = 0; ok && i < nloop; i++) {
                    std::uint32_t reg = w[q + 4 + i * 4 + 2] & 0xFF;
                    ok = reg > 0x05 && reg != 0x0C && reg != 0x0D && reg != 0x61;
                    if (!ok && std::getenv("SLY1_PART_DIFF")) {
                        char name[96];
                        std::snprintf(name, sizeof name, "refusal: packet writes register %02X", reg);
                        DiffCount(name);
                    }
                    if (reg == 0x06 && !r->texAt.empty()) {
                        std::uint64_t v;
                        std::memcpy(&v, &w[q + 4 + i * 4], 8);
                        std::size_t which = 0;
                        while (which < r->texWas.size() && r->texWas[which] != v)
                            which++;
                        if (which < r->texWas.size())
                            r->texFix.push_back({(std::uint32_t)k, (std::uint32_t)(flat.size() + 1), (std::uint32_t)which});
                        else
                            r->texStrict = true;
                    }
                    if (ok) {
                        flat.push_back(reg);
                        flat.push_back(w[q + 4 + i * 4]);
                        flat.push_back(w[q + 4 + i * 4 + 1]);
                    }
                }
                q += 4 + (std::size_t)nloop * 4;
            }
            static const bool asWrites = !(std::getenv("SLY1_PART_WRITES") && std::getenv("SLY1_PART_WRITES")[0] == '0');
            if (ok && asWrites) {
                const std::size_t was = w.size();
                r->words = r->words + flat.size() - std::min<std::uint64_t>(r->words, was);
                g_partWords = g_partWords + flat.size() - std::min<std::uint64_t>(g_partWords, was);
                r->items[k] = {7, std::make_shared<const std::vector<std::uint32_t>>(std::move(flat))};
            } else if (asWrites) {
                // (texFix's places were the writes': a packet refused is not kept, so they are not used)
            } else {
                // SLY1_PART_WRITES=0: the packet as it was, and the TEX0s' places in it
                for (PartRecord::TexFix& f : r->texFix)
                    if (f.item == k)
                        r->texStrict = true;
            }
        }
    }
    if (!ok && std::getenv("SLY1_PART_DIFF")) {
        const char* why = jobWhy ? jobWhy : r->bad ? "an item not a part's" : !g_vif1.BetweenCodes() ? "ends inside a VIFcode" :
                          vu1::PartTrouble() != r->troubleWas ? "trouble (a program not taken)" : g_counts.vifInterrupts != r->interruptsWas ? "an interrupt" :
                          g_counts.finishes != r->finishesWas ? "a FINISH" : r->items.empty() ? "nothing queued" : "a batch's matrix or a packet's registers";
        char name[96];
        std::snprintf(name, sizeof name, "refused %s mode %d: %s", r->slow ? "long" : "cached", (int)r->header[37], why);
        DiffCount(name);
    }
    r->after = after;
    r->litStrict = litStrict;
    if (litStrict)
        r->lights.clear();
    r->alphaStrict = alphaStrict;
    r->stStrict = stStrict || stUsed != (r->stAt.size() >= 64 ? ~0ull : (1ull << r->stAt.size()) - 1);
    r->swayStrict = swayStrict || swayUsed != (r->swayAt.size() >= 64 ? ~0ull : (1ull << r->swayAt.size()) - 1);
    r->boneStrict = boneStrict || std::find(boneUsed.begin(), boneUsed.end(), 0) != boneUsed.end();
    {   // SLY1_SKIN_LOG: why a part with bones is compared by them (partdiff:)
        static const bool log = std::getenv("SLY1_SKIN_LOG") != nullptr;
        if (log && !r->boneAt.empty()) {
            int nulls = 0, skinned = 0, lit = 0, badSource = 0;
            for (const MaskAt& m : r->boneAt)
                nulls += m.p == nullptr;
            for (const PartKept& kk : r->items) {
                if (kk.path != 4)
                    continue;
                gs::Batch b;
                std::memcpy(&b, &(*kk.words)[1], sizeof b);
                lit += b.stride == 4;
                if (b.skinWords) {
                    skinned++;
                    for (int j = 0; j < 4; j++)
                        badSource += b.boneSource[j] >= r->boneAt.size();
                }
            }
            char name[128];
            std::snprintf(name, sizeof name, "skin: %s: bones %zu (%d in the hash), batches lit %d skinned %d, sources unknown %d, bones unused %d", r->boneStrict ? "strict" : "free",
                          r->boneAt.size(), nulls, lit, skinned, badSource, (int)std::count(boneUsed.begin(), boneUsed.end(), 0));
            DiffCount(name);
        }
    }
    r->firstId = minId == ~0u ? 0 : minId;
    r->ids = minId == ~0u ? 0 : maxId - minId + 1;
    r->keys = g_partKey + 1 - r->keyFirst;
    r->ok = ok;
    if (ok) {
        // The part's items in one piece of memory, one after another, as the GS thread goes
        // through them (each in a block of its own, the GS thread waited on memory at each:
        // SLY1_PART_PACK=0: as they were)
        static const bool pack = !(std::getenv("SLY1_PART_PACK") && std::getenv("SLY1_PART_PACK")[0] == '0');
        if (pack) {
            std::size_t total = 0;
            for (const PartKept& k : r->items)
                total += (k.words->size() + 15) & ~std::size_t(15);   // (each at 64 bytes)
            auto block = std::make_shared<std::vector<std::uint32_t>>(total + 16);
            std::uint32_t* base = block->data();
            base += (16 - ((reinterpret_cast<std::uintptr_t>(base) / 4) & 15)) & 15;
            std::size_t at = 0;
            for (PartKept& k : r->items) {
                std::memcpy(base + at, k.words->data(), k.words->size() * 4);
                k.data = base + at;
                k.size = (std::uint32_t)k.words->size();
                at += (k.words->size() + 15) & ~std::size_t(15);
            }
            const std::shared_ptr<const std::vector<std::uint32_t>> held = block;
            for (PartKept& k : r->items)
                k.words = held;   // (each item's own words let go: the block holds them)
        } else {
            for (PartKept& k : r->items) {
                k.data = k.words->data();
                k.size = (std::uint32_t)k.words->size();
            }
        }
    }
    r->kept = std::make_shared<const std::vector<PartKept>>(std::move(r->items));
    r->items.clear();
    if (!ok) {
        g_partWords -= std::min<std::uint64_t>(g_partWords, r->words);
        r->words = 0;
        r->keys = 0;   // the renderer never saw them
        r->lockout = std::min<std::uint32_t>(std::max<std::uint32_t>(r->lockout * 2, 30), 960);
        r->notBefore = gs::FramesShown() + r->lockout;   // not tried again at once
    }
    (ok ? g_partsRecorded : g_partsRefused)++;
    if (r->slow)
        (ok ? g_partsSlowKept : g_partsSlowRefused)++;
}
}  // namespace

void PartCountsLine(const char* what)
{
    PartDiffReport();
    std::fprintf(stderr, "partparams: %s: parts drawn again with another alpha's scale %llu, with a texture offset moved %llu, lit by this frame's lights %llu\n", what,
                 (unsigned long long)g_partsAlphaMoved, (unsigned long long)g_partsStMoved, (unsigned long long)g_partsLitNow);
    std::fprintf(stderr, "partsway: %s: parts drawn again with their vertices moved by this frame's sway %llu, skinned by this frame's bones %llu\n", what,
                 (unsigned long long)g_partsSwayed, (unsigned long long)g_partsSkinned);
    std::fprintf(stderr, "partsway: %s: parts whose bones were not at their places in this frame's chain %llu\n", what, (unsigned long long)g_bonesMislaid);
    std::fprintf(stderr, "partprep: %s: parts' per-frame work made by the helper %llu, by the GS thread %llu, done the GS thread's own way %llu, by the render thread %llu; the GS thread waited %.3f s\n", what,
                 (unsigned long long)g_prepsMade[0].load(), (unsigned long long)g_prepsMade[1].load(), (unsigned long long)g_prepsMade[2].load(),
                 (unsigned long long)g_prepsMade[3].load(), g_prepWaitNs.load() / 1e9);
    std::fprintf(stderr, "path1: %llu of VU1's packets joined the one before in its item; %llu outline edges put with the edge before\n", (unsigned long long)g_path1Joined.load(),
                 (unsigned long long)g_outlinesJoined.load());
    std::fprintf(stderr, "partjobs: making them: the shape %.2f s, the job %.2f, the copy %.2f, the queue %.2f; the render thread waited %.2f s for them at its lists' ends\n",
                 g_jobMakeNs[0] / 1e9, g_jobMakeNs[1] / 1e9, g_jobMakeNs[2] / 1e9, g_jobMakeNs[3] / 1e9, g_jobDrainWaitNs / 1e9);
    std::fprintf(stderr, "partjobs: %s: %llu made (%llu not, the shape other), %llu wrong, %llu run again by a thread that needed them; run by the helper %llu (%.2f s), the render thread %llu (%.2f s), "
                 "the GS thread %llu (%.2f s)\n", what, (unsigned long long)g_jobsMade, (unsigned long long)g_jobsShape, (unsigned long long)g_jobsWrong.load(), (unsigned long long)g_jobsAgain.load(),
                 (unsigned long long)g_jobsBy[0].load(), g_jobNsBy[0].load() / 1e9, (unsigned long long)g_jobsBy[1].load(), g_jobNsBy[1].load() / 1e9,
                 (unsigned long long)g_jobsBy[2].load(), g_jobNsBy[2].load() / 1e9);
    std::fprintf(stderr, "partcounts: %s: drawn again %llu (long way %llu, texture moved %llu), kept %llu, refused %llu, stale by hash %llu regs %llu header %llu, fickle %llu, "
                 "moving %llu, hashed here %.2f s, too late %llu, kept words %.1f MB, let go %llu, batches queued %llu\n", what,
                 (unsigned long long)g_partsReplayed, (unsigned long long)g_partsSlowReplayed, (unsigned long long)g_partsTexMoved,
                 (unsigned long long)g_partsRecorded, (unsigned long long)g_partsRefused, (unsigned long long)g_partsStaleBy[0], (unsigned long long)g_partsStaleBy[1],
                 (unsigned long long)g_partsStaleBy[2], (unsigned long long)g_partsFickle, (unsigned long long)g_partsMoving, g_partHashNs / 1e9,
                 (unsigned long long)g_hashTooLate.load(), g_partWords * 4 / 1e6, (unsigned long long)g_partsCleared,
                 (unsigned long long)g_batchesQueued.load());
}

namespace {
}  // namespace

// The chain's walk hands the data over a quadword at a time; quadwords that follow each other in
// memory are given to the VIF as one stretch, so that it takes whole unpacks and whole runs of a
// GIF tag's loops at once, as from a copy. (A quadword at a time cost the Switch's render thread 5
// ms a frame.) The stretch is read when it is given, after everything before it has been done:
// what an interrupt's handler wrote further on is there.
struct LiveSink {
    const std::uint32_t* from = nullptr;
    int count = 0;
    void Flush()
    {
        if (count)
            g_vif1.Words(from, count);
        count = 0;
    }
    void OnVif(const std::uint32_t* w, int n)
    {
        if (count && w == from + count && count < (1 << 20)) {
            count += n;
            return;
        }
        Flush();
        from = w;
        count = n;
    }
    void OnGif(const std::uint32_t*) {}
    bool SkipData(std::uint32_t) { return false; }
    bool Direct() const { return true; }
    ~LiveSink() { Flush(); }

    // A model part drawn from its cache (tools/patches.py's hook: a NOP naming the cache's packet,
    // then the DMA call of it): from the NOP to the tag that returns from the call. PartCall says
    // whether the call is left out, the part drawn from what was kept of the packet.
    int depth = 0, partDepth = -1, partWhy = 0;
    std::uint64_t partBegan = 0;
    bool SkipCall(std::uint32_t target)
    {
        Flush();   // the words before the call first, the NOP among them
        const bool slow = g_vif1.PartSlow();
        std::uint32_t marker = g_vif1.TakePartBegin();
        if (!marker)
            return false;
        const std::uint64_t t0 = NowNs();   // (for SLY1_PERF_LOG's perfparts: where the parts' time goes)
        if (PartCall(marker, target, slow)) {
            g_vif1.PartEnd();
            g_partTimes[0] += NowNs() - t0;
            return true;
        }
        partDepth = depth;
        partBegan = t0;
        partWhy = g_fullWhy;
        return false;
    }
    void OnTagDone(std::uint32_t id)
    {
        if (id == 5) {
            depth++;
        } else if (id == 6 && depth > 0) {   // ret
            depth--;
            if (depth == partDepth) {
                Flush();
                PartDone();
                g_vif1.PartEnd();
                partDepth = -1;
                g_partTimes[1] += NowNs() - partBegan;
                g_fullNsBy[partWhy] += NowNs() - partBegan;
                g_fullBy[partWhy]++;
            }
        }
    }
};

// How many of VIF1's codes in a list have the interrupt bit: the list walked by its codes' lengths
// (as Vif::Code has them), nothing done. The walk goes on from one list to the next, as the VIF's.
struct InterruptScan {
    std::uint32_t left = 0, cl = 1, wl = 1;
    std::uint32_t Count(const std::uint32_t* w, std::size_t n)
    {
        std::uint32_t found = 0;
        for (std::size_t k = 0; k < n;) {
            if (left != 0) {
                std::size_t skip = std::min<std::size_t>(left, n - k);
                k += skip;
                left -= (std::uint32_t)skip;
                continue;
            }
            std::uint32_t code = w[k++], cmd = (code >> 24) & 0x7F, imm = code & 0xFFFF, num = (code >> 16) & 0xFF;
            found += code >> 31;
            if ((cmd & 0x60) == 0x60) {   // UNPACK
                std::uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3, count = num ? num : 256;
                std::uint32_t vectors = count;
                if (wl > cl)
                    vectors = cl * (count / wl) + (count % wl < cl ? count % wl : cl);
                std::uint32_t bits = (vn == 3 && vl == 3) ? 16 : (32u >> vl) * (vn + 1);
                left = (vectors * bits + 31) / 32;
            } else if (cmd == 0x01) {
                cl = imm & 0xFF;
                wl = (imm >> 8) & 0xFF;
                if (wl == 0)
                    wl = 256;
            } else if (cmd == 0x20) {
                left = 1;
            } else if (cmd == 0x30 || cmd == 0x31) {
                left = 4;
            } else if (cmd == 0x4A) {
                left = (num ? num : 256) * 2;
            } else if (cmd == 0x50 || cmd == 0x51) {
                left = (imm ? imm : 65536) * 4;
            }
        }
        return found;
    }
};
InterruptScan g_interruptScan;
std::atomic<std::uint64_t> g_liveLists, g_copiedLists;

// (Quadwords that follow each other in memory are copied in one piece: a quadword at a time was
// half of what the game's own render thread did on the Switch, before the port's could start.)
struct CaptureSink {
    std::vector<std::uint32_t>& out;
    const std::uint32_t* from = nullptr;
    std::size_t count = 0;
    void Flush()
    {
        if (count)
            out.insert(out.end(), from, from + count);
        count = 0;
    }
    void Add(const std::uint32_t* w, std::size_t n)
    {
        if (count && w == from + count) {
            count += n;
            return;
        }
        Flush();
        from = w;
        count = n;
    }
    void OnVif(const std::uint32_t* w, int n) { Add(w, (std::size_t)n); }
    void OnGif(const std::uint32_t* q) { Add(q, 4); }
    ~CaptureSink() { Flush(); }
    void OnTagDone(std::uint32_t) {}
    bool SkipData(std::uint32_t) { return false; }
    bool SkipCall(std::uint32_t) { return false; }
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
        {   // (seen at a start now and then, the game's channel pointer holding another word: what lies
            // around the five pointers InitDmaChannels sets, 0x00261F10 to 0x00261F24, to tell what wrote there)
            const std::uint32_t* w = static_cast<const std::uint32_t*>(Native(0x00261EE0));
            std::fprintf(stderr, "dmac: the words at 0x00261EE0:");
            for (int k = 0; k < 32; k++)
                std::fprintf(stderr, " %08X", w[k]);
            std::fprintf(stderr, "\n");
        }
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
            if (sink.SkipCall(addr | (spr ? 0x80000000u : 0))) {   // drawn from what was kept of it (PartReplay)
                next = data + qwc * 16;
                id = 1;
                break;
            }
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
        for (std::uint32_t i = sink.SkipData(qwc) ? qwc : 0; i < qwc; i++) {
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
        sink.OnTagDone(id);
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
        // A frame's list of VIF1's (a megabyte or two) is not copied: the copy took the game's
        // thread 7 ms of a heavy frame on the Switch before the render thread could start. Its
        // tags are walked to see how long it is, and the render thread then walks it where it
        // lies, as VIF1 does on the PS2 while the game goes on. A short list is copied, as it was.
        // SLY1_COPY_LISTS=1: every list copied.
        static const bool copyAll = std::getenv("SLY1_COPY_LISTS") != nullptr;
        if (regs == kVif1 && !copyAll) {
            CountSink count;
            WalkChain(regs, tadr, count);
            if (count.words >= kLiveListWords) {
                g_interruptScan = InterruptScan();   // not walked by its codes: the next list starts at a code
                item.kind = RenderItem::Vif1Live;
                item.regs = regs;
                item.tadr = tadr;
                g_liveLists++;
                Enqueue(std::move(item));
                g_counts.dmaNanoseconds += NowNs() - t0;
                return;
            }
        }
        item.words = SpareBuffer();
        {
            CaptureSink sink{item.words};
            WalkChain(regs, tadr, sink);
        }
        // A list of VIF1's with interrupts is not drawn from the copy. The interrupt's handler (the
        // decompilation's PackFrameColors) packs the colours VU1 has just lit a mesh with into the
        // mesh's cache, in the game's memory, and the list goes on to call the cache's packets:
        // the copy, made before the handler ran, has them as they were (frame 848 of the opening
        // drew 24 meshes with no colour so). The render thread walks such a list where it lies,
        // as VIF1 does on the PS2; the game waits for its FINISH before it touches the frame's
        // packets again. SLY1_COPY_LISTS=1: every list from its copy, as before.
        if (regs == kVif1 && g_interruptScan.Count(item.words.data(), item.words.size()) != 0 && !copyAll) {
            {
                std::lock_guard<std::mutex> lk(g_renderLock);
                if (g_renderSpare.size() < 64)
                    g_renderSpare.push_back(std::move(item.words));
            }
            item.words = {};
            item.kind = RenderItem::Vif1Live;
            item.regs = regs;
            item.tadr = tadr;
            g_liveLists++;
        } else if (regs == kVif1) {
            g_copiedLists++;
        }
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
    ProfileThisThread("gs");   // (SLY1_PROFILE)
    gs::SetBatchSink(QueueBatch);
    g_gsStarted.store(true, std::memory_order_release);
    // Everything queued is taken at once, and the emptied buffers given back then: the lock once a
    // lot, not twice an item.
    std::vector<GsItem> taken;
    std::size_t takenAt = 0;
    std::vector<std::vector<std::uint32_t>> emptied;
    std::shared_ptr<PartJob> jobNow;   // the job whose items are being taken, and the next of them
    std::size_t jobAt = 0;
    for (;;) {
        if (!jobNow && takenAt == taken.size()) {
            std::unique_lock<std::mutex> lk(g_gsLock);
            // (A frame has a thousand items and more where much is in sight. With a stock of 256 the
            // render thread ran out each frame and allocated the rest: a third of its work, walking
            // through A Stealthy Approach on the Switch, the allocator's lock among it.)
            for (auto& v : emptied)
                if (g_gsSpare.size() < kGsSpareMost)
                    g_gsSpare.push_back(std::move(v));
            emptied.clear();
            g_gsBusy = false;
            if (g_gsQueue.empty()) {
                g_gsIdle.notify_all();
                g_gsWaiting = true;
                g_gsWake.wait(lk, [] { return !g_gsQueue.empty(); });
                g_gsWaiting = false;
            }
            taken.clear();
            takenAt = 0;
            taken.swap(g_gsQueue);
            g_gsBusy = true;
        }
        // The item where it lies, and the next one's words asked for: the render thread wrote them
        // on another core, and a batch's vertices met cold, one line after the other, were a tenth
        // of this thread's work in slow frames on the Switch.
        const auto ahead = [](const GsItem& next) {
            if (next.path == 0 || next.path > 4 || next.kept)
                return;
            const char* p = reinterpret_cast<const char*>(next.words.data());
            // (SLY1_PREFETCH=bytes: how much of it, 8,192 unless set; =ab: 8,192 and 1,024 in turn,
            // 600 frames each: the loop of prefetches was 2.4% of this thread on the Switch's profile)
            static const char* const pfEnv = std::getenv("SLY1_PREFETCH");
            const std::size_t most = !pfEnv ? 8192 : pfEnv[0] == 'a' ? (gs::FramesShown() / 600 % 2 == 0 ? 8192 : 1024) : (std::size_t)std::atoi(pfEnv);
            const std::size_t n = std::min<std::size_t>(next.words.size() * 4, most);
            for (std::size_t at = 0; at < n; at += 64)
                __builtin_prefetch(p + at);
        };
        GsItem* itemp = nullptr;
        bool fromQueue = false;
        if (!jobNow) {
        GsItem& item = taken[takenAt++];
        if (takenAt < taken.size())
            ahead(taken[takenAt]);
        if (item.path == 6) {   // a part's job: its items in this one's place, the job run here if not begun
            PartJob& j = *item.job;
            PartJobNeed(j, 2);
            if (j.hasRef) {
                static int printed;
                bool same = j.ref.used == j.out.used;
                std::size_t at = 0, word = 0;
                for (; same && at < j.out.used; at++) {
                    const GsItem& a = j.out.items[at], &b = j.ref.items[at];
                    same = a.path == b.path && a.words.size() == b.words.size();
                    // (a batch's record has where its vertices lay in the thread's lane: not compared)
                    const std::size_t skip = a.path == 4 ? 1 + offsetof(gs::Batch, quads) / 4 : ~std::size_t(0);
                    for (word = 0; same && word < a.words.size(); word++)
                        same = word == skip || word == skip + 1 || (a.path == 4 && (word == 1 + offsetof(gs::Batch, ofx) / 4 || word == 1 + offsetof(gs::Batch, ofy) / 4 ||
                                                                                     word == offsetof(gs::Batch, cull) / 4 + 1 || word == offsetof(gs::Batch, id) / 4 + 2)) ||
                               a.words[word] == b.words[word];   // (nor what the GS thread fills in, nor the padding beside cull)
                    if (!same)
                        break;
                }
                g_jobsChecked++;
                if (!same && printed++ < 40)
                    std::fprintf(stderr, "jobcheck: frame %llu, part %08X at 0x%08X: %zu items against %zu; item %zu differs at word %zu\n",
                                 (unsigned long long)gs::FramesShown(), j.key, j.target, j.out.used, j.ref.used, at, word ? word - 1 : 0);
                if (!same)
                    g_jobsDiffered++;
            }
            jobNow = std::move(item.job);   // its items in their turn, where they are (they stay the job's)
            jobAt = 0;
            continue;
        }
        itemp = &item;
        fromQueue = true;
        }
        if (jobNow) {
            if (jobAt >= jobNow->out.used) {
                jobNow.reset();
                continue;
            }
            itemp = &jobNow->out.items[jobAt++];
            if (jobAt < jobNow->out.used)
                ahead(jobNow->out.items[jobAt]);
        }
        GsItem& item = *itemp;
        g_itemFinishTold = item.finishTold;
        struct ToldCheck {   // (an item marked whose FINISH the GIF did not come to: the count put right, and said)
            ~ToldCheck()
            {
                if (!g_itemFinishTold)
                    return;
                g_itemFinishTold = false;
                g_finishAhead.fetch_sub(1, std::memory_order_acq_rel);
                static int said;
                if (said++ < 4)
                    std::fprintf(stderr, "dmac: a FINISH told to the game early was not in its item\n");
            }
        } toldCheck;
        const std::uint64_t gsStarted = NowNs();
        struct Busy {
            std::uint64_t from;
            int path;
            ~Busy()
            {
                const std::uint64_t ns = NowNs() - from;
                Bump(g_gsBusyNs, ns);
                Bump(g_gsNsByPath[path & 7], ns);   // (SLY1_PERF_LOG's perfgs)
                Bump(g_gsItemsByPath[path & 7], 1);
            }
        } busy{gsStarted, item.path};
        // The GS held once for the item, not once for each packet and batch in it (a call may wait
        // for the display, and holds it for itself as it needs).
        struct HeldIf {
            bool on;
            explicit HeldIf(bool o) : on(o) { if (on) new (hold) gs::Hold; }
            ~HeldIf() { if (on) reinterpret_cast<gs::Hold*>(hold)->~Hold(); }
            alignas(gs::Hold) unsigned char hold[sizeof(gs::Hold)];
        } held(item.path != 0);
        if (item.path == 0) {
            const std::uint64_t shownBefore = gs::FramesShown();
            item.fn(item.a, item.b);
            {   // SLY1_SLOW_GS=ms: each frame's swap takes that much longer (a GS thread that cannot keep up)
                static const int slow = std::getenv("SLY1_SLOW_GS") ? std::atoi(std::getenv("SLY1_SLOW_GS")) : 0;
                if (slow > 0 && gs::FramesShown() != shownBefore)
                    std::this_thread::sleep_for(std::chrono::milliseconds(slow));
            }
            if (item.listAt && gs::FramesShown() != shownBefore) {   // the swap: the frame is the display's now
                {   // SLY1_PERF_LOG's late: a frame that came more than 20 ms after the one before, and what
                    // each thread did between the two (at most 8 a second are printed; all are counted)
                    static const bool perfLog = std::getenv("SLY1_PERF_LOG") != nullptr;
                    if (perfLog) {
                        static std::uint64_t at, was[12], said, saidSecond, pathWas[8];
                        std::uint64_t pathIs[8];
                        for (int k = 0; k < 8; k++)
                            pathIs[k] = g_gsNsByPath[k].load();
                        const std::uint64_t now = NowNs();
                        const std::uint64_t is[12] = {g_renderNs[0] + g_renderNs[1] + g_renderNs[2], g_gsBusyNs.load(), KernelIdleNs(), g_jobNsBy[0].load(), g_jobsBy[0].load() + g_jobsBy[1].load() + g_jobsBy[2].load(),
                                                      g_fullBy[0], g_fullBy[3] + g_fullBy[4], g_fullNsBy[0] + g_fullNsBy[1] + g_fullNsBy[2] + g_fullNsBy[3] + g_fullNsBy[4] + g_fullNsBy[5] + g_fullNsBy[6],
                                                      gs::UploadSerial(), g_partsReplayed, g_batchesQueued.load(), g_lagWaitNs.load()};
                        const double dt = at ? (now - at) / 1e6 : 0.0;
                        if (dt > 20.0 && dt < 200.0) {
                            if (now / 1000000000ull != saidSecond) {
                                saidSecond = now / 1000000000ull;
                                said = 0;
                            }
                            if (said++ < 8)
                                std::fprintf(stderr, "late: frame %llu after %.1f ms: render thread %.1f ms, GS thread %.1f, the game's threads %.1f, the helper's jobs %.1f; %llu jobs, %llu parts kept, "
                                             "%llu new or watched, parts in full %.1f ms, %llu uploads, %llu parts drawn again, %llu batches, the render thread waited %.1f ms for the GS thread; held %.1f ms\n",
                                             (unsigned long long)gs::FramesShown(), dt, (is[0] - was[0]) / 1e6, (is[1] - was[1]) / 1e6, dt - (is[2] - was[2]) / 1e6, (is[3] - was[3]) / 1e6,
                                             (unsigned long long)(is[4] - was[4]), (unsigned long long)(is[5] - was[5]), (unsigned long long)(is[6] - was[6]), (is[7] - was[7]) / 1e6,
                                             (unsigned long long)(is[8] - was[8]), (unsigned long long)(is[9] - was[9]), (unsigned long long)(is[10] - was[10]), (is[11] - was[11]) / 1e6,
                                             (now - item.listAt) / 1e6);
                            if (said <= 8)
                                std::fprintf(stderr, "lategs: frame %llu: the GS thread's ms: calls %.1f, VU1's packets %.1f, DIRECT %.1f, the GIF channel %.1f, batches %.1f, parts drawn again %.1f, jobs' items %.1f\n",
                                             (unsigned long long)gs::FramesShown(), (pathIs[0] - pathWas[0]) / 1e6, (pathIs[1] - pathWas[1]) / 1e6, (pathIs[2] - pathWas[2]) / 1e6,
                                             (pathIs[3] - pathWas[3]) / 1e6, (pathIs[4] - pathWas[4]) / 1e6, (pathIs[5] - pathWas[5]) / 1e6, (pathIs[6] - pathWas[6]) / 1e6);
                        }
                        {   // lateparts: in the same frames, the parts' jobs by thread and the render thread's
                            // waits for them, and the parts not drawn as the list before had them
                            static std::uint64_t jw[7];
                            const std::uint64_t ji[7] = {g_jobsBy[0].load(), g_jobNsBy[0].load(), g_jobsBy[1].load(), g_jobNsBy[1].load(),
                                                         g_jobsBy[2].load(), g_jobNsBy[2].load(), g_jobDrainWaitNs};
                            if (dt > 20.0 && dt < 200.0 && said <= 8)
                                std::fprintf(stderr, "lateparts: frame %llu: jobs by the helper %llu (%.1f ms), the render thread %llu (%.1f ms), the GS thread %llu (%.1f ms); "
                                             "the render thread waited %.1f ms for jobs\n", (unsigned long long)gs::FramesShown(), (unsigned long long)(ji[0] - jw[0]), (ji[1] - jw[1]) / 1e6,
                                             (unsigned long long)(ji[2] - jw[2]), (ji[3] - jw[3]) / 1e6, (unsigned long long)(ji[4] - jw[4]), (ji[5] - jw[5]) / 1e6, (ji[6] - jw[6]) / 1e6);
                            std::memcpy(jw, ji, sizeof jw);
                        }
                        std::memcpy(pathWas, pathIs, sizeof pathWas);
                        at = now;
                        std::memcpy(was, is, sizeof was);
                    }
                }
                const std::uint64_t held = NowNs() - item.listAt;
                g_heldNs += held;
                g_heldCount++;
                if (held > g_heldMaxNs.load())
                    g_heldMaxNs = held;
            }
        } else if (item.path == 5) {   // a part drawn again: its kept packets and batches in turn
            float matrix[16], viewport[8];
            std::memcpy(matrix, &item.words[0], sizeof matrix);
            std::memcpy(viewport, &item.words[16], sizeof viewport);
            const std::uint32_t firstId = item.words[24], keptFirstId = item.words[25];
            const std::size_t head = 1 + (sizeof(gs::Batch) + 3) / 4;
            const std::vector<PartKept>& part = *item.part;
            float alphaNow;
            std::memcpy(&alphaNow, &item.words[26], 4);
            const ReplayLayout layout(item.words.data());
            const std::uint32_t stCount = layout.stCount;
            std::size_t fix = layout.fix;
            // what the helper made of the part's batches this frame: made here if it has not begun,
            // waited for a little if it has, done this thread's own way if it is still at it
            PrepTask* prep = nullptr;
            const std::uint64_t prepFrom = NowNs();
            Bump(g_replayNs[4], prepFrom - gsStarted);
            if (item.prep) {
                PrepTask& t = *item.prep;
                if (!PrepRun(t, 1)) {
                    const std::uint64_t w0 = NowNs();
                    while (t.state.load(std::memory_order_acquire) == 1 && NowNs() - w0 < 200000)
                        std::this_thread::yield();
                    g_prepWaitNs += NowNs() - w0;
                }
                if (t.state.load(std::memory_order_acquire) == 2 && t.quads.size() == part.size())
                    prep = &t;
                else
                    g_prepsMade[2]++;
            }
            std::uint64_t t0 = NowNs();
            Bump(g_replayNs[3], t0 - prepFrom);
            for (std::size_t i = 0; i < part.size(); i++) {
                const PartKept& kept = part[i];
                if (i + 2 < part.size()) {
                    __builtin_prefetch(part[i + 2].data);
                    __builtin_prefetch(part[i + 2].data + 16);
                    __builtin_prefetch(part[i + 2].data + 32);
                }
                if (i + 1 < part.size() && part[i + 1].path == 4) {
                    // the next batch's record was asked for a turn ago: its place in the renderer's
                    // table of kept batches asked for now
                    std::uint64_t keep;
                    std::memcpy(&keep, reinterpret_cast<const char*>(part[i + 1].data + 1) + offsetof(gs::Batch, keep), sizeof keep);
                    gs::KeptAhead(keep);
                }
                const std::uint32_t* w = kept.data;
                const std::size_t size = kept.size;
                if (fix + 4 <= item.words.size() && item.words[fix] == i) {
                    // its TEX0 writes are other values now: the packet from a copy with them
                    static std::vector<std::uint32_t> copy;
                    copy.assign(w, w + size);
                    for (; fix + 4 <= item.words.size() && item.words[fix] == i; fix += 4) {
                        copy[item.words[fix + 1]] = item.words[fix + 2];
                        copy[item.words[fix + 1] + 1] = item.words[fix + 3];
                    }
                    w = copy.data();
                }
                if (kept.path == 4) {
                    gs::Batch b;
                    std::memcpy(&b, &w[1], sizeof b);
                    {   // SLY1_FUSE_SURVEY: batches that follow one of another id with nothing between and the same layout
                        static const bool survey = std::getenv("SLY1_FUSE_SURVEY") != nullptr;
                        if (survey) {
                            static std::uint64_t all, fusable, vertices;
                            static std::uint32_t prevTag, prevId;
                            static std::size_t prevI = ~std::size_t(0);
                            static const void* prevPart;
                            static gs::Batch prev;
                            all++;
                            static std::uint64_t adjacent, sameTag, otherId, gap1;
                            if (prevPart == &part && prevI + 1 == i) {
                                adjacent++;
                                sameTag += prevTag == w[0];
                                otherId += prevTag == w[0] && prevId != b.id;
                            }
                            gap1 += prevPart == &part && prevI + 2 == i;
                            if ((all & 0x3FFFF) == 0)
                                std::fprintf(stderr, "fuse: adjacent %llu, same tag %llu, of another id %llu; one item between %llu\n", (unsigned long long)adjacent,
                                             (unsigned long long)sameTag, (unsigned long long)otherId, (unsigned long long)gap1);
                            if (prevPart == &part && prevI + 1 == i && prevTag == w[0] && prevId != b.id && b.id != 0 && prev.stride == b.stride &&
                                prev.positionAt == b.positionAt && prev.stqAt == b.stqAt && prev.colourAt == b.colourAt && prev.cull == b.cull &&
                                prev.fogScale == b.fogScale && prev.wMax == b.wMax && prev.alphaDeferred == b.alphaDeferred && prev.stSource == b.stSource &&
                                (prev.litWords != 0) == (b.litWords != 0) && prev.litKick == b.litKick && prev.swaySource == b.swaySource &&
                                (prev.skinWords != 0) == (b.skinWords != 0) && prev.projected == b.projected && prev.projSource == b.projSource) {
                                fusable++;
                                vertices += b.count;
                            }
                            prev = b;
                            prevTag = w[0];
                            prevId = b.id;
                            prevI = i;
                            prevPart = &part;
                            if ((all & 0x3FFFF) == 0)
                                std::fprintf(stderr, "fuse: %llu kept batches drawn, %llu could join the one before (%.1f vertices each)\n", (unsigned long long)all,
                                             (unsigned long long)fusable, fusable ? (double)vertices / fusable : 0.0);
                        }
                    }
                    std::memcpy(b.matrix, matrix, sizeof b.matrix);
                    std::memcpy(b.scale, viewport, 16);
                    std::memcpy(b.offset, viewport + 4, 16);
                    if (b.id != 0)
                        b.id = firstId + (b.id - keptFirstId);
                    if (b.alphaDeferred)
                        b.alphaScale = alphaNow;
                    if (b.stApplied && b.stSource < stCount)
                        std::memcpy(b.stOffset, &item.words[28 + 2 * b.stSource], 8);
                    if (b.projected && b.projSource < layout.swayCount) {   // laid on with this frame's matrix
                        const std::uint32_t* pj = &item.words[layout.projAt + 24 * (std::size_t)b.projSource];
                        std::memcpy(b.proj, pj, 64);
                        std::memcpy(b.projColour, pj + 16, 16);
                        std::memcpy(b.projLimits, pj + 20, 16);
                    }
                    b.quads = w + head;
                    if (prep) {
                        if (!prep->quads[i].empty()) {
                            b.quads = prep->quads[i].data();
                            b.skinMoved = (prep->moved[i] & 1) != 0;
                            b.swayMoved = (prep->moved[i] & 2) != 0;
                        }
                        b.lit = b.litWords ? w + head + (std::size_t)b.count * b.stride * 4 : nullptr;
                        b.litNow = nullptr;
                        b.coloursNow = prep->colours[i].empty() ? nullptr : prep->colours[i].data();
                    } else {
                        static thread_local BatchScratch scratch;
                        BatchNow(item.words.data(), layout, &part, w, b, scratch);
                    }
                    if (!gs::DrawBatch(w[0], b))
                        g_batchesRefused++;
                    const std::uint64_t t1 = NowNs();
                    Bump(g_replayNs[0], t1 - t0);
                    t0 = t1;
                } else if (kept.path == 7) {   // a packet kept as its register writes (PartKeepFinish)
                    gs::Hold hold;
                    for (std::size_t k = 0; k + 3 <= size; k += 3)
                        gs::Write(w[k], w[k + 1] | (std::uint64_t)w[k + 2] << 32);
                    const std::uint64_t t1 = NowNs();
                    Bump(g_replayNs[1], t1 - t0);
                    t0 = t1;
                } else {
                    Gif& gif = kept.path == 1 ? g_path1 : g_path2;
                    gs::Hold hold;
                    for (std::size_t k = 0; k + 4 <= size;) {
                        std::uint32_t taken = gif.Left() ? gif.Run(&w[k], (std::uint32_t)((size - k) / 4)) : 0;
                        if (taken == 0) {
                            gif.Qword(&w[k]);
                            taken = 1;
                        }
                        k += (std::size_t)taken * 4;
                    }
                    const std::uint64_t t1 = NowNs();
                    Bump(g_replayNs[2], t1 - t0);
                    t0 = t1;
                }
            }
        } else if (item.path == 4) {   // a batch (QueueBatch)
            gs::Batch b;
            const std::size_t head = 1 + (sizeof(gs::Batch) + 3) / 4;
            std::memcpy(&b, &item.words[1], sizeof b);
            b.quads = item.kept ? item.kept->data() + head : &item.words[head];
            b.lit = b.litWords ? b.quads + (std::size_t)b.count * b.stride * 4 : nullptr;
            b.litNow = nullptr;
            b.sway = b.swayWords ? b.quads + (std::size_t)b.count * b.stride * 4 + b.litWords : nullptr;
            b.swayMoved = false;
            b.skin = b.skinWords ? b.quads + (std::size_t)b.count * b.stride * 4 + b.litWords + b.swayWords : nullptr;
            b.skinMoved = false;
            if (!gs::DrawBatch(item.words[0], b))
                g_batchesRefused++;   // not a strip the renderer takes: nothing drawn of it
        } else {
            // whole runs of a tag's loops at once where the data allows (Gif::Run), as without the thread
            Gif& gif = item.path == 1 ? g_path1 : item.path == 2 ? g_path2 : g_gif;
            gs::Hold hold;
            const std::vector<std::uint32_t>& words = item.kept ? *item.kept : item.words;
            for (std::size_t k = 0; k + 4 <= words.size();) {
                std::uint32_t taken = gif.Left() ? gif.Run(&words[k], (std::uint32_t)((words.size() - k) / 4)) : 0;
                if (taken == 0) {
                    gif.Qword(&words[k]);
                    taken = 1;
                }
                k += (std::size_t)taken * 4;
            }
        }
        if (fromQueue && item.words.capacity() && emptied.size() < kGsSpareMost)
            emptied.push_back(std::move(item.words));
    }
}

// The render thread's time in each kind of item (VIF1 data, GIF data, calls), for the report.

void RenderThreadMain()
{
    ProfileThisThread("render");   // (SLY1_PROFILE=render)
    g_renderStarted.store(true, std::memory_order_release);
    for (;;) {
        RenderItem item;
        {
            std::unique_lock<std::mutex> lk(g_renderLock);
            g_renderBusy = false;
            if (g_renderQueue.empty() && g_drainOwed) {   // nothing queued: the last list's jobs seen done now, a step at a time
                g_renderBusy = true;
                lk.unlock();
                if (PartJobsDrainStep()) {
                    g_drainOwed = false;
                    TIMELINE("render: the jobs are done");
                }
                continue;
            }
            if (g_renderQueue.empty() && !(g_partGrave.empty() && g_partGraveItems.empty())) {
                g_renderBusy = true;
                lk.unlock();
                g_partGrave.clear();
                g_partGraveItems.clear();
                continue;
            }
            if (g_renderQueue.empty() && PrepRenderOn() && PrepWaiting()) {
                // nothing queued: a part's per-frame work the helper has not come to, made here (the
                // render thread waits half of a slow frame, and the GS thread, which limits it, would
                // make it itself)
                g_renderBusy = true;
                lk.unlock();
                if (std::shared_ptr<PrepTask> p = PrepTake())
                    PrepRun(*p, 3);
                continue;
            }
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
        TIMELINE("render starts %s", kind == 0 ? "VIF1 (copy)" : kind == 3 ? "VIF1 (in place)" : kind == 1 ? "GIF" : "a call");
        if ((kind == 0 || kind == 3) && g_drainOwed) {   // (a list begins with the last one's jobs done: PartJobsBegin)
            PartJobsDrain();
            g_drainOwed = false;
        }
        if (kind == 0 || kind == 3)
            g_listAt = started;
        if (gsThread && (kind == 0 || kind == 3) && FinishLag())
            FinishLagWait();
        switch (item.kind) {
        case RenderItem::Vif1: {
            std::optional<gs::Hold> hold;
            if (!gsThread)
                hold.emplace();
            g_vif1.Words(item.words.data(), (int)item.words.size());
            break;
        }
        case RenderItem::Vif1Live: {
            std::optional<gs::Hold> hold;
            if (!gsThread)
                hold.emplace();
            {
                g_partTurns.clear();
                g_listsBegun.store(++g_listSerial, std::memory_order_release);
                PartJobsBegin();
                if (PartJobsOn()) {
                    g_listWalked.store(true, std::memory_order_release);
                    std::unique_lock<std::mutex> lk(g_hashLock, std::try_to_lock);   // (held: the helper is not asleep)
                    if (lk.owns_lock() && g_jobHelperWaiting)
                        g_hashWake.notify_one();
                }
                LiveSink sink;
                WalkChain(item.regs, item.tadr, sink);
            }
            TIMELINE("render: the list walked");
            g_listWalked.store(false, std::memory_order_release);
            if (g_jobsOut.load(std::memory_order_acquire) != 0) {
                GsPassOn();   // (the GS thread may take what is left as it comes to it)
                // With every job on a copy of its packet the game is told the list is finished now, and
                // the jobs are seen done after: when this thread has nothing queued, and before its
                // next list in any case. Waiting for them here was 3 ms of a frame on the Switch
                // standing and 7 to 8 walking, with the game's frame held for as long.
                if (g_listJobsLive) {
                    PartJobsDrain();
                } else {
                    g_drainOwed = true;
                    g_drainsLater++;
                }
            }
            TIMELINE("render: the parts are in");
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
                g.listAt = g_listAt.load();
                GsEnqueue(std::move(g));
            } else {
                item.fn(item.a, item.b);
            }
            break;
        }
        GsPassOn();
        HashPassOn();
        if (kind == 3)
            TIMELINE("render: passed on");
        if (gsThread)
            FinishEarly();
        {   // SLY1_SLOW_RENDER=ms: each list of VIF1's takes that much longer (a slow machine's frames, on a fast one)
            static const int slow = std::getenv("SLY1_SLOW_RENDER") ? std::atoi(std::getenv("SLY1_SLOW_RENDER")) : 0;
            if (slow > 0 && (kind == 0 || kind == 3))
                std::this_thread::sleep_for(std::chrono::milliseconds(slow));
        }
        TIMELINE("render ends it");
        g_renderNs[kind < 3 ? kind : 0] += NowNs() - started;
        // SLY1_PERF_LOG=1: every 60 frames shown, the mean time a frame took and how much of it
        // the render thread, the GS thread and the game's threads (any of them running) were busy
        static const bool perfLog = std::getenv("SLY1_PERF_LOG") != nullptr;
        if (perfLog) {
            static std::uint64_t shownWas, since, renderWas, gsWas, idleWas;
            const std::uint64_t shown = gs::FramesShown();
            if (shown >= shownWas + 60) {
                const std::uint64_t now = NowNs(), render = g_renderNs[0] + g_renderNs[1] + g_renderNs[2], gsNs = g_gsBusyNs.load();
                const double n = (double)(shown - shownWas) * 1e6;
                const std::uint64_t idle = KernelIdleNs();
                if (since)
                    {
                        static std::uint64_t earlyWas, lateWas;
                        const std::uint64_t early = g_finishesEarly.load(), late = g_finishesLate.load();
                        if (FinishLag() && gsThread)
                            std::fprintf(stderr, "perffinish: to %llu: FINISH told to the game %llu times from the render thread, %llu from the GS thread; the GS thread owes %d now\n",
                                         (unsigned long long)shown, (unsigned long long)(early - earlyWas), (unsigned long long)(late - lateWas), g_finishAhead.load());
                        earlyWas = early;
                        lateWas = late;
                    }
                    {
                        static std::uint64_t heldWas, countWas, waitWas;
                        const std::uint64_t held = g_heldNs.load(), count = g_heldCount.load(), wait = g_lagWaitNs.load();
                        if (count > countWas)
                            std::fprintf(stderr, "perfheld: to %llu: a frame held %.1f ms from its list to the display's queue (%.1f at most); the render thread waited %.1f ms a frame for the GS thread\n",
                                         (unsigned long long)shown, (held - heldWas) / 1e6 / (double)(count - countWas), g_heldMaxNs.exchange(0) / 1e6, (wait - waitWas) / n);
                        heldWas = held;
                        countWas = count;
                        waitWas = wait;
                    }
                    std::fprintf(stderr, "perf: frames to %llu: %.1f ms a frame; render thread %.1f, GS thread %.1f, the game's threads %.1f\n",
                                 (unsigned long long)shown, (now - since) / n, (render - renderWas) / n, (gsNs - gsWas) / n,
                                 ((double)(now - since) - (double)(idle - idleWas)) / n);
                {
                    // the parts' counters over the same frames, a frame
                    static std::uint64_t timesWas[2];
                    const std::uint64_t times[2] = {g_partTimes[0], g_partTimes[1]};
                    static std::uint64_t was[8];
                    const std::uint64_t is[8] = {g_partsReplayed, g_partsRecorded, g_partsRefused, g_partsStaleBy[0], g_partsStaleBy[1],
                                                 g_partsStaleBy[2], g_partsFickle, g_batchesQueued.load()};
                    const double f = (double)(shown - shownWas);
                    if (since)
                        std::fprintf(stderr, "perfparts: to %llu, a frame: drawn again %.0f, kept %.1f, refused %.1f, stale by hash %.1f regs %.1f header %.1f, "
                                     "fickle %.0f, batches %.0f; ms: drawing again %.2f, parts in full %.2f\n", (unsigned long long)shown, (is[0] - was[0]) / f, (is[1] - was[1]) / f,
                                     (is[2] - was[2]) / f, (is[3] - was[3]) / f, (is[4] - was[4]) / f, (is[5] - was[5]) / f, (is[6] - was[6]) / f,
                                     (is[7] - was[7]) / f, (times[0] - timesWas[0]) / f / 1e6, (times[1] - timesWas[1]) / f / 1e6);
                    {
                        static std::uint64_t nsWas[8], itemsWas[8], qwWas, palWas[2], replayWas[5], prepsWas[4];
                        std::uint64_t ns[8], items[8], pal[2];
                        for (int k = 0; k < 8; k++) {
                            ns[k] = g_gsNsByPath[k].load();
                            items[k] = g_gsItemsByPath[k].load();
                        }
                        const std::uint64_t qw = gs::UploadedQuadwords(pal);
                        static const char* const kPath[7] = {"calls", "VU1's packets", "DIRECT", "the GIF channel", "batches", "parts drawn again", "jobs' items"};
                        if (since) {
                            std::fprintf(stderr, "perfgs: to %llu, the GS thread a frame:", (unsigned long long)shown);
                            for (int k = 0; k < 7; k++)
                                std::fprintf(stderr, " %s %.2f ms (%.0f)%s", kPath[k], (ns[k] - nsWas[k]) / f / 1e6, (items[k] - itemsWas[k]) / f, k < 6 ? "," : "");
                            std::fprintf(stderr, "; uploads %.1f KB, palettes read %.1f, from the cache %.1f; parts drawn again: batches %.2f ms, register writes %.2f, packets %.2f, prep %.2f, before %.2f; preps by the helper %.1f, the render thread %.1f, this thread %.1f, its own way %.1f\n",
                                         (qw - qwWas) * 16 / f / 1024.0, (pal[0] - palWas[0]) / f, (pal[1] - palWas[1]) / f, (g_replayNs[0] - replayWas[0]) / f / 1e6,
                                         (g_replayNs[1] - replayWas[1]) / f / 1e6, (g_replayNs[2] - replayWas[2]) / f / 1e6, (g_replayNs[3] - replayWas[3]) / f / 1e6,
                                         (g_replayNs[4] - replayWas[4]) / f / 1e6, (g_prepsMade[0] - prepsWas[0]) / f, (g_prepsMade[3] - prepsWas[3]) / f,
                                         (g_prepsMade[1] - prepsWas[1]) / f, (g_prepsMade[2] - prepsWas[2]) / f);
                        }
                        std::memcpy(nsWas, ns, sizeof nsWas);
                        std::memcpy(itemsWas, items, sizeof itemsWas);
                        qwWas = qw;
                        palWas[0] = pal[0];
                        palWas[1] = pal[1];
                        for (int k = 0; k < 5; k++)
                            replayWas[k] = g_replayNs[k].load();
                        for (int k = 0; k < 4; k++)
                            prepsWas[k] = g_prepsMade[k].load();
                    }
                    {
                        static std::uint64_t keepWas[6], watchWas[3];
                        if (since)
                            std::fprintf(stderr, "perfkeep: to %llu, a frame: kept anew: never kept %.1f, other by hash %.1f, registers %.1f, header %.1f, moving mark ended %.1f, "
                                         "known and not kept %.1f; watched: never run here cleanly %.1f, no registers for its shape %.1f, other registers %.1f; kept %.1f MB, let go %llu times\n",
                                         (unsigned long long)shown, (g_keepWhy[0] - keepWas[0]) / f, (g_keepWhy[1] - keepWas[1]) / f, (g_keepWhy[2] - keepWas[2]) / f,
                                         (g_keepWhy[3] - keepWas[3]) / f, (g_keepWhy[4] - keepWas[4]) / f, (g_keepWhy[5] - keepWas[5]) / f, (g_watchWhy[0] - watchWas[0]) / f,
                                         (g_watchWhy[1] - watchWas[1]) / f, (g_watchWhy[2] - watchWas[2]) / f, g_partWords * 4 / 1e6, (unsigned long long)g_partsCleared);
                        std::fprintf(stderr, "perfkeep: to %llu: %llu parts let go for room so far, %llu of them drawn in the 120 frames before; %llu keepings put off to a later list, "
                                     "%llu parts made jobs at first sight by their shape; %llu parts kept from their jobs, %llu not (the job's run was not a part's)\n", (unsigned long long)shown,
                                     (unsigned long long)g_partsLetGo, (unsigned long long)g_partsLetGoRecent, (unsigned long long)g_partsKeepPutOff, (unsigned long long)g_jobsByShape,
                                     (unsigned long long)g_keptByJob, (unsigned long long)g_keptByJobRefused);
                        std::memcpy(keepWas, g_keepWhy, sizeof keepWas);
                        std::memcpy(watchWas, g_watchWhy, sizeof watchWas);
                    }
                    {   // the parts' jobs: how many each thread ran and for how long, and the render thread's wait at its list's end
                        static std::uint64_t jobsWas[3], nsWas[3], drainWas, waitedWas, spareWas[3], copiedWas;
                        std::uint64_t jobs[3], ns[3];
                        for (int k = 0; k < 3; k++) {
                            jobs[k] = g_jobsBy[k].load();
                            ns[k] = g_jobNsBy[k].load();
                        }
                        if (since)
                            std::fprintf(stderr, "perfjobs: to %llu, a frame: jobs by the helper %.1f (%.2f ms), the render thread %.1f (%.2f ms), the GS thread %.1f (%.2f ms); "
                                         "the render thread waited %.2f ms for jobs at its list's end (a job begun elsewhere run again here after %d us); parts waiting to be the same twice %.1f; buffers from the stock %.0f, allocated %.0f, grown %.0f; codes a stale job left alone %llu; jobs not on a copy %llu, words copied a frame %.0f\n",
                                         (unsigned long long)shown, (jobs[0] - jobsWas[0]) / f, (ns[0] - nsWas[0]) / f / 1e6, (jobs[1] - jobsWas[1]) / f, (ns[1] - nsWas[1]) / f / 1e6,
                                         (jobs[2] - jobsWas[2]) / f, (ns[2] - nsWas[2]) / f / 1e6, (g_jobDrainWaitNs - drainWas) / f / 1e6, (int)(JobWaitNs() / 1000),
                                         (g_partsWaited - waitedWas) / f, (g_gsSpareHad.load() - spareWas[0]) / f, (g_gsSpareNone.load() - spareWas[1]) / f, (g_gsSpareGrown.load() - spareWas[2]) / f, (unsigned long long)g_jobStaleCodes.load(), (unsigned long long)g_jobsNotCopied, (g_jobWordsCopied - copiedWas) / f);
                        copiedWas = g_jobWordsCopied;
                        {
                            static std::uint64_t was[2][8][3];
                            if (since) {
                                std::fprintf(stderr, "perfmodes: to %llu, a frame (again/job/full):", (unsigned long long)shown);
                                for (int w = 0; w < 2; w++)
                                    for (int m = 0; m < 8; m++) {
                                        const std::uint64_t* c = g_modeCensus[w][m];
                                        const std::uint64_t* o = was[w][m];
                                        if (c[0] + c[1] + c[2] != o[0] + o[1] + o[2])
                                            std::fprintf(stderr, " %s mode %d %.1f/%.1f/%.1f;", w ? "long" : "cache", m - 1, (c[0] - o[0]) / f, (c[1] - o[1]) / f, (c[2] - o[2]) / f);
                                    }
                                std::fprintf(stderr, "\n");
                            }
                            std::memcpy(was, g_modeCensus, sizeof was);
                        }
                        {
                            static std::uint64_t nsWas[7], nWas[7];
                            static const char* const kWhy[7] = {"being kept", "no key", "lit by volumes", "not known", "watched", "no shape", "shape changed"};
                            if (since) {
                                std::fprintf(stderr, "perffull: to %llu, parts run in full a frame:", (unsigned long long)shown);
                                for (int k = 0; k < 7; k++)
                                    std::fprintf(stderr, " %s %.1f (%.2f ms)%s", kWhy[k], (g_fullBy[k] - nWas[k]) / f, (g_fullNsBy[k] - nsWas[k]) / f / 1e6, k < 6 ? "," : "\n");
                            }
                            std::memcpy(nsWas, g_fullNsBy, sizeof nsWas);
                            std::memcpy(nWas, g_fullBy, sizeof nWas);
                        }
                        {
                            static std::uint64_t was[12];
                            if (since)
                                std::fprintf(stderr, "perfwait: to %llu, a frame: parts not as the list before had them: from their cache %.1f, the long way %.1f; not in the list before %.1f, "
                                             "registers other %.1f, header other %.1f (inverse matrix %.1f, fog %.1f, alpha's scale %.1f, mode and count %.1f, centre %.1f, colours' scale %.1f; near enough %.1f)\n",
                                             (unsigned long long)shown, (g_waitWhy[0] - was[0]) / f, (g_waitWhy[1] - was[1]) / f, (g_waitWhy[2] - was[2]) / f, (g_waitWhy[3] - was[3]) / f,
                                             (g_waitWhy[4] - was[4]) / f, (g_waitWhy[5] - was[5]) / f, (g_waitWhy[6] - was[6]) / f, (g_waitWhy[7] - was[7]) / f, (g_waitWhy[8] - was[8]) / f,
                                             (g_waitWhy[9] - was[9]) / f, (g_waitWhy[10] - was[10]) / f, (g_waitWhy[11] - was[11]) / f);
                            std::memcpy(was, g_waitWhy, sizeof was);
                        }
                        spareWas[2] = g_gsSpareGrown.load();
                        spareWas[0] = g_gsSpareHad.load();
                        spareWas[1] = g_gsSpareNone.load();
                        for (int k = 0; k < 3; k++) {
                            jobsWas[k] = jobs[k];
                            nsWas[k] = ns[k];
                        }
                        drainWas = g_jobDrainWaitNs;
                        waitedWas = g_partsWaited;
                    }
                    {   // where the player is (g_player's position), to find the place again
                        const std::uint32_t player = *static_cast<const std::uint32_t*>(Native(0x00262E10));
                        if (player >= 0x00100000 && player < kRamSize - 0x200) {
                            float pos[3];
                            std::memcpy(pos, Native(player + 0x100), sizeof pos);
                            std::fprintf(stderr, "perfpos: to %llu: the player at %.0f %.0f %.0f\n", (unsigned long long)shown, (double)pos[0], (double)pos[1], (double)pos[2]);
                            const std::uint32_t cam = *static_cast<const std::uint32_t*>(Native(0x00261990));   // g_camera
                            if (cam >= 0x00100000 && cam < kRamSize - 0x300) {
                                float at[3];
                                std::int32_t mask;
                                std::memcpy(at, Native(cam + 0x40), sizeof at);
                                std::memcpy(&mask, Native(cam + 0x21C), 4);
                                std::fprintf(stderr, "perfpos: to %llu: the camera at %.0f %.0f %.0f, its area mask 0x%X\n", (unsigned long long)shown, (double)at[0], (double)at[1], (double)at[2], (unsigned)mask);
                            }
                        }
                    }
                    std::memcpy(timesWas, times, sizeof timesWas);
                    std::memcpy(was, is, sizeof was);
                }
                idleWas = idle;
                shownWas = shown;
                since = now;
                renderWas = render;
                gsWas = gsNs;
            }
        }
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
    {
        std::uint64_t in[2], out[2], parts;
        gs::PartCounts(in, out, &parts);
        if (parts)
            std::fprintf(stderr, "dmac: %llu model parts drawn from their caches' packets: %llu batches (%llu vertices) in them, %llu (%llu) not\n",
                         (unsigned long long)parts, (unsigned long long)in[0], (unsigned long long)in[1], (unsigned long long)out[0], (unsigned long long)out[1]);
    }
    if (g_partsRecorded || g_partsRefused)
        std::fprintf(stderr, "dmac: model parts drawn again from what was kept of their packets: %llu (%llu packets and batches); kept anew %llu times, of them %llu "
                     "because the packet or the header had changed; not kept %llu times; run as ever because their vertices move %llu times\n", (unsigned long long)g_partsReplayed,
                     (unsigned long long)g_partItemsReplayed, (unsigned long long)g_partsRecorded, (unsigned long long)g_partsStale, (unsigned long long)g_partsRefused,
                     (unsigned long long)g_partsMoving);
    if (g_partsRecorded || g_partsRefused)
        std::fprintf(stderr, "dmac: parts kept anew because of the hash %llu, VIF1's registers %llu, the header %llu; %.2f s hashing packets; %.1f MB kept, "
                     "all let go %llu times; run as ever %llu times because they kept changing\n", (unsigned long long)g_partsStaleBy[0], (unsigned long long)g_partsStaleBy[1],
                     (unsigned long long)g_partsStaleBy[2], g_partHashNs / 1e9, g_partWords * 4 / 1e6, (unsigned long long)g_partsCleared, (unsigned long long)g_partsFickle);
    if (g_partsSlowKept || g_partsSlowRefused)
        std::fprintf(stderr, "dmac: parts drawn the long way: drawn again %llu times, kept %llu times, not kept %llu times\n",
                     (unsigned long long)g_partsSlowReplayed, (unsigned long long)g_partsSlowKept, (unsigned long long)g_partsSlowRefused);
        if (g_jobsChecked.load())
        std::fprintf(stderr, "dmac: jobs checked against the same work done at once: %llu, %llu differed\n", (unsigned long long)g_jobsChecked.load(),
                     (unsigned long long)g_jobsDiffered.load());
    std::fprintf(stderr, "dmac: parts run beside the render thread: %llu (%llu not, the shape other; %llu went wrong); by the helper %llu in %.2f s, the render thread %llu in %.2f s, the GS thread %llu in %.2f s\n",
                     (unsigned long long)g_jobsMade, (unsigned long long)g_jobsShape, (unsigned long long)g_jobsWrong.load(), (unsigned long long)g_jobsBy[0].load(), g_jobNsBy[0].load() / 1e9,
                     (unsigned long long)g_jobsBy[1].load(), g_jobNsBy[1].load() / 1e9, (unsigned long long)g_jobsBy[2].load(), g_jobNsBy[2].load() / 1e9);
    if (g_hashTooLate.load())
        std::fprintf(stderr, "dmac: %llu hashes of parts drawn the long way not made in time (a later list had begun)\n", (unsigned long long)g_hashTooLate.load());
    if (g_hashHelped.load())
        std::fprintf(stderr, "dmac: %llu parts drawn again on the hash of the frame before; the helper hashed %llu parts in %.2f s\n",
                     (unsigned long long)g_partsLate, (unsigned long long)g_hashHelped.load(), g_hashHelperNs.load() / 1e9);
    if (g_liveLists.load())
        std::fprintf(stderr, "dmac: %llu lists of VIF1's walked where they lie (a frame's, or one with interrupts), %llu from their copies\n",
                     (unsigned long long)g_liveLists.load(), (unsigned long long)g_copiedLists.load());
    if (g_batchesQueued.load())
        std::fprintf(stderr, "dmac: %llu batches queued for the GS thread, %llu of them not taken by the renderer (not drawn); the GS thread took %.2f s\n",
                     (unsigned long long)g_batchesQueued.load(), (unsigned long long)g_batchesRefused.load(), g_gsBusyNs.load() / 1e9);
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
