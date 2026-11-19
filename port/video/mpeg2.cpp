// SPDX-License-Identifier: AGPL-3.0-or-later
// The MPEG-2 video decoder of port/mpeg2.h, written for this project from the public standard
// (ISO/IEC 13818-2): its syntax, its variable-length code tables (annex B), its inverse
// quantisation, transform and prediction. It covers what the game's movies use and counts
// anything else as an error: frame pictures, 4:2:0, frame prediction and the frame transform.
#include "port/mpeg2.h"

#include <cmath>
#include <cstring>

namespace port {
namespace video {

// ---------------------------------------------------------------------------------------------
// Bits of a range of bytes, most significant first. Past the end they read as zeros.
struct Mpeg2::Bits {
    const std::uint8_t* p;
    std::size_t size;
    std::size_t at = 0;   // in bits

    std::uint32_t Peek(int n) const
    {
        std::size_t byte = at >> 3;
        std::uint64_t w = 0;
        for (int k = 0; k < 5; k++)
            w = w << 8 | (byte + (std::size_t)k < size ? p[byte + (std::size_t)k] : 0);
        return (std::uint32_t)((w >> (40 - (int)(at & 7) - n)) & ((1ull << n) - 1));
    }
    void Skip(int n) { at += (std::size_t)n; }
    std::uint32_t Get(int n)
    {
        const std::uint32_t v = Peek(n);
        at += (std::size_t)n;
        return v;
    }
    bool Over() const { return at > size * 8; }
};

namespace {

// ---------------------------------------------------------------------------------------------
// The variable-length codes (annex B), each as its bits and its value, made into tables looked
// up by the next bits.
struct Code {
    const char* bits;
    int value;
};

struct Table {
    int length = 0;                    // the longest code's
    std::vector<std::int16_t> value;   // by the next `length` bits: the value, or -32768
    std::vector<std::uint8_t> size;    // and the code's length
    void Build(const Code* codes, int count)
    {
        for (int k = 0; k < count; k++) {
            const int n = (int)std::strlen(codes[k].bits);
            if (n > length)
                length = n;
        }
        value.assign((std::size_t)1 << length, -32768);
        size.assign((std::size_t)1 << length, 0);
        for (int k = 0; k < count; k++) {
            const int n = (int)std::strlen(codes[k].bits);
            std::uint32_t c = 0;
            for (int i = 0; i < n; i++)
                c = c << 1 | (std::uint32_t)(codes[k].bits[i] == '1');
            const std::uint32_t first = c << (length - n), span = 1u << (length - n);
            for (std::uint32_t i = 0; i < span; i++) {
                value[first + i] = (std::int16_t)codes[k].value;
                size[first + i] = (std::uint8_t)n;
            }
        }
    }
};

// B-1: the macroblock address increment.
const Code kIncrement[] = {
    {"1", 1}, {"011", 2}, {"010", 3}, {"0011", 4}, {"0010", 5}, {"00011", 6}, {"00010", 7}, {"0000111", 8}, {"0000110", 9},
    {"00001011", 10}, {"00001010", 11}, {"00001001", 12}, {"00001000", 13}, {"00000111", 14}, {"00000110", 15},
    {"0000010111", 16}, {"0000010110", 17}, {"0000010101", 18}, {"0000010100", 19}, {"0000010011", 20}, {"0000010010", 21},
    {"00000100011", 22}, {"00000100010", 23}, {"00000100001", 24}, {"00000100000", 25}, {"00000011111", 26},
    {"00000011110", 27}, {"00000011101", 28}, {"00000011100", 29}, {"00000011011", 30}, {"00000011010", 31},
    {"00000011001", 32}, {"00000011000", 33}, {"00000001000", 34 /* the escape: 33 more */},
};

// B-2, B-3, B-4: the macroblock type, as bits: 1 its own quantiser, 2 forward, 4 backward,
// 8 a coded pattern, 16 intra.
enum { kQuant = 1, kForward = 2, kBackward = 4, kPattern = 8, kIntra = 16 };
const Code kTypeI[] = {{"1", kIntra}, {"01", kIntra | kQuant}};
const Code kTypeP[] = {
    {"1", kForward | kPattern}, {"01", kPattern}, {"001", kForward}, {"00011", kIntra},
    {"00010", kForward | kPattern | kQuant}, {"00001", kPattern | kQuant}, {"000001", kIntra | kQuant},
};
const Code kTypeB[] = {
    {"10", kForward | kBackward}, {"11", kForward | kBackward | kPattern}, {"010", kBackward}, {"011", kBackward | kPattern},
    {"0010", kForward}, {"0011", kForward | kPattern}, {"00011", kIntra}, {"00010", kForward | kBackward | kPattern | kQuant},
    {"000011", kForward | kPattern | kQuant}, {"000010", kBackward | kPattern | kQuant}, {"000001", kIntra | kQuant},
};

// B-9: the coded block pattern.
const Code kPatternCodes[] = {
    {"111", 60}, {"1101", 4}, {"1100", 8}, {"1011", 16}, {"1010", 32}, {"10011", 12}, {"10010", 48}, {"10001", 20}, {"10000", 40},
    {"01111", 28}, {"01110", 44}, {"01101", 52}, {"01100", 56}, {"01011", 1}, {"01010", 61}, {"01001", 2}, {"01000", 62},
    {"001111", 24}, {"001110", 36}, {"001101", 3}, {"001100", 63}, {"0010111", 5}, {"0010110", 9}, {"0010101", 17},
    {"0010100", 33}, {"0010011", 6}, {"0010010", 10}, {"0010001", 18}, {"0010000", 34}, {"00011111", 7}, {"00011110", 11},
    {"00011101", 19}, {"00011100", 35}, {"00011011", 13}, {"00011010", 49}, {"00011001", 21}, {"00011000", 41},
    {"00010111", 14}, {"00010110", 50}, {"00010101", 22}, {"00010100", 42}, {"00010011", 15}, {"00010010", 51},
    {"00010001", 23}, {"00010000", 43}, {"00001111", 25}, {"00001110", 37}, {"00001101", 26}, {"00001100", 38},
    {"00001011", 29}, {"00001010", 45}, {"00001001", 53}, {"00001000", 57}, {"00000111", 30}, {"00000110", 46},
    {"00000101", 54}, {"00000100", 58}, {"000000111", 31}, {"000000110", 47}, {"000000101", 55}, {"000000100", 59},
    {"000000011", 27}, {"000000010", 39}, {"000000001", 0},
};

// B-10: the motion code's size, 0 to 16; a sign bit follows all but 0.
const Code kMotion[] = {
    {"1", 0}, {"01", 1}, {"001", 2}, {"0001", 3}, {"000011", 4}, {"0000101", 5}, {"0000100", 6}, {"0000011", 7},
    {"000001011", 8}, {"000001010", 9}, {"000001001", 10}, {"0000010001", 11}, {"0000010000", 12}, {"0000001111", 13},
    {"0000001110", 14}, {"0000001101", 15}, {"0000001100", 16},
};

// B-12, B-13: the size of an intra block's DC difference.
const Code kDcLuma[] = {
    {"100", 0}, {"00", 1}, {"01", 2}, {"101", 3}, {"110", 4}, {"1110", 5}, {"11110", 6}, {"111110", 7}, {"1111110", 8},
    {"11111110", 9}, {"111111110", 10}, {"111111111", 11},
};
const Code kDcChroma[] = {
    {"00", 0}, {"01", 1}, {"10", 2}, {"110", 3}, {"1110", 4}, {"11110", 5}, {"111110", 6}, {"1111110", 7}, {"11111110", 8},
    {"111111110", 9}, {"1111111110", 10}, {"1111111111", 11},
};

// B-14 and B-15: a coefficient's run of zeros and its level, as run * 64 + level; a sign bit
// follows. kEnd is the end of the block, kEscape a run and a level written out.
enum { kEnd = 64 * 64, kEscape = 64 * 64 + 1 };
#define RL(run, level) ((run) * 64 + (level))
// The codes the two tables share (the long ones).
#define DCT_SHARED \
    {"000001", kEscape}, \
    {"000000011100", RL(3, 3)}, {"000000010010", RL(4, 3)}, {"000000011110", RL(6, 2)}, {"000000010101", RL(7, 2)}, \
    {"000000010001", RL(8, 2)}, {"000000011111", RL(17, 1)}, {"000000011010", RL(18, 1)}, {"000000011001", RL(19, 1)}, \
    {"000000010111", RL(20, 1)}, {"000000010110", RL(21, 1)}, \
    {"0000000010110", RL(1, 6)}, {"0000000010101", RL(1, 7)}, {"0000000010100", RL(2, 5)}, {"0000000010011", RL(3, 4)}, \
    {"0000000010010", RL(5, 3)}, {"0000000010001", RL(9, 2)}, {"0000000010000", RL(10, 2)}, {"0000000011111", RL(22, 1)}, \
    {"0000000011110", RL(23, 1)}, {"0000000011101", RL(24, 1)}, {"0000000011100", RL(25, 1)}, {"0000000011011", RL(26, 1)}, \
    {"00000000011111", RL(0, 16)}, {"00000000011110", RL(0, 17)}, {"00000000011101", RL(0, 18)}, {"00000000011100", RL(0, 19)}, \
    {"00000000011011", RL(0, 20)}, {"00000000011010", RL(0, 21)}, {"00000000011001", RL(0, 22)}, {"00000000011000", RL(0, 23)}, \
    {"00000000010111", RL(0, 24)}, {"00000000010110", RL(0, 25)}, {"00000000010101", RL(0, 26)}, {"00000000010100", RL(0, 27)}, \
    {"00000000010011", RL(0, 28)}, {"00000000010010", RL(0, 29)}, {"00000000010001", RL(0, 30)}, {"00000000010000", RL(0, 31)}, \
    {"000000000011000", RL(0, 32)}, {"000000000010111", RL(0, 33)}, {"000000000010110", RL(0, 34)}, {"000000000010101", RL(0, 35)}, \
    {"000000000010100", RL(0, 36)}, {"000000000010011", RL(0, 37)}, {"000000000010010", RL(0, 38)}, {"000000000010001", RL(0, 39)}, \
    {"000000000010000", RL(0, 40)}, {"000000000011111", RL(1, 8)}, {"000000000011110", RL(1, 9)}, {"000000000011101", RL(1, 10)}, \
    {"000000000011100", RL(1, 11)}, {"000000000011011", RL(1, 12)}, {"000000000011010", RL(1, 13)}, {"000000000011001", RL(1, 14)}, \
    {"0000000000010011", RL(1, 15)}, {"0000000000010010", RL(1, 16)}, {"0000000000010001", RL(1, 17)}, {"0000000000010000", RL(1, 18)}, \
    {"0000000000010100", RL(6, 3)}, {"0000000000011010", RL(11, 2)}, {"0000000000011001", RL(12, 2)}, {"0000000000011000", RL(13, 2)}, \
    {"0000000000010111", RL(14, 2)}, {"0000000000010110", RL(15, 2)}, {"0000000000010101", RL(16, 2)}, {"0000000000011111", RL(27, 1)}, \
    {"0000000000011110", RL(28, 1)}, {"0000000000011101", RL(29, 1)}, {"0000000000011100", RL(30, 1)}, {"0000000000011011", RL(31, 1)}

// B-14 (table zero). The block's first coefficient of a non-intra block has "1" for run 0,
// level 1, where the others have "11" and "10" ends the block: Block() sees to that.
const Code kDctZero[] = {
    {"10", kEnd}, {"11", RL(0, 1)}, {"011", RL(1, 1)}, {"0100", RL(0, 2)}, {"0101", RL(2, 1)}, {"00101", RL(0, 3)},
    {"00111", RL(3, 1)}, {"00110", RL(4, 1)}, {"000110", RL(1, 2)}, {"000111", RL(5, 1)}, {"000101", RL(6, 1)},
    {"000100", RL(7, 1)}, {"0000110", RL(0, 4)}, {"0000100", RL(2, 2)}, {"0000111", RL(8, 1)}, {"0000101", RL(9, 1)},
    {"00100110", RL(0, 5)}, {"00100001", RL(0, 6)}, {"00100101", RL(1, 3)}, {"00100100", RL(3, 2)}, {"00100111", RL(10, 1)},
    {"00100011", RL(11, 1)}, {"00100010", RL(12, 1)}, {"00100000", RL(13, 1)},
    {"0000001010", RL(0, 7)}, {"0000001100", RL(1, 4)}, {"0000001011", RL(2, 3)}, {"0000001111", RL(4, 2)},
    {"0000001001", RL(5, 2)}, {"0000001110", RL(14, 1)}, {"0000001101", RL(15, 1)}, {"0000001000", RL(16, 1)},
    {"000000011101", RL(0, 8)}, {"000000011000", RL(0, 9)}, {"000000010011", RL(0, 10)}, {"000000010000", RL(0, 11)},
    {"000000011011", RL(1, 5)}, {"000000010100", RL(2, 4)},
    {"0000000011010", RL(0, 12)}, {"0000000011001", RL(0, 13)}, {"0000000011000", RL(0, 14)}, {"0000000010111", RL(0, 15)},
    DCT_SHARED,
};

// B-15 (table one), for intra blocks where the picture says so.
const Code kDctOne[] = {
    {"0110", kEnd}, {"10", RL(0, 1)}, {"010", RL(1, 1)}, {"110", RL(0, 2)}, {"00101", RL(2, 1)}, {"0111", RL(0, 3)},
    {"00111", RL(3, 1)}, {"000110", RL(4, 1)}, {"00110", RL(1, 2)}, {"000111", RL(5, 1)}, {"0000110", RL(6, 1)},
    {"0000100", RL(7, 1)}, {"11100", RL(0, 4)}, {"0000111", RL(2, 2)}, {"0000101", RL(8, 1)}, {"1111000", RL(9, 1)},
    {"11101", RL(0, 5)}, {"000101", RL(0, 6)}, {"1111001", RL(1, 3)}, {"00100110", RL(3, 2)}, {"1111010", RL(10, 1)},
    {"00100001", RL(11, 1)}, {"00100101", RL(12, 1)}, {"00100100", RL(13, 1)},
    {"000100", RL(0, 7)}, {"00100111", RL(1, 4)}, {"11111100", RL(2, 3)}, {"11111101", RL(4, 2)},
    {"000000100", RL(5, 2)}, {"000000101", RL(14, 1)}, {"000000111", RL(15, 1)}, {"0000001101", RL(16, 1)},
    {"1111011", RL(0, 8)}, {"1111100", RL(0, 9)}, {"00100011", RL(0, 10)}, {"00100010", RL(0, 11)},
    {"00100000", RL(1, 5)}, {"0000001100", RL(2, 4)},
    {"11111010", RL(0, 12)}, {"11111011", RL(0, 13)}, {"11111110", RL(0, 14)}, {"11111111", RL(0, 15)},
    DCT_SHARED,
};
#undef RL

#define COUNT(a) ((int)(sizeof(a) / sizeof(a[0])))

struct Tables {
    Table increment, typeI, typeP, typeB, pattern, motion, dcLuma, dcChroma, dctZero, dctOne;
    float idct[8][8];   // [x][u]: half of C(u) cos((2x + 1) u pi / 16)
    Tables()
    {
        increment.Build(kIncrement, COUNT(kIncrement));
        typeI.Build(kTypeI, COUNT(kTypeI));
        typeP.Build(kTypeP, COUNT(kTypeP));
        typeB.Build(kTypeB, COUNT(kTypeB));
        pattern.Build(kPatternCodes, COUNT(kPatternCodes));
        motion.Build(kMotion, COUNT(kMotion));
        dcLuma.Build(kDcLuma, COUNT(kDcLuma));
        dcChroma.Build(kDcChroma, COUNT(kDcChroma));
        dctZero.Build(kDctZero, COUNT(kDctZero));
        dctOne.Build(kDctOne, COUNT(kDctOne));
        for (int x = 0; x < 8; x++)
            for (int u = 0; u < 8; u++)
                idct[x][u] = (float)(0.5 * (u == 0 ? std::sqrt(0.5) : 1.0) * std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0));
    }
};
const Tables& T()
{
    static const Tables t;
    return t;
}

// The two orders a block's coefficients come in (figures 7-2 and 7-3): the place in the block
// of each in turn.
const std::uint8_t kZigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};
const std::uint8_t kAlternate[64] = {
    0,  8,  16, 24, 1,  9,  2,  10, 17, 25, 32, 40, 48, 56, 57, 49, 41, 33, 26, 18, 3,  11,
    4,  12, 19, 27, 34, 42, 50, 58, 35, 43, 51, 59, 20, 28, 5,  13, 6,  14, 21, 29, 36, 44,
    52, 60, 37, 45, 53, 61, 22, 30, 7,  15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63,
};

// The default intra quantiser matrix (6.3.11), in the block's own order.
const std::uint8_t kDefaultIntra[64] = {
    8,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34,
    34, 38, 22, 22, 26, 27, 29, 34, 37, 40, 22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32,
    35, 40, 48, 58, 26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83,
};

// The quantiser scale of a code where the picture's q_scale_type is 1 (table 7-6).
const std::uint8_t kNonLinearScale[32] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 18, 20, 22, 24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 104, 112,
};

