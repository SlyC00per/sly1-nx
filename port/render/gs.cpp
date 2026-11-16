// SPDX-License-Identifier: AGPL-3.0-or-later
// The GS (port/gs.h): registers, local memory, transfers, primitives, and a rasteriser that draws
// as the GS does, written from the public descriptions of its registers and pixel pipeline. It is
// the host's reference renderer; the Switch's GPU renderer is checked against what it draws.
//
// Coordinates are the GS's: 12.4 fixed point, less XYOFFSET, sampled at whole pixels (a sprite
// from 0 to 640 covers pixels 0 to 639); triangles follow the top-left rule. Colours are 0 to 255
// with 0x80 as 1.0 in texture modulation and blending.
#include "port/gs.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "gs_memory.h"
#include "port/ptr32.h"

namespace port {
namespace gs {

namespace {

std::uint8_t g_local[kLocalBytes];
std::mutex g_lock;   // the three paths can reach the GS from different threads
thread_local int t_held;   // this thread's depth of holding g_lock

// Takes g_lock unless this thread holds it already.
struct Guard {
    Guard() { if (t_held++ == 0) g_lock.lock(); }
    ~Guard() { if (--t_held == 0) g_lock.unlock(); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};

std::uint64_t Bits(std::uint64_t v, int lo, int n) { return (v >> lo) & ((n == 64 ? ~0ull : (1ull << n) - 1)); }

float FloatOf(std::uint32_t b)
{
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

// ---------------------------------------------------------------------------------------------
// Registers

struct Vertex {
    std::int32_t x, y;   // 12.4, before XYOFFSET
    std::uint32_t z;
    std::uint8_t f;
    std::uint8_t r, g, b, a;
    float s, t, q;
    std::uint32_t u, v;  // 10.4 (UV)
};

struct Context {
    std::uint64_t tex0 = 0, clamp = 0, tex1 = 0, tex2 = 0, xyoffset = 0, scissor = 0, alpha = 0, test = 0,
                  fba = 0, frame = 0, zbuf = 0, miptbp1 = 0, miptbp2 = 0;
};

struct Regs {
    std::uint64_t prim = 0, prmode = 0, prmodecont = 1, texclut = 0, scanmsk = 0, texa = 0, fogcol = 0,
                  dimx = 0, dthe = 0, colclamp = 1, pabe = 0, bitbltbuf = 0, trxpos = 0, trxreg = 0, trxdir = 0;
    std::uint8_t r = 0, g = 0, b = 0, a = 0x80;
    float q = 1.0f, s = 0, t = 0;
    std::uint32_t u = 0, v = 0;
    std::uint8_t fog = 0;
    Context ctx[2];
} g_regs;

Vertex g_queue[3];
int g_queued = 0;
std::atomic<std::uint64_t> g_drawn{0};
bool g_rasterise;   // the host's reference drawing: on for --dump-fb (registers and uploads always)

// The CLUT buffer: 256 32-bit entries (a 16-bit palette's entries kept expanded), and the CBP the
// last loads used (CLD's conditions).
std::uint32_t g_clut[256];
std::uint32_t g_clutPsm = PSMCT32;
std::uint32_t g_cbp0 = ~0u, g_cbp1 = ~0u;

// The display, as the game last swapped it.
std::uint64_t g_dispfb = 0, g_display = 0;
bool g_displaySet = false;

// ---------------------------------------------------------------------------------------------
// Pixels

std::uint32_t Read32(std::uint32_t a) { std::uint32_t v; std::memcpy(&v, g_local + a, 4); return v; }
void Write32(std::uint32_t a, std::uint32_t v) { std::memcpy(g_local + a, &v, 4); }
std::uint16_t Read16(std::uint32_t a) { std::uint16_t v; std::memcpy(&v, g_local + a, 2); return v; }
void Write16(std::uint32_t a, std::uint16_t v) { std::memcpy(g_local + a, &v, 2); }

}  // namespace

std::uint8_t* Local() { return g_local; }

std::uint32_t ReadPixel(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y)
{
    switch (psm) {
    case PSMCT32: case PSMZ32: return Read32(Addr32(bp, bw, x, y, psm));
    case PSMCT24: case PSMZ24: return Read32(Addr32(bp, bw, x, y, psm)) & 0xFFFFFF;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return Read16(Addr16(bp, bw, x, y, psm));
    case PSMT8: return g_local[Addr8(bp, bw, x, y)];
    case PSMT4: {
        std::uint32_t n = Addr4(bp, bw, x, y);
        return (g_local[n >> 1] >> ((n & 1) * 4)) & 0xF;
    }
    case PSMT8H: return Read32(Addr32(bp, bw, x, y)) >> 24;
    case PSMT4HL: return (Read32(Addr32(bp, bw, x, y)) >> 24) & 0xF;
    case PSMT4HH: return Read32(Addr32(bp, bw, x, y)) >> 28;
    }
    return 0;
}

void WritePixel(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y, std::uint32_t v)
{
    switch (psm) {
    case PSMCT32: case PSMZ32: Write32(Addr32(bp, bw, x, y, psm), v); return;
    case PSMCT24: case PSMZ24: {
        std::uint32_t a = Addr32(bp, bw, x, y, psm);
        Write32(a, (Read32(a) & 0xFF000000u) | (v & 0xFFFFFF));
        return;
    }
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: Write16(Addr16(bp, bw, x, y, psm), (std::uint16_t)v); return;
    case PSMT8: g_local[Addr8(bp, bw, x, y)] = (std::uint8_t)v; return;
    case PSMT4: {
        std::uint32_t n = Addr4(bp, bw, x, y);
        std::uint8_t& b = g_local[n >> 1];
        b = (n & 1) ? (std::uint8_t)((b & 0x0F) | (v & 0xF) << 4) : (std::uint8_t)((b & 0xF0) | (v & 0xF));
        return;
    }
    case PSMT8H: { std::uint32_t a = Addr32(bp, bw, x, y); Write32(a, (Read32(a) & 0x00FFFFFFu) | (v & 0xFF) << 24); return; }
    case PSMT4HL: { std::uint32_t a = Addr32(bp, bw, x, y); Write32(a, (Read32(a) & 0xF0FFFFFFu) | (v & 0xF) << 24); return; }
    case PSMT4HH: { std::uint32_t a = Addr32(bp, bw, x, y); Write32(a, (Read32(a) & 0x0FFFFFFFu) | (v & 0xF) << 28); return; }
    }
}

namespace {

// A 16-bit colour (RGBA 5551) as 32-bit, its alpha by TEXA.
std::uint32_t Expand16(std::uint32_t c, bool forTexture)
{
    std::uint32_t r = (c & 0x1F) << 3, g = ((c >> 5) & 0x1F) << 3, b = ((c >> 10) & 0x1F) << 3;
    std::uint32_t a;
    if (!forTexture) {
        a = (c & 0x8000) ? 0x80 : 0;
    } else {
        std::uint64_t texa = g_regs.texa;
        bool aem = Bits(texa, 15, 1);
        if (c & 0x8000)
            a = (std::uint32_t)Bits(texa, 32, 8);
        else
            a = (aem && (c & 0x7FFF) == 0) ? 0 : (std::uint32_t)Bits(texa, 0, 8);
    }
    return r | g << 8 | b << 16 | a << 24;
}

std::uint32_t Expand24(std::uint32_t c)
{
    std::uint64_t texa = g_regs.texa;
    bool aem = Bits(texa, 15, 1);
    std::uint32_t a = (aem && (c & 0xFFFFFF) == 0) ? 0 : (std::uint32_t)Bits(texa, 0, 8);
    return (c & 0xFFFFFF) | a << 24;
}

// ---------------------------------------------------------------------------------------------
// The CLUT buffer: loaded from CBP when TEX0 (or TEX2) says so, in CSM1's arrangement (an 8-bit
// palette's 16 by 16 entries with index bits 3 and 4 exchanged; a 4-bit palette's 8 by 2 at
// CSA's place).

void LoadClut(std::uint64_t tex)
{
    std::uint32_t cld = (std::uint32_t)Bits(tex, 61, 3), cbp = (std::uint32_t)Bits(tex, 37, 14);
    bool load = false;
    switch (cld) {
    case 1: load = true; break;
    case 2: load = true; g_cbp0 = cbp; break;
    case 3: load = true; g_cbp1 = cbp; break;
    case 4: load = cbp != g_cbp0; g_cbp0 = cbp; break;
    case 5: load = cbp != g_cbp1; g_cbp1 = cbp; break;
    default: break;
    }
    if (!load)
        return;
    std::uint32_t psm = (std::uint32_t)Bits(tex, 20, 6), cpsm = (std::uint32_t)Bits(tex, 51, 4);
    std::uint32_t csm = (std::uint32_t)Bits(tex, 55, 1), csa = (std::uint32_t)Bits(tex, 56, 5);
    g_clutPsm = cpsm;
    bool c16 = cpsm == PSMCT16 || cpsm == PSMCT16S;
    auto fetch = [&](std::uint32_t x, std::uint32_t y) -> std::uint32_t {
        if (c16)
            return ReadPixel(cpsm, cbp, 1, x, y) | 0x80000000u;   // marked 16-bit: expanded at lookup
        return ReadPixel(PSMCT32, cbp, 1, x, y);
    };
    if (csm) {   // CSM2: a line from TEXCLUT's offset (16-bit palettes only)
        std::uint64_t tc = g_regs.texclut;
        std::uint32_t cbw = (std::uint32_t)Bits(tc, 0, 6), cou = (std::uint32_t)Bits(tc, 6, 6) * 16,
                      cov = (std::uint32_t)Bits(tc, 12, 10);
        std::uint32_t n = (psm == PSMT8 || psm == PSMT8H) ? 256 : 16;
        for (std::uint32_t i = 0; i < n; i++)
            g_clut[i] = ReadPixel(cpsm, cbp, cbw, cou + i, cov) | (c16 ? 0x80000000u : 0);
        return;
    }
    if (psm == PSMT8 || psm == PSMT8H) {
        for (std::uint32_t i = 0; i < 256; i++) {
            std::uint32_t x = (i & 7) | ((i & 0x10) >> 1), y = ((i & 8) >> 3) | ((i & 0xE0) >> 4);
            g_clut[i] = fetch(x, y);
        }
    } else {
        std::uint32_t base = (csa & (c16 ? 31 : 15)) * 16;
        for (std::uint32_t i = 0; i < 16; i++)
            g_clut[(base + i) & 255] = fetch(i & 7, i >> 3);
    }
}

// ---------------------------------------------------------------------------------------------
// Texture sampling

struct Tex {
    std::uint32_t tbp, tbw, psm, tw, th, tcc, tfx, csa, cpsm;
    std::uint32_t wms, wmt, minu, maxu, minv, maxv;
    bool bilinear;
};

Tex TexOf(const Context& c)
{
    Tex t;
    std::uint64_t tex0 = c.tex0, clamp = c.clamp, tex1 = c.tex1;
    t.tbp = (std::uint32_t)Bits(tex0, 0, 14);
    t.tbw = (std::uint32_t)Bits(tex0, 14, 6);
    t.psm = (std::uint32_t)Bits(tex0, 20, 6);
    t.tw = 1u << std::min<std::uint32_t>((std::uint32_t)Bits(tex0, 26, 4), 10);
    t.th = 1u << std::min<std::uint32_t>((std::uint32_t)Bits(tex0, 30, 4), 10);
    t.tcc = (std::uint32_t)Bits(tex0, 34, 1);
    t.tfx = (std::uint32_t)Bits(tex0, 35, 2);
    t.cpsm = (std::uint32_t)Bits(tex0, 51, 4);
    t.csa = (std::uint32_t)Bits(tex0, 56, 5);
    t.wms = (std::uint32_t)Bits(clamp, 0, 2);
    t.wmt = (std::uint32_t)Bits(clamp, 2, 2);
    t.minu = (std::uint32_t)Bits(clamp, 4, 10);
    t.maxu = (std::uint32_t)Bits(clamp, 14, 10);
    t.minv = (std::uint32_t)Bits(clamp, 24, 10);
    t.maxv = (std::uint32_t)Bits(clamp, 34, 10);
    t.bilinear = Bits(tex1, 5, 1) != 0;   // MMAG (magnification) decides; mipmaps are not used
    return t;
}

int Wrap(int c, std::uint32_t size, std::uint32_t mode, std::uint32_t mn, std::uint32_t mx)
{
    switch (mode) {
    case 0: return c & (int)(size - 1);                                       // REPEAT
    case 1: return std::clamp(c, 0, (int)size - 1);                           // CLAMP
    case 2: return std::clamp(c, (int)mn, (int)mx);                           // REGION_CLAMP
    default: return (int)(((std::uint32_t)c & mn) | mx) & (int)(1023);        // REGION_REPEAT
    }
}

std::uint32_t Texel(const Tex& t, int x, int y)
{
    x = Wrap(x, t.tw, t.wms, t.minu, t.maxu);
    y = Wrap(y, t.th, t.wmt, t.minv, t.maxv);
    std::uint32_t raw = ReadPixel(t.psm, t.tbp, t.tbw, (std::uint32_t)x, (std::uint32_t)y);
    switch (t.psm) {
    case PSMCT32: return raw;
    case PSMCT24: return Expand24(raw);
    case PSMCT16: case PSMCT16S: return Expand16(raw, true);
    case PSMT8: case PSMT8H: {
        std::uint32_t e = g_clut[raw & 255];
        return (e & 0x80000000u && (t.cpsm == PSMCT16 || t.cpsm == PSMCT16S)) ? Expand16(e & 0xFFFF, true) : e;
    }
    default: {   // 4-bit
        std::uint32_t c16 = t.cpsm == PSMCT16 || t.cpsm == PSMCT16S;
        std::uint32_t e = g_clut[((t.csa & (c16 ? 31 : 15)) * 16 + raw) & 255];
        return (e & 0x80000000u && c16) ? Expand16(e & 0xFFFF, true) : e;
    }
    }
}

std::uint32_t Sample(const Tex& t, float u, float v)
{
    if (!t.bilinear)
        return Texel(t, (int)std::floor(u), (int)std::floor(v));
    float fu = u - 0.5f, fv = v - 0.5f;
    int x0 = (int)std::floor(fu), y0 = (int)std::floor(fv);
    float ax = fu - x0, ay = fv - y0;
    std::uint32_t c00 = Texel(t, x0, y0), c10 = Texel(t, x0 + 1, y0), c01 = Texel(t, x0, y0 + 1),
                  c11 = Texel(t, x0 + 1, y0 + 1);
    std::uint32_t out = 0;
    for (int k = 0; k < 32; k += 8) {
        float a = ((c00 >> k) & 255) * (1 - ax) + ((c10 >> k) & 255) * ax;
        float b = ((c01 >> k) & 255) * (1 - ax) + ((c11 >> k) & 255) * ax;
        out |= (std::uint32_t)(a * (1 - ay) + b * ay + 0.5f) << k;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// The pixel pipeline

struct Draw {
    const Context* c;
    bool tme, fge, abe, iip, fst;
    Tex tex;
    std::uint32_t fbp, fbw, fpsm, fbmsk;
    std::uint32_t zbp, zpsm;
    bool zmsk;
    int ofx, ofy;
    int scax0, scax1, scay0, scay1;
    bool ate, date, datm, zte;
    std::uint32_t atst, aref, afail, ztst;
    std::uint32_t ba, bb, bc, bd, bfix;
    bool fba;
};

Draw DrawOf(std::uint64_t prim)
{
    Draw d;
    std::uint64_t attrs = Bits(g_regs.prmodecont, 0, 1) ? prim : g_regs.prmode;
    d.iip = Bits(attrs, 3, 1);
    d.tme = Bits(attrs, 4, 1);
    d.fge = Bits(attrs, 5, 1);
    d.abe = Bits(attrs, 6, 1);
    d.fst = Bits(attrs, 8, 1);
    const Context& c = g_regs.ctx[Bits(attrs, 9, 1)];
    d.c = &c;
    d.tex = TexOf(c);
    d.fbp = (std::uint32_t)Bits(c.frame, 0, 9) * 32;
    d.fbw = (std::uint32_t)Bits(c.frame, 16, 6);
    d.fpsm = (std::uint32_t)Bits(c.frame, 24, 6);
    d.fbmsk = (std::uint32_t)Bits(c.frame, 32, 32);
    d.zbp = (std::uint32_t)Bits(c.zbuf, 0, 9) * 32;
    d.zpsm = (std::uint32_t)Bits(c.zbuf, 24, 4) | 0x30;
    d.zmsk = Bits(c.zbuf, 32, 1);
    d.ofx = (int)Bits(c.xyoffset, 0, 16);
    d.ofy = (int)Bits(c.xyoffset, 32, 16);
    d.scax0 = (int)Bits(c.scissor, 0, 11);
    d.scax1 = (int)Bits(c.scissor, 16, 11);
    d.scay0 = (int)Bits(c.scissor, 32, 11);
    d.scay1 = (int)Bits(c.scissor, 48, 11);
    d.ate = Bits(c.test, 0, 1);
    d.atst = (std::uint32_t)Bits(c.test, 1, 3);
    d.aref = (std::uint32_t)Bits(c.test, 4, 8);
    d.afail = (std::uint32_t)Bits(c.test, 12, 2);
    d.date = Bits(c.test, 14, 1);
    d.datm = Bits(c.test, 15, 1);
    d.zte = Bits(c.test, 16, 1);
    d.ztst = (std::uint32_t)Bits(c.test, 17, 2);
    d.ba = (std::uint32_t)Bits(c.alpha, 0, 2);
    d.bb = (std::uint32_t)Bits(c.alpha, 2, 2);
    d.bc = (std::uint32_t)Bits(c.alpha, 4, 2);
    d.bd = (std::uint32_t)Bits(c.alpha, 6, 2);
    d.bfix = (std::uint32_t)Bits(c.alpha, 32, 8);
    d.fba = Bits(c.fba, 0, 1);
    return d;
}

// One pixel at window position (x, y): colour r g b a (0 to 255), depth z, fog f, texture
// coordinates (texels) u v.
void Pixel(const Draw& d, int x, int y, int r, int g, int b, int a, std::uint32_t z, int f, float u, float v)
{
    if (x < d.scax0 || x > d.scax1 || y < d.scay0 || y > d.scay1 || x < 0 || y < 0)
        return;
    if (d.tme) {
        std::uint32_t t = Sample(d.tex, u, v);
        int tr = t & 255, tg = (t >> 8) & 255, tb = (t >> 16) & 255, ta = t >> 24;
        switch (d.tex.tfx) {
        case 0:   // MODULATE
            r = std::min(255, (tr * r) >> 7);
            g = std::min(255, (tg * g) >> 7);
            b = std::min(255, (tb * b) >> 7);
            if (d.tex.tcc)
                a = std::min(255, (ta * a) >> 7);
            break;
        case 1:   // DECAL
            r = tr; g = tg; b = tb;
            if (d.tex.tcc)
                a = ta;
            break;
        case 2:   // HIGHLIGHT
            r = std::min(255, ((tr * r) >> 7) + a);
            g = std::min(255, ((tg * g) >> 7) + a);
            b = std::min(255, ((tb * b) >> 7) + a);
            if (d.tex.tcc)
                a = std::min(255, ta + a);
            break;
        default:  // HIGHLIGHT2
            r = std::min(255, ((tr * r) >> 7) + a);
            g = std::min(255, ((tg * g) >> 7) + a);
            b = std::min(255, ((tb * b) >> 7) + a);
            if (d.tex.tcc)
                a = ta;
            break;
        }
    }
    if (d.fge) {
        int fr = (int)Bits(g_regs.fogcol, 0, 8), fg = (int)Bits(g_regs.fogcol, 8, 8), fb = (int)Bits(g_regs.fogcol, 16, 8);
        r = (r * f + fr * (255 - f)) >> 8;
        g = (g * f + fg * (255 - f)) >> 8;
        b = (b * f + fb * (255 - f)) >> 8;
    }
    bool writeFb = true, writeZ = !d.zmsk, writeRgbOnly = false;
    if (d.ate) {
        bool pass;
        switch (d.atst) {
        case 0: pass = false; break;
        case 1: pass = true; break;
        case 2: pass = (std::uint32_t)a < d.aref; break;
        case 3: pass = (std::uint32_t)a <= d.aref; break;
        case 4: pass = (std::uint32_t)a == d.aref; break;
        case 5: pass = (std::uint32_t)a >= d.aref; break;
        case 6: pass = (std::uint32_t)a > d.aref; break;
        default: pass = (std::uint32_t)a != d.aref; break;
        }
        if (!pass) {
            switch (d.afail) {
            case 0: return;                                  // KEEP
            case 1: writeZ = false; break;                   // FB_ONLY
            case 2: writeFb = false; break;                  // ZB_ONLY
            default: writeZ = false; writeRgbOnly = true; break;   // RGB_ONLY
            }
        }
    }
    std::uint32_t ux = (std::uint32_t)x, uy = (std::uint32_t)y;
    std::uint32_t dst = ReadPixel(d.fpsm, d.fbp, d.fbw, ux, uy);
    std::uint32_t dst32 = (d.fpsm == PSMCT16 || d.fpsm == PSMCT16S) ? Expand16(dst, false)
                        : d.fpsm == PSMCT24 ? (dst | 0x80000000u) : dst;
    if (d.date) {
        bool bit = (d.fpsm == PSMCT16 || d.fpsm == PSMCT16S) ? (dst & 0x8000) != 0 : (dst & 0x80000000u) != 0;
        if (bit != d.datm)
            return;
    }
    if (d.zte) {
        std::uint32_t zmax = d.zpsm == PSMZ32 ? 0xFFFFFFFFu : d.zpsm == PSMZ24 ? 0xFFFFFFu : 0xFFFFu;
        std::uint32_t zz = std::min(z, zmax);
        std::uint32_t old = ReadPixel(d.zpsm, d.zbp, d.fbw, ux, uy);
        bool pass;
        switch (d.ztst) {
        case 0: pass = false; break;
        case 1: pass = true; break;
        case 2: pass = zz >= old; break;
        default: pass = zz > old; break;
        }
        if (!pass)
            return;
        z = zz;
    }
    if (d.abe && !(Bits(g_regs.pabe, 0, 1) && a < 0x80)) {
        int cs[3] = {r, g, b}, cd[3] = {(int)(dst32 & 255), (int)((dst32 >> 8) & 255), (int)((dst32 >> 16) & 255)};
        int ad = (int)(dst32 >> 24);
        int cc = d.bc == 0 ? a : d.bc == 1 ? ad : (int)d.bfix;
        int out[3];
        for (int k = 0; k < 3; k++) {
            int A = d.ba == 0 ? cs[k] : d.ba == 1 ? cd[k] : 0;
            int B = d.bb == 0 ? cs[k] : d.bb == 1 ? cd[k] : 0;
            int D = d.bd == 0 ? cs[k] : d.bd == 1 ? cd[k] : 0;
            out[k] = (((A - B) * cc) >> 7) + D;
        }
        r = out[0]; g = out[1]; b = out[2];
    }
    if (Bits(g_regs.colclamp, 0, 1)) {
        r = std::clamp(r, 0, 255);
        g = std::clamp(g, 0, 255);
        b = std::clamp(b, 0, 255);
    } else {
        r &= 255; g &= 255; b &= 255;
    }
    if (d.fba)
        a |= 0x80;
    if (writeFb) {
        std::uint32_t c32 = (std::uint32_t)r | (std::uint32_t)g << 8 | (std::uint32_t)b << 16 | (std::uint32_t)(a & 255) << 24;
        std::uint32_t mask = d.fbmsk;
        if (writeRgbOnly)
            mask |= 0xFF000000u;
        if (d.fpsm == PSMCT16 || d.fpsm == PSMCT16S) {
            std::uint32_t c16 = (std::uint32_t)(r >> 3) | (std::uint32_t)(g >> 3) << 5 | (std::uint32_t)(b >> 3) << 10 |
                                (std::uint32_t)((a & 0x80) ? 0x8000 : 0);
            std::uint32_t m16 = ((mask >> 3) & 0x1F) | ((mask >> 11) & 0x1F) << 5 | ((mask >> 19) & 0x1F) << 10 |
                                ((mask >> 31) & 1) << 15;
            WritePixel(d.fpsm, d.fbp, d.fbw, ux, uy, (dst & m16) | (c16 & ~m16));
        } else {
            std::uint32_t old = d.fpsm == PSMCT24 ? (dst & 0xFFFFFF) : dst;
            WritePixel(d.fpsm, d.fbp, d.fbw, ux, uy, (old & mask) | (c32 & ~mask));
        }
    }
    if (d.zte && writeZ)
        WritePixel(d.zpsm, d.zbp, d.fbw, ux, uy, z);
}

// ---------------------------------------------------------------------------------------------
// Primitives

struct Attr {   // what a vertex gives a pixel, as floats for interpolation
    float x, y, z, r, g, b, a, f, s, t, q, u, v;
};

Attr AttrOf(const Draw& d, const Vertex& v)
{
    Attr a;
    a.x = (float)(v.x - d.ofx) / 16.0f;
    a.y = (float)(v.y - d.ofy) / 16.0f;
    a.z = (float)v.z;
    a.r = v.r; a.g = v.g; a.b = v.b; a.a = v.a;
    a.f = v.f;
    a.s = v.s; a.t = v.t; a.q = v.q;
    a.u = (float)v.u / 16.0f;
    a.v = (float)v.v / 16.0f;
    return a;
}

void Shade(const Draw& d, int x, int y, const Attr& p, const Attr& flat)
{
    const Attr& c = d.iip ? p : flat;
    float u = 0, v = 0;
    if (d.tme) {
        if (d.fst) {
            u = p.u;
            v = p.v;
        } else {
            float q = p.q != 0.0f ? p.q : 1.0f;
            u = p.s / q * (float)d.tex.tw;
            v = p.t / q * (float)d.tex.th;
        }
    }
    double zd = std::max(0.0, std::min(4294967295.0, (double)p.z));
    Pixel(d, x, y, (int)c.r, (int)c.g, (int)c.b, (int)c.a, (std::uint32_t)zd, (int)p.f, u, v);
}

Attr Lerp3(const Attr& a, const Attr& b, const Attr& c, float wa, float wb, float wc)
{
    Attr o;
    const float* pa = &a.x; const float* pb = &b.x; const float* pc = &c.x;
    float* po = &o.x;
    for (int k = 0; k < (int)(sizeof(Attr) / sizeof(float)); k++)
        po[k] = pa[k] * wa + pb[k] * wb + pc[k] * wc;
    return o;
}

void Triangle(const Draw& d, const Vertex& v0, const Vertex& v1, const Vertex& v2)
{
    Attr a = AttrOf(d, v0), b = AttrOf(d, v1), c = AttrOf(d, v2);
    // STQ: interpolate S, T and Q linearly in screen space, divide per pixel (perspective)
    float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    if (area == 0.0f)
        return;
    int x0 = std::max((int)std::ceil(std::min({a.x, b.x, c.x})), std::max(d.scax0, 0));
    int x1 = std::min((int)std::ceil(std::max({a.x, b.x, c.x})) - 1, d.scax1);
    int y0 = std::max((int)std::ceil(std::min({a.y, b.y, c.y})), std::max(d.scay0, 0));
    int y1 = std::min((int)std::ceil(std::max({a.y, b.y, c.y})) - 1, d.scay1);
    if (x1 - x0 > 2048 || y1 - y0 > 2048)
        return;
    // the top-left rule: a pixel on a top or left edge is drawn, on a bottom or right one not
    auto topLeft = [&](const Attr& p, const Attr& q) {
        float ex = q.x - p.x, ey = q.y - p.y;
        return area > 0 ? (ey < 0 || (ey == 0 && ex > 0)) : (ey > 0 || (ey == 0 && ex < 0));
    };
    bool tlA = topLeft(b, c), tlB = topLeft(c, a), tlC = topLeft(a, b);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float px = (float)x, py = (float)y;
            float wa = ((b.x - px) * (c.y - py) - (c.x - px) * (b.y - py)) / area;
            float wb = ((c.x - px) * (a.y - py) - (a.x - px) * (c.y - py)) / area;
            float wc = 1.0f - wa - wb;
            if (wa < 0 || wb < 0 || wc < 0)
                continue;
            if ((wa == 0 && !tlA) || (wb == 0 && !tlB) || (wc == 0 && !tlC))
                continue;
            Shade(d, x, y, Lerp3(a, b, c, wa, wb, wc), c);
        }
    }
}

void Sprite(const Draw& d, const Vertex& v0, const Vertex& v1)
{
    Attr a = AttrOf(d, v0), b = AttrOf(d, v1);
    if (a.x > b.x) std::swap(a.x, b.x), std::swap(a.s, b.s), std::swap(a.u, b.u);
    if (a.y > b.y) std::swap(a.y, b.y), std::swap(a.t, b.t), std::swap(a.v, b.v);
    int x0 = std::max((int)std::ceil(a.x), std::max(d.scax0, 0)), x1 = std::min((int)std::ceil(b.x) - 1, d.scax1);
    int y0 = std::max((int)std::ceil(a.y), std::max(d.scay0, 0)), y1 = std::min((int)std::ceil(b.y) - 1, d.scay1);
    if (x1 - x0 > 2048 || y1 - y0 > 2048)
        return;
    float w = b.x - a.x, h = b.y - a.y;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float fx = w ? ((float)x - a.x) / w : 0, fy = h ? ((float)y - a.y) / h : 0;
            Attr p = b;   // flat: the second vertex's colour, depth and fog
            p.s = a.s + (b.s - a.s) * fx;
            p.t = a.t + (b.t - a.t) * fy;
            p.u = a.u + (b.u - a.u) * fx;
            p.v = a.v + (b.v - a.v) * fy;
            p.q = b.q;
            Shade(d, x, y, p, b);
        }
    }
}

void Line(const Draw& d, const Vertex& v0, const Vertex& v1)
{
    Attr a = AttrOf(d, v0), b = AttrOf(d, v1);
    float dx = b.x - a.x, dy = b.y - a.y;
    int n = (int)std::ceil(std::max(std::fabs(dx), std::fabs(dy)));
    if (n <= 0 || n > 4096)
        return;
    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)n;
        Attr p = Lerp3(a, b, b, 1 - t, t, 0);
        Shade(d, (int)std::floor(p.x), (int)std::floor(p.y), p, b);
    }
}

void Kick(bool draw)
{
    std::uint64_t prim = g_regs.prim;
    std::uint32_t type = (std::uint32_t)Bits(prim, 0, 3);
    static const int kNeeded[8] = {1, 2, 2, 3, 3, 3, 2, 1};
    int needed = kNeeded[type];
    if (g_queued < needed)
        return;
    if (draw && g_rasterise) {
        Draw d = DrawOf(prim);
        g_drawn++;
        static int traced;
        if (std::getenv("SLY1_GS_TRACE") && traced++ < 12)
            std::fprintf(stderr, "gs: prim %u: FRAME FBP %u FBW %u PSM 0x%X, ZBUF %u, scissor %d-%d x %d-%d, offset %d,%d, "
                         "v0 (%d,%d) tme %d psm 0x%X\n", type, d.fbp / 32, d.fbw, d.fpsm, d.zbp / 32, d.scax0, d.scax1,
                         d.scay0, d.scay1, d.ofx / 16, d.ofy / 16, g_queue[0].x / 16, g_queue[0].y / 16, d.tme, d.tex.psm);
        switch (type) {
        case 0: { Attr p = AttrOf(d, g_queue[0]); Shade(d, (int)p.x, (int)p.y, p, p); break; }
        case 1: case 2: Line(d, g_queue[0], g_queue[1]); break;
        case 3: case 4: case 5: Triangle(d, g_queue[0], g_queue[1], g_queue[2]); break;
        case 6: Sprite(d, g_queue[0], g_queue[1]); break;
        default: break;
        }
    }
    switch (type) {
    case 2: g_queue[0] = g_queue[1]; g_queued = 1; break;                       // line strip
    case 4: g_queue[0] = g_queue[1]; g_queue[1] = g_queue[2]; g_queued = 2; break;   // triangle strip
    case 5: g_queue[1] = g_queue[2]; g_queued = 2; break;                       // triangle fan
    default: g_queued = 0; break;
    }
}

void PushVertex(std::int32_t x, std::int32_t y, std::uint32_t z, std::uint8_t f, bool draw)
{
    Vertex v;
    v.x = x;
    v.y = y;
    v.z = z;
    v.f = f;
    v.r = g_regs.r; v.g = g_regs.g; v.b = g_regs.b; v.a = g_regs.a;
    v.s = g_regs.s; v.t = g_regs.t; v.q = g_regs.q;
    v.u = g_regs.u; v.v = g_regs.v;
    if (g_queued >= 3)
        g_queued = 2;
    g_queue[g_queued++] = v;
    Kick(draw);
}

// ---------------------------------------------------------------------------------------------
// Transfers: host to local memory (IMAGE data) and local to local.

struct Transfer {
    bool active = false;
    std::uint32_t dbp, dbw, dpsm, dsax, dsay, rrw, rrh, x, y;
    std::uint64_t bits = 0;
    int nbits = 0;
} g_trx;

std::uint32_t BitsPerPixel(std::uint32_t psm)
{
    switch (psm) {
    case PSMCT32: case PSMZ32: return 32;
    case PSMCT24: case PSMZ24: return 24;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return 16;
    case PSMT8: case PSMT8H: return 8;
    default: return 4;
    }
}

void StartTransfer()
{
    std::uint64_t bb = g_regs.bitbltbuf, pos = g_regs.trxpos, reg = g_regs.trxreg;
    std::uint32_t dir = (std::uint32_t)Bits(g_regs.trxdir, 0, 2);
    g_trx = Transfer();
    if (dir == 0) {   // host to local
        g_trx.active = true;
        g_trx.dbp = (std::uint32_t)Bits(bb, 32, 14);
        g_trx.dbw = (std::uint32_t)Bits(bb, 48, 6);
        g_trx.dpsm = (std::uint32_t)Bits(bb, 56, 6);
        g_trx.dsax = (std::uint32_t)Bits(pos, 32, 11);
        g_trx.dsay = (std::uint32_t)Bits(pos, 48, 11);
        g_trx.rrw = (std::uint32_t)Bits(reg, 0, 12);
        g_trx.rrh = (std::uint32_t)Bits(reg, 32, 12);
    } else if (dir == 2) {   // local to local
        std::uint32_t sbp = (std::uint32_t)Bits(bb, 0, 14), sbw = (std::uint32_t)Bits(bb, 16, 6),
                      spsm = (std::uint32_t)Bits(bb, 24, 6);
        std::uint32_t dbp = (std::uint32_t)Bits(bb, 32, 14), dbw = (std::uint32_t)Bits(bb, 48, 6),
                      dpsm = (std::uint32_t)Bits(bb, 56, 6);
        std::uint32_t ssax = (std::uint32_t)Bits(pos, 0, 11), ssay = (std::uint32_t)Bits(pos, 16, 11);
        std::uint32_t dsax = (std::uint32_t)Bits(pos, 32, 11), dsay = (std::uint32_t)Bits(pos, 48, 11);
        std::uint32_t w = (std::uint32_t)Bits(reg, 0, 12), h = (std::uint32_t)Bits(reg, 32, 12);
        std::vector<std::uint32_t> tmp((std::size_t)w * h);
        for (std::uint32_t yy = 0; yy < h; yy++)
            for (std::uint32_t xx = 0; xx < w; xx++)
                tmp[(std::size_t)yy * w + xx] = ReadPixel(spsm, sbp, sbw, (ssax + xx) & 2047, (ssay + yy) & 2047);
        for (std::uint32_t yy = 0; yy < h; yy++)
            for (std::uint32_t xx = 0; xx < w; xx++)
                WritePixel(dpsm, dbp, dbw, (dsax + xx) & 2047, (dsay + yy) & 2047, tmp[(std::size_t)yy * w + xx]);
    }
}

}  // namespace

