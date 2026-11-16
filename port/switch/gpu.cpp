// SPDX-License-Identifier: AGPL-3.0-or-later
// The Switch's GPU renderer: what the GS is given to draw (port/gs_backend.h), drawn with deko3d.
//
// The game draws 640 by 224 fields into two frame buffers in turn and shows the other one. Each
// is an image of kWidth by kHeight here, drawn as a whole picture (the odd field's half line is
// dropped by the GS), with one depth image between them; the one DISPFB names is copied to the
// screen at each swap. The GS's vertices arrive transformed, so the vertex shader only scales
// them; its texture function and fog are the fragment shader's, its tests and blend the GPU's
// state. Textures are decoded from the GS's local memory when first drawn with and again when
// an upload has changed them.
//
// Not done yet: drawing into the small buffers the game reads as textures, the destination
// alpha test, the blends whose coefficient is the frame buffer's alpha (drawn at half strength).
#include "port/gpu.h"

#if defined(__SWITCH__)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <switch.h>
#include <deko3d.h>

#include "port/gs_backend.h"
#include "port/system.h"
#include "gs_vsh_dksh.h"
#include "gs_fsh_dksh.h"

namespace port {
namespace gpu {

namespace {

constexpr std::uint32_t kScreenWidth = 1280, kScreenHeight = 720;
constexpr std::uint32_t kWidth = 960, kHeight = 720;     // a frame buffer's image (4:3)
constexpr float kFieldWidth = 640.0f, kFieldHeight = 224.0f;
constexpr std::uint32_t kFrameMemory = 24u << 20;        // a frame's vertices and texture uploads
constexpr std::uint32_t kCommandMemory = 8u << 20;
constexpr std::uint32_t kTextureMemory = 64u << 20;
constexpr std::uint32_t kMaxTextures = 2048;
constexpr std::uint32_t kMaxDraws = 12000;               // a frame's draws before its commands are sent

std::uint32_t Align(std::uint32_t v, std::uint32_t a) { return (v + a - 1) & ~(a - 1); }

void Debug(void*, const char* context, DkResult result, const char* message)
{
    std::fprintf(stderr, "gpu: deko3d: %s: %s (result %d)\n", context ? context : "?", message ? message : "?", (int)result);
}

struct VertexParams {
    float toClip[4];
};

struct FragmentParams {
    std::int32_t mode[4];
    float fogColour[4];
    float blend[4];
};

struct Texture {
    DkImage image;
    std::uint32_t id;
    std::uint32_t generation;
    std::uint64_t hash;
    int width, height;
};

class Renderer : public gs::Backend {
public:
    bool Create();
    void Begin(const gs::DrawState& state) override;
    void Draw(const gs::DrawState& state, const gs::DrawVertex* v, std::size_t vertices, const std::uint32_t* index,
              std::size_t indices) override;
    void Present(std::uint64_t dispfb, std::uint64_t display) override;
    void Report();

private:
    DkMemBlock Block(std::uint32_t size, std::uint32_t flags);
    void BeginCommands();
    void Send(bool wait);
    void BindTarget(int target);
    const Texture* TextureFor(const gs::DrawState& state);
    void Capture(int target);
    static int TargetOf(std::uint64_t fbp);

    DkDevice device_ = nullptr;
    DkQueue queue_ = nullptr;
    DkSwapchain swapchain_ = nullptr;
    DkCmdBuf cmd_ = nullptr;
    DkMemBlock imageMem_ = nullptr, frameMem_ = nullptr, commandMem_ = nullptr, codeMem_ = nullptr, dataMem_ = nullptr,
               textureMem_ = nullptr;
    DkImage screen_[2], colour_[2], depth_;
    DkShader vertexShader_, fragmentShader_;
    std::uint32_t frameUsed_ = 0, draws_ = 0, textureUsed_ = 0, nextTexture_ = 0;
    int bound_ = -1;
    const Texture* texture_ = nullptr;   // the run's, from Begin
    std::uint32_t vertexUbo_ = 0, fragmentUbo_ = 0, imageSet_ = 0, samplerSet_ = 0, capture_ = 0;   // offsets in dataMem_
    std::unordered_map<std::uint64_t, Texture> textures_;
    std::vector<std::uint32_t> pixels_;