// A code of a table read; false where the bits are no code.
inline bool Read(Mpeg2::Bits& b, const Table& t, int* value);

// The inverse transform of a block, in place: rows then columns, each by the 8-point cosines.
// Rows that are all zero are passed over.
void InverseTransform(std::int16_t* block)
{
    const Tables& t = T();
    float tmp[64];
    for (int r = 0; r < 8; r++) {
        const std::int16_t* in = block + r * 8;
        float* out = tmp + r * 8;
        if (!(in[1] | in[2] | in[3] | in[4] | in[5] | in[6] | in[7])) {
            const float v = (float)in[0] * t.idct[0][0];
            for (int x = 0; x < 8; x++)
                out[x] = v;
            continue;
        }
        for (int x = 0; x < 8; x++) {
            float s = 0.0f;
            for (int u = 0; u < 8; u++)
                s += t.idct[x][u] * (float)in[u];
            out[x] = s;
        }
    }
    for (int c = 0; c < 8; c++) {
        for (int y = 0; y < 8; y++) {
            float s = 0.0f;
            for (int v = 0; v < 8; v++)
                s += t.idct[y][v] * tmp[v * 8 + c];
            const long r = std::lrintf(s);
            block[y * 8 + c] = (std::int16_t)(r < -256 ? -256 : r > 255 ? 255 : r);
        }
    }
}

