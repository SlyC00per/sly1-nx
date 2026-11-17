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

#include <cmath>
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
#include "gs_lite_fsh_dksh.h"
#include "batch_vsh_dksh.h"

namespace port {
namespace gpu {

namespace {

constexpr std::uint32_t kScreenWidth = 1280, kScreenHeight = 720;
constexpr std::uint32_t kWidth = 960, kHeight = 720;     // a frame buffer's image (4:3)
constexpr float kFieldWidth = 640.0f, kFieldHeight = 224.0f;
constexpr std::uint32_t kFrameMemory = 24u << 20;        // a frame's vertices and texture uploads
constexpr std::uint32_t kCommandMemory = 16u << 20;
// The sets of a frame's memory (its vertices and its commands), used in turn. A set is taken again
// once the GPU has done with the frame recorded in it. With two the render thread waited at each
// frame's end for the GPU to finish the frame before (5 to 8 ms of the Switch's 32 a frame, which
// the GPU spent on that frame while the thread did nothing); with three it waits for the frame
// before that, which is done unless the GPU is slower than the thread.
constexpr int kSets = 3;
constexpr std::uint32_t kTextureMemory = 64u << 20;
constexpr std::uint32_t kMaxTextures = 2048;
constexpr int kSmallTargets = 8;                      // the buffers other than the frame's two the game draws into (a shadow's picture)
constexpr std::uint32_t kSmallScale = 1;               // an image's pixels to one of such a buffer's, each way (1: the game's
                                                       // own, so that its blur of a shadow's picture spreads as on the GS)
constexpr std::uint32_t kTargetMemory = 8u << 20;
constexpr std::uint32_t kMaxDraws = 12000;               // a frame's draws before its commands are sent

std::uint32_t Align(std::uint32_t v, std::uint32_t a) { return (v + a - 1) & ~(a - 1); }

void Debug(void*, const char* context, DkResult result, const char* message)
{
    std::fprintf(stderr, "gpu: deko3d: %s: %s (result %d)\n", context ? context : "?", message ? message : "?", (int)result);
}

// The renderer's own timers (SLY1_GPU_TIMERS): a read of the clock per draw costs little on the
// Switch but much in an emulator, where it may leave the guest, so it is off unless asked for.
inline std::uint64_t Tick()
{
    static const bool on = std::getenv("SLY1_GPU_TIMERS") != nullptr;
    return on ? NowNs() : 0;
}

struct VertexParams {
    float toClip[4];
};

struct BatchParams {   // batch_vsh.glsl's
    float matrix[16];
    float toClip[4];
    float depth[4];
    float fog[4];
};

// What a draw's vertices are: the GS's (gs_vsh.glsl) or a batch's (batch_vsh.glsl).
struct Source {
    const void* vertices;
    std::uint32_t vertexBytes, vertexSize;
    const std::uint32_t* index;
    std::uint32_t indices;
    const gs::Batch* batch;   // null for the GS's
    // a batch's, put in the frame's memory already (DrawBatch): its positions and STQ (32 bytes a
    // vertex), its colours (16), its indices, as offsets in the frame's memory
    bool placed = false;
    std::uint32_t vertexAt = 0, colourAt = 0, indexAt = 0;
};

struct FragmentParams {
    std::int32_t mode[4];
    float fogColour[4];
    float coef[4];
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
    bool TakesBatches() const override { return true; }
    bool DrawBatch(const gs::DrawState& state, const gs::Batch& batch) override;
    void Present(std::uint64_t dispfb, std::uint64_t display) override;
    void Report();

private:
    DkMemBlock Block(std::uint32_t size, std::uint32_t flags);
    void BeginCommands();
    void Send(bool wait);
    void BindTarget(int target);
    void BindSource(const Source& src);
    void Submit(const gs::DrawState& s, const Source& src);
    void FlushPending();
    // The GS's run held back: the next is added to it when its state and texture are the same
    // (the game sends a character's outline as hundreds of runs of one line each, the registers
    // written again between them), and the whole is one draw.
    gs::DrawState pendState_;
    std::vector<gs::DrawVertex> pendVertices_;
    std::vector<std::uint32_t> pendIndex_;
    const Texture* pendTexture_ = nullptr;
    bool pending_ = false;
    std::size_t lastCount_ = 0, lastIndices_ = 0;   // of the last run added
    double area_[4] = {};
    int areaRuns_[4] = {};
    // SLY1_GPU_AB=1: a measurement in one run. Every 30 frames the next of six ways of drawing: 0 as it
    // is, 1 the long pixel shader for every run, 2 no destination alpha test, 3 no batches, 4 none of the GS's
    // runs into the frame, 5 everything cut to a sixteenth of the picture. For each, the mean time
    // from one frame's end to the next and the mean wait for the GPU (the picture is not to look at).
    int ab_ = 0;
    bool abOn_ = false;
    std::uint64_t abAt_ = 0, abNs_[6] = {}, abWaitNs_[6] = {}, abFrames_[6] = {};
    std::uint64_t merged_ = 0, dropped_ = 0;
    const Texture* TextureFor(const gs::DrawState& state);
    void Capture(int target);
    static int TargetOf(std::uint64_t fbp);
    int TargetFor(const gs::DrawState& s);
    struct Small {
        // two images: a run that reads the buffer it draws into (the game's blur of a shadow's
        // picture) reads the current one and draws the other, which becomes the current
        DkImage image[2];
        Texture texture[2];         // each as a texture (its id among the descriptors, its size)
        int cur = 0;
        std::uint32_t fbp = ~0u;
        std::uint32_t width = 0, height = 0;   // in the GS's pixels
        bool drawn = false;         // drawn into since it was last read as a texture
        std::uint32_t serial = 0;   // gs::UploadSerial() when it was last drawn into
    };
    static constexpr std::uint32_t kRetired = 0xFFFFFFFEu;   // a small target's fbp once the game has put textures in its buffer
    Small small_[kSmallTargets];
    int smallCount_ = 0;
    DkMemBlock targetMem_ = nullptr;
    std::uint32_t targetUsed_ = 0;
    // the bound target's size: in the GS's pixels (a field of the frame) and in the image's
    float fieldWidth_ = kFieldWidth, fieldHeight_ = kFieldHeight;
    std::uint32_t imageWidth_ = kWidth, imageHeight_ = kHeight;
    std::uint64_t smallRuns_ = 0, smallTextured_ = 0, smallSelf_ = 0, smallSteps_ = 0;
    bool inSelf_ = false;       // drawing one step of such a run: the target is the buffer's other image
    int smallTrace_ = 0, smallSelfTrace_ = 0, smallIntoTrace_ = 0;

    DkDevice device_ = nullptr;
    DkQueue queue_ = nullptr;
    DkSwapchain swapchain_ = nullptr;
    DkCmdBuf cmd_ = nullptr;
    // a frame's vertices, uploads and commands: two sets in turn, so that a frame is recorded while
    // the GPU draws the one before
    DkMemBlock frameMems_[kSets] = {}, commandMems_[kSets] = {};
    DkFence fences_[kSets];
    bool fenced_[kSets] = {};
    int set_ = 0;
    DkMemBlock imageMem_ = nullptr, frameMem_ = nullptr, commandMem_ = nullptr, codeMem_ = nullptr, dataMem_ = nullptr,
               textureMem_ = nullptr;
    DkImage screen_[2], colour_[2], depth_;
    DkShader vertexShader_, fragmentShader_, batchShader_, liteShader_;
    bool liteBound_ = false;   // gs_lite_fsh.glsl in place of gs_fsh.glsl
    std::uint64_t liteRuns_ = 0;
    std::uint32_t batchUbo_ = 0;
    // what the commands so far draw with: whose vertices, their layout, the viewport's depths
    int source_ = -1;
    std::uint32_t layout_ = 0;
    float near_ = -1.0f, far_ = -1.0f;
    bool batchSet_ = false;
    bool frameRead_ = false;       // the frame's alpha has been read by a run with DATE since a run without it
    int frameBound_ = -1;          // the frame buffer bound as the test's texture in this command list
    std::uint64_t dateBarriers_ = 0;
    std::uint64_t frameTextured_ = 0, targetTextured_ = 0;
    std::uint32_t otherSerial_[512] = {};   // the uploads so far when a buffer this renderer does not draw into was last drawn into; 0: never
    std::uint64_t retired_ = 0;
    BatchParams lastBatch_;
    std::vector<std::uint32_t> batchIndex_;
    struct BatchCache {   // the last batch put in the frame's memory (DrawBatch)
        std::uint32_t id = 0;
        std::uint64_t sends = 0;
        const std::uint32_t* quads = nullptr;
        std::uint32_t count = 0, stride = 0, positionAt = 0, stqAt = 0;
        std::uint32_t vertexAt = 0, indexAt = 0, indices = 0;
    } batchCache_;
    std::vector<std::uint32_t> staging_;   // a batch's indices, positions or colours made before they are copied
    std::uint64_t batches_ = 0, batchVertices_ = 0;
    float cullFront_ = 1.0f;
    int batchInFrame_ = 0;
    long drawInFrame_ = 0;
    bool stopped_ = true, paintLast_ = false;
    long allDraws_ = 0;
    // SLY1_GPU_REPLAY=n: the first frame of n draws or more kept (record_), then drawn again from the
    // record onto a cleared target in the frames after, cut short at each of SLY1_GPU_REPLAY_AT's
    // counts in turn, each printed (SLY1_GPU_CAPTURE's lines) with its brightness
    struct Recorded {
        gs::DrawState s;
        std::vector<std::uint8_t> vertices;
        std::uint32_t vertexSize = 0;
        std::vector<std::uint32_t> index;
        bool isBatch = false;
        gs::Batch batch;
        const Texture* texture = nullptr;
    };
    std::vector<Recorded> record_;
    bool recording_ = false, replaying_ = false;
    int recordTarget_ = 0;
    std::vector<long> replayAt_;
    std::size_t replayNext_ = 0;
    int replayState_ = 0;   // 0 waiting for the frame, 1 recording it, 2 replaying, 3 done
    void Replay(long count, bool capture = true);
    double SkyBlue(int target);
    void Search();
    void DumpDraw(long k);
    long listAt_ = -1;
    int listed_ = 0;
    int view_ = 0;   // SLY1_GPU_CAPTURE's views of the batches, a frame each after the first: 1 no depth test,
                    // 2 divided by w in the shader, 3 depths clamped, 4 no backface dropped, 5 facing
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
    std::uint64_t indexNs_ = 0, copyNs_ = 0, copiedBytes_ = 0;
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
    queueMaker.flags = DkQueueFlags_Graphics | (std::getenv("SLY1_GPU_NO_ZCULL") ? DkQueueFlags_DisableZcull : 0);
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
    std::uint32_t batchAt = Align(fragmentAt + (std::uint32_t)gs_fsh_dksh_size, DK_SHADER_CODE_ALIGNMENT);
    std::memcpy(dkMemBlockGetCpuAddr(codeMem_), gs_vsh_dksh, gs_vsh_dksh_size);
    std::memcpy((std::uint8_t*)dkMemBlockGetCpuAddr(codeMem_) + fragmentAt, gs_fsh_dksh, gs_fsh_dksh_size);
    std::memcpy((std::uint8_t*)dkMemBlockGetCpuAddr(codeMem_) + batchAt, batch_vsh_dksh, batch_vsh_dksh_size);
    DkShaderMaker shaderMaker;
    dkShaderMakerDefaults(&shaderMaker, codeMem_, 0);
    dkShaderInitialize(&vertexShader_, &shaderMaker);
    dkShaderMakerDefaults(&shaderMaker, codeMem_, fragmentAt);
    dkShaderInitialize(&fragmentShader_, &shaderMaker);
    dkShaderMakerDefaults(&shaderMaker, codeMem_, batchAt);
    dkShaderInitialize(&batchShader_, &shaderMaker);
    std::uint32_t liteAt = Align(batchAt + (std::uint32_t)batch_vsh_dksh_size, DK_SHADER_CODE_ALIGNMENT);
    std::memcpy((std::uint8_t*)dkMemBlockGetCpuAddr(codeMem_) + liteAt, gs_lite_fsh_dksh, gs_lite_fsh_dksh_size);
    dkShaderMakerDefaults(&shaderMaker, codeMem_, liteAt);
    dkShaderInitialize(&liteShader_, &shaderMaker);
    if (std::getenv("SLY1_GPU_CULL_FLIP"))
        cullFront_ = 0.0f;