Hold::Hold() { if (t_held++ == 0) g_lock.lock(); }
Hold::~Hold() { if (--t_held == 0) g_lock.unlock(); }

void Image(const std::uint32_t* q)
{
    Guard lk;
    Transfer& t = g_trx;
    if (!t.active)
        return;
    std::uint32_t bpp = BitsPerPixel(t.dpsm);
    for (int w = 0; w < 4; w++) {
        t.bits |= (std::uint64_t)q[w] << t.nbits;
        t.nbits += 32;
        while (t.nbits >= (int)bpp) {
            std::uint32_t v = (std::uint32_t)(t.bits & ((1ull << bpp) - 1));
            t.bits >>= bpp;
            t.nbits -= (int)bpp;
            if (t.y < t.rrh)
                WritePixel(t.dpsm, t.dbp, t.dbw, (t.dsax + t.x) & 2047, (t.dsay + t.y) & 2047, v);
            if (++t.x >= t.rrw) {
                t.x = 0;
                if (++t.y >= t.rrh) {
                    t.active = false;
                    return;
                }
            }
        }
    }
}

void Write(std::uint32_t reg, std::uint64_t value)
{
    Guard lk;
    Regs& r = g_regs;
    switch (reg) {
    case 0x00:   // PRIM: a new primitive empties the vertex queue
        r.prim = value;
        g_queued = 0;
        break;
    case 0x01:
        r.r = (std::uint8_t)Bits(value, 0, 8);
        r.g = (std::uint8_t)Bits(value, 8, 8);
        r.b = (std::uint8_t)Bits(value, 16, 8);
        r.a = (std::uint8_t)Bits(value, 24, 8);
        r.q = FloatOf((std::uint32_t)(value >> 32));
        break;
    case 0x02:
        r.s = FloatOf((std::uint32_t)value);
        r.t = FloatOf((std::uint32_t)(value >> 32));
        break;
    case 0x03:
        r.u = (std::uint32_t)Bits(value, 0, 14);
        r.v = (std::uint32_t)Bits(value, 16, 14);
        break;
    case 0x04: case 0x0C:   // XYZF2, XYZF3
        PushVertex((std::int32_t)Bits(value, 0, 16), (std::int32_t)Bits(value, 16, 16), (std::uint32_t)Bits(value, 32, 24),
                   (std::uint8_t)Bits(value, 56, 8), reg == 0x04);
        break;
    case 0x05: case 0x0D:   // XYZ2, XYZ3
        PushVertex((std::int32_t)Bits(value, 0, 16), (std::int32_t)Bits(value, 16, 16), (std::uint32_t)(value >> 32),
                   r.fog, reg == 0x05);
        break;
    case 0x06: case 0x07: r.ctx[reg - 0x06].tex0 = value; LoadClut(value); break;
    case 0x08: case 0x09: r.ctx[reg - 0x08].clamp = value; break;
    case 0x0A: r.fog = (std::uint8_t)Bits(value, 56, 8); break;
    case 0x14: case 0x15: r.ctx[reg - 0x14].tex1 = value; break;
    case 0x16: case 0x17: {   // TEX2: TEX0's palette fields
        std::uint64_t mask = (0x3Full << 20) | (0x7FFFFFFull << 37);
        Context& c = r.ctx[reg - 0x16];
        c.tex0 = (c.tex0 & ~mask) | (value & mask);
        LoadClut(c.tex0);
        break;
    }
    case 0x18: case 0x19: r.ctx[reg - 0x18].xyoffset = value; break;
    case 0x1A: r.prmodecont = value; break;
    case 0x1B: r.prmode = value; break;
    case 0x1C: r.texclut = value; break;
    case 0x22: r.scanmsk = value; break;
    case 0x34: case 0x35: r.ctx[reg - 0x34].miptbp1 = value; break;
    case 0x36: case 0x37: r.ctx[reg - 0x36].miptbp2 = value; break;
    case 0x3B: r.texa = value; break;
    case 0x3D: r.fogcol = value; break;
    case 0x40: case 0x41: r.ctx[reg - 0x40].scissor = value; break;
    case 0x42: case 0x43: r.ctx[reg - 0x42].alpha = value; break;
    case 0x44: r.dimx = value; break;
    case 0x45: r.dthe = value; break;
    case 0x46: r.colclamp = value; break;
    case 0x47: case 0x48: r.ctx[reg - 0x47].test = value; break;
    case 0x49: r.pabe = value; break;
    case 0x4A: case 0x4B: r.ctx[reg - 0x4A].fba = value; break;
    case 0x4C: case 0x4D: r.ctx[reg - 0x4C].frame = value; break;
    case 0x4E: case 0x4F: r.ctx[reg - 0x4E].zbuf = value; break;
    case 0x50: r.bitbltbuf = value; break;
    case 0x51: r.trxpos = value; break;
    case 0x52: r.trxreg = value; break;
    case 0x53: r.trxdir = value; StartTransfer(); break;
    default: break;   // TEXFLUSH, SIGNAL, FINISH (port/platform/dmac.cpp), LABEL, HWREG
    }
}