inline std::uint8_t Clip(int v) { return (std::uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

}  // namespace

namespace {
inline bool Read(Mpeg2::Bits& b, const Table& t, int* value)
{
    const std::uint32_t next = b.Peek(t.length);
    if (t.size[next] == 0)
        return false;
    *value = t.value[next];
    b.Skip(t.size[next]);
    return true;
}
}  // namespace

// ---------------------------------------------------------------------------------------------

void Mpeg2::Reset()
{
    es_.clear();
    at_ = scanned_ = 0;
    ended_ = finishing_ = false;
    errors_ = 0;
    haveSequence_ = false;
    forward_ = backward_ = -1;
    current_ = 0;
    pending_ = false;
    pendingFrame_ = -1;
}

void Mpeg2::Feed(const std::uint8_t* data, std::size_t size)
{
    if (at_ > (1u << 20)) {   // what has been decoded is let go
        es_.erase(es_.begin(), es_.begin() + (std::ptrdiff_t)at_);
        scanned_ = scanned_ > at_ ? scanned_ - at_ : 0;
        at_ = 0;
    }
    es_.insert(es_.end(), data, data + size);
}

void Mpeg2::Finish() { finishing_ = true; }

void Mpeg2::Give(const Frame& f, Picture* out) const
{
    out->width = width_;
    out->height = height_;
    out->stride = mbw_ * 16;
    out->chromaStride = mbw_ * 8;
    out->y = f.y.data();
    out->cb = f.cb.data();
    out->cr = f.cr.data();
}

// The next start code at or after `from`: its place and its code; false when there is none yet.
static bool StartCode(const std::vector<std::uint8_t>& es, std::size_t from, std::size_t* at)
{
    const std::size_t n = es.size();
    for (std::size_t k = from; k + 3 < n; k++) {
        if (es[k + 2] > 1) {
            k += 2;
            continue;
        }
        if (es[k] == 0 && es[k + 1] == 0 && es[k + 2] == 1) {
            *at = k;
            return true;
        }
    }
    return false;
}

bool Mpeg2::Next(Picture* out)
{
    for (;;) {
        if (ended_) {
            if (pending_) {
                pending_ = false;
                Give(frames_[pendingFrame_], out);
                return true;
            }
            return false;
        }
        std::size_t p;
        if (!StartCode(es_, at_, &p)) {
            at_ = es_.size() > 3 ? es_.size() - 3 : 0;
            if (finishing_)
                ended_ = true;
            else
                return false;
            continue;
        }
        at_ = p;
        const int code = es_[p + 3];
        if (code == 0xB7) {   // the sequence's end
            at_ = p + 4;
            ended_ = true;
            continue;
        }
        // the unit's end: for a picture, the next picture, sequence header, group or end; for a
        // header, the next start code
        std::size_t q = p + 4;
        bool found = false;
        if (code == 0x00) {
            std::size_t from = scanned_ > p + 4 ? scanned_ : p + 4;
            while (StartCode(es_, from, &q)) {
                const int c = es_[q + 3];
                if (c == 0x00 || c == 0xB3 || c == 0xB8 || c == 0xB7) {
                    found = true;
                    break;
                }
                from = q + 4;
            }
            if (!found)
                scanned_ = es_.size() > 3 ? es_.size() - 3 : 0;
        } else {
            found = StartCode(es_, p + 4, &q);
        }
        if (!found) {
            if (!finishing_)
                return false;
            q = es_.size();
        }
        scanned_ = 0;
        at_ = q;
        if (code == 0xB3) {
            SequenceHeader(&es_[p + 4], q - p - 4);
        } else if (code == 0xB5) {
            Extension(&es_[p + 4], q - p - 4);
        } else if (code == 0x00 && haveSequence_) {
            const int was = pendingFrame_;
            const bool had = pending_;
            if (DecodePicture(&es_[p], q - p)) {
                if (type_ == 3) {
                    Give(frames_[current_], out);
                    return true;
                }
                // a reference picture: the one before it is given out now, and it waits its turn
                pending_ = true;
                pendingFrame_ = current_;
                if (had) {
                    Give(frames_[was], out);
                    return true;
                }
            }
        }
    }
}

void Mpeg2::SequenceHeader(const std::uint8_t* p, std::size_t n)
{
    Bits b{p, n};
    const int width = (int)b.Get(12), height = (int)b.Get(12);
    b.Skip(4);
    rateCode_ = (int)b.Get(4);
    b.Skip(18 + 1 + 10 + 1);
    if (b.Get(1)) {
        for (int k = 0; k < 64; k++)
            intraQ_[kZigzag[k]] = (std::uint8_t)b.Get(8);
    } else {
        std::memcpy(intraQ_, kDefaultIntra, 64);
    }
    if (b.Get(1)) {
        for (int k = 0; k < 64; k++)
            nonIntraQ_[kZigzag[k]] = (std::uint8_t)b.Get(8);
    } else {
        std::memset(nonIntraQ_, 16, 64);
    }
    if (b.Over() || width <= 0 || height <= 0 || width > 1920 || height > 1152) {
        errors_++;
        return;
    }
    if (!haveSequence_ || width != width_ || height != height_) {
        width_ = width;
        height_ = height;
        mbw_ = (width + 15) / 16;
        mbh_ = (height + 15) / 16;
        for (Frame& f : frames_) {
            f.y.assign((std::size_t)mbw_ * 16 * (std::size_t)mbh_ * 16, 0);
            f.cb.assign((std::size_t)mbw_ * 8 * (std::size_t)mbh_ * 8, 128);
            f.cr.assign((std::size_t)mbw_ * 8 * (std::size_t)mbh_ * 8, 128);
        }
        forward_ = backward_ = -1;
        pending_ = false;
    }
    haveSequence_ = true;
    // (an MPEG-1 picture has none of the picture coding extension: its values as that standard has them)
    dcPrecision_ = 0;
    structure_ = 3;
    framePredFrameDct_ = true;
    concealment_ = qScaleType_ = intraVlc_ = alternate_ = false;
}

void Mpeg2::Extension(const std::uint8_t* p, std::size_t n)
{
    Bits b{p, n};
    const int id = (int)b.Get(4);
    if (id == 8) {   // the picture coding extension
        fcode_[0][0] = (int)b.Get(4);
        fcode_[0][1] = (int)b.Get(4);
        fcode_[1][0] = (int)b.Get(4);
        fcode_[1][1] = (int)b.Get(4);
        dcPrecision_ = (int)b.Get(2);
        structure_ = (int)b.Get(2);
        b.Skip(1);   // top field first
        framePredFrameDct_ = b.Get(1) != 0;
        concealment_ = b.Get(1) != 0;
        qScaleType_ = b.Get(1) != 0;
        intraVlc_ = b.Get(1) != 0;
        alternate_ = b.Get(1) != 0;
    } else if (id == 3) {   // the quantiser matrices
        if (b.Get(1))
            for (int k = 0; k < 64; k++)
                intraQ_[kZigzag[k]] = (std::uint8_t)b.Get(8);
        if (b.Get(1))
            for (int k = 0; k < 64; k++)
                nonIntraQ_[kZigzag[k]] = (std::uint8_t)b.Get(8);
    }
    // (the sequence's own extensions, 1 and 2, say nothing the decoding needs)
}

bool Mpeg2::DecodePicture(const std::uint8_t* p, std::size_t n)
{
    {
        Bits b{p + 4, n - 4};
        b.Skip(10);   // the temporal reference
        type_ = (int)b.Get(3);
        b.Skip(16);
        if (type_ == 2 || type_ == 3) {   // MPEG-1's vector codes, which the extension replaces
            b.Skip(1);
            const int f = (int)b.Get(3);
            fcode_[0][0] = fcode_[0][1] = f;
        }
        if (type_ == 3) {
            b.Skip(1);
            const int f = (int)b.Get(3);
            fcode_[1][0] = fcode_[1][1] = f;
        }
    }
    if (type_ < 1 || type_ > 3) {
        errors_++;
        return false;
    }
    // the picture's own extensions, then its slices
    std::vector<std::uint8_t> view;   // (the unit as a vector, for the start code search)
    view.assign(p, p + n);
    std::size_t at = 4, q;
    bool begun = false;
    while (StartCode(view, at, &q)) {
        const int code = view[q + 3];
        std::size_t next;
        if (!StartCode(view, q + 4, &next))
            next = n;
        if (code == 0xB5) {
            Extension(&view[q + 4], next - q - 4);
        } else if (code >= 0x01 && code <= 0xAF) {
            if (!begun) {
                begun = true;
                if (structure_ != 3 || !framePredFrameDct_) {   // a field picture, or field prediction: none of the movies has one
                    errors_++;
                    return false;
                }
                // where it is decoded to, and what it is predicted from
                if (type_ == 3) {
                    if (forward_ < 0 || backward_ < 0)
                        return false;   // (a B picture with a reference missing: at a stream's start)
                    current_ = 3 - forward_ - backward_;
                } else {
                    const int target = backward_ < 0 ? 0 : forward_ < 0 ? 1 : forward_;
                    forward_ = backward_;
                    backward_ = target;
                    current_ = target;
                    if (type_ == 2 && forward_ < 0)
                        return false;
                }
            }
            Bits b{&view[q + 4], next - q - 4};
            Slice(b, code - 1);
        }
        at = next;
        if (next >= n)
            break;
    }
    return begun;
}

// One motion vector component: the code and its residual added to the prediction, kept in
// range (7.6.3.1).
static int VectorComponent(Mpeg2::Bits& b, int fcode, int pred, bool* bad)
{
    int size;
    if (!Read(b, T().motion, &size)) {
        *bad = true;
        return pred;
    }
    int delta = 0;
    const int rsize = fcode - 1, f = 1 << rsize;
    if (size != 0) {
        const bool negative = b.Get(1) != 0;
        const int residual = rsize > 0 ? (int)b.Get(rsize) : 0;
        delta = (size - 1) * f + residual + 1;
        if (negative)
            delta = -delta;
    }
    int v = pred + delta;
    const int low = -16 * f, high = 16 * f - 1, range = 32 * f;
    if (v < low)
        v += range;
    else if (v > high)
        v -= range;
    return v;
}

void Mpeg2::Slice(Bits& b, int row)
{
    if (row >= mbh_) {
        errors_++;
        return;
    }
    Frame& cur = frames_[current_];
    auto scale = [&](int code) { return qScaleType_ ? (int)kNonLinearScale[code] : code * 2; };
    qscale_ = scale((int)b.Get(5));
    while (b.Get(1))
        b.Skip(8);
    const int dcReset = 1 << (dcPrecision_ + 7);
    dcPred_[0] = dcPred_[1] = dcPred_[2] = dcReset;
    std::memset(pmv_, 0, sizeof pmv_);
    int address = row * mbw_ - 1;
    bool first = true;
    int lastFlags = kForward;   // the motion of the macroblock before, for a B picture's skipped ones
    const int total = mbw_ * mbh_;
    std::int16_t block[64];
    static const int zero[2] = {0, 0};

    for (;;) {
        if (b.Over() || b.Peek(23) == 0)
            break;
        int increment = 0;
        for (;;) {
            int v;
            if (!Read(b, T().increment, &v)) {
                errors_++;
                return;
            }
            if (v == 34) {
                increment += 33;
                continue;
            }
            increment += v;
            break;
        }
        if (first) {
            address = row * mbw_ + increment - 1;
            first = false;
        } else {
            // the macroblocks passed over: no difference, and the motion of none (P) or of the last (B)
            for (int k = 1; k < increment; k++) {
                const int a = address + k;
                if (a >= total)
                    break;
                const int mbx = a % mbw_, mby = a / mbw_;
                if (type_ == 2) {
                    Predict(mbx, mby, true, false, zero, zero);
                    std::memset(pmv_, 0, sizeof pmv_);
                } else {
                    Predict(mbx, mby, (lastFlags & kForward) != 0, (lastFlags & kBackward) != 0, pmv_[0], pmv_[1]);
                }
                for (int y = 0; y < 16; y++)
                    std::memcpy(&cur.y[(std::size_t)(mby * 16 + y) * (std::size_t)(mbw_ * 16) + (std::size_t)mbx * 16], predY_ + y * 16, 16);
                for (int y = 0; y < 8; y++) {
                    std::memcpy(&cur.cb[(std::size_t)(mby * 8 + y) * (std::size_t)(mbw_ * 8) + (std::size_t)mbx * 8], predCb_ + y * 8, 8);
                    std::memcpy(&cur.cr[(std::size_t)(mby * 8 + y) * (std::size_t)(mbw_ * 8) + (std::size_t)mbx * 8], predCr_ + y * 8, 8);
                }
            }
            if (increment > 1)
                dcPred_[0] = dcPred_[1] = dcPred_[2] = dcReset;
            address += increment;
        }
        if (address >= total) {
            errors_++;
            return;
        }
        const int mbx = address % mbw_, mby = address / mbw_;

        int flags;
        if (!Read(b, type_ == 1 ? T().typeI : type_ == 2 ? T().typeP : T().typeB, &flags)) {
            errors_++;
            return;
        }
        if (flags & kQuant)
            qscale_ = scale((int)b.Get(5));
        bool bad = false;
        if ((flags & kForward) || ((flags & kIntra) && concealment_)) {
            pmv_[0][0] = VectorComponent(b, fcode_[0][0], pmv_[0][0], &bad);
            pmv_[0][1] = VectorComponent(b, fcode_[0][1], pmv_[0][1], &bad);
        }
        if (flags & kBackward) {
            pmv_[1][0] = VectorComponent(b, fcode_[1][0], pmv_[1][0], &bad);
            pmv_[1][1] = VectorComponent(b, fcode_[1][1], pmv_[1][1], &bad);
        }
        if (bad) {
            errors_++;
            return;
        }
        if ((flags & kIntra) && concealment_)
            b.Skip(1);

        int pattern = 0;
        if (flags & kIntra) {
            pattern = 63;
            if (!concealment_)
                std::memset(pmv_, 0, sizeof pmv_);
            std::memset(predY_, 0, sizeof predY_);
            std::memset(predCb_, 0, sizeof predCb_);
            std::memset(predCr_, 0, sizeof predCr_);
        } else {
            dcPred_[0] = dcPred_[1] = dcPred_[2] = dcReset;
            if (flags & kPattern) {
                if (!Read(b, T().pattern, &pattern)) {
                    errors_++;
                    return;
                }
            }
            if (type_ == 2) {
                if (!(flags & kForward))
                    std::memset(pmv_, 0, sizeof pmv_);   // no vector: none, and the prediction of it forgotten
                Predict(mbx, mby, true, false, pmv_[0], zero);
            } else {
                Predict(mbx, mby, (flags & kForward) != 0, (flags & kBackward) != 0, pmv_[0], pmv_[1]);
            }
            lastFlags = flags;
        }

        // the six blocks: four of luminance, one of each colour difference
        for (int k = 0; k < 6; k++) {
            std::uint8_t* pred;
            std::uint8_t* dst;
            int stride;
            if (k < 4) {
                pred = predY_ + (k >> 1) * 8 * 16 + (k & 1) * 8;
                stride = mbw_ * 16;
                dst = &cur.y[(std::size_t)(mby * 16 + (k >> 1) * 8) * (std::size_t)stride + (std::size_t)(mbx * 16 + (k & 1) * 8)];
            } else {
                pred = k == 4 ? predCb_ : predCr_;
                stride = mbw_ * 8;
                dst = &(k == 4 ? cur.cb : cur.cr)[(std::size_t)(mby * 8) * (std::size_t)stride + (std::size_t)(mbx * 8)];
            }
            const int predStride = k < 4 ? 16 : 8;
            if (pattern & (32 >> k)) {
                if (!Block(b, k < 4 ? 0 : k - 3, (flags & kIntra) != 0, block)) {
                    errors_++;
                    return;
                }
                InverseTransform(block);
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++)
                        dst[y * stride + x] = Clip(pred[y * predStride + x] + block[y * 8 + x]);
            } else {
                for (int y = 0; y < 8; y++)
                    std::memcpy(dst + y * stride, pred + y * predStride, 8);
            }
        }
    }
}

