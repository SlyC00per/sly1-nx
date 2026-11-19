// SPDX-License-Identifier: AGPL-3.0-or-later
// An MPEG-2 video decoder for the game's movies (port/video/mpeg2.cpp), written for this
// project from the public standard (ISO/IEC 13818-2). It decodes what the 17 movies use: frame
// pictures of 4:2:0 with frame prediction and the frame transform only, I, P and B pictures,
// both coefficient tables, both scans and both quantiser scales.
#ifndef PORT_MPEG2_H
#define PORT_MPEG2_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace port {
namespace video {

// A decoded picture: three planes, the two colour ones at half the size each way.
struct Picture {
    int width = 0, height = 0;         // the picture's own size
    int stride = 0, chromaStride = 0;  // a plane's row length (the size in whole macroblocks)
    const std::uint8_t* y = nullptr;
    const std::uint8_t* cb = nullptr;
    const std::uint8_t* cr = nullptr;
};

class Mpeg2 {
public:
    // More of the video's elementary stream.
    void Feed(const std::uint8_t* data, std::size_t size);
    // No more will come: what is held back for the display order is given out.
    void Finish();
    // The next picture in display order; false when more of the stream is needed (or it is over).
    bool Next(Picture* out);
    // The sequence's end code has been read (or Finish called) and every picture given out.
    bool Over() const { return ended_ && !pending_; }
    // Back to the start, for another stream.
    void Reset();

    int Width() const { return width_; }
    int Height() const { return height_; }
    int FrameRateCode() const { return rateCode_; }
    long Errors() const { return errors_; }
    std::size_t Buffered() const { return es_.size() - at_; }

    struct Bits;   // (the bit reader, which the tables' readers share)

    // Two at once: `run` calls fn(arg, 0) and fn(arg, 1) on two threads and returns when both
    // have. With one set, a picture's slices are decoded half by each (a slice is its own run of
    // bits and its own rows of the picture).
    typedef void (*Pair)(void (*fn)(void* arg, int which), void* arg);
    static void SetPair(Pair run);

private:
    struct Frame {
        std::vector<std::uint8_t> y, cb, cr;
    };

    bool Unit(std::size_t* from, std::size_t* to, int* code);
    void SequenceHeader(const std::uint8_t* p, std::size_t n);
    void Extension(const std::uint8_t* p, std::size_t n);
    bool DecodePicture(const std::uint8_t* p, std::size_t n);
    // what a slice keeps as it goes: each thread decoding slices has its own
    struct SliceState {
        int qscale = 0;
        int dcPred[3] = {};
        int pmv[2][2] = {};
        long errors = 0;
        std::uint8_t predY[256], predCb[64], predCr[64];
    };
    struct SliceJob {
        const std::uint8_t* data;
        std::size_t size;
        int row;
    };
    void Slice(Bits& b, int row, SliceState& st);
    bool Block(Bits& b, int component, bool intra, std::int16_t* out, SliceState& st, int* last);
    void Predict(int mbx, int mby, bool forward, bool backward, const int fwd[2], const int bwd[2], SliceState& st);
    static void SliceHalf(void* arg, int which);
    void Fetch(const Frame& ref, int mbx, int mby, const int mv[2], std::uint8_t* y, std::uint8_t* cb, std::uint8_t* cr) const;
    void Give(const Frame& f, Picture* out) const;

    std::vector<std::uint8_t> es_;
    std::size_t at_ = 0, scanned_ = 0;
    bool ended_ = false, finishing_ = false;
    long errors_ = 0;

    int width_ = 0, height_ = 0, mbw_ = 0, mbh_ = 0, rateCode_ = 0;
    std::uint8_t intraQ_[64], nonIntraQ_[64];
    bool haveSequence_ = false;

    // the picture being decoded
    int type_ = 0;   // 1 I, 2 P, 3 B
    int fcode_[2][2] = {};
    int dcPrecision_ = 0, structure_ = 3;
    bool framePredFrameDct_ = true, concealment_ = false, qScaleType_ = false, intraVlc_ = false, alternate_ = false;
    std::vector<SliceJob> jobs_;
    std::vector<std::uint8_t> unit_;   // the picture being decoded, copied (the slices point into it)

    Frame frames_[3];
    int current_ = 0, forward_ = -1, backward_ = -1;   // indices of frames_
    bool pending_ = false;                               // the last reference is still to be given out
    int pendingFrame_ = -1;
};

}  // namespace video
}  // namespace port

#endif