void WritePacked(std::uint32_t reg, const std::uint32_t* q)
{
    Guard lk;
    Regs& r = g_regs;
    switch (reg) {
    case 0x0: Write(0x00, q[0] & 0x7FF); break;
    case 0x1:
        r.r = (std::uint8_t)q[0];
        r.g = (std::uint8_t)q[1];
        r.b = (std::uint8_t)q[2];
        r.a = (std::uint8_t)q[3];
        break;   // Q stays as the last ST gave it
    case 0x2:
        r.s = FloatOf(q[0]);
        r.t = FloatOf(q[1]);
        r.q = FloatOf(q[2]);
        break;
    case 0x3:
        r.u = q[0] & 0x3FFF;
        r.v = q[1] & 0x3FFF;
        break;
    case 0x4: {   // XYZF2 (XYZF3 with ADC)
        bool adc = (q[3] >> 15) & 1;
        PushVertex((std::int32_t)(q[0] & 0xFFFF), (std::int32_t)(q[1] & 0xFFFF), (q[2] >> 4) & 0xFFFFFF,
                   (std::uint8_t)((q[3] >> 4) & 0xFF), !adc);
        break;
    }
    case 0x5: {   // XYZ2 (XYZ3 with ADC)
        bool adc = (q[3] >> 15) & 1;
        PushVertex((std::int32_t)(q[0] & 0xFFFF), (std::int32_t)(q[1] & 0xFFFF), q[2], r.fog, !adc);
        break;
    }
    case 0xA: r.fog = (std::uint8_t)((q[3] >> 4) & 0xFF); break;
    case 0xF: break;   // NOP
    default: Write(reg, q[0] | (std::uint64_t)q[1] << 32); break;   // TEX0, CLAMP, XYZF3, XYZ3 as they are
    }
}