// A block's coefficients, read, scaled back and put in their places (7.2, 7.3, 7.4).
bool Mpeg2::Block(Bits& b, int component, bool intra, std::int16_t* out)
{
    std::memset(out, 0, 64 * sizeof(std::int16_t));
    const std::uint8_t* scan = alternate_ ? kAlternate : kZigzag;
    const std::uint8_t* matrix = intra ? intraQ_ : nonIntraQ_;
    const Table& table = intra && intraVlc_ ? T().dctOne : T().dctZero;
    int index = 0, sum = 0;
    if (intra) {
        int size;
        if (!Read(b, component == 0 ? T().dcLuma : T().dcChroma, &size))
            return false;
        int diff = 0;
        if (size) {
            diff = (int)b.Get(size);
            if (diff < (1 << (size - 1)))
                diff = diff - (1 << size) + 1;
        }
        dcPred_[component] += diff;
        out[0] = (std::int16_t)(dcPred_[component] << (3 - dcPrecision_));
        sum = out[0];
        index = 1;
    }
    for (bool first = !intra;; first = false) {
        int run, level;
        if (first && b.Peek(1)) {   // a non-intra block's first coefficient: "1" is run 0, level 1
            b.Skip(1);
            run = 0;
            level = 1;
            if (b.Get(1))
                level = -1;
        } else {
            int v;
            if (!Read(b, table, &v))
                return false;
            if (v == kEnd)
                break;
            if (v == kEscape) {
                run = (int)b.Get(6);
                level = (int)b.Get(12);
                if (level & 0x800)
                    level -= 4096;
                if (level == 0 || level == -2048)
                    return false;
            } else {
                run = v >> 6;
                level = v & 63;
                if (b.Get(1))
                    level = -level;
            }
        }
        index += run;
        if (index > 63)
            return false;
        const int at = scan[index];
        int value;
        if (intra)
            value = level * (int)matrix[at] * qscale_ / 16;
        else
            value = (2 * level + (level > 0 ? 1 : -1)) * (int)matrix[at] * qscale_ / 32;
        value = value < -2048 ? -2048 : value > 2047 ? 2047 : value;
        out[at] = (std::int16_t)value;
        sum += value;
        index++;
    }
    if ((sum & 1) == 0)   // the mismatch control: the sum made odd by the last coefficient's lowest bit
        out[63] ^= 1;
    return !b.Over();
}