    // the report's counts
    std::uint64_t frames_ = 0, runs_ = 0, vertices_ = 0, uploads_ = 0, decodes_ = 0, hashes_ = 0, otherTargets_ = 0, cacheResets_ = 0,
                  sends_ = 0, destinationAlpha_ = 0, unsupportedBlends_ = 0;
    long captureAt_ = -1;
    std::uint64_t drawNs_ = 0, textureNs_ = 0, presentNs_ = 0, waitNs_ = 0, acquireNs_ = 0;

    // what the commands so far last bound, to bind only what changes
    struct Bound {
        bool any = false;
        DkBlendState blend;
        FragmentParams fp;
        DkResHandle texture;
        DkScissor scissor;
        std::uint32_t colour, write, depth, aref;
    } last_;
};

DkMemBlock Renderer::Block(std::uint32_t size, std::uint32_t flags)
{
    DkMemBlockMaker maker;
    dkMemBlockMakerDefaults(&maker, device_, Align(size, DK_MEMBLOCK_ALIGNMENT));
    maker.flags = flags;
    return dkMemBlockCreate(&maker);
}

bool Renderer::Create()
{
    DkDeviceMaker deviceMaker;
    dkDeviceMakerDefaults(&deviceMaker);
    deviceMaker.cbDebug = Debug;
    device_ = dkDeviceCreate(&deviceMaker);
    if (!device_)
        return false;

    DkQueueMaker queueMaker;
    dkQueueMakerDefaults(&queueMaker, device_);
    queueMaker.flags = DkQueueFlags_Graphics;
    queue_ = dkQueueCreate(&queueMaker);

    // the screen's two images, the two frame buffers and the depth buffer
    DkImageLayoutMaker lm;
    DkImageLayout screenLayout, colourLayout, depthLayout;
    dkImageLayoutMakerDefaults(&lm, device_);
    lm.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine | DkImageFlags_HwCompression;
    lm.format = DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = kScreenWidth;
    lm.dimensions[1] = kScreenHeight;
    dkImageLayoutInitialize(&screenLayout, &lm);
    dkImageLayoutMakerDefaults(&lm, device_);
    lm.flags = DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine;
    lm.format = DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = kWidth;
    lm.dimensions[1] = kHeight;
    dkImageLayoutInitialize(&colourLayout, &lm);
    dkImageLayoutMakerDefaults(&lm, device_);
    lm.flags = DkImageFlags_UsageRender;
    lm.format = DkImageFormat_ZF32;
    lm.dimensions[0] = kWidth;
    lm.dimensions[1] = kHeight;
    dkImageLayoutInitialize(&depthLayout, &lm);
    struct Placed { DkImage* image; const DkImageLayout* layout; std::uint32_t offset; };
    Placed placed[5] = {{&screen_[0], &screenLayout, 0}, {&screen_[1], &screenLayout, 0}, {&colour_[0], &colourLayout, 0},
                        {&colour_[1], &colourLayout, 0}, {&depth_, &depthLayout, 0}};
    std::uint32_t total = 0;
    for (Placed& p : placed) {
        total = Align(total, dkImageLayoutGetAlignment(p.layout));
        p.offset = total;
        total += (std::uint32_t)dkImageLayoutGetSize(p.layout);
    }
    imageMem_ = Block(total, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
    for (Placed& p : placed)
        dkImageInitialize(p.image, p.layout, imageMem_, p.offset);
    const DkImage* screens[2] = {&screen_[0], &screen_[1]};
    DkSwapchainMaker swapchainMaker;
    dkSwapchainMakerDefaults(&swapchainMaker, device_, nwindowGetDefault(), screens, 2);
    swapchain_ = dkSwapchainCreate(&swapchainMaker);

    // the shaders
    codeMem_ = Block(64u << 10, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code);
    std::uint32_t fragmentAt = Align((std::uint32_t)gs_vsh_dksh_size, DK_SHADER_CODE_ALIGNMENT);
    std::memcpy(dkMemBlockGetCpuAddr(codeMem_), gs_vsh_dksh, gs_vsh_dksh_size);
    std::memcpy((std::uint8_t*)dkMemBlockGetCpuAddr(codeMem_) + fragmentAt, gs_fsh_dksh, gs_fsh_dksh_size);
    DkShaderMaker shaderMaker;
    dkShaderMakerDefaults(&shaderMaker, codeMem_, 0);
    dkShaderInitialize(&vertexShader_, &shaderMaker);
    dkShaderMakerDefaults(&shaderMaker, codeMem_, fragmentAt);
    dkShaderInitialize(&fragmentShader_, &shaderMaker);

    // the uniform buffers, the descriptor sets and a frame's capture
    vertexUbo_ = 0;
    fragmentUbo_ = Align(sizeof(VertexParams), DK_UNIFORM_BUF_ALIGNMENT);
    imageSet_ = Align(fragmentUbo_ + sizeof(FragmentParams), DK_UNIFORM_BUF_ALIGNMENT);
    samplerSet_ = imageSet_ + kMaxTextures * sizeof(DkImageDescriptor);
    capture_ = Align(samplerSet_ + 8 * sizeof(DkSamplerDescriptor), 0x1000);
    dataMem_ = Block(capture_ + kWidth * kHeight * 4, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    for (int k = 0; k < 4; k++) {   // bilinear, S and T each repeated or clamped
        DkSampler sampler;
        dkSamplerDefaults(&sampler);
        sampler.minFilter = sampler.magFilter = DkFilter_Linear;
        sampler.wrapMode[0] = (k & 1) ? DkWrapMode_ClampToEdge : DkWrapMode_Repeat;
        sampler.wrapMode[1] = (k & 2) ? DkWrapMode_ClampToEdge : DkWrapMode_Repeat;
        DkSamplerDescriptor d;
        dkSamplerDescriptorInitialize(&d, &sampler);
        std::memcpy((std::uint8_t*)dkMemBlockGetCpuAddr(dataMem_) + samplerSet_ + k * sizeof d, &d, sizeof d);
    }

    textureMem_ = Block(kTextureMemory, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
    frameMem_ = Block(kFrameMemory, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    commandMem_ = Block(kCommandMemory, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    DkCmdBufMaker cmdMaker;
    dkCmdBufMakerDefaults(&cmdMaker, device_);
    cmd_ = dkCmdBufCreate(&cmdMaker);
    if (!queue_ || !swapchain_ || !imageMem_ || !dataMem_ || !textureMem_ || !frameMem_ || !commandMem_ || !cmd_)
        return false;
    if (const char* at = std::getenv("SLY1_GPU_CAPTURE"))
        captureAt_ = std::atol(at);

    // both frame buffers and the depth buffer start empty
    BeginCommands();
    for (int t = 0; t < 2; t++) {
        BindTarget(t);
        float black[4] = {0, 0, 0, 0};
        dkCmdBufClearColor(cmd_, 0, DkColorMask_RGBA, black);
        dkCmdBufClearDepthStencil(cmd_, true, 0.0f, 0, 0);
    }
    Send(true);
    return true;
}

// A frame's commands from the start of the command memory: the state that never changes.
void Renderer::BeginCommands()
{
    dkCmdBufClear(cmd_);
    dkCmdBufAddMemory(cmd_, commandMem_, 0, kCommandMemory);
    frameUsed_ = 0;
    draws_ = 0;
    bound_ = -1;
    last_.any = false;
    DkGpuAddr data = dkMemBlockGetGpuAddr(dataMem_);
    const DkShader* shaders[2] = {&vertexShader_, &fragmentShader_};
    dkCmdBufBindShaders(cmd_, DkStageFlag_GraphicsMask, shaders, 2);
    DkBufExtents vertexUbo = {data + vertexUbo_, sizeof(VertexParams)}, fragmentUbo = {data + fragmentUbo_, sizeof(FragmentParams)};
    dkCmdBufBindUniformBuffers(cmd_, DkStage_Vertex, 0, &vertexUbo, 1);
    dkCmdBufBindUniformBuffers(cmd_, DkStage_Fragment, 0, &fragmentUbo, 1);
    dkCmdBufBindImageDescriptorSet(cmd_, data + imageSet_, kMaxTextures);
    dkCmdBufBindSamplerDescriptorSet(cmd_, data + samplerSet_, 8);
    DkRasterizerState raster;
    dkRasterizerStateDefaults(&raster);
    raster.cullMode = DkFace_None;
    dkCmdBufBindRasterizerState(cmd_, &raster);
    static const DkVtxAttribState attribs[5] = {
        {0, 0, offsetof(gs::DrawVertex, x), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
        {0, 0, offsetof(gs::DrawVertex, z), DkVtxAttribSize_1x32, DkVtxAttribType_Uint, 0},
        {0, 0, offsetof(gs::DrawVertex, r), DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0},
        {0, 0, offsetof(gs::DrawVertex, s), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
        {0, 0, offsetof(gs::DrawVertex, fog), DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0},
    };
    static const DkVtxBufferState buffer = {sizeof(gs::DrawVertex), 0};
    dkCmdBufBindVtxAttribState(cmd_, attribs, 5);
    dkCmdBufBindVtxBufferState(cmd_, &buffer, 1);
    DkBufExtents vertices = {dkMemBlockGetGpuAddr(frameMem_), kFrameMemory};
    dkCmdBufBindVtxBuffers(cmd_, 0, &vertices, 1);
    dkCmdBufBindIdxBuffer(cmd_, DkIdxFormat_Uint32, dkMemBlockGetGpuAddr(frameMem_));
    dkCmdBufSetLineWidth(cmd_, 1.5f);
    dkCmdBufSetPointSize(cmd_, 2.0f);
    // window pixels to clip space: x right, y up, the image's first row at the top
    VertexParams vp = {{2.0f / kFieldWidth, -2.0f / kFieldHeight, -1.0f, 1.0f}};
    dkCmdBufPushConstants(cmd_, data + vertexUbo_, sizeof vp, 0, sizeof vp, &vp);
}

// The commands so far sent to the GPU; with wait, done before the return (the frame's memory
// is free again).
void Renderer::Send(bool wait)
{
    dkQueueSubmitCommands(queue_, dkCmdBufFinishList(cmd_));
    sends_++;
    if (wait) {
        dkQueueWaitIdle(queue_);
        BeginCommands();
    }
}

void Renderer::BindTarget(int target)
{
    if (bound_ == target)
        return;
    bound_ = target;
    DkImageView colour, depth;
    dkImageViewDefaults(&colour, &colour_[target]);
    dkImageViewDefaults(&depth, &depth_);
    const DkImageView* colours[1] = {&colour};
    dkCmdBufBindRenderTargets(cmd_, colours, 1, &depth);
    DkViewport viewport = {0.0f, 0.0f, (float)kWidth, (float)kHeight, 0.0f, 1.0f};
    dkCmdBufSetViewports(cmd_, 0, &viewport, 1);
}

// The game's two frame buffers (FBP 0 and 70, 640 wide); -1 for any other.
int Renderer::TargetOf(std::uint64_t fbp)
{
    return fbp == 0 ? 0 : fbp == 70 ? 1 : -1;
}

const Texture* Renderer::TextureFor(const gs::DrawState& state)
{
    // what names the texels: TBP0, TBW, PSM, TW, TH and the palette's CBP, CPSM, CSM, CSA
    std::uint64_t key = state.tex0 & 0x1FFFFFE3FFFFFFFFull;
    std::uint32_t generation = gs::TextureGeneration(state);
    auto it = textures_.find(key);
    if (it != textures_.end() && it->second.generation == generation)
        return &it->second;
    std::uint64_t hash = gs::TextureSource(state);
    hashes_++;
    if (it != textures_.end() && it->second.hash == hash) {
        it->second.generation = generation;
        return &it->second;
    }
    int w, h;
    gs::DecodeTexture(state, pixels_, w, h);
    decodes_++;
    std::uint32_t bytes = (std::uint32_t)pixels_.size() * 4;
    if (frameUsed_ + bytes > kFrameMemory)
        Send(true);
    if (it == textures_.end()) {
        DkImageLayoutMaker lm;
        DkImageLayout layout;
        dkImageLayoutMakerDefaults(&lm, device_);
        lm.flags = 0;
        lm.format = DkImageFormat_RGBA8_Unorm;
        lm.dimensions[0] = (std::uint32_t)w;
        lm.dimensions[1] = (std::uint32_t)h;
        dkImageLayoutInitialize(&layout, &lm);
        std::uint32_t at = Align(textureUsed_, dkImageLayoutGetAlignment(&layout));
        std::uint32_t size = (std::uint32_t)dkImageLayoutGetSize(&layout);
        if (at + size > kTextureMemory || nextTexture_ == kMaxTextures) {
            // full: everything drawn with the old textures first, then all of them forgotten
            Send(true);
            textures_.clear();
            textureUsed_ = 0;
            nextTexture_ = 0;
            cacheResets_++;
            at = 0;
        }
        Texture t;
        dkImageInitialize(&t.image, &layout, textureMem_, at);
        textureUsed_ = at + size;
        t.id = nextTexture_++;
        t.width = w;
        t.height = h;
        it = textures_.emplace(key, t).first;
        DkImageView view;
        dkImageViewDefaults(&view, &it->second.image);
        DkImageDescriptor d;
        dkImageDescriptorInitialize(&d, &view, false, false);
        dkCmdBufPushData(cmd_, dkMemBlockGetGpuAddr(dataMem_) + imageSet_ + it->second.id * sizeof d, &d, sizeof d);
    }
    Texture& t = it->second;
    t.generation = generation;
    t.hash = hash;
    std::memcpy((std::uint8_t*)dkMemBlockGetCpuAddr(frameMem_) + frameUsed_, pixels_.data(), bytes);
    DkImageView view;
    dkImageViewDefaults(&view, &t.image);
    DkCopyBuf from = {dkMemBlockGetGpuAddr(frameMem_) + frameUsed_, 0, 0};
    DkImageRect rect = {0, 0, 0, (std::uint32_t)t.width, (std::uint32_t)t.height, 1};
    dkCmdBufCopyBufferToImage(cmd_, &from, &view, &rect, 0);
    frameUsed_ = Align(frameUsed_ + bytes, 64);
    uploads_++;
    return &t;
}

void Renderer::Begin(const gs::DrawState& s)
{
    std::uint64_t t0 = NowNs();
    texture_ = s.tme && TargetOf(s.frame & 0x1FF) >= 0 ? TextureFor(s) : nullptr;
    textureNs_ += NowNs() - t0;
}

void Renderer::Draw(const gs::DrawState& s, const gs::DrawVertex* v, std::size_t count, const std::uint32_t* index,
                    std::size_t indices)
{
    struct Timed {
        std::uint64_t& total;
        std::uint64_t start = NowNs();
        ~Timed() { total += NowNs() - start; }
    } timed{drawNs_};
    int target = TargetOf(s.frame & 0x1FF);
    if (target < 0) {
        otherTargets_++;
        return;
    }
    std::uint32_t bytes = (std::uint32_t)(count * sizeof(gs::DrawVertex)), indexBytes = (std::uint32_t)(indices * 4);
    if (bytes + indexBytes + 64 > kFrameMemory)
        return;
    if (frameUsed_ + bytes + indexBytes + 64 > kFrameMemory || draws_ >= kMaxDraws)
        Send(true);
    const Texture* texture = texture_;
    bool fresh = !last_.any;
    last_.any = true;
    BindTarget(target);
    runs_++;
    vertices_ += indices;
    draws_++;

    // the tests: ATE's and what a failing pixel still writes (AFAIL), the depth test
    std::uint32_t test = (std::uint32_t)s.test;
    bool ate = test & 1;
    std::uint32_t atst = (test >> 1) & 7, aref = (test >> 4) & 0xFF, afail = (test >> 12) & 3;
    bool date = (test >> 14) & 1, zte = (test >> 16) & 1;
    std::uint32_t ztst = (test >> 17) & 3;
    bool zmsk = (s.zbuf >> 32) & 1;
    if (date)
        destinationAlpha_++;
    if (zte && ztst == 0)
        return;   // nothing passes

    // the blend: (A - B) * C + D of Cs, Cd and 0, C the second output (As or FIX) or the frame's alpha
    DkBlendState blend;
    dkBlendStateDefaults(&blend);
    blend.alphaBlendOp = DkBlendOp_Add;
    blend.srcAlphaBlendFactor = DkBlendFactor_One;
    blend.dstAlphaBlendFactor = DkBlendFactor_Zero;
    FragmentParams fp;
    std::memset(&fp, 0, sizeof fp);
    if (s.abe) {
        std::uint32_t a = s.alpha & 3, b = (s.alpha >> 2) & 3, c = (s.alpha >> 4) & 3, d = (s.alpha >> 6) & 3;
        fp.blend[0] = c == 0 ? 1.0f : 0.0f;
        fp.blend[1] = ((s.alpha >> 32) & 0xFF) / 128.0f;
        bool frameAlpha = c == 1;
        // each of Cs and Cd: how many times C (-1, 0, 1) and how many times 1 (0, 1)
        int sc = (a == 0) - (b == 0), s1 = d == 0, dc = (a == 1) - (b == 1), d1 = d == 1;
        auto factor = [&](int timesC, int times1, bool& negative) {
            negative = false;
            if (timesC == 0)
                return times1 ? DkBlendFactor_One : DkBlendFactor_Zero;
            if (timesC > 0) {
                if (times1)
                    unsupportedBlends_++;   // 1 + C: as 1
                return times1 ? DkBlendFactor_One : frameAlpha ? DkBlendFactor_DstAlpha : DkBlendFactor_Src1Color;
            }
            if (times1)
                return frameAlpha ? DkBlendFactor_InvDstAlpha : DkBlendFactor_InvSrc1Color;
            negative = true;
            return frameAlpha ? DkBlendFactor_DstAlpha : DkBlendFactor_Src1Color;
        };
        bool sneg, dneg;
        blend.srcColorBlendFactor = factor(sc, s1, sneg);
        blend.dstColorBlendFactor = factor(dc, d1, dneg);
        blend.colorBlendOp = sneg ? DkBlendOp_RevSub : dneg ? DkBlendOp_Sub : DkBlendOp_Add;
        if (sneg && dneg)
            unsupportedBlends_++;
    }
    if (fresh || std::memcmp(&blend, &last_.blend, sizeof blend) != 0) {
        dkCmdBufBindBlendStates(cmd_, 0, &blend, 1);
        last_.blend = blend;
    }

    fp.mode[0] = s.tme;
    fp.mode[1] = (std::int32_t)((s.tex0 >> 35) & 3);
    fp.mode[2] = (std::int32_t)((s.tex0 >> 34) & 1);
    fp.mode[3] = s.fge;
    fp.fogColour[0] = (float)(s.fogcol & 0xFF);
    fp.fogColour[1] = (float)((s.fogcol >> 8) & 0xFF);
    fp.fogColour[2] = (float)((s.fogcol >> 16) & 0xFF);
    if (fresh || std::memcmp(&fp, &last_.fp, sizeof fp) != 0) {
        dkCmdBufPushConstants(cmd_, dkMemBlockGetGpuAddr(dataMem_) + fragmentUbo_, sizeof fp, 0, sizeof fp, &fp);
        last_.fp = fp;
    }
    if (texture) {
        std::uint32_t sampler = (((s.clamp >> 0) & 3) == 1 ? 1 : 0) | (((s.clamp >> 2) & 3) == 1 ? 2 : 0);
        DkResHandle handle = dkMakeTextureHandle(texture->id, sampler);
        if (fresh || handle != last_.texture) {
            dkCmdBufBindTextures(cmd_, DkStage_Fragment, 0, &handle, 1);
            last_.texture = handle;
        }
    } else if (fresh) {
        last_.texture = ~0u;
    }

    // SCISSOR's rectangle (inclusive) in the image's pixels
    {
        float sx = kWidth / kFieldWidth, sy = kHeight / kFieldHeight;
        std::uint32_t x0 = (std::uint32_t)(s.scissor & 0x7FF), x1 = (std::uint32_t)((s.scissor >> 16) & 0x7FF) + 1,
                      y0 = (std::uint32_t)((s.scissor >> 32) & 0x7FF), y1 = (std::uint32_t)((s.scissor >> 48) & 0x7FF) + 1;
        DkScissor sc;
        sc.x = std::min((std::uint32_t)(x0 * sx), kWidth);
        sc.y = std::min((std::uint32_t)(y0 * sy), kHeight);
        sc.width = std::min((std::uint32_t)(x1 * sx + 0.5f), kWidth) - sc.x;
        sc.height = std::min((std::uint32_t)(y1 * sy + 0.5f), kHeight) - sc.y;
        if (fresh || std::memcmp(&sc, &last_.scissor, sizeof sc) != 0) {
            dkCmdBufSetScissors(cmd_, 0, &sc, 1);
            last_.scissor = sc;
        }
    }
    if (fresh)
        last_.colour = last_.write = last_.depth = last_.aref = ~0u;

    std::uint32_t first = frameUsed_ / sizeof(gs::DrawVertex);
    frameUsed_ = Align(frameUsed_, sizeof(gs::DrawVertex));
    first = frameUsed_ / sizeof(gs::DrawVertex);
    std::memcpy((std::uint8_t*)dkMemBlockGetCpuAddr(frameMem_) + frameUsed_, v, bytes);
    frameUsed_ += bytes;
    std::uint32_t firstIndex = frameUsed_ / 4;
    std::memcpy((std::uint8_t*)dkMemBlockGetCpuAddr(frameMem_) + frameUsed_, index, indexBytes);
    frameUsed_ += indexBytes;
    static const DkPrimitive kPrimitive[3] = {DkPrimitive_Points, DkPrimitive_Lines, DkPrimitive_Triangles};
    DkPrimitive primitive = kPrimitive[s.kind < 3 ? s.kind : 2];

    // one pass of the pixels an alpha comparison lets through, writing colour, alpha and depth or not
    auto pass = [&](DkCompareOp alphaOp, bool rgb, bool alpha, bool depthWrite) {
        DkColorState colour;
        dkColorStateDefaults(&colour);
        colour.blendEnableMask = s.abe ? 1 : 0;
        colour.alphaCompareOp = alphaOp;
        std::uint32_t colourKey = (s.abe ? 1u : 0u) | (std::uint32_t)alphaOp << 1;
        if (colourKey != last_.colour) {
            dkCmdBufBindColorState(cmd_, &colour);
            last_.colour = colourKey;
        }
        if (aref != last_.aref) {
            dkCmdBufSetAlphaRef(cmd_, aref / 255.0f);
            last_.aref = aref;
        }
        DkColorWriteState write;
        dkColorWriteStateDefaults(&write);
        write.masks = (rgb ? DkColorMask_RGB : 0) | (alpha ? DkColorMask_A : 0);
        if (write.masks != last_.write) {
            dkCmdBufBindColorWriteState(cmd_, &write);
            last_.write = write.masks;
        }
        DkDepthStencilState ds;
        dkDepthStencilStateDefaults(&ds);
        ds.depthTestEnable = 1;
        ds.depthWriteEnable = depthWrite && !zmsk;
        ds.depthCompareOp = !zte || ztst == 1 ? DkCompareOp_Always : ztst == 2 ? DkCompareOp_Gequal : DkCompareOp_Greater;
        std::uint32_t depthKey = (std::uint32_t)ds.depthWriteEnable | (std::uint32_t)ds.depthCompareOp << 1;
        if (depthKey != last_.depth) {
            dkCmdBufBindDepthStencilState(cmd_, &ds);
            last_.depth = depthKey;
        }
        dkCmdBufDrawIndexed(cmd_, primitive, (std::uint32_t)indices, 1, firstIndex, (std::int32_t)first, 0);
    };
    // ATST: NEVER, ALWAYS, LESS, LEQUAL, EQUAL, GEQUAL, GREATER, NOTEQUAL
    static const DkCompareOp kPasses[8] = {DkCompareOp_Never, DkCompareOp_Always, DkCompareOp_Less, DkCompareOp_Lequal,
                                           DkCompareOp_Equal, DkCompareOp_Gequal, DkCompareOp_Greater, DkCompareOp_NotEqual};
    static const DkCompareOp kFails[8] = {DkCompareOp_Always, DkCompareOp_Never, DkCompareOp_Gequal, DkCompareOp_Greater,
                                          DkCompareOp_NotEqual, DkCompareOp_Less, DkCompareOp_Lequal, DkCompareOp_Equal};
    if (!ate || atst == 1) {
        pass(DkCompareOp_Always, true, true, true);
        return;
    }
    if (atst != 0)
        pass(kPasses[atst], true, true, true);
    // AFAIL: KEEP, FB_ONLY, ZB_ONLY, RGB_ONLY
    if (afail == 1)
        pass(kFails[atst], true, true, false);
    else if (afail == 2)
        pass(kFails[atst], false, false, true);
    else if (afail == 3)
        pass(kFails[atst], true, false, false);
}

// The frame buffer's image read back, halved each way and printed as lines of hexadecimal RGB
// (tools/gpu_capture.py makes a PNG of them).
void Renderer::Capture(int target)
{
    DkImageView view;
    dkImageViewDefaults(&view, &colour_[target]);
    DkImageRect rect = {0, 0, 0, kWidth, kHeight, 1};
    DkCopyBuf to = {dkMemBlockGetGpuAddr(dataMem_) + capture_, 0, 0};
    dkCmdBufCopyImageToBuffer(cmd_, &view, &rect, &to, 0);
    Send(true);
    const std::uint8_t* p = (const std::uint8_t*)dkMemBlockGetCpuAddr(dataMem_) + capture_;
    std::uint32_t w = kWidth / 2, h = kHeight / 2;
    std::fprintf(stderr, "gpucap: begin %u %u frame %llu\n", w, h, (unsigned long long)frames_);
    std::vector<char> line(w * 6 + 1);
    static const char kHex[] = "0123456789abcdef";
    for (std::uint32_t y = 0; y < h; y++) {
        for (std::uint32_t x = 0; x < w; x++)
            for (int c = 0; c < 3; c++) {
                const std::uint8_t* q = p + ((y * 2) * kWidth + x * 2) * 4 + c;
                unsigned v = (q[0] + q[4] + q[kWidth * 4] + q[kWidth * 4 + 4] + 2) / 4;
                line[(x * 3 + c) * 2] = kHex[v >> 4];
                line[(x * 3 + c) * 2 + 1] = kHex[v & 15];
            }
        line[w * 6] = 0;
        std::fprintf(stderr, "gpucap: %s\n", line.data());
    }
    std::fprintf(stderr, "gpucap: end\n");
}

void Renderer::Present(std::uint64_t dispfb, std::uint64_t)
{
    frames_++;
    int target = TargetOf(dispfb & 0x1FF);
    if (target < 0)
        target = 0;
    if (captureAt_ >= 0 && (long)frames_ == captureAt_)
        Capture(target);
    std::uint64_t t0 = NowNs();
    int slot = dkQueueAcquireImage(queue_, swapchain_);
    acquireNs_ += NowNs() - t0;
    DkImageView screen, from;
    dkImageViewDefaults(&screen, &screen_[slot]);
    dkImageViewDefaults(&from, &colour_[target]);
    const DkImageView* screens[1] = {&screen};
    dkCmdBufBindRenderTargets(cmd_, screens, 1, nullptr);
    DkScissor all = {0, 0, kScreenWidth, kScreenHeight};
    dkCmdBufSetScissors(cmd_, 0, &all, 1);
    float black[4] = {0, 0, 0, 1};
    dkCmdBufClearColor(cmd_, 0, DkColorMask_RGBA, black);
    DkImageRect src = {0, 0, 0, kWidth, kHeight, 1}, dst = {(kScreenWidth - kWidth) / 2, 0, 0, kWidth, kHeight, 1};
    dkCmdBufBlitImage(cmd_, &from, &src, &screen, &dst, 0, 0);
    Send(false);
    dkQueuePresentImage(queue_, swapchain_, slot);
    std::uint64_t t1 = NowNs();
    dkQueueWaitIdle(queue_);
    waitNs_ += NowNs() - t1;
    BeginCommands();
    presentNs_ += NowNs() - t0;
}

void Renderer::Report()
{
    std::fprintf(stderr, "gpu: %llu frames, %llu runs of primitives (%llu vertices), %llu command lists sent\n",
                 (unsigned long long)frames_, (unsigned long long)runs_, (unsigned long long)vertices_,
                 (unsigned long long)sends_);
    std::fprintf(stderr, "gpu: %zu textures held (%u KB), %llu checked after uploads, %llu decoded, %llu uploaded, %llu times the cache "
                 "was emptied\n", textures_.size(), textureUsed_ >> 10, (unsigned long long)hashes_, (unsigned long long)decodes_, (unsigned long long)uploads_,
                 (unsigned long long)cacheResets_);
    std::fprintf(stderr, "gpu: %.3f s in draws (%.3f s of it textures), %.3f s in swaps (%.3f s for a screen image, %.3f s "
                 "for the GPU to finish)\n", drawNs_ / 1e9, textureNs_ / 1e9, presentNs_ / 1e9, acquireNs_ / 1e9, waitNs_ / 1e9);
    std::fprintf(stderr, "gpu: not drawn as the GS would: %llu runs into other buffers, %llu with the destination alpha test, "
                 "%llu blends approximated\n", (unsigned long long)otherTargets_, (unsigned long long)destinationAlpha_,
                 (unsigned long long)unsupportedBlends_);
}

Renderer g_renderer;
bool g_started;

}  // namespace

bool Start()
{
    if (!g_renderer.Create()) {
        std::fprintf(stderr, "gpu: no renderer (deko3d's device, queue or memory)\n");
        return false;
    }
    gs::SetBackend(&g_renderer);
    g_started = true;
    return true;
}

void Report()
{
    if (g_started)
        g_renderer.Report();
}

}  // namespace gpu
}  // namespace port

#else

namespace port {
namespace gpu {
bool Start() { return false; }
void Report() {}
}  // namespace gpu
}  // namespace port

#endif
