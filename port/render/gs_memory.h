// SPDX-License-Identifier: AGPL-3.0-or-later
// The GS's local memory (4 MiB) and where each pixel format keeps a pixel in it, from the public
// descriptions of the GS's memory arrangement: memory is pages of 8 KiB, a page 32 blocks of 256
// bytes, a block 4 columns of 64 bytes. A buffer's base pointer counts blocks and its width 64
// pixels. The arrangements are written as the rules they follow (the order of words in a column,
// of blocks in a page as a Z order, the 8- and 4-bit formats' interleaving of a column's rows)
// rather than as tables.
#ifndef PORT_RENDER_GS_MEMORY_H
#define PORT_RENDER_GS_MEMORY_H

#include <cstdint>

namespace port {
namespace gs {

constexpr std::uint32_t kLocalBytes = 4u << 20;

enum Psm : std::uint32_t {
    PSMCT32 = 0x00, PSMCT24 = 0x01, PSMCT16 = 0x02, PSMCT16S = 0x0A,
    PSMT8 = 0x13, PSMT4 = 0x14, PSMT8H = 0x1B, PSMT4HL = 0x24, PSMT4HH = 0x2C,
    PSMZ32 = 0x30, PSMZ24 = 0x31, PSMZ16 = 0x32, PSMZ16S = 0x3A,
};

std::uint8_t* Local();

// A column's word for a 32-bit pixel at (x, y) of its 8 by 2 pixels.
inline std::uint32_t ColumnWord32(std::uint32_t x, std::uint32_t y)
{
    return (x & 1) | ((x >> 1) & 3) << 2 | (y & 1) << 1;
}

// A block's place in a page laid out 8 blocks wide and 4 high (32- and 8-bit formats).
inline std::uint32_t BlockWide(std::uint32_t bx, std::uint32_t by)
{
    return (bx & 1) | (by & 1) << 1 | (bx & 2) << 1 | (by & 2) << 2 | (bx & 4) << 2;
}

// A block's place in a page laid out 4 blocks wide and 8 high (16- and 4-bit formats).
inline std::uint32_t BlockTall(std::uint32_t bx, std::uint32_t by)
{
    return (by & 1) | (bx & 1) << 1 | (by & 2) << 1 | (bx & 2) << 2 | (by & 4) << 2;
}

// The depth formats' blocks are the colour formats' with the page's halves exchanged.
inline std::uint32_t ZBlock(std::uint32_t psm, std::uint32_t block)
{
    return (psm & 0x30) == 0x30 ? block ^ 24 : block;
}

// Byte address of the 32-bit pixel (PSMCT32, PSMCT24, PSMZ32, PSMZ24) at (x, y).
inline std::uint32_t Addr32(std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y, std::uint32_t psm = PSMCT32)
{
    std::uint32_t page = (y >> 5) * (bw ? bw : 1) + (x >> 6);
    std::uint32_t block = bp + page * 32 + ZBlock(psm, BlockWide((x >> 3) & 7, (y >> 3) & 3));
    std::uint32_t word = ((y >> 1) & 3) * 16 + ColumnWord32(x & 7, y & 1);
    return (block * 256 + word * 4) & (kLocalBytes - 1);
}

// Byte address of the 16-bit pixel (PSMCT16, PSMCT16S, PSMZ16, PSMZ16S) at (x, y).
inline std::uint32_t Addr16(std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y, std::uint32_t psm = PSMCT16)
{
    std::uint32_t page = (y >> 6) * (bw ? bw : 1) + (x >> 6);
    std::uint32_t bx = (x >> 4) & 3, by = (y >> 3) & 7, b;
    if (psm == PSMCT16S || psm == PSMZ16S)   // the S formats order their blocks differently
        b = (by & 1) | (bx & 1) << 1 | (bx & 2) << 3 | (by & 2) << 2 | (by & 4);
    else
        b = BlockTall(bx, by);
    std::uint32_t block = bp + page * 32 + ZBlock(psm, b);
    std::uint32_t half = ((y >> 1) & 3) * 32 + ColumnWord32(x & 7, y & 1) * 2 + ((x >> 3) & 1);
    return (block * 256 + half * 2) & (kLocalBytes - 1);
}

// Byte address of the 8-bit pixel (PSMT8) at (x, y): a column's 16 by 4 pixels interleave two
// rows' bytes in each word, the odd columns with their halves exchanged.
inline std::uint32_t Addr8(std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y)
{
    std::uint32_t pw = (bw ? bw : 2) >> 1;   // pages 128 pixels wide
    std::uint32_t page = (y >> 6) * (pw ? pw : 1) + (x >> 7);
    std::uint32_t block = bp + page * 32 + BlockWide((x >> 4) & 7, (y >> 4) & 3);
    std::uint32_t column = (y >> 2) & 3, r = y & 3;
    std::uint32_t swap = ((r >> 1) ^ column) & 1;
    std::uint32_t x32 = ((x & 7) + (swap ? 4 : 0)) & 7;
    std::uint32_t word = ColumnWord32(x32, r & 1);
    std::uint32_t byte = (r >> 1) + ((x >> 3) & 1) * 2;
    return (block * 256 + column * 64 + word * 4 + byte) & (kLocalBytes - 1);
}

// The nibble address (bytes * 2, low nibble first) of the 4-bit pixel (PSMT4) at (x, y): as the
// 8-bit format, a column 32 by 4 pixels.
inline std::uint32_t Addr4(std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y)
{
    std::uint32_t pw = (bw ? bw : 2) >> 1;   // pages 128 pixels wide, 128 high
    std::uint32_t page = (y >> 7) * (pw ? pw : 1) + (x >> 7);
    std::uint32_t block = bp + page * 32 + BlockTall((x >> 5) & 3, (y >> 4) & 7);
    std::uint32_t column = (y >> 2) & 3, r = y & 3;
    std::uint32_t swap = ((r >> 1) ^ column) & 1;
    std::uint32_t x32 = ((x & 7) + (swap ? 4 : 0)) & 7;
    std::uint32_t word = ColumnWord32(x32, r & 1);
    std::uint32_t nibble = (r >> 1) + ((x >> 3) & 3) * 2;
    return ((block * 256 + column * 64 + word * 4) * 2 + nibble) & (kLocalBytes * 2 - 1);
}

// A pixel of any format, as the bits it holds (read) or is given (write).
std::uint32_t ReadPixel(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y);
void WritePixel(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y, std::uint32_t v);

}  // namespace gs
}  // namespace port

#endif