// A macroblock's prediction from a reference at a vector in half samples (7.6.4): whole
// samples, or the rounded mean of the two or four around the place.
void Mpeg2::Fetch(const Frame& ref, int mbx, int mby, const int mv[2], std::uint8_t* y, std::uint8_t* cb, std::uint8_t* cr) const
{
    auto plane = [](const std::uint8_t* src, int stride, int width, int height, int x0, int y0, int hx, int hy, int size, std::uint8_t* dst) {
        const bool inside = x0 >= 0 && y0 >= 0 && x0 + size + hx <= width && y0 + size + hy <= height;
        if (inside) {
            const std::uint8_t* s = src + (std::size_t)y0 * (std::size_t)stride + (std::size_t)x0;
            for (int j = 0; j < size; j++, s += stride, dst += size) {
                if (!hx && !hy)
                    std::memcpy(dst, s, (std::size_t)size);
                else if (hx && !hy)
                    for (int i = 0; i < size; i++)
                        dst[i] = (std::uint8_t)((s[i] + s[i + 1] + 1) >> 1);
                else if (!hx && hy)
                    for (int i = 0; i < size; i++)
                        dst[i] = (std::uint8_t)((s[i] + s[i + stride] + 1) >> 1);
                else
                    for (int i = 0; i < size; i++)
                        dst[i] = (std::uint8_t)((s[i] + s[i + 1] + s[i + stride] + s[i + stride + 1] + 2) >> 2);
            }
            return;
        }
        auto at = [&](int x, int yy) {   // (a vector out of the picture: its edge)
            x = x < 0 ? 0 : x >= width ? width - 1 : x;
            yy = yy < 0 ? 0 : yy >= height ? height - 1 : yy;
            return (int)src[(std::size_t)yy * (std::size_t)stride + (std::size_t)x];
        };
        for (int j = 0; j < size; j++)
            for (int i = 0; i < size; i++) {
                const int x = x0 + i, yy = y0 + j;
                dst[j * size + i] = (std::uint8_t)(!hx && !hy   ? at(x, yy)
                                                   : hx && !hy  ? (at(x, yy) + at(x + 1, yy) + 1) >> 1
                                                   : !hx && hy  ? (at(x, yy) + at(x, yy + 1) + 1) >> 1
                                                                : (at(x, yy) + at(x + 1, yy) + at(x, yy + 1) + at(x + 1, yy + 1) + 2) >> 2);
            }
    };
    const int w = mbw_ * 16, h = mbh_ * 16;
    plane(ref.y.data(), w, w, h, mbx * 16 + (mv[0] >> 1), mby * 16 + (mv[1] >> 1), mv[0] & 1, mv[1] & 1, 16, y);
    const int cx = mv[0] / 2, cy = mv[1] / 2;   // the colour planes' vector: halved toward zero
    plane(ref.cb.data(), w / 2, w / 2, h / 2, mbx * 8 + (cx >> 1), mby * 8 + (cy >> 1), cx & 1, cy & 1, 8, cb);
    plane(ref.cr.data(), w / 2, w / 2, h / 2, mbx * 8 + (cx >> 1), mby * 8 + (cy >> 1), cx & 1, cy & 1, 8, cr);
}