void SetDisplay(std::uint64_t dispfb, std::uint64_t display)
{
    Guard lk;
    static int traced;
    if (std::getenv("SLY1_GS_TRACE") && traced++ < 4)
        std::fprintf(stderr, "gs: display DISPFB %016llX (FBP %llu, FBW %llu, PSM 0x%llX), DISPLAY %016llX\n",
                     (unsigned long long)dispfb, (unsigned long long)Bits(dispfb, 0, 9), (unsigned long long)Bits(dispfb, 9, 6),
                     (unsigned long long)Bits(dispfb, 15, 5), (unsigned long long)display);
    g_dispfb = dispfb;
    g_display = display;
    g_displaySet = true;
}

bool ReadDisplay(std::string& rgb, int& width, int& height)
{
    Guard lk;
    if (!g_displaySet)
        return false;
    std::uint32_t fbp = (std::uint32_t)Bits(g_dispfb, 0, 9) * 32, fbw = (std::uint32_t)Bits(g_dispfb, 9, 6);
    std::uint32_t psm = (std::uint32_t)Bits(g_dispfb, 15, 5);
    std::uint32_t dbx = (std::uint32_t)Bits(g_dispfb, 32, 11), dby = (std::uint32_t)Bits(g_dispfb, 43, 11);
    std::uint32_t magh = (std::uint32_t)Bits(g_display, 23, 4) + 1;
    std::uint32_t dw = (std::uint32_t)Bits(g_display, 32, 12) + 1, dh = (std::uint32_t)Bits(g_display, 44, 11) + 1;
    width = (int)std::min<std::uint32_t>(dw / magh, 1024);
    height = (int)std::min<std::uint32_t>(dh, 1024);
    if (width <= 0 || height <= 0 || fbw == 0) {
        width = 640;
        height = 448;
        fbw = fbw ? fbw : 10;
    }
    // SMODE2 (its privileged register, as the graphics library wrote it): interlaced in field mode,
    // each field shows the frame's lines once, so the buffer holds half the display's lines; the
    // picture doubles them.
    std::uint64_t smode2 = *(const volatile std::uint64_t*)Native(0x12000020);
    int lines = height, repeat = 1;
    if ((smode2 & 3) == 3) {
        lines = height / 2;
        repeat = 2;
    }
    rgb.resize((std::size_t)width * height * 3);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            std::uint32_t c = ReadPixel(psm, fbp, fbw, dbx + (std::uint32_t)x, dby + (std::uint32_t)(y / repeat < lines ? y / repeat : lines - 1));
            if (psm == PSMCT16 || psm == PSMCT16S)
                c = Expand16(c, false);
            char* p = &rgb[((std::size_t)y * width + x) * 3];
            p[0] = (char)(c & 255);
            p[1] = (char)((c >> 8) & 255);
            p[2] = (char)((c >> 16) & 255);
        }
    }
    return true;
}