    // the uniform buffers, the descriptor sets and a frame's capture
    vertexUbo_ = 0;
    fragmentUbo_ = Align(sizeof(VertexParams), DK_UNIFORM_BUF_ALIGNMENT);
    batchUbo_ = Align(fragmentUbo_ + sizeof(FragmentParams), DK_UNIFORM_BUF_ALIGNMENT);
    imageSet_ = Align(batchUbo_ + sizeof(BatchParams), DK_UNIFORM_BUF_ALIGNMENT);
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
    targetMem_ = Block(kTargetMemory, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
    for (int k = 0; k < kSets; k++) {
        frameMems_[k] = Block(kFrameMemory, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
        commandMems_[k] = Block(kCommandMemory, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
        if (!frameMems_[k] || !commandMems_[k])
            return false;
    }
    frameMem_ = frameMems_[0];
    commandMem_ = commandMems_[0];
    DkCmdBufMaker cmdMaker;
    dkCmdBufMakerDefaults(&cmdMaker, device_);
    cmd_ = dkCmdBufCreate(&cmdMaker);
    if (!queue_ || !swapchain_ || !imageMem_ || !dataMem_ || !textureMem_ || !frameMem_ || !commandMem_ || !cmd_)
        return false;
    if (const char* at = std::getenv("SLY1_GPU_LIST"))
        listAt_ = std::atol(at);
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
    source_ = -1;
    near_ = far_ = -1.0f;
    batchSet_ = false;
    frameRead_ = false;
    frameBound_ = -1;
    DkGpuAddr data = dkMemBlockGetGpuAddr(dataMem_);
    DkBufExtents fragmentUbo = {data + fragmentUbo_, sizeof(FragmentParams)};
    dkCmdBufBindUniformBuffers(cmd_, DkStage_Fragment, 0, &fragmentUbo, 1);
    dkCmdBufBindImageDescriptorSet(cmd_, data + imageSet_, kMaxTextures);
    dkCmdBufBindSamplerDescriptorSet(cmd_, data + samplerSet_, 8);
    DkRasterizerState raster;
    dkRasterizerStateDefaults(&raster);
    raster.depthClampEnable = 0;
    raster.cullMode = DkFace_None;   // a batch's backfaces are the fragment shader's to drop, triangle by triangle
    raster.provokingVertex = DkProvokingVertex_Last;
    dkCmdBufBindRasterizerState(cmd_, &raster);
    DkBufExtents vertices = {dkMemBlockGetGpuAddr(frameMem_), kFrameMemory};
    dkCmdBufBindVtxBuffers(cmd_, 0, &vertices, 1);
    dkCmdBufBindIdxBuffer(cmd_, DkIdxFormat_Uint32, dkMemBlockGetGpuAddr(frameMem_));
    dkCmdBufSetLineWidth(cmd_, 1.5f);
    dkCmdBufSetPointSize(cmd_, 2.0f);
    // window pixels to clip space: x right, y up, the image's first row at the top
    VertexParams vp = {{2.0f / kFieldWidth, -2.0f / kFieldHeight, -1.0f, 1.0f}};
    dkCmdBufPushConstants(cmd_, data + vertexUbo_, sizeof vp, 0, sizeof vp, &vp);
}

// The shaders, the vertices' layout, the vertex shader's parameters and the viewport's depths for
// the GS's vertices or a batch's, bound when they change.
void Renderer::BindSource(const Source& src)
{
    DkGpuAddr data = dkMemBlockGetGpuAddr(dataMem_);
    const gs::Batch* b = src.batch;
    const bool small = bound_ >= 2;
    int source = b ? 1 : 0;
    std::uint32_t layout = b ? 1 : 0;
    if (source != source_ || layout != layout_) {
        const DkShader* shaders[2] = {b ? &batchShader_ : &vertexShader_, &fragmentShader_};
        dkCmdBufBindShaders(cmd_, DkStageFlag_GraphicsMask, shaders, 2);
        liteBound_ = false;
        DkBufExtents ubo = {data + (b ? batchUbo_ : vertexUbo_), (std::uint32_t)(b ? sizeof(BatchParams) : sizeof(VertexParams))};
        dkCmdBufBindUniformBuffers(cmd_, DkStage_Vertex, 0, &ubo, 1);
        if (b) {
            // DrawBatch's layout: a vertex's position and control word, then its STQ, in one stream;
            // its colour in a second
            static const DkVtxAttribState attribs[4] = {
                {0, 0, 0, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
                {0, 0, 12, DkVtxAttribSize_1x32, DkVtxAttribType_Uint, 0},
                {0, 0, 16, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
                {1, 0, 0, DkVtxAttribSize_4x32, DkVtxAttribType_Uint, 0},
            };
            static const DkVtxBufferState buffers[2] = {{32, 0}, {16, 0}};
            dkCmdBufBindVtxAttribState(cmd_, attribs, 4);
            dkCmdBufBindVtxBufferState(cmd_, buffers, 2);
        } else {
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
        }
        // everything of the vertices' source again, in the order a list's start binds it
        DkBufExtents vertices = {dkMemBlockGetGpuAddr(frameMem_), kFrameMemory};
        dkCmdBufBindVtxBuffers(cmd_, 0, &vertices, 1);
        dkCmdBufBindIdxBuffer(cmd_, DkIdxFormat_Uint32, dkMemBlockGetGpuAddr(frameMem_));
        if (!b) {
            // A small target is drawn as the GS draws: it takes a pixel's values at the pixel's corner
            // (x, y), the GPU at its centre, so everything is put half a pixel right and down. The
            // game's blur of a shadow's picture depends on it: each of its copies lies half a texel
            // or a whole one aside, and which neighbours a pixel averages goes by where it is taken.
            const float half = small ? 0.5f : 0.0f;
            VertexParams vp = {{2.0f / fieldWidth_, -2.0f / fieldHeight_, -1.0f + half * 2.0f / fieldWidth_, 1.0f - half * 2.0f / fieldHeight_}};
            dkCmdBufPushConstants(cmd_, data + vertexUbo_, sizeof vp, 0, sizeof vp, &vp);
        }
        batchSet_ = false;
        source_ = source;
        layout_ = layout;
    }
    float near = 0.0f, far = 1.0f;
    if (b) {
        // VertexOf's arithmetic as clip coordinates: the GS's x and y (in pixels, XYOFFSET taken
        // off) to the image's, and the depths the near and far planes come to
        const float base = 8589934592.0f, k = 1.0f / 16384.0f;
        BatchParams bp;
        std::memcpy(bp.matrix, b->matrix, sizeof bp.matrix);
        bp.toClip[0] = b->scale[0] * k * (2.0f / fieldWidth_);
        bp.toClip[1] = -b->scale[1] * k * (2.0f / fieldHeight_);
        const float half = small ? 0.5f : 0.0f;   // as for the GS's vertices above
        bp.toClip[2] = ((b->offset[0] - base) * k - b->ofx / 16.0f + half) * (2.0f / fieldWidth_) - 1.0f;
        bp.toClip[3] = 1.0f - ((b->offset[1] - base) * k - b->ofy / 16.0f + half) * (2.0f / fieldHeight_);
        bp.depth[0] = bp.depth[1] = bp.depth[2] = bp.depth[3] = 0.0f;
        // the depths the program's far and near planes (z of -w and w) come to
        float depth = (b->offset[2] - base) * k, range = b->scale[2] * k;
        near = (depth - range) / 4294967296.0f;
        far = (depth + range) / 4294967296.0f;
        bp.fog[0] = b->fogScale;
        bp.fog[1] = b->wMax;
        bp.fog[2] = b->cull ? 1.0f : 0.0f;
        bp.fog[3] = 0.0f;
        if (!batchSet_ || std::memcmp(&bp, &lastBatch_, sizeof bp) != 0) {
            dkCmdBufPushConstants(cmd_, data + batchUbo_, sizeof bp, 0, sizeof bp, &bp);
            lastBatch_ = bp;
            batchSet_ = true;
        }
    }
    if (near != near_ || far != far_) {
        DkViewport viewport = {0.0f, 0.0f, (float)imageWidth_, (float)imageHeight_, near, far};
        dkCmdBufSetViewports(cmd_, 0, &viewport, 1);
        near_ = near;
        far_ = far;
    }
}

// The commands so far sent to the GPU; with wait, done before the return (the frame's memory
// is free again).
void Renderer::Send(bool wait)
{
    if (listAt_ >= 0 && (long)frames_ + 1 == listAt_)
        std::fprintf(stderr, "gpulist: send (wait %d) after %u draws\n", (int)wait, draws_);
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
    float fw = kFieldWidth, fh = kFieldHeight;
    std::uint32_t iw = kWidth, ih = kHeight;
    if (target >= 2) {   // a small target: no depth buffer (the game draws into it without one)
        const Small& t = small_[target - 2];
        dkImageViewDefaults(&colour, &t.image[inSelf_ ? t.cur ^ 1 : t.cur]);
        const DkImageView* colours[1] = {&colour};
        dkCmdBufBindRenderTargets(cmd_, colours, 1, nullptr);
        fw = (float)t.width;
        fh = (float)t.height;
        iw = t.width * kSmallScale;
        ih = t.height * kSmallScale;
    } else {
        dkImageViewDefaults(&colour, &colour_[target]);
        dkImageViewDefaults(&depth, &depth_);
        const DkImageView* colours[1] = {&colour};
        dkCmdBufBindRenderTargets(cmd_, colours, 1, &depth);
    }
    if (fw != fieldWidth_ || fh != fieldHeight_ || iw != imageWidth_ || ih != imageHeight_) {
        fieldWidth_ = fw;
        fieldHeight_ = fh;
        imageWidth_ = iw;
        imageHeight_ = ih;
        source_ = -1;        // the vertices' scale, the viewport and the scissor are the target's
        near_ = far_ = -1.0f;
        batchSet_ = false;
        last_.any = false;
    }
}

// The target a run draws into: the frame's two buffers (0 and 1), or a small buffer of the game's
// (2 and up: 64 to 256 pixels wide, 32 bits a pixel, taken as square; made when first drawn into),
// or -1 for any other.
int Renderer::TargetFor(const gs::DrawState& s)
{
    const std::uint32_t fbp = (std::uint32_t)(s.frame & 0x1FF);
    int target = TargetOf(fbp);
    if (target >= 0)
        return target;
    // SLY1_GPU_NO_SMALL_TARGETS=1: none, as before them (a run textured from such a buffer is then left out).
    static const bool off = std::getenv("SLY1_GPU_NO_SMALL_TARGETS") != nullptr;
    const std::uint32_t fbw = (std::uint32_t)((s.frame >> 16) & 0x3F), psm = (std::uint32_t)((s.frame >> 24) & 0x3F);
    if (off || fbw == 0 || fbw > 4 || psm != 0)
        return -1;
    for (int k = 0; k < smallCount_; k++)
        if (small_[k].fbp == fbp)
            return 2 + k;
    for (int k = 0; k < smallCount_; k++)
        if (small_[k].fbp == kRetired && small_[k].width == fbw * 64) {   // a retired one's images, for this buffer
            Small& t = small_[k];
            t.fbp = fbp;
            t.drawn = false;
            for (int i = 0; i < 2; i++) {
                t.cur = i;
                bound_ = -1;
                BindTarget(2 + k);
                DkScissor all = {0, 0, t.width * kSmallScale, t.height * kSmallScale};
                dkCmdBufSetScissors(cmd_, 0, &all, 1);
                float none[4] = {0, 0, 0, 0};
                dkCmdBufClearColor(cmd_, 0, DkColorMask_RGBA, none);
            }
            t.cur = 0;
            bound_ = -1;
            last_.any = false;
            return 2 + k;
        }
    if (smallCount_ == kSmallTargets)
        return -1;
    Small& t = small_[smallCount_];
    DkImageLayoutMaker lm;
    DkImageLayout layout;
    dkImageLayoutMakerDefaults(&lm, device_);
    lm.flags = DkImageFlags_UsageRender;
    lm.format = DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = fbw * 64 * kSmallScale;
    lm.dimensions[1] = fbw * 64 * kSmallScale;
    dkImageLayoutInitialize(&layout, &lm);
    std::uint32_t size = (std::uint32_t)dkImageLayoutGetSize(&layout);
    std::uint32_t at0 = Align(targetUsed_, dkImageLayoutGetAlignment(&layout));
    std::uint32_t at1 = Align(at0 + size, dkImageLayoutGetAlignment(&layout));
    if (at1 + size > kTargetMemory)
        return -1;
    targetUsed_ = at1 + size;
    t.fbp = fbp;
    t.width = t.height = fbw * 64;
    t.cur = 0;
    for (int i = 0; i < 2; i++) {
        dkImageInitialize(&t.image[i], &layout, targetMem_, i ? at1 : at0);
        t.texture[i].id = kMaxTextures - 2 - 2 * kSmallTargets + 2 * (std::uint32_t)smallCount_ + (std::uint32_t)i;
        t.texture[i].width = (int)t.width;
        t.texture[i].height = (int)t.height;
        DkImageView view;
        dkImageViewDefaults(&view, &t.image[i]);
        DkImageDescriptor d;
        dkImageDescriptorInitialize(&d, &view, false, false);
        dkCmdBufPushData(cmd_, dkMemBlockGetGpuAddr(dataMem_) + imageSet_ + t.texture[i].id * sizeof d, &d, sizeof d);
    }
    smallCount_++;
    for (int i = 0; i < 2; i++) {   // both empty to start with
        t.cur = i;
        bound_ = -1;
        BindTarget(2 + (smallCount_ - 1));
        DkScissor all = {0, 0, t.width * kSmallScale, t.height * kSmallScale};
        dkCmdBufSetScissors(cmd_, 0, &all, 1);
        float none[4] = {0, 0, 0, 0};
        dkCmdBufClearColor(cmd_, 0, DkColorMask_RGBA, none);
    }
    t.cur = 0;
    bound_ = -1;
    last_.any = false;
    return 2 + (smallCount_ - 1);
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
    FlushPending();   // what was drawn before, with the textures as they were
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
        if (at + size > kTextureMemory || nextTexture_ == kMaxTextures - 2 - 2 * kSmallTargets) {   // the last are the frame buffers' and the small targets'
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
    std::uint64_t t0 = Tick();
    texture_ = nullptr;
    if (s.tme && TargetFor(s) >= 0) {
        const std::uint32_t tbp = (std::uint32_t)(s.tex0 & 0x3FFF);
        // A buffer the game drew into is the game's to use for textures again (another level's): an
        // upload into it since it was last drawn into ends that. The small target is retired (its
        // images kept for the next buffer of its size), and the texture is the uploaded one.
        for (int k = 0; k < smallCount_ && !texture_; k++) {   // a small target the game drew into, read as a texture
            Small& t = small_[k];
            if (t.fbp * 32 != tbp || t.fbp == kRetired)
                continue;
            if (gs::BufferGeneration(t.fbp, t.width / 64, t.width, t.height) > t.serial) {
                FlushPending();
                t.fbp = kRetired;
                retired_++;
            } else {
                texture_ = &t.texture[0];   // which of its two images is decided when the run is drawn
            }
        }
        bool other = false;   // a buffer this renderer does not draw into: nothing to show of it
        if (!texture_ && tbp % 32 == 0) {
            std::uint32_t& serial = otherSerial_[(tbp / 32) & 0x1FF];
            if (serial != 0 && gs::BufferGeneration(tbp / 32, 1, 64, 64) > serial)
                serial = 0;
            other = serial != 0;
        }
        if (!texture_ && !other && tbp != 0 && tbp != 70 * 32)
            texture_ = TextureFor(s);
    }
    textureNs_ += Tick() - t0;
}

void Renderer::Draw(const gs::DrawState& s, const gs::DrawVertex* v, std::size_t count, const std::uint32_t* index,
                    std::size_t indices)
{
    static const bool noMerge = std::getenv("SLY1_GPU_NO_MERGE") != nullptr;
    if (noMerge || !index || indices == 0) {
        FlushPending();
        Source src = {v, (std::uint32_t)(count * sizeof(gs::DrawVertex)), sizeof(gs::DrawVertex), index, (std::uint32_t)indices, nullptr};
        Submit(s, src);
        return;
    }
    const gs::DrawState& p = pendState_;
    // A run that can change nothing is left out: the same vertices as the run before, which wrote
    // its depth wherever it drew, drawn again for the depth alone (ATST NEVER, AFAIL ZB_ONLY) under
    // the same test, GREATER, which the depth just written does not pass. The game draws each line
    // of a character's outline twice so (the second for the pixels the GS's antialiasing leaves
    // without depth, which is not done here).
    if (pending_ && s.kind == p.kind && s.frame == p.frame && s.zbuf == p.zbuf && s.scissor == p.scissor && !(s.zbuf >> 32 & 1) &&
        (s.test & 0x7F00F) == 0x72001 && (p.test & 0x74001) == 0x70000 && count == lastCount_ && indices == lastIndices_) {
        const gs::DrawVertex* was = pendVertices_.data() + pendVertices_.size() - count;
        const std::uint32_t* wasIndex = pendIndex_.data() + pendIndex_.size() - indices;
        const std::uint32_t base = (std::uint32_t)(pendVertices_.size() - count);
        bool equal = true;
        for (std::size_t k = 0; k < count && equal; k++)
            equal = v[k].x == was[k].x && v[k].y == was[k].y && v[k].z == was[k].z;
        for (std::size_t k = 0; k < indices && equal; k++)
            equal = index[k] + base == wasIndex[k];
        if (equal) {
            dropped_++;
            return;
        }
    }
    const bool same = pending_ && texture_ == pendTexture_ && pendVertices_.size() + count <= 32768 && s.kind == p.kind &&
                      s.tme == p.tme && s.fge == p.fge && s.abe == p.abe && s.fba == p.fba && s.tex0 == p.tex0 && s.tex1 == p.tex1 &&
                      s.clamp == p.clamp && s.alpha == p.alpha && s.test == p.test && s.frame == p.frame && s.zbuf == p.zbuf &&
                      s.scissor == p.scissor && s.texa == p.texa && s.fogcol == p.fogcol;
    if (!same) {
        FlushPending();
        pendState_ = s;
        pendTexture_ = texture_;
        pendVertices_.clear();
        pendIndex_.clear();
        pending_ = true;
    } else {
        merged_++;
    }
    lastCount_ = count;
    lastIndices_ = indices;
    const std::uint32_t base = (std::uint32_t)pendVertices_.size();
    pendVertices_.insert(pendVertices_.end(), v, v + count);
    const std::size_t at = pendIndex_.size();
    pendIndex_.resize(at + indices);
    for (std::size_t k = 0; k < indices; k++)
        pendIndex_[at + k] = index[k] + base;
}

// The run held back, drawn: before anything else that goes to the GPU (a batch, a texture's
// upload, the frame's end), so that everything is drawn in the game's order.
void Renderer::FlushPending()
{
    if (!pending_)
        return;
    pending_ = false;
    const Texture* now = texture_;
    texture_ = pendTexture_;
    Source src = {pendVertices_.data(), (std::uint32_t)(pendVertices_.size() * sizeof(gs::DrawVertex)), sizeof(gs::DrawVertex),
                  pendIndex_.data(), (std::uint32_t)pendIndex_.size(), nullptr};
    Submit(pendState_, src);
    texture_ = now;
}

// A batch's strip as triangles: one for each vertex from the third that does not start a strip,
// wound the same way round along the strip (the program's backface test alternates), the vertex
// that closes it last (its control word says whether a backface is dropped).
bool Renderer::DrawBatch(const gs::DrawState& s, const gs::Batch& b)
{
    FlushPending();
    static const bool asRuns = std::getenv("SLY1_GPU_BATCH_AS_RUNS") != nullptr;
    if (asRuns) {   // diagnosis: the batch made the GS's vertices here and drawn the GS's way (nothing cut:
                    // a triangle with a vertex outside the view is left out)
        std::vector<gs::DrawVertex> v(b.count);
        std::vector<bool> inside(b.count);
        for (std::uint32_t k = 0; k < b.count; k++) {
            gs::BatchVertex bv = gs::VertexOf(b, k);
            float w = bv.clip[3];
            inside[k] = w > 0.0f && std::fabs(bv.clip[0]) <= w && std::fabs(bv.clip[1]) <= w && std::fabs(bv.clip[2]) <= w;
            const std::uint32_t* q = b.quads + k * b.stride * 4;
            gs::DrawVertex& o = v[k];
            o.x = (bv.x - b.ofx) / 16.0f;
            o.y = (bv.y - b.ofy) / 16.0f;
            o.z = inside[k] ? (std::uint32_t)bv.z : 0;
            const std::uint32_t* c = q + b.colourAt * 4;
            o.r = (std::uint8_t)c[0]; o.g = (std::uint8_t)c[1]; o.b = (std::uint8_t)c[2]; o.a = (std::uint8_t)c[3];
            float stq[3];
            std::memcpy(stq, q + b.stqAt * 4, 12);
            float over = w != 0.0f ? 1.0f / w : 0.0f;
            o.s = stq[0] * over; o.t = stq[1] * over; o.q = stq[2] * over;
            o.fog = bv.fog;
        }
        batchIndex_.clear();
        for (std::uint32_t k = 2; k < b.count; k++) {
            if ((b.quads[(k * b.stride + b.positionAt) * 4 + 3] & gs::kStripStart) || !inside[k] || !inside[k - 1] || !inside[k - 2])
                continue;
            batchIndex_.push_back(k - 2);
            batchIndex_.push_back(k - 1);
            batchIndex_.push_back(k);
        }
        batches_++;
        if (batchIndex_.empty())
            return true;
        Source src = {v.data(), (std::uint32_t)(v.size() * sizeof(gs::DrawVertex)), sizeof(gs::DrawVertex), batchIndex_.data(),
                      (std::uint32_t)batchIndex_.size(), nullptr};
        Submit(s, src);
        return true;
    }
    if (recording_ && !replaying_) {   // SLY1_GPU_REPLAY: the batch kept with its vertices as they are
        Recorded rec;
        rec.s = s;
        const std::uint8_t* q = reinterpret_cast<const std::uint8_t*>(b.quads);
        rec.vertices.assign(q, q + (std::size_t)b.count * b.stride * 16);
        rec.vertexSize = b.stride * 16;
        rec.isBatch = true;
        rec.batch = b;
        rec.batch.id = 0;
        rec.texture = texture_;
        record_.push_back(std::move(rec));
    }
    if (TargetFor(s) < 0) {
        otherTargets_++;
        otherSerial_[(s.frame & 0x1FF)] = gs::UploadSerial();
        return true;
    }
    batches_++;
    batchVertices_ += b.count;
    // The batch's positions and STQ, and its triangles' indices, put in the frame's memory at its
    // first kick; its later kicks (the second pass's), with the same id, add only their colours.
    BatchCache& c = batchCache_;
    bool same = b.id != 0 && b.id == c.id && c.sends == sends_ && c.quads == b.quads && c.count == b.count &&
                c.stride == b.stride && c.positionAt == b.positionAt && c.stqAt == b.stqAt;
    const std::uint32_t colourBytes = b.count * 16;
    std::uint32_t need = colourBytes + 64 + (same ? 0 : b.count * 32 + 64 + b.count * 12);
    if (frameUsed_ + need > kFrameMemory || draws_ >= kMaxDraws) {
        Send(true);
        same = false;
        need = colourBytes + 64 + b.count * 32 + 64 + b.count * 12;
        if (need > kFrameMemory)
            return true;
    }
    std::uint8_t* mem = static_cast<std::uint8_t*>(dkMemBlockGetCpuAddr(frameMem_));
    if (!same) {
        std::uint64_t t0 = Tick();
        // the triangles: one for each vertex from the third that does not start a strip, wound the
        // same way round along the strip, the vertex that closes it last
        // made in cached memory first and copied in one piece: the frame's memory is uncached, where
        // small writes one by one cost far more than a copy
        frameUsed_ = Align(frameUsed_, 4);
        staging_.resize((std::size_t)b.count * 3 + 8);
        std::uint32_t* index = staging_.data();
        std::uint32_t n = 0;
        const std::uint32_t controlAt = b.positionAt * 4 + 3, step = b.stride * 4;
        for (std::uint32_t k = 2; k < b.count; k++) {
            if (b.quads[k * step + controlAt] & gs::kStripStart)
                continue;
            index[n] = (k & 1) ? k - 2 : k - 1;
            index[n + 1] = (k & 1) ? k - 1 : k - 2;
            index[n + 2] = k;
            n += 3;
        }
        c.indexAt = frameUsed_;
        c.indices = n;
        std::memcpy(mem + frameUsed_, index, (std::size_t)n * 4);
        frameUsed_ += n * 4;
        indexNs_ += Tick() - t0;
        std::uint64_t c0 = Tick();
        frameUsed_ = Align(frameUsed_, 64);
        c.vertexAt = frameUsed_;
        staging_.resize((std::size_t)b.count * 8);
        std::uint8_t* out = reinterpret_cast<std::uint8_t*>(staging_.data());
        for (std::uint32_t k = 0; k < b.count; k++, out += 32) {
            const std::uint32_t* v = b.quads + k * step;
            std::memcpy(out, v + b.positionAt * 4, 16);
            std::memcpy(out + 16, v + b.stqAt * 4, 16);
        }
        std::memcpy(mem + frameUsed_, staging_.data(), (std::size_t)b.count * 32);
        frameUsed_ += b.count * 32;
        copiedBytes_ += b.count * 32 + n * 4;
        copyNs_ += Tick() - c0;
        c.id = b.id;
        c.sends = sends_;
        c.quads = b.quads;
        c.count = b.count;
        c.stride = b.stride;
        c.positionAt = b.positionAt;
        c.stqAt = b.stqAt;
        if (b.id == 0)
            c.id = 0;   // nothing to share with the next kick
    }
    if (c.indices == 0)
        return true;
    std::uint64_t c0 = Tick();
    frameUsed_ = Align(frameUsed_, 64);
    std::uint32_t colourAt = frameUsed_;
    staging_.resize((std::size_t)b.count * 4);
    std::uint32_t* colours = staging_.data();
    for (std::uint32_t k = 0; k < b.count; k++)
        std::memcpy(colours + k * 4, b.quads + k * b.stride * 4 + b.colourAt * 4, 16);
    std::memcpy(mem + frameUsed_, colours, colourBytes);
    frameUsed_ += colourBytes;
    copiedBytes_ += colourBytes;
    copyNs_ += Tick() - c0;
    Source src = {nullptr, 0, 32, nullptr, c.indices, &b};
    src.placed = true;
    src.vertexAt = c.vertexAt;
    src.colourAt = colourAt;
    src.indexAt = c.indexAt;
    Submit(s, src);
    return true;
}

void Renderer::Submit(const gs::DrawState& s, const Source& src)
{
    const std::uint32_t indices = src.indices;
    struct Timed {
        std::uint64_t& total;
        std::uint64_t start = Tick();
        ~Timed() { total += Tick() - start; }
    } timed{drawNs_};
    int target = TargetFor(s);
    if (target < 0) {
        otherTargets_++;
        otherSerial_[(s.frame & 0x1FF)] = gs::UploadSerial();   // a buffer the game draws into and this renderer does not
        return;
    }
    // A small target (a character's picture, laid on the ground afterwards as its shadow): drawn
    // into like a frame buffer; as a texture, its image is bound in place of a decoded one, after a
    // barrier when it has been drawn into since. A run that draws into the buffer it reads (the
    // game softens the picture's edge so) is left out: the picture keeps its hard edge.
    if (target >= 2) {
        small_[target - 2].drawn = true;
        small_[target - 2].serial = gs::UploadSerial();
        smallRuns_++;
        if (std::getenv("SLY1_GPU_SHADOW_TRACE") && frames_ > 500 && smallIntoTrace_ < 14) {
            smallIntoTrace_++;
            std::fprintf(stderr, "gpuinto: frame %llu: %s %u indices kind %u tme %u abe %u alpha %llX test %llX fba %u frame %llX", (unsigned long long)frames_, src.batch ? "batch" : "run",
                         (unsigned)src.indices, s.kind, s.tme, s.abe, (unsigned long long)s.alpha, (unsigned long long)s.test, s.fba, (unsigned long long)s.frame);
            if (src.batch) {
                const std::uint32_t* c = src.batch->quads + src.batch->colourAt * 4;
                std::fprintf(stderr, " | batch of %u, first colour %u %u %u %u", src.batch->count, c[0], c[1], c[2], c[3]);
            } else if (src.vertices) {
                const gs::DrawVertex* dv = static_cast<const gs::DrawVertex*>(src.vertices);
                std::fprintf(stderr, " | first vertex (%.1f,%.1f) rgba %02X%02X%02X%02X", dv[0].x, dv[0].y, dv[0].r, dv[0].g, dv[0].b, dv[0].a);
            }
            std::fprintf(stderr, "\n");
        }
    }
    const Texture* smallTexture = nullptr;
    for (int k = 0; k < smallCount_; k++)
        if (texture_ == &small_[k].texture[0]) {
            Small& t = small_[k];
            if (target == 2 + k && !inSelf_) {
                // A run that reads the buffer it draws into: the game's blur of a shadow's picture,
                // sprites over the whole buffer that copy it onto itself half a texel aside with the
                // bilinear filter, six in a run. Each is drawn by itself, from the current image into
                // the other, which is then the current one. (Any other such run is left out.)
                smallSelf_++;
                if (s.abe || s.kind != 2 || src.batch || !src.index || src.indices == 0 || src.indices % 6 != 0)
                    return;
                inSelf_ = true;
                for (std::uint32_t at = 0; at < src.indices; at += 6) {
                    Source step = src;
                    step.index = src.index + at;
                    step.indices = 6;
                    bound_ = -1;
                    Submit(s, step);
                    t.cur ^= 1;
                    t.drawn = true;
                    smallSteps_++;
                }
                inSelf_ = false;
                bound_ = -1;
                return;
            }
            if (t.drawn) {
                dkCmdBufBarrier(cmd_, DkBarrier_Fragments, DkInvalidateFlags_Image);
                t.drawn = false;
            }
            smallTexture = &t.texture[t.cur];
            smallTextured_++;
        }
    // The game's motion blur: each frame ends with a sprite textured from the other frame buffer
    // (the frame before), blended over the new one at FIX 0x30. A frame buffer is not a texture
    // here (its image is the GPU's, not the GS's memory the textures are decoded from), so the
    // sprite drew that memory's black at 37.5% over every frame: a picture darker than retail's
    // throughout. Left out: the port has no motion blur (a setting, off, if it ever has).
    if (s.tme) {
        const std::uint32_t tbp = (std::uint32_t)(s.tex0 & 0x3FFF);
        if (tbp == 0 || tbp == 70 * 32) {
            frameTextured_++;
            static const bool keep = std::getenv("SLY1_GPU_KEEP_BLUR") != nullptr;   // as before, to compare
            if (!keep)
                return;
        }
    }
    {   // SLY1_GPU_SKIP: kinds of draw left out, to find which one a fault comes with (bits: 1 batches,
        // 2 the GS's untextured, 4 the GS's textured, 8 depth-only, 16 the GS's lines and points)
        static const int skip = std::getenv("SLY1_GPU_SKIP") ? std::atoi(std::getenv("SLY1_GPU_SKIP")) : 0;
        std::uint32_t t = (std::uint32_t)s.test;
        bool depthOnly = (t & 1) && ((t >> 1) & 7) == 0 && ((t >> 12) & 3) == 2;
        if ((skip & 1 && src.batch) || (skip & 2 && !src.batch && !s.tme && !depthOnly) || (skip & 4 && !src.batch && s.tme) ||
            (skip & 8 && depthOnly && s.kind == 2 && ((t >> 17) & 3) != 1) || (skip & 16 && !src.batch && s.kind != 2))
            return;
    }
    {   // SLY1_GPU_STOP_AFTER=n: in the captured frame, only its first n draws
        static const long stopAfter = std::getenv("SLY1_GPU_STOP_AFTER") ? std::atol(std::getenv("SLY1_GPU_STOP_AFTER")) : -1;
        static const long omit = std::getenv("SLY1_GPU_OMIT") ? std::atol(std::getenv("SLY1_GPU_OMIT")) : -1;
        if (omit >= 0 && stopAfter < 0 && captureAt_ >= 0 && (long)frames_ + 1 == captureAt_ && ++drawInFrame_ == omit)
            return;   // SLY1_GPU_OMIT=n: the captured frame without its n-th draw
        if (stopAfter >= 0 && captureAt_ >= 0 && (long)frames_ + 1 == captureAt_) {
            if (drawInFrame_++ >= stopAfter && stopped_)
                return;
            paintLast_ = drawInFrame_ == stopAfter && std::getenv("SLY1_GPU_PAINT_LAST") != nullptr;
            if (drawInFrame_ == stopAfter)
                std::fprintf(stderr, "gpustop: the last draw kept: %s, %u indices, test %08X, zbuf mask %u, abe %u, tme %u, kind %u\n",
                             src.batch ? "batch" : "run", indices, (unsigned)s.test, (unsigned)((s.zbuf >> 32) & 1), s.abe, s.tme, s.kind);
            if (drawInFrame_ == stopAfter && src.batch) {
                const gs::Batch& b = *src.batch;
                std::fprintf(stderr, "gpustop: alpha %016llX tex0 %016llX fge %u fogcol %06llX; count %u stride %u at %u/%u/%u cull %d fog %g wMax %g\n",
                             (unsigned long long)s.alpha, (unsigned long long)s.tex0, s.fge, (unsigned long long)s.fogcol, b.count, b.stride,
                             b.positionAt, b.stqAt, b.colourAt, (int)b.cull, b.fogScale, b.wMax);
                std::fprintf(stderr, "gpustop: scale %g %g %g offset %g %g %g\n", b.scale[0], b.scale[1], b.scale[2], b.offset[0], b.offset[1], b.offset[2]);
                for (std::uint32_t k = 0; k < b.count && k < 14; k++) {
                    const std::uint32_t* q = b.quads + k * b.stride * 4;
                    gs::BatchVertex v = gs::VertexOf(b, k);
                    const float* p = (const float*)(q + b.positionAt * 4);
                    const std::uint32_t* c = q + b.colourAt * 4;
                    std::fprintf(stderr, "gpustop: v%u pos %g %g %g ctl %08X colour %X %X %X %X clip %g %g %g %g -> x %g y %g z %g fog %g\n", k, p[0], p[1],
                                 p[2], q[b.positionAt * 4 + 3], c[0], c[1], c[2], c[3], v.clip[0], v.clip[1], v.clip[2], v.clip[3], v.x / 16 - b.ofx / 16,
                                 v.y / 16 - b.ofy / 16, v.z, v.fog);
                }
            }
        }
    }
    // SLY1_GPU_AREA=1: how much of the frame the GS's runs cover, in screens, each frame (triangles
    // with their corners brought inside the picture: near enough), by kind of run.
    static const bool area = std::getenv("SLY1_GPU_AREA") != nullptr;
    if (area && !src.batch && src.vertices && src.index && s.kind == 2 && target < 2) {
        const gs::DrawVertex* v = static_cast<const gs::DrawVertex*>(src.vertices);
        double sum = 0;
        for (std::uint32_t k = 0; k + 2 < src.indices; k += 3) {
            float x[3], y[3];
            for (int c = 0; c < 3; c++) {
                x[c] = std::min(std::max(v[src.index[k + c]].x, 0.0f), kFieldWidth);
                y[c] = std::min(std::max(v[src.index[k + c]].y, 0.0f), kFieldHeight);
            }
            sum += std::fabs((x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0])) * 0.5;
        }
        const bool date = (s.test >> 14) & 1;
        int kind = date ? 3 : !s.abe ? 0 : s.tme ? 1 : 2;
        area_[kind] += sum / (kFieldWidth * kFieldHeight);
        areaRuns_[kind]++;
    }
    allDraws_++;
    if (recording_ && !replaying_ && !src.placed) {   // SLY1_GPU_REPLAY: the frame's draws kept as they are submitted (batches: DrawBatch)
        Recorded rec;
        rec.s = s;
        rec.vertices.assign((const std::uint8_t*)src.vertices, (const std::uint8_t*)src.vertices + src.vertexBytes);
        rec.vertexSize = src.vertexSize;
        rec.index.assign(src.index, src.index + src.indices);
        rec.isBatch = src.batch != nullptr;
        if (rec.isBatch)
            rec.batch = *src.batch;
        rec.texture = texture_;
        record_.push_back(std::move(rec));
    }
    std::uint32_t bytes = src.vertexBytes, indexBytes = indices * 4;
    if (!src.placed) {
        if (bytes + indexBytes + 256 > kFrameMemory)
            return;
        if (frameUsed_ + bytes + indexBytes + 256 > kFrameMemory || draws_ >= kMaxDraws)
            Send(true);
    }
    const Texture* texture = smallTexture ? smallTexture : texture_;
    bool fresh = !last_.any;
    last_.any = true;
    if (listAt_ >= 0 && (long)frames_ + 1 == listAt_ && listed_ < 60) {   // SLY1_GPU_LIST=N: frame N's first draws
        listed_++;
        std::uint32_t t = (std::uint32_t)s.test;
        static const float none[2] = {0, 0};
        const float* p = src.vertices ? (const float*)src.vertices : none;
        std::fprintf(stderr, "gpulist: %s %u indices, %u bytes at %u, target %d, fresh %d, ZTST %u ZMSK %u ATE %u ATST %u AFAIL %u ABE %u "
                     "tme %u scissor %016llX first %g %g %08X\n", src.batch ? "batch" : "run", indices, src.vertexBytes, frameUsed_,
                     target, (int)!last_.any, (t >> 17) & 3, (unsigned)((s.zbuf >> 32) & 1), t & 1, (t >> 1) & 7, (t >> 12) & 3,
                     s.abe, s.tme, (unsigned long long)s.scissor, p[0], p[1], src.vertices ? ((const std::uint32_t*)src.vertices)[2] : 0u);
    }
    BindTarget(target);
    BindSource(src);
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
    // DATE: the pixel shader reads the frame's alpha under each pixel and drops the pixel when its
    // top bit is not DATM (gs_fsh.glsl). The frame buffer is the target and a texture at once for
    // such a run: what was drawn before it must be in the image when it is read, so a barrier comes
    // first when a run without DATE has come since. (A copy of the bit to a stencil was tried
    // first: right, but a depth buffer with a stencil is twice the size, and a dozen copies a frame
    // took the Switch from 24 to 17 frames a second.) SLY1_GPU_NO_DATE leaves the test out.
    static const bool noDate = std::getenv("SLY1_GPU_NO_DATE") != nullptr;
    const bool dateRun = date && !noDate && target < 2 && ab_ != 2;
    if ((ab_ == 3 && src.batch) || (ab_ == 4 && !src.batch && target < 2))
        return;
    if (date)
        destinationAlpha_++;
    if (dateRun) {
        const std::uint32_t id = kMaxTextures - 2 + (std::uint32_t)target;
        if (frameBound_ != target) {
            DkImageView view;
            dkImageViewDefaults(&view, &colour_[target]);
            DkImageDescriptor d;
            dkImageDescriptorInitialize(&d, &view, false, false);
            dkCmdBufPushData(cmd_, dkMemBlockGetGpuAddr(dataMem_) + imageSet_ + id * sizeof d, &d, sizeof d);
            DkResHandle handle = dkMakeTextureHandle(id, 3);
            dkCmdBufBindTextures(cmd_, DkStage_Fragment, 1, &handle, 1);
            frameBound_ = target;
        }
        if (!frameRead_) {
            dkCmdBufBarrier(cmd_, DkBarrier_Fragments, DkInvalidateFlags_Image);
            dateBarriers_++;
            frameRead_ = true;
        }
    } else {
        frameRead_ = false;
    }
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
    fp.coef[0] = 99.0f;   // without a blend, or with the blend state's factors
    if (s.abe) {
        std::uint32_t a = s.alpha & 3, b = (s.alpha >> 2) & 3, c = (s.alpha >> 4) & 3, d = (s.alpha >> 6) & 3;
        fp.blend[0] = c == 0 ? 1.0f : 0.0f;
        fp.blend[1] = ((s.alpha >> 32) & 0xFF) / 128.0f;
        bool frameAlpha = c == 1;
        // each of Cs and Cd: how many times C (-1, 0, 1) and how many times 1 (0, 1)
        int sc = (a == 0) - (b == 0), s1 = d == 0, dc = (a == 1) - (b == 1), d1 = d == 1;
        if (!frameAlpha && target >= 2) {
            // A small target (a shadow's picture): the coefficients go to the pixel shader, which gives
            // the colour times its own and one less the frame's as the second output, its alpha 1
            // (gs_fsh.glsl), so that the alpha written is the source's, as on the GS. With the blend
            // state's factors the GPU gives the frame's alpha the colour's factor whatever the state's
            // own for alpha: the game's clear of the picture (alpha 0, blend 0x44) left what was
            // there, and each frame's outline piled on the last's until the shadow was solid.
            fp.coef[0] = (float)sc;
            fp.coef[1] = (float)s1;
            fp.coef[2] = (float)dc;
            fp.coef[3] = (float)d1;
            bool sneg = sc < 0 && !s1, dneg = dc < 0 && !d1;
            if ((sc > 0 && s1) || (dc > 0 && d1))
                unsupportedBlends_++;   // 1 + C: as 1
            blend.srcColorBlendFactor = DkBlendFactor_One;
            blend.dstColorBlendFactor = DkBlendFactor_InvSrc1Color;
            blend.colorBlendOp = sneg ? DkBlendOp_RevSub : dneg ? DkBlendOp_Sub : DkBlendOp_Add;
            if (sneg && dneg)
                unsupportedBlends_++;
        } else {
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
    }
    if (fresh || std::memcmp(&blend, &last_.blend, sizeof blend) != 0) {
        dkCmdBufBindBlendStates(cmd_, 0, &blend, 1);
        last_.blend = blend;
    }

    fp.blend[2] = cullFront_;
    // SLY1_GPU_CAPTURE's views: 1 a batch's triangles that reach outside the program's clip volume
    // left out, 2 those triangles red and the others green, 3 depths cut and not clamped, 4 the depth-only draws (the game's clear of Z)
    // flat-coloured, 5 the frame's first three batches flat-coloured without the depth test
    bool always = false, shown = false;
    int fsView = 0;
    if (src.batch) {
        batchInFrame_++;
        always = view_ == 5 && batchInFrame_ <= 3;
        fsView = always ? 6 : view_ == 1 ? 8 : view_ == 2 ? 9 : 0;
    } else if (view_ == 4 && ate && atst == 0 && afail == 2) {
        shown = true;
        fsView = 6;
    }
    if (paintLast_) {   // SLY1_GPU_STOP_AFTER's last draw flat-coloured, whatever its depth
        always = true;
        fsView = 6;
    }
    fp.blend[3] = (float)fsView;
    {   // the pixel shader: the short one where the run needs no more (gs_lite_fsh.glsl)
        static const bool noLite = std::getenv("SLY1_GPU_NO_LITE") != nullptr;
        const bool lite = !noLite && ab_ != 1 && !src.batch && !dateRun && fsView == 0 && view_ == 0 && fp.coef[0] == 99.0f;
        if (lite != liteBound_) {
            const DkShader* shader[1] = {lite ? &liteShader_ : &fragmentShader_};
            dkCmdBufBindShaders(cmd_, DkStageFlag_Fragment, shader, 1);
            liteBound_ = lite;
        }
        liteRuns_ += lite ? 1 : 0;
    }
    fp.mode[0] = s.tme;
    fp.mode[1] = (std::int32_t)((s.tex0 >> 35) & 3);
    fp.mode[2] = (std::int32_t)((s.tex0 >> 34) & 1);
    fp.mode[3] = s.fge;
    fp.fogColour[0] = (float)(s.fogcol & 0xFF);
    fp.fogColour[1] = (float)((s.fogcol >> 8) & 0xFF);
    fp.fogColour[2] = (float)((s.fogcol >> 16) & 0xFF);
    fp.fogColour[3] = dateRun ? (((test >> 15) & 1) ? 1.0f : 2.0f) : 0.0f;
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
        float sx = imageWidth_ / fieldWidth_, sy = imageHeight_ / fieldHeight_;
        const std::uint32_t kWidth = imageWidth_, kHeight = imageHeight_;   // the bound target's, in this block
        std::uint32_t x0 = (std::uint32_t)(s.scissor & 0x7FF), x1 = (std::uint32_t)((s.scissor >> 16) & 0x7FF) + 1,
                      y0 = (std::uint32_t)((s.scissor >> 32) & 0x7FF), y1 = (std::uint32_t)((s.scissor >> 48) & 0x7FF) + 1;
        DkScissor sc;
        sc.x = std::min((std::uint32_t)(x0 * sx), kWidth);
        sc.y = std::min((std::uint32_t)(y0 * sy), kHeight);
        sc.width = std::min((std::uint32_t)(x1 * sx + 0.5f), kWidth) - sc.x;
        sc.height = std::min((std::uint32_t)(y1 * sy + 0.5f), kHeight) - sc.y;
        if (ab_ == 5) {
            sc.width /= 4;
            sc.height /= 4;
        }
        if (fresh || std::memcmp(&sc, &last_.scissor, sizeof sc) != 0) {
            dkCmdBufSetScissors(cmd_, 0, &sc, 1);
            last_.scissor = sc;
        }
    }
    if (fresh)
        last_.colour = last_.write = last_.depth = last_.aref = ~0u;

    std::uint32_t first = 0, firstIndex = 0;
    std::uint64_t c0 = Tick();
    if (src.placed) {   // the batch's two streams where DrawBatch put them
        DkGpuAddr base = dkMemBlockGetGpuAddr(frameMem_);
        const std::uint32_t count = src.batch->count;   // the streams' sizes exactly (an emulator tracks what is bound)
        DkBufExtents streams[2] = {{base + src.vertexAt, count * 32}, {base + src.colourAt, count * 16}};
        dkCmdBufBindVtxBuffers(cmd_, 0, streams, 2);
        firstIndex = src.indexAt / 4;
    } else {
    frameUsed_ = (frameUsed_ + src.vertexSize - 1) / src.vertexSize * src.vertexSize;
    first = frameUsed_ / src.vertexSize;
    std::memcpy((std::uint8_t*)dkMemBlockGetCpuAddr(frameMem_) + frameUsed_, src.vertices, bytes);
    frameUsed_ += bytes;
    copiedBytes_ += bytes + indexBytes;
    if (captureAt_ >= 0 && (long)frames_ + 1 == captureAt_ && drawInFrame_ >= 2796 && drawInFrame_ <= 2803)
        std::fprintf(stderr, "gpustop: draw %ld: %s, vertices %u bytes of %u each at %u (first %u), %u indices at %u, sends %llu, texture %d\n",
                     drawInFrame_, src.batch ? "batch" : "run", bytes, src.vertexSize, frameUsed_ - bytes, first, indices, frameUsed_,
                     (unsigned long long)sends_, texture ? (int)texture->id : -1);
    if (captureAt_ >= 0 && (long)frames_ + 1 == captureAt_ && (drawInFrame_ <= 2 || (drawInFrame_ >= 2796 && drawInFrame_ <= 2803) || drawInFrame_ % 500 == 0))
        std::fprintf(stderr, "gpustop: draw %ld target %d frame %016llX test %08X zbuf %llX\n", drawInFrame_, target, (unsigned long long)s.frame,
                     (unsigned)s.test, (unsigned long long)s.zbuf);
    firstIndex = frameUsed_ / 4;
    std::memcpy((std::uint8_t*)dkMemBlockGetCpuAddr(frameMem_) + frameUsed_, src.index, indexBytes);
    frameUsed_ = Align(frameUsed_ + indexBytes, 4);
    }
    copyNs_ += Tick() - c0;
    static const DkPrimitive kPrimitive[3] = {DkPrimitive_Points, DkPrimitive_Lines, DkPrimitive_Triangles};
    DkPrimitive primitive = kPrimitive[s.kind < 3 ? s.kind : 2];

    // one pass of the pixels an alpha comparison lets through, writing colour, alpha and depth or not
    auto pass = [&](DkCompareOp alphaOp, bool rgb, bool alpha, bool depthWrite) {
        DkColorState colour;
        dkColorStateDefaults(&colour);
        colour.blendEnableMask = s.abe ? 1 : 0;
        colour.alphaCompareOp = alphaOp;
        std::uint32_t colourKey = (colour.blendEnableMask ? 1u : 0u) | (std::uint32_t)alphaOp << 1;
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
        ds.depthTestEnable = target < 2;   // a small target has no depth buffer
        ds.depthWriteEnable = target < 2 && depthWrite && !zmsk;
        ds.depthCompareOp = !zte || ztst == 1 ? DkCompareOp_Always : ztst == 2 ? DkCompareOp_Gequal : DkCompareOp_Greater;
        if (always)
            ds.depthCompareOp = DkCompareOp_Always;
        std::uint32_t depthKey = (std::uint32_t)ds.depthWriteEnable | (std::uint32_t)ds.depthCompareOp << 1 | (target >= 2 ? 0x100u : 0u);
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
        pass(kFails[atst], shown, false, true);
    else if (afail == 3)
        pass(kFails[atst], true, false, false);
}

// The frame buffer's image read back, quartered each way and printed as lines of hexadecimal RGB
// (tools/gpu_capture.py makes a PNG of them).
// The top quarter of the frame's left two thirds (the sky, in the opening): its mean blue, 0 to 255.
double Renderer::SkyBlue(int target)
{
    DkImageView view;
    dkImageViewDefaults(&view, &colour_[target]);
    DkImageRect rect = {0, 0, 0, kWidth, kHeight, 1};
    DkCopyBuf to = {dkMemBlockGetGpuAddr(dataMem_) + capture_, 0, 0};
    dkCmdBufCopyImageToBuffer(cmd_, &view, &rect, &to, 0);
    Send(true);
    const std::uint8_t* p = (const std::uint8_t*)dkMemBlockGetCpuAddr(dataMem_) + capture_;
    double sum = 0;
    long n = 0;
    for (std::uint32_t y = 8; y < kHeight / 4; y += 4)
        for (std::uint32_t x = 8; x < kWidth * 2 / 3; x += 4, n++)
            sum += p[(y * kWidth + x) * 4 + 2];
    return n ? sum / n : 0;
}

void Renderer::Capture(int target)
{
    DkImageView view;
    dkImageViewDefaults(&view, &colour_[target]);
    DkImageRect rect = {0, 0, 0, kWidth, kHeight, 1};
    if (std::getenv("SLY1_GPU_CAPTURE_SMALL") && smallCount_ > 0) {   // the first small target, over the frame's top left corner
        DkImageView from;
        dkImageViewDefaults(&from, &small_[0].image[small_[0].cur]);
        DkImageRect src = {0, 0, 0, small_[0].width * kSmallScale, small_[0].height * kSmallScale, 1}, dst = {0, 0, 0, 512, 512, 1};
        dkCmdBufBlitImage(cmd_, &from, &src, &view, &dst, 0, 0);
    }
    DkCopyBuf to = {dkMemBlockGetGpuAddr(dataMem_) + capture_, 0, 0};
    dkCmdBufCopyImageToBuffer(cmd_, &view, &rect, &to, 0);
    Send(true);
    const std::uint8_t* p = (const std::uint8_t*)dkMemBlockGetCpuAddr(dataMem_) + capture_;
    std::uint32_t w = kWidth / 4, h = kHeight / 4;
    if (std::getenv("SLY1_GPU_CAPTURE_SMALL") && smallCount_ > 0) {   // the small target's alpha, counted (it is laid 8 times its size)
        long hist[9] = {};
        for (std::uint32_t y = 0; y < 64; y++)
            for (std::uint32_t x = 0; x < 64; x++)
                hist[std::min<std::uint32_t>(8, p[((y * 8 + 4) * kWidth + x * 8 + 4) * 4 + 3] / 16)]++;
        std::fprintf(stderr, "gpusmall: frame %llu: alpha 00-0F %ld, 10s %ld, 20s %ld, 30s %ld, 40s %ld, 50s %ld, 60s %ld, 70s %ld, 80+ %ld\n", (unsigned long long)frames_, hist[0], hist[1], hist[2],
                     hist[3], hist[4], hist[5], hist[6], hist[7], hist[8]);
    }
    static const bool full = std::getenv("SLY1_GPU_CAPTURE_FULL") != nullptr;   // every pixel, not the mean of each four by four
    if (full) {
        w = kWidth;
        h = kHeight;
    }
    // a row goes out 160 pixels a line: a longer line is cut by the emulator's log
    const std::uint32_t kChunk = 160;
    std::fprintf(stderr, "gpucap: begin %u %u frame %llu chunk %u\n", w, h, (unsigned long long)frames_, kChunk);
    std::vector<char> line(w * 6 + 1);
    static const char kHex[] = "0123456789abcdef";
    for (std::uint32_t y = 0; y < h; y++) {
        for (std::uint32_t x = 0; x < w; x++)
            for (int c = 0; c < 3; c++) {
                // SLY1_GPU_CAPTURE_ALPHA: the frame's alpha, red where its top bit is clear (0x60 to 0x7F
                // brighter), green where it is set
                static const bool alphaView = std::getenv("SLY1_GPU_CAPTURE_ALPHA") != nullptr;
                const std::uint8_t* q = p + ((y * 4 + 1) * kWidth + x * 4 + 1) * 4 + c;
                unsigned v = (q[0] + q[4] + q[kWidth * 4] + q[kWidth * 4 + 4] + 2) / 4;
                if (full)
                    v = p[(y * kWidth + x) * 4 + c];
                if (alphaView) {
                    const std::uint8_t al = (p + ((y * 4 + 1) * kWidth + x * 4 + 1) * 4)[3];
                    v = c == 0 ? (al < 0x60 ? 0x50 : al < 0x80 ? 0xFF : 0) : c == 1 ? (al >= 0x80 ? (al == 0x80 ? 0x80 : 0xFF) : 0) : 0;
                }
                line[(x * 3 + c) * 2] = kHex[v >> 4];
                line[(x * 3 + c) * 2 + 1] = kHex[v & 15];
            }
        for (std::uint32_t x = 0; x < w; x += kChunk) {
            const std::uint32_t n = std::min(kChunk, w - x);
            std::fprintf(stderr, "gpucap: %.*s\n", (int)(n * 6), line.data() + x * 6);
        }
    }
    std::fprintf(stderr, "gpucap: end\n");
}

void Renderer::Replay(long count, bool capture)
{
    count = std::max(0L, std::min(count, (long)record_.size()));
    int target = recordTarget_;
    BindTarget(target);
    DkScissor all = {0, 0, kWidth, kHeight};
    dkCmdBufSetScissors(cmd_, 0, &all, 1);
    DkColorWriteState write;
    dkColorWriteStateDefaults(&write);
    dkCmdBufBindColorWriteState(cmd_, &write);
    DkDepthStencilState ds;
    dkDepthStencilStateDefaults(&ds);
    ds.depthWriteEnable = 1;
    dkCmdBufBindDepthStencilState(cmd_, &ds);
    float grey[4] = {0.5f, 0.0f, 0.5f, 1.0f};   // purple: what nothing drew over
    dkCmdBufClearColor(cmd_, 0, DkColorMask_RGBA, grey);
    static const float startDepth = std::getenv("SLY1_GPU_REPLAY_DEPTH") ? (float)std::atof(std::getenv("SLY1_GPU_REPLAY_DEPTH")) : 0.0f;
    dkCmdBufClearDepthStencil(cmd_, true, startDepth, 0, 0);
    last_.any = false;
    replaying_ = true;
    for (long k = 0; k < count; k++) {
        Recorded& r = record_[(std::size_t)k];
        texture_ = r.texture;
        if (r.isBatch) {
            r.batch.quads = reinterpret_cast<const std::uint32_t*>(r.vertices.data());
            DrawBatch(r.s, r.batch);
            continue;
        }
        Source src = {r.vertices.data(), (std::uint32_t)r.vertices.size(), r.vertexSize, r.index.data(),
                      (std::uint32_t)r.index.size(), nullptr};
        Submit(r.s, src);
    }
    replaying_ = false;
    if (capture) {
        std::fprintf(stderr, "gpureplay: the first %ld of %zu draws\n", count, record_.size());
        Capture(target);
    }
}

// SLY1_GPU_REPLAY_SEARCH: the first draw after which the sky is dark, by halving: the frame replayed
// up to a count, its sky measured, until two counts a draw apart; that draw printed in full, and the
// frame captured just before and just after it.
void Renderer::Search()
{
    long n = (long)record_.size();
    long lo = 4, hi = n;
    Replay(lo, false);
    double bright = SkyBlue(recordTarget_);
    Replay(hi, false);
    double dark = SkyBlue(recordTarget_);
    std::fprintf(stderr, "gpusearch: the sky's blue %.1f after %ld draws, %.1f after all %ld\n", bright, lo, dark, n);
    if (!(dark < bright * 0.5)) {
        std::fprintf(stderr, "gpusearch: the frame's sky is not dark: nothing to look for\n");
        return;
    }
    double limit = bright * 0.5;
    while (hi - lo > 1) {
        long mid = (lo + hi) / 2;
        Replay(mid, false);
        double b = SkyBlue(recordTarget_);
        std::fprintf(stderr, "gpusearch: %ld draws: %.1f\n", mid, b);
        if (b >= limit)
            lo = mid;
        else
            hi = mid;
    }
    std::fprintf(stderr, "gpusearch: dark from draw %ld (index %ld)\n", hi, hi - 1);
    DumpDraw(hi - 1);
    if (hi >= 2)
        DumpDraw(hi - 2);
    Replay(lo, true);
    Replay(hi, true);
}

void Renderer::DumpDraw(long k)
{
    const Recorded& r = record_[(std::size_t)k];
    const gs::DrawState& s = r.s;
    std::fprintf(stderr, "gpusearch: draw %ld: %s, %zu indices, %zu vertex bytes of %u; kind %u tme %u fge %u abe %u; test %016llX "
                 "alpha %016llX frame %016llX zbuf %016llX scissor %016llX tex0 %016llX clamp %016llX\n", k,
                 r.isBatch ? "batch" : "run", r.index.size(), r.vertices.size(), r.vertexSize, s.kind, s.tme, s.fge, s.abe,
                 (unsigned long long)s.test, (unsigned long long)s.alpha, (unsigned long long)s.frame, (unsigned long long)s.zbuf,
                 (unsigned long long)s.scissor, (unsigned long long)s.tex0, (unsigned long long)s.clamp);
    if (r.isBatch) {
        gs::Batch b = r.batch;
        b.quads = reinterpret_cast<const std::uint32_t*>(r.vertices.data());
        std::fprintf(stderr, "gpusearch:   batch: count %u stride %u at %u/%u/%u cull %d fog %g wMax %g ofx %d ofy %d scale %g %g %g offset %g %g %g\n",
                     b.count, b.stride, b.positionAt, b.stqAt, b.colourAt, (int)b.cull, b.fogScale, b.wMax, b.ofx, b.ofy, b.scale[0],
                     b.scale[1], b.scale[2], b.offset[0], b.offset[1], b.offset[2]);
        std::fprintf(stderr, "gpusearch:   matrix %g %g %g %g / %g %g %g %g / %g %g %g %g / %g %g %g %g\n", b.matrix[0], b.matrix[1],
                     b.matrix[2], b.matrix[3], b.matrix[4], b.matrix[5], b.matrix[6], b.matrix[7], b.matrix[8], b.matrix[9], b.matrix[10],
                     b.matrix[11], b.matrix[12], b.matrix[13], b.matrix[14], b.matrix[15]);
        for (std::uint32_t v = 0; v < b.count && v < 24; v++) {
            const std::uint32_t* q = b.quads + v * b.stride * 4;
            const float* pos = reinterpret_cast<const float*>(q + b.positionAt * 4);
            const std::uint32_t* c = q + b.colourAt * 4;
            const float* stq = reinterpret_cast<const float*>(q + b.stqAt * 4);
            gs::BatchVertex bv = gs::VertexOf(b, v);
            std::fprintf(stderr, "gpusearch:   v%u pos %g %g %g ctl %08X stq %g %g %g colour %X %X %X %X clip %g %g %g %g screen %.1f %.1f z %.0f fog %.0f\n",
                         v, pos[0], pos[1], pos[2], q[b.positionAt * 4 + 3], stq[0], stq[1], stq[2], c[0], c[1], c[2], c[3], bv.clip[0],
                         bv.clip[1], bv.clip[2], bv.clip[3], (bv.x - b.ofx) / 16, (bv.y - b.ofy) / 16, bv.z, bv.fog);
        }
    } else {
        const gs::DrawVertex* v = reinterpret_cast<const gs::DrawVertex*>(r.vertices.data());
        std::size_t nv = r.vertices.size() / sizeof(gs::DrawVertex);
        for (std::size_t i = 0; i < nv && i < 24; i++)
            std::fprintf(stderr, "gpusearch:   v%zu %.1f %.1f z %u colour %u %u %u %u stq %g %g %g fog %g\n", i, v[i].x, v[i].y, v[i].z,
                         v[i].r, v[i].g, v[i].b, v[i].a, v[i].s, v[i].t, v[i].q, v[i].fog);
    }
}

void Renderer::Present(std::uint64_t dispfb, std::uint64_t)
{
    FlushPending();
    if (std::getenv("SLY1_GPU_AREA")) {
        if (frames_ % 20 == 0)
            std::fprintf(stderr, "gpuarea: frame %llu: opaque %.1f screens in %d runs, blended textured %.1f in %d, blended plain %.1f in %d, "
                         "with the destination alpha test %.1f in %d\n", (unsigned long long)frames_, area_[0], areaRuns_[0], area_[1], areaRuns_[1],
                         area_[2], areaRuns_[2], area_[3], areaRuns_[3]);
        for (int k = 0; k < 4; k++) {
            area_[k] = 0;
            areaRuns_[k] = 0;
        }
    }
    static const bool abAsked = std::getenv("SLY1_GPU_AB") != nullptr;
    abOn_ = abAsked;
    {
        static const long replayHeavy = std::getenv("SLY1_GPU_REPLAY") ? std::atol(std::getenv("SLY1_GPU_REPLAY")) : 0;
        if (replayHeavy > 0) {
            if (replayState_ == 0 && allDraws_ >= replayHeavy) {
                replayState_ = 1;   // the next frame is recorded
                recording_ = true;
                record_.clear();
            } else if (replayState_ == 1) {
                recording_ = false;
                replayState_ = 2;
                recordTarget_ = record_.empty() ? 0 : TargetOf(record_.back().s.frame & 0x1FF);   // the buffer the frame drew into
                long n = (long)record_.size();
                std::fprintf(stderr, "gpureplay: recorded %ld draws; the frame as drawn live:\n", n);
                Capture(recordTarget_);
                if (const char* at = std::getenv("SLY1_GPU_REPLAY_AT")) {
                    for (const char* p = at; *p;) {
                        long v = std::strtol(p, const_cast<char**>(&p), 10);
                        replayAt_.push_back(v < 0 ? n + v : v);
                        while (*p == ',') p++;
                    }
                } else {
                    for (int k = 0; k <= 8; k++)
                        replayAt_.push_back(n * k / 8);
                }
            } else if (replayState_ == 2 && std::getenv("SLY1_GPU_REPLAY_SEARCH")) {
                Search();
                replayState_ = 3;
                std::fprintf(stderr, "gpureplay: done\n");
            } else if (replayState_ == 2) {
                if (replayNext_ < replayAt_.size()) {
                    Replay(replayAt_[replayNext_]);
                    replayNext_++;
                } else {
                    replayState_ = 3;
                    std::fprintf(stderr, "gpureplay: done\n");
                }
            }
        }
    }
    {   // SLY1_GPU_CAPTURE_HEAVY=n: the capture two frames after the first frame of n draws or more (the
        // same scene in every run, which a frame's number is not: the game keeps real time)
        static const long heavy = std::getenv("SLY1_GPU_CAPTURE_HEAVY") ? std::atol(std::getenv("SLY1_GPU_CAPTURE_HEAVY")) : 0;
        static bool set;
        if (heavy > 0 && !set && allDraws_ >= heavy) {
            set = true;
            captureAt_ = (long)frames_ + 3;
            std::fprintf(stderr, "gpustop: a frame of %ld draws: capture at %ld\n", allDraws_, captureAt_);
        }
        allDraws_ = 0;
    }
    if (listAt_ >= 0 && ((long)frames_ + 1 == listAt_ || (long)frames_ + 2 == listAt_))
        std::fprintf(stderr, "gpulist: present %llu, DISPFB %llX, %llu other-target draws so far\n", (unsigned long long)frames_ + 1, (unsigned long long)dispfb, (unsigned long long)otherTargets_);
    frames_++;
    int target = TargetOf(dispfb & 0x1FF);
    if (target < 0)
        target = 0;
    // SLY1_GPU_CAPTURE's last view: the depth buffer as the frame left it, as bands of colour (each
    // pixel the colour of the smallest of eight depths that is not below its own)
    static const long stepEarly = std::getenv("SLY1_GPU_CAPTURE_STEP") ? 1 : 0;
    static const bool zmap = std::getenv("SLY1_GPU_ZMAP") != nullptr;   // frame N, then its depths, in one run
    paintLast_ = false;
    if (zmap && captureAt_ >= 0 && (long)frames_ == captureAt_) {
        stopped_ = false;
        Capture(target);
    }
    if ((view_ == 5 && !stepEarly) || (zmap && captureAt_ >= 0 && (long)frames_ == captureAt_)) {
        static const std::uint32_t kDepth[8] = {0xFFFFFFFFu, 16777215, 524288, 131072, 32768, 8192, 2048, 0};
        static const std::uint8_t kColour[8][3] = {{255, 0, 255}, {255, 0, 0}, {255, 128, 0}, {255, 255, 0},
                                                   {0, 255, 0}, {0, 255, 255}, {0, 0, 255}, {64, 64, 64}};
        gs::DrawState st;
        std::memset(&st, 0, sizeof st);
        st.kind = 2;
        st.frame = target == 1 ? 70 : 0;
        st.test = (1u << 16) | (2u << 17);          // ZTE, GEQUAL
        st.zbuf = 1ull << 32;                        // no depth written
        st.scissor = 0x00DF0000027F0000ull;
        for (int k = 0; k < 8; k++) {
            gs::DrawVertex v[4];
            std::memset(v, 0, sizeof v);
            const float xs[4] = {0, 640, 0, 640}, ys[4] = {0, 0, 224, 224};
            for (int c = 0; c < 4; c++) {
                v[c].x = xs[c]; v[c].y = ys[c]; v[c].z = kDepth[k];
                v[c].r = kColour[k][0]; v[c].g = kColour[k][1]; v[c].b = kColour[k][2]; v[c].a = 128;
                v[c].q = 1.0f;
            }
            static const std::uint32_t index[6] = {0, 1, 2, 1, 3, 2};
            int keep = view_;
            view_ = 0;
            Source src = {v, sizeof v, sizeof(gs::DrawVertex), index, 6, nullptr};
            Submit(st, src);
            view_ = keep;
        }
    }
    // SLY1_GPU_CAPTURE: frame N as it is, then five frames with the batches shown otherwise
    if (zmap && captureAt_ >= 0 && (long)frames_ == captureAt_)
        Capture(target);
    if (zmap)
        captureAt_ = (long)frames_ == captureAt_ ? -2 : captureAt_;
    static const long step = std::getenv("SLY1_GPU_CAPTURE_STEP") ? std::atol(std::getenv("SLY1_GPU_CAPTURE_STEP")) : 0;
    if (step > 0) {   // six frames, step apart, each as it is
        if (captureAt_ >= 0 && (long)frames_ >= captureAt_ && (long)frames_ <= captureAt_ + 5 * step &&
            ((long)frames_ - captureAt_) % step == 0)
            Capture(target);
    } else if (captureAt_ >= 0 && (long)frames_ >= captureAt_ && (long)frames_ <= captureAt_ + 5)
        Capture(target);
    if (captureAt_ == -2 || (captureAt_ >= 0 && frames_ + 1 >= (std::uint64_t)captureAt_ && frames_ <= (std::uint64_t)captureAt_ + 1))
        std::fprintf(stderr, "gpustop: at present %llu: %ld draws counted, captureAt %ld, showing target %d (DISPFB %llX)\n", (unsigned long long)frames_, drawInFrame_, captureAt_, target, (unsigned long long)dispfb);
    batchInFrame_ = 0;
    drawInFrame_ = 0;
    stopped_ = true;
    view_ = step == 0 && captureAt_ >= 0 && (long)frames_ >= captureAt_ && (long)frames_ < captureAt_ + 5 ? (int)(frames_ - captureAt_) + 1 : 0;
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
    dkQueueSignalFence(queue_, &fences_[set_], false);
    fenced_[set_] = true;
    dkQueuePresentImage(queue_, swapchain_, slot);
    // the next set, once the GPU has done with the frame recorded in it
    set_ = (set_ + 1) % kSets;
    std::uint64_t t1 = NowNs();
    if (fenced_[set_])
        dkFenceWait(&fences_[set_], -1);
    fenced_[set_] = false;
    waitNs_ += NowNs() - t1;
    if (abOn_) {
        const std::uint64_t now = NowNs();
        if (frames_ % 30 >= 6 && abAt_) {   // the first frames after a change left out
            abNs_[ab_] += now - abAt_;
            abWaitNs_[ab_] += now - t1;
            abFrames_[ab_]++;
        }
        abAt_ = now;
        ab_ = (int)(frames_ / 30 % 6);
    }
    frameMem_ = frameMems_[set_];
    commandMem_ = commandMems_[set_];
    BeginCommands();
    presentNs_ += NowNs() - t0;
}

void Renderer::Report()
{
    std::fprintf(stderr, "gpu: %llu frames, %llu runs of primitives (%llu vertices), %llu command lists sent\n",
                 (unsigned long long)frames_, (unsigned long long)runs_, (unsigned long long)vertices_,
                 (unsigned long long)sends_);
    std::fprintf(stderr, "gpu: %llu runs added to the run before (the same state and texture) and drawn with it; %llu left out (the run before again, for its depth alone)\n",
                 (unsigned long long)merged_, (unsigned long long)dropped_);
    static const char* const kAb[6] = {"as it is", "the long pixel shader", "no destination alpha test", "no batches", "no runs into the frame",
                                       "a sixteenth of the picture"};
    for (int k = 0; abOn_ && k < 6; k++)
        std::fprintf(stderr, "gpuab: %-26s %4llu frames, %6.2f ms a frame, %6.2f ms of it waiting for the GPU\n", kAb[k], (unsigned long long)abFrames_[k],
                     abFrames_[k] ? abNs_[k] / 1e6 / abFrames_[k] : 0.0, abFrames_[k] ? abWaitNs_[k] / 1e6 / abFrames_[k] : 0.0);
    std::fprintf(stderr, "gpu: %llu small targets retired (the game put textures in their buffers)\n", (unsigned long long)retired_);
    std::fprintf(stderr, "gpu: %llu runs with the short pixel shader\n", (unsigned long long)liteRuns_);
    std::fprintf(stderr, "gpu: %llu batches transformed by the GPU (%llu vertices); %.3f s making their triangles' indices\n",
                 (unsigned long long)batches_, (unsigned long long)batchVertices_, indexNs_ / 1e9);
    std::fprintf(stderr, "gpu: %.3f s copying %.1f MB of vertices and indices to the GPU's memory\n", copyNs_ / 1e9, copiedBytes_ / 1e6);
    std::fprintf(stderr, "gpu: %zu textures held (%u KB), %llu checked after uploads, %llu decoded, %llu uploaded, %llu times the cache "
                 "was emptied\n", textures_.size(), textureUsed_ >> 10, (unsigned long long)hashes_, (unsigned long long)decodes_, (unsigned long long)uploads_,
                 (unsigned long long)cacheResets_);
    std::fprintf(stderr, "gpu: %.3f s in draws (%.3f s of it textures), %.3f s in swaps (%.3f s for a screen image, %.3f s "
                 "for the GPU to finish)\n", drawNs_ / 1e9, textureNs_ / 1e9, presentNs_ / 1e9, acquireNs_ / 1e9, waitNs_ / 1e9);
    std::fprintf(stderr, "gpu: %llu runs with the destination alpha test, %llu barriers before them\n",
                 (unsigned long long)destinationAlpha_, (unsigned long long)dateBarriers_);
    std::fprintf(stderr, "gpu: %d small targets, %llu runs into them, %llu textured from them, %llu that read the target they draw (%llu steps)\n",
                 smallCount_, (unsigned long long)smallRuns_, (unsigned long long)smallTextured_, (unsigned long long)smallSelf_,
                 (unsigned long long)smallSteps_);
    std::fprintf(stderr, "gpu: not drawn as the GS would: %llu runs into other buffers and %llu textured from them (the shadows' pictures, left "
                 "out), %llu textured from a frame buffer (the motion blur, left out), %llu blends approximated\n", (unsigned long long)otherTargets_,
                 (unsigned long long)targetTextured_, (unsigned long long)frameTextured_, (unsigned long long)unsupportedBlends_);
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