void Mpeg2::Predict(int mbx, int mby, bool forward, bool backward, const int fwd[2], const int bwd[2])
{
    // a P picture's reference is the last one decoded; a B picture's are the two around it
    const int fwdFrame = forward_, bwdFrame = backward_;
    if (forward && backward && fwdFrame >= 0 && bwdFrame >= 0) {
        std::uint8_t y[256], cb[64], cr[64];
        Fetch(frames_[fwdFrame], mbx, mby, fwd, predY_, predCb_, predCr_);
        Fetch(frames_[bwdFrame], mbx, mby, bwd, y, cb, cr);
        for (int k = 0; k < 256; k++)
            predY_[k] = (std::uint8_t)((predY_[k] + y[k] + 1) >> 1);
        for (int k = 0; k < 64; k++) {
            predCb_[k] = (std::uint8_t)((predCb_[k] + cb[k] + 1) >> 1);
            predCr_[k] = (std::uint8_t)((predCr_[k] + cr[k] + 1) >> 1);
        }
    } else if (backward && bwdFrame >= 0) {
        Fetch(frames_[bwdFrame], mbx, mby, bwd, predY_, predCb_, predCr_);
    } else if (fwdFrame >= 0) {
        Fetch(frames_[fwdFrame], mbx, mby, fwd, predY_, predCb_, predCr_);
    } else {
        std::memset(predY_, 0, sizeof predY_);
        std::memset(predCb_, 128, sizeof predCb_);
        std::memset(predCr_, 128, sizeof predCr_);
    }
}

}  // namespace video
}  // namespace port