std::uint64_t PrimitivesDrawn() { return g_drawn.load(); }

namespace {

std::string g_dumpDir;
int g_dumpEvery = 60;
std::uint64_t g_framesShown;

std::uint32_t Crc32(const unsigned char* p, std::size_t n, std::uint32_t c = 0xFFFFFFFFu)
{
    for (std::size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return c;
}

// A PNG of 8-bit RGB, its image data in stored (uncompressed) deflate blocks: no library needed.
void WritePng(const std::string& path, const std::string& rgb, int w, int h)
{
    std::string raw;
    for (int y = 0; y < h; y++) {
        raw.push_back('\0');
        raw.append(rgb, (std::size_t)y * w * 3, (std::size_t)w * 3);
    }
    std::string z = "\x78\x01";
    std::uint32_t a = 1, b = 0;
    for (unsigned char c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    for (std::size_t i = 0; i < raw.size(); i += 65535) {
        std::size_t n = std::min<std::size_t>(65535, raw.size() - i);
        z.push_back(i + n >= raw.size() ? 1 : 0);
        z.push_back((char)(n & 255));
        z.push_back((char)(n >> 8));
        z.push_back((char)(~n & 255));
        z.push_back((char)((~n >> 8) & 255));
        z.append(raw, i, n);
    }
    std::uint32_t adler = b << 16 | a;
    for (int k = 3; k >= 0; k--)
        z.push_back((char)(adler >> (8 * k)));
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return;
    auto be = [&](std::uint32_t v) { unsigned char x[4] = {(unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v}; std::fwrite(x, 1, 4, f); };
    auto chunk = [&](const char* type, const std::string& data) {
        be((std::uint32_t)data.size());
        std::string td = std::string(type, 4) + data;
        std::fwrite(td.data(), 1, td.size(), f);
        be(Crc32(reinterpret_cast<const unsigned char*>(td.data()), td.size()) ^ 0xFFFFFFFFu);
    };
    std::fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    std::string ihdr(13, '\0');
    for (int k = 0; k < 4; k++) {
        ihdr[k] = (char)(w >> (24 - 8 * k));
        ihdr[4 + k] = (char)(h >> (24 - 8 * k));
    }
    ihdr[8] = 8;
    ihdr[9] = 2;
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", "");
    std::fclose(f);
}

}  // namespace

void SetFrameDump(const std::string& dir, int every)
{
    g_rasterise = !dir.empty();
    g_dumpDir = dir;
    g_dumpEvery = every > 0 ? every : 60;
}

void FrameShown()
{
    std::uint64_t n = ++g_framesShown;
    if (g_dumpDir.empty() || n % (std::uint64_t)g_dumpEvery)
        return;
    std::string rgb;
    int w, h;
    if (!ReadDisplay(rgb, w, h))
        return;
    char name[64];
    std::snprintf(name, sizeof name, "/frame-%05llu.png", (unsigned long long)n);
    WritePng(g_dumpDir + name, rgb, w, h);
}

}  // namespace gs
}  // namespace port
