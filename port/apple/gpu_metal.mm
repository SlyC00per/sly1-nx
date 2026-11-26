// SPDX-License-Identifier: AGPL-3.0-or-later
// The Metal renderer, for Apple's GPUs (the Mac's build, and the iPhone's: docs/IOS.md): what the
// GS is given to draw (port/gs_backend.h), drawn with Metal.
//
// As the Switch's renderer (port/switch/gpu.cpp), the game's two frame buffers are images here,
// with one depth image between them, and the small buffers the game draws into and reads as
// textures are images too. The GS's vertices arrive transformed. What differs is the pixel
// pipeline: Apple's GPUs give a pixel shader the colour already in the frame under its pixel, so
// the whole of the GS's pipeline after the rasteriser is the shader's, in the GS's own integer
// arithmetic (port/render/gs.cpp's Pixel is the reference, line for line): the texture function,
// the fog, the alpha test, the destination alpha test, the blend, FBA and the frame's mask. There
// is no blend state, and none of the approximations a fixed blend needs.
//
// SLY1_METAL_COMPARE=1 (with --dump-fb): the images at the GS's own size, and each frame shown
// compared with the reference drawing's picture of it, pixel by pixel.
#include "port/gpu.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#endif

#include <atomic>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "port/apple.h"
#include "port/gs.h"
#include "port/retail_data.h"
#include "port/gs_backend.h"
#include "port/system.h"

namespace port {
namespace gpu {

namespace {

constexpr float kFieldWidth = 640.0f, kFieldHeight = 224.0f;   // a field of the game's frame, in the GS's pixels
constexpr int kSmallTargets = 8;                               // the buffers other than the frame's two the game draws into
constexpr std::size_t kFrameBufferBytes = 8u << 20;            // a block of a frame's vertices and indices

// The shaders. gs_vertex: window pixels to clip space, the depth as a fraction (bigger is nearer,
// as the GS has it) and w 1: the GS interpolates colour, S, T and Q across the screen and divides
// by Q at each pixel. gs_fragment: the GS's pixel pipeline.
const char* const kShaders = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float2 position [[attribute(0)]];
    uint depth [[attribute(1)]];
    float4 colour [[attribute(2)]];   // 0 to 1 for the GS's 0 to 255
    float3 stq [[attribute(3)]];
    float fog [[attribute(4)]];
};

struct VertexOut {
    float4 position [[position]];
    float4 colour [[center_no_perspective]];   // 0 to 255
    float3 stq [[center_no_perspective]];
    float fog [[center_no_perspective]];
    float size [[point_size]];
};

struct VertexParams {
    float4 toClip;   // x and y scales, then the offsets
    float4 depth;    // x: the greatest depth the Z buffer's format holds; y: a point's size in pixels
};

vertex VertexOut gs_vertex(VertexIn in [[stage_in]], constant VertexParams& u [[buffer(1)]])
{
    VertexOut out;
    out.position = float4(in.position * u.toClip.xy + u.toClip.zw, min(float(in.depth), u.depth.x) * (1.0 / 4294967296.0), 1.0);
    out.colour = in.colour * 255.0;
    out.stq = in.stq;
    out.fog = in.fog;
    out.size = u.depth.y;
    return out;
}

struct FragmentIn {   // (VertexOut without the point's size, which a pixel shader may not take)
    float4 position [[position]];
    float4 colour [[center_no_perspective]];
    float3 stq [[center_no_perspective]];
    float fog [[center_no_perspective]];
};

struct FragmentParams {
    int4 mode;      // TME, TFX, TCC, FGE
    float4 fog;     // FOGCOL's R, G, B
    int4 test;      // ATE, ATST, AREF, AFAIL
    int4 pass;      // x: 0 every pixel, 1 only those that pass the alpha test, 2 only those that fail; DATE, DATM, ABE
    int4 blend;     // A, B, C, D
    int4 misc;      // FIX, FBA, 1 for a frame without alpha (PSMCT24), the texture's region clamps (1 S, 2 T)
    uint4 mask;     // FBMSK
    float4 region;  // REGION_CLAMP's limits as coordinates: S's two, T's two
    float4 image;   // x, y: S and T times these (a buffer the game drew into, read as a texture: the texture's
                    // size over the buffer's own); z: its alpha when the texture's format has none (PSMCT24), else below 0
};

fragment float4 gs_fragment(FragmentIn in [[stage_in]], float4 frame [[color(0)]], constant FragmentParams& u [[buffer(0)]],
                            texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]])
{
    int4 c = int4(round(clamp(in.colour, 0.0, 255.0)));
    if (u.mode.x != 0) {
        float2 uv = in.stq.xy / (in.stq.z != 0.0 ? in.stq.z : 1.0) * u.image.xy;
        if ((u.misc.w & 1) != 0)
            uv.x = clamp(uv.x, u.region.x, u.region.y);
        if ((u.misc.w & 2) != 0)
            uv.y = clamp(uv.y, u.region.z, u.region.w);
        int4 t = int4(round(tex.sample(smp, uv) * 255.0));
        if (u.image.z >= 0.0)
            t.a = int(u.image.z);
        if (u.mode.y == 0) {          // MODULATE
            c.rgb = min((t.rgb * c.rgb) >> 7, 255);
            if (u.mode.z != 0)
                c.a = min((t.a * c.a) >> 7, 255);
        } else if (u.mode.y == 1) {   // DECAL
            c.rgb = t.rgb;
            if (u.mode.z != 0)
                c.a = t.a;
        } else {                      // HIGHLIGHT, HIGHLIGHT2
            c.rgb = min(((t.rgb * c.rgb) >> 7) + c.a, 255);
            if (u.mode.z != 0)
                c.a = u.mode.y == 2 ? min(t.a + c.a, 255) : t.a;
        }
    }
    if (u.mode.w != 0) {
        int f = int(round(clamp(in.fog, 0.0, 255.0)));
        c.rgb = (c.rgb * f + int3(u.fog.rgb) * (255 - f)) >> 8;
    }
    // the alpha test, and what a pixel that fails it still writes (AFAIL)
    bool passes = true;
    if (u.test.x != 0) {
        int a = c.a, ref = u.test.z;
        switch (u.test.y) {
        case 0: passes = false; break;
        case 1: passes = true; break;
        case 2: passes = a < ref; break;
        case 3: passes = a <= ref; break;
        case 4: passes = a == ref; break;
        case 5: passes = a >= ref; break;
        case 6: passes = a > ref; break;
        default: passes = a != ref; break;
        }
    }
    if (u.pass.x == 1 && !passes)
        discard_fragment();
    if (u.pass.x == 2 && passes)
        discard_fragment();
    bool rgbOnly = false;
    if (!passes) {
        if (u.test.w == 0)        // KEEP
            discard_fragment();
        else if (u.test.w == 2) { // ZB_ONLY: the frame as it is (the pass writes the depth, or nothing)
            if (u.pass.x == 0)
                discard_fragment();
            return frame;
        } else if (u.test.w == 3) // RGB_ONLY
            rgbOnly = true;
    }
    int4 d = int4(round(frame * 255.0));
    if (u.misc.z != 0)
        d.a = 0x80;
    if (u.pass.y != 0 && (d.a >= 0x80) != (u.pass.z != 0))   // DATE: the frame's alpha's top bit against DATM
        discard_fragment();
    if (u.pass.w != 0) {   // (A - B) * C + D
        int cc = u.blend.z == 0 ? c.a : u.blend.z == 1 ? d.a : u.misc.x;
        int3 A = u.blend.x == 0 ? c.rgb : u.blend.x == 1 ? d.rgb : int3(0);
        int3 B = u.blend.y == 0 ? c.rgb : u.blend.y == 1 ? d.rgb : int3(0);
        int3 D = u.blend.w == 0 ? c.rgb : u.blend.w == 1 ? d.rgb : int3(0);
        c.rgb = (((A - B) * cc) >> 7) + D;
    }
    c.rgb = clamp(c.rgb, 0, 255);
    if (u.misc.y != 0)
        c.a |= 0x80;
    uint4 keep = (uint4(u.mask.x) >> uint4(0, 8, 16, 24)) & 0xFFu;   // FBMSK's bits of each channel: kept as the frame has them
    if (rgbOnly || u.misc.z != 0)
        keep.a = 0xFFu;
    uint4 was = uint4(round(frame * 255.0));
    uint4 now = (was & keep) | (uint4(c & 0xFF) & ~keep);
    return float4(now) / 255.0;
}

// A picture put on another (the frame shown, to the screen's image or to one read back).
struct CopyOut {
    float4 position [[position]];
    float2 uv;
};

vertex CopyOut copy_vertex(uint id [[vertex_id]])
{
    float2 p = float2(float((id << 1) & 2), float(id & 2));   // (0,0) (2,0) (0,2)
    CopyOut out;
    out.uv = p;
    out.position = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
    return out;
}

fragment float4 copy_fragment(CopyOut in [[stage_in]], constant float4& u [[buffer(0)]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]])
{
    float3 rgb = tex.sample(smp, in.uv).rgb;
    float grey = dot(rgb, float3(0.299, 0.587, 0.114));   // (the Vivid picture: port/switch/shaders/present_fsh.glsl)
    return float4(clamp(grey + (rgb - grey) * u.x, 0.0, 1.0), 1.0);
}
)METAL";

struct VertexParams {
    float toClip[4];
    float depth[4];
};

struct FragmentParams {
    std::int32_t mode[4];
    float fog[4];
    std::int32_t test[4];
    std::int32_t pass[4];
    std::int32_t blend[4];
    std::int32_t misc[4];
    std::uint32_t mask[4];
    float region[4];
    float image[4];
};

struct Texture {
    id<MTLTexture> texture = nil;
    std::uint32_t generation = 0;
    std::uint64_t hash = 0;
    int width = 0, height = 0;
    std::uint32_t direct = 0;   // the movie picture last put in it (gs::DirectPicture's serial)
};

// What a run is textured from (Begin's choice).
struct Chosen {
    enum Kind { kNone, kDecoded, kSmall, kFrame } kind = kNone;
    const Texture* decoded = nullptr;
    int index = 0;   // the small target's, or the frame buffer's
};

class Renderer : public gs::Backend {
public:
    bool Create();
    void Begin(const gs::DrawState& state) override;
    void Draw(const gs::DrawState& state, const gs::DrawVertex* v, std::size_t vertices, const std::uint32_t* index,
              std::size_t indices) override;
    void Present(std::uint64_t dispfb, std::uint64_t display) override;
    void Report();
    // The screen's layer (a window's): each frame shown is put on it.
    void SetLayer(CAMetalLayer* layer);
    id<MTLDevice> Device() const { return device_; }
    std::uint32_t Width() const { return width_; }
    std::uint32_t Height() const { return height_; }

private:
    struct Small {
        // two images: a run that reads the buffer it draws into (the game's blur of a shadow's
        // picture) reads the current one and draws the other, which becomes the current
        id<MTLTexture> image[2] = {nil, nil};
        bool cleared[2] = {false, false};
        int cur = 0;
        std::uint32_t fbp = ~0u;
        std::uint32_t width = 0, height = 0;   // in the GS's pixels
        std::uint32_t serial = 0;              // gs::UploadSerial() when it was last drawn into
    };
    static constexpr std::uint32_t kRetired = 0xFFFFFFFEu;   // a small target's fbp once the game has put textures in its buffer
    struct Block {
        id<MTLBuffer> buffer = nil;
        std::size_t used = 0;
    };

    static int TargetOf(std::uint64_t fbp) { return fbp == 0 ? 0 : fbp == 70 ? 1 : -1; }
    int TargetFor(const gs::DrawState& s);
    const Texture* TextureFor(const gs::DrawState& state);
    void Submit(const gs::DrawState& s, const gs::DrawVertex* v, std::size_t count, const std::uint32_t* index, std::size_t indices,
                bool selfStep);
    void EnsureCommands();
    void EnsurePass(int target, bool other);
    void EndPass();
    void TakeBlock();
    id<MTLTexture> Image(std::uint32_t width, std::uint32_t height, MTLPixelFormat format, bool target);
    void Compare(int target);

    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    id<MTLRenderPipelineState> pipeline_[2] = {nil, nil};   // without a depth image, and with
    id<MTLRenderPipelineState> copyPipeline_ = nil;         // the frame shown onto the screen's image
    id<MTLLibrary> library_ = nil;
    CAMetalLayer* layer_ = nil;
    std::mutex layerLock_;
    float lineWidth_ = 4.5f;   // a line's width in the pixels of a 720-line image (the Switch's renderer's, in play)
    std::vector<gs::DrawVertex> lineVertices_;
    std::vector<std::uint32_t> lineIndex_;
    id<MTLDepthStencilState> depth_[3][2];                   // the test (always, GEQUAL, GREATER) and whether the depth is written
    id<MTLSamplerState> sampler_[2][2][2];                   // bilinear or not, S clamped or not, T clamped or not
    id<MTLTexture> colour_[2] = {nil, nil}, depthImage_ = nil, white_ = nil;
    bool colourCleared_[2] = {false, false}, depthCleared_ = false;
    std::uint32_t width_ = 1280, height_ = 720;   // a frame buffer's image
    Small small_[kSmallTargets];
    int smallCount_ = 0;
    std::uint32_t otherSerial_[512] = {};   // the uploads so far when a buffer this renderer does not draw into was last drawn into; 0: never

    id<MTLCommandBuffer> commands_ = nil;
    id<MTLRenderCommandEncoder> encoder_ = nil;
    int passTarget_ = -2;
    bool passOther_ = false;
    float fieldWidth_ = kFieldWidth, fieldHeight_ = kFieldHeight;
    std::uint32_t imageWidth_ = 0, imageHeight_ = 0;
    bool passDepth_ = false;

    std::mutex poolLock_;
    std::vector<id<MTLBuffer>> pool_;      // blocks no frame the GPU is still drawing uses
    std::vector<id<MTLBuffer>> frameBlocks_;   // this frame's, given back when the GPU has drawn it
    Block block_;

    Chosen chosen_;
    std::unordered_map<std::uint64_t, Texture> textures_;
    std::vector<std::uint32_t> pixels_;

    bool compare_ = false, blur_ = false, gsFilter_ = false;
    std::string dumpDir_;
    int dumpEvery_ = 0;

    // the report's counts
    std::uint64_t frames_ = 0, runs_ = 0, indices_ = 0, decodes_ = 0, otherTargets_ = 0, frameTextured_ = 0, smallRuns_ = 0, smallSelf_ = 0,
                  twoPass_ = 0, regionRepeat_ = 0, lowFrames_ = 0, passes_ = 0;
    std::unordered_map<std::uint64_t, std::uint64_t> otherFrames_;   // the FRAME registers of the runs into other buffers, counted
    double cmpSum_ = 0, cmpWorst_ = 0;
    std::uint64_t cmpFrames_ = 0, cmpWorstFrame_ = 0;
    double cmpWithin_[3] = {0, 0, 0};
};

id<MTLTexture> Renderer::Image(std::uint32_t width, std::uint32_t height, MTLPixelFormat format, bool target)
{
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | (target ? MTLTextureUsageRenderTarget : 0);
    d.storageMode = format == MTLPixelFormatDepth32Float ? MTLStorageModePrivate : MTLStorageModeShared;
    return [device_ newTextureWithDescriptor:d];
}

bool Renderer::Create()
{
    @autoreleasepool {
        device_ = MTLCreateSystemDefaultDevice();
#if TARGET_OS_OSX
        if (!device_) {   // (a command-line program may be given no default one)
            NSArray<id<MTLDevice>>* all = MTLCopyAllDevices();
            device_ = all.count ? all[0] : nil;
        }
#endif
        if (!device_) {
            std::fprintf(stderr, "gpu: Metal: no device\n");
            return false;
        }
        // The pixel shader reads the frame under its pixel: Apple's own GPUs only.
        if (![device_ supportsFamily:MTLGPUFamilyApple4]) {
            std::fprintf(stderr, "gpu: Metal: %s is not one of Apple's GPUs (the pixel shader reads the frame's colour)\n", device_.name.UTF8String);
            return false;
        }
        queue_ = [device_ newCommandQueue];
        NSError* error = nil;
        MTLCompileOptions* options = [MTLCompileOptions new];
        options.fastMathEnabled = NO;
        id<MTLLibrary> library = [device_ newLibraryWithSource:[NSString stringWithUTF8String:kShaders] options:options error:&error];
        if (!library) {
            std::fprintf(stderr, "gpu: Metal: the shaders: %s\n", error.localizedDescription.UTF8String);
            return false;
        }
        library_ = library;
        {
            MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
            d.vertexFunction = [library newFunctionWithName:@"copy_vertex"];
            d.fragmentFunction = [library newFunctionWithName:@"copy_fragment"];
            d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            copyPipeline_ = [device_ newRenderPipelineStateWithDescriptor:d error:&error];
            if (!copyPipeline_) {
                std::fprintf(stderr, "gpu: Metal: the screen's pipeline: %s\n", error.localizedDescription.UTF8String);
                return false;
            }
        }
        if (const char* line = std::getenv("SLY1_METAL_LINE"))   // SLY1_METAL_LINE=W: a line's width, in pixels of a 720-line image
            lineWidth_ = std::max(1.0f, (float)std::atof(line));
        MTLVertexDescriptor* layout = [MTLVertexDescriptor vertexDescriptor];
        layout.attributes[0].format = MTLVertexFormatFloat2;
        layout.attributes[0].offset = offsetof(gs::DrawVertex, x);
        layout.attributes[1].format = MTLVertexFormatUInt;
        layout.attributes[1].offset = offsetof(gs::DrawVertex, z);
        layout.attributes[2].format = MTLVertexFormatUChar4Normalized;
        layout.attributes[2].offset = offsetof(gs::DrawVertex, r);
        layout.attributes[3].format = MTLVertexFormatFloat3;
        layout.attributes[3].offset = offsetof(gs::DrawVertex, s);
        layout.attributes[4].format = MTLVertexFormatFloat;
        layout.attributes[4].offset = offsetof(gs::DrawVertex, fog);
        layout.layouts[0].stride = sizeof(gs::DrawVertex);
        for (int withDepth = 0; withDepth < 2; withDepth++) {
            MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
            d.vertexFunction = [library newFunctionWithName:@"gs_vertex"];
            d.fragmentFunction = [library newFunctionWithName:@"gs_fragment"];
            d.vertexDescriptor = layout;
            d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            d.depthAttachmentPixelFormat = withDepth ? MTLPixelFormatDepth32Float : MTLPixelFormatInvalid;
            pipeline_[withDepth] = [device_ newRenderPipelineStateWithDescriptor:d error:&error];
            if (!pipeline_[withDepth]) {
                std::fprintf(stderr, "gpu: Metal: the pipeline: %s\n", error.localizedDescription.UTF8String);
                return false;
            }
        }
        static const MTLCompareFunction kTest[3] = {MTLCompareFunctionAlways, MTLCompareFunctionGreaterEqual, MTLCompareFunctionGreater};
        for (int test = 0; test < 3; test++)
            for (int write = 0; write < 2; write++) {
                MTLDepthStencilDescriptor* d = [MTLDepthStencilDescriptor new];
                d.depthCompareFunction = kTest[test];
                d.depthWriteEnabled = write != 0;
                depth_[test][write] = [device_ newDepthStencilStateWithDescriptor:d];
            }
        for (int linear = 0; linear < 2; linear++)
            for (int cs = 0; cs < 2; cs++)
                for (int ct = 0; ct < 2; ct++) {
                    MTLSamplerDescriptor* d = [MTLSamplerDescriptor new];
                    d.minFilter = d.magFilter = linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
                    d.sAddressMode = cs ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
                    d.tAddressMode = ct ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
                    sampler_[linear][cs][ct] = [device_ newSamplerStateWithDescriptor:d];
                }

        // SLY1_METAL_COMPARE: the images as the GS's own (a field's 640 by 224), the textures
        // filtered as the GS's registers say, and the game's motion blur drawn (the reference has it).
        compare_ = std::getenv("SLY1_METAL_COMPARE") != nullptr;
        // 720 lines, as wide as the picture's shape makes them (port::Aspect: 1,280 for 16:9)
        width_ = ((std::uint32_t)std::lround(720.0f * Aspect()) + 1) & ~1u;
        height_ = 720;
        if (compare_) {
            width_ = (std::uint32_t)kFieldWidth;
            height_ = (std::uint32_t)kFieldHeight;
        } else if (const char* size = std::getenv("SLY1_METAL_SIZE")) {   // SLY1_METAL_SIZE=WxH: the frame buffers' images
            unsigned w = 0, h = 0;
            if (std::sscanf(size, "%ux%u", &w, &h) == 2 && w >= 64 && h >= 64 && w <= 8192 && h <= 8192) {
                width_ = w;
                height_ = h;
            }
        }
        // SLY1_METAL_FILTER=gs: MMAG decides, as on the GS; linear (the Switch's renderer's): always bilinear.
        const char* filter = std::getenv("SLY1_METAL_FILTER");
        gsFilter_ = filter ? !std::strcmp(filter, "gs") : compare_;
        // SLY1_METAL_BLUR=1: the game's motion blur drawn (the port has none: docs/PS3_PARITY.md); 0: not.
        const char* blur = std::getenv("SLY1_METAL_BLUR");
        blur_ = blur ? blur[0] != '0' : compare_;
        // SLY1_METAL_DUMP=DIR (and SLY1_METAL_DUMP_EVERY=N): every N-th frame shown written there as metal-NNNNN.png
        if (const char* dir = std::getenv("SLY1_METAL_DUMP")) {
            dumpDir_ = dir;
            dumpEvery_ = std::getenv("SLY1_METAL_DUMP_EVERY") ? std::max(1, std::atoi(std::getenv("SLY1_METAL_DUMP_EVERY"))) : 60;
        }

        for (int k = 0; k < 2; k++)
            colour_[k] = Image(width_, height_, MTLPixelFormatRGBA8Unorm, true);
        depthImage_ = Image(width_, height_, MTLPixelFormatDepth32Float, true);
        white_ = Image(1, 1, MTLPixelFormatRGBA8Unorm, false);
        const std::uint32_t one = 0xFFFFFFFFu;
        [white_ replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&one bytesPerRow:4];
        std::fprintf(stderr, "gpu: Metal on %s: frame buffers of %u by %u%s\n", device_.name.UTF8String, width_, height_,
                     compare_ ? ", compared with the reference drawing" : "");
    }
    return true;
}

// A block of memory for the frame's vertices and indices: one no frame still being drawn uses.
void Renderer::TakeBlock()
{
    id<MTLBuffer> buffer = nil;
    {
        std::lock_guard<std::mutex> lk(poolLock_);
        if (!pool_.empty()) {
            buffer = pool_.back();
            pool_.pop_back();
        }
    }
    if (!buffer)
        buffer = [device_ newBufferWithLength:kFrameBufferBytes options:MTLResourceStorageModeShared];
    frameBlocks_.push_back(buffer);
    block_.buffer = buffer;
    block_.used = 0;
    if (encoder_)
        [encoder_ setVertexBuffer:buffer offset:0 atIndex:0];
}

void Renderer::EnsureCommands()
{
    if (commands_)
        return;
    commands_ = [queue_ commandBuffer];
    if (!block_.buffer)
        TakeBlock();
}

void Renderer::EndPass()
{
    if (encoder_) {
        [encoder_ endEncoding];
        encoder_ = nil;
    }
    passTarget_ = -2;
}

// The pass that draws into a target: the frame's two buffers (0 and 1) with the depth image, or a
// small target (2 and up; other: its image that is not the current one) without one.
void Renderer::EnsurePass(int target, bool other)
{
    if (encoder_ && passTarget_ == target && passOther_ == other)
        return;
    EndPass();
    EnsureCommands();
    MTLRenderPassDescriptor* d = [MTLRenderPassDescriptor renderPassDescriptor];
    bool* cleared;
    if (target >= 2) {
        Small& t = small_[target - 2];
        const int which = other ? t.cur ^ 1 : t.cur;
        d.colorAttachments[0].texture = t.image[which];
        cleared = &t.cleared[which];
        fieldWidth_ = (float)t.width;
        fieldHeight_ = (float)t.height;
        imageWidth_ = t.width;
        imageHeight_ = t.height;
        passDepth_ = false;
    } else {
        d.colorAttachments[0].texture = colour_[target];
        cleared = &colourCleared_[target];
        d.depthAttachment.texture = depthImage_;
        d.depthAttachment.loadAction = depthCleared_ ? MTLLoadActionLoad : MTLLoadActionClear;
        d.depthAttachment.clearDepth = 0.0;
        d.depthAttachment.storeAction = MTLStoreActionStore;
        depthCleared_ = true;
        fieldWidth_ = kFieldWidth;
        fieldHeight_ = kFieldHeight;
        imageWidth_ = width_;
        imageHeight_ = height_;
        passDepth_ = true;
    }
    d.colorAttachments[0].loadAction = *cleared ? MTLLoadActionLoad : MTLLoadActionClear;
    d.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    d.colorAttachments[0].storeAction = MTLStoreActionStore;
    *cleared = true;
    encoder_ = [commands_ renderCommandEncoderWithDescriptor:d];
    [encoder_ setRenderPipelineState:pipeline_[passDepth_ ? 1 : 0]];
    [encoder_ setVertexBuffer:block_.buffer offset:0 atIndex:0];
    passTarget_ = target;
    passOther_ = other;
    passes_++;
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
    const std::uint32_t fbw = (std::uint32_t)((s.frame >> 16) & 0x3F), psm = (std::uint32_t)((s.frame >> 24) & 0x3F);
    if (fbw == 0 || fbw > 4 || psm != 0)
        return -1;
    for (int k = 0; k < smallCount_; k++)
        if (small_[k].fbp == fbp)
            return 2 + k;
    for (int k = 0; k < smallCount_; k++)
        if (small_[k].fbp == kRetired && small_[k].width == fbw * 64) {   // a retired one's images, for this buffer
            Small& t = small_[k];
            t.fbp = fbp;
            t.cur = 0;
            t.cleared[0] = t.cleared[1] = false;   // both empty to start with
            if (passTarget_ == 2 + k)
                EndPass();
            return 2 + k;
        }
    if (smallCount_ == kSmallTargets)
        return -1;
    Small& t = small_[smallCount_];
    t.fbp = fbp;
    t.width = t.height = fbw * 64;
    t.cur = 0;
    for (int i = 0; i < 2; i++) {
        t.image[i] = Image(t.width, t.height, MTLPixelFormatRGBA8Unorm, true);
        t.cleared[i] = false;
    }
    return 2 + smallCount_++;
}

const Texture* Renderer::TextureFor(const gs::DrawState& state)
{
    // what names the texels: TBP0, TBW, PSM, TW, TH and the palette's CBP, CPSM, CSM, CSA
    const std::uint64_t key = state.tex0 & 0x1FFFFFE3FFFFFFFFull;
    const std::uint32_t generation = gs::TextureGeneration(state);
    auto it = textures_.find(key);
    {   // a movie's picture: copied into the texture's corner as it is
        const std::uint32_t* picture;
        int pw, ph;
        std::uint32_t serial;
        if (it != textures_.end() && gs::DirectPicture(state, &picture, &pw, &ph, &serial) && it->second.width >= pw && it->second.height >= ph) {
            Texture& t = it->second;
            if (t.direct != serial) {
                // (a new image: the draws recorded so far read the old one)
                t.texture = Image((std::uint32_t)t.width, (std::uint32_t)t.height, MTLPixelFormatRGBA8Unorm, false);
                [t.texture replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)pw, (NSUInteger)ph) mipmapLevel:0 withBytes:picture bytesPerRow:(NSUInteger)pw * 4];
                t.direct = serial;
            }
            t.generation = generation;
            return &t;
        }
    }
    if (it != textures_.end() && it->second.generation == generation)
        return &it->second;
    const std::uint64_t hash = gs::TextureSource(state);
    if (it != textures_.end() && it->second.hash == hash) {
        it->second.generation = generation;
        return &it->second;
    }
    int w, h;
    gs::DecodeTexture(state, pixels_, w, h);
    decodes_++;
    if (it == textures_.end())
        it = textures_.emplace(key, Texture()).first;
    Texture& t = it->second;
    // A new image each time the texels change: the draws recorded so far, not yet the GPU's, read the old one.
    t.texture = Image((std::uint32_t)w, (std::uint32_t)h, MTLPixelFormatRGBA8Unorm, false);
    [t.texture replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h) mipmapLevel:0 withBytes:pixels_.data() bytesPerRow:(NSUInteger)w * 4];
    t.width = w;
    t.height = h;
    t.generation = generation;
    t.hash = hash;
    t.direct = 0;
    return &t;
}

void Renderer::Begin(const gs::DrawState& s)
{
    @autoreleasepool {
        chosen_ = Chosen();
        if (!s.tme || TargetFor(s) < 0)
            return;
        const std::uint32_t tbp = (std::uint32_t)(s.tex0 & 0x3FFF);
        if (tbp == 0 || tbp == 70 * 32) {   // a frame buffer as a texture: the game's motion blur (Submit)
            chosen_.kind = Chosen::kFrame;
            chosen_.index = tbp == 0 ? 0 : 1;
            return;
        }
        // A buffer the game drew into is the game's to use for textures again (another level's): an
        // upload into it since it was last drawn into ends that. The small target is retired (its
        // images kept for the next buffer of its size), and the texture is the uploaded one.
        for (int k = 0; k < smallCount_; k++) {
            Small& t = small_[k];
            if (t.fbp * 32 != tbp || t.fbp == kRetired)
                continue;
            if (gs::BufferGeneration(t.fbp, t.width / 64, t.width, t.height) > t.serial) {
                t.fbp = kRetired;
            } else {
                chosen_.kind = Chosen::kSmall;
                chosen_.index = k;
                return;
            }
        }
        if (tbp % 32 == 0) {   // a buffer this renderer does not draw into: nothing to show of it
            std::uint32_t& serial = otherSerial_[(tbp / 32) & 0x1FF];
            if (serial != 0 && gs::BufferGeneration(tbp / 32, 1, 64, 64) > serial)
                serial = 0;
            if (serial != 0)
                return;
        }
        chosen_.kind = Chosen::kDecoded;
        chosen_.decoded = TextureFor(s);
    }
}

void Renderer::Draw(const gs::DrawState& s, const gs::DrawVertex* v, std::size_t count, const std::uint32_t* index, std::size_t indices)
{
    if (!index || indices == 0 || count == 0)
        return;
    @autoreleasepool {
        Submit(s, v, count, index, indices, false);
    }
}

void Renderer::Submit(const gs::DrawState& s, const gs::DrawVertex* v, std::size_t count, const std::uint32_t* index, std::size_t indices,
                      bool selfStep)
{
    const int target = TargetFor(s);
    if (target < 0) {
        otherTargets_++;
        otherFrames_[s.frame & 0xFFFFFFFFull]++;
        otherSerial_[s.frame & 0x1FF] = gs::UploadSerial();   // a buffer the game draws into and this renderer does not
        return;
    }
    if (target >= 2) {
        small_[target - 2].serial = gs::UploadSerial();
        smallRuns_ += selfStep ? 0 : 1;
    }
    id<MTLTexture> texture = white_;
    // A buffer the game drew into, read as a texture: the game names it as a texture of tw by th of
    // the GS's pixels, of which the buffer's own are the top left corner; the image here is the
    // buffer's alone. Its alpha is TEXA's TA0 when the texture's format has none.
    float imageScale[2] = {1.0f, 1.0f}, imageAlpha = -1.0f;
    auto asTexture = [&](float bufferWidth, float bufferHeight) {
        imageScale[0] = (float)(1u << std::min<std::uint32_t>((std::uint32_t)((s.tex0 >> 26) & 0xF), 10)) / bufferWidth;
        imageScale[1] = (float)(1u << std::min<std::uint32_t>((std::uint32_t)((s.tex0 >> 30) & 0xF), 10)) / bufferHeight;
        if (((s.tex0 >> 20) & 0x3F) == 1)
            imageAlpha = (float)(s.texa & 0xFF);
    };
    if (s.tme) {
        if (chosen_.kind == Chosen::kNone)
            return;   // a buffer nothing is shown of
        if (chosen_.kind == Chosen::kFrame) {
            // The game's motion blur: each frame ends with a sprite textured from the other frame
            // buffer (the frame before), blended over the new one. The port has none
            // (docs/PS3_PARITY.md); drawn for the comparison with the reference, which has it.
            frameTextured_++;
            if (!blur_ || chosen_.index == target)
                return;
            texture = colour_[chosen_.index];
            asTexture(kFieldWidth, kFieldHeight);
        } else if (chosen_.kind == Chosen::kSmall) {
            Small& t = small_[chosen_.index];
            if (target == 2 + chosen_.index && !selfStep) {
                // A run that reads the buffer it draws into: the game's blur of a shadow's picture,
                // sprites over the whole buffer that copy it onto itself half a texel aside with the
                // bilinear filter, six in a run. Each is drawn by itself, from the current image into
                // the other, which is then the current one. (Any other such run is left out.)
                smallSelf_++;
                if (s.abe || s.kind != 2 || indices % 6 != 0)
                    return;
                for (std::size_t at = 0; at < indices; at += 6) {
                    Submit(s, v, count, index + at, 6, true);
                    EndPass();
                    t.cur ^= 1;
                }
                return;
            }
            texture = t.image[t.cur];
            asTexture((float)t.width, (float)t.height);
        } else {
            texture = chosen_.decoded->texture;
        }
    }
    const std::uint32_t test = (std::uint32_t)s.test;
    const bool ate = test & 1, date = (test >> 14) & 1, datm = (test >> 15) & 1, zte = (test >> 16) & 1;
    const std::uint32_t atst = (test >> 1) & 7, aref = (test >> 4) & 0xFF, afail = (test >> 12) & 3, ztst = (test >> 17) & 3;
    const bool zmsk = (s.zbuf >> 32) & 1;
    if (zte && ztst == 0)
        return;   // nothing passes

    EnsurePass(target, selfStep);
    // Lines in an image bigger than the GS's: Metal draws a line one pixel wide, which is a hair
    // there. Each is drawn as a rectangle of the width the Switch's renderer gives the game's lines
    // (the characters' outlines: docs/PS3_PARITY.md).
    std::uint32_t kind = s.kind < 3 ? s.kind : 2;
    if (kind == 1 && imageWidth_ > (std::uint32_t)fieldWidth_) {
        const float sx = imageWidth_ / fieldWidth_, sy = imageHeight_ / fieldHeight_, half = 0.5f * lineWidth_ * (float)imageHeight_ / 720.0f;
        lineVertices_.clear();
        lineIndex_.clear();
        for (std::size_t k = 0; k + 1 < indices; k += 2) {
            const gs::DrawVertex& a = v[index[k]];
            const gs::DrawVertex& b = v[index[k + 1]];
            const float dx = (b.x - a.x) * sx, dy = (b.y - a.y) * sy, length = std::sqrt(dx * dx + dy * dy);
            if (!(length > 0.0f))
                continue;
            const float nx = -dy / length * half / sx, ny = dx / length * half / sy;
            const std::uint32_t base = (std::uint32_t)lineVertices_.size();
            for (int corner = 0; corner < 4; corner++) {
                gs::DrawVertex o = corner < 2 ? a : b;
                const float side = corner & 1 ? 1.0f : -1.0f;
                o.x += nx * side;
                o.y += ny * side;
                lineVertices_.push_back(o);
            }
            for (std::uint32_t i : {0u, 1u, 2u, 1u, 3u, 2u})
                lineIndex_.push_back(base + i);
        }
        if (lineIndex_.empty())
            return;
        v = lineVertices_.data();
        count = lineVertices_.size();
        index = lineIndex_.data();
        indices = lineIndex_.size();
        kind = 2;
    }
    // the run's vertices and indices into the frame's memory
    const std::size_t vertexBytes = count * sizeof(gs::DrawVertex), indexBytes = indices * 4;
    if (vertexBytes + indexBytes + 64 > kFrameBufferBytes)
        return;
    std::size_t vertexAt = (block_.used + sizeof(gs::DrawVertex) - 1) / sizeof(gs::DrawVertex) * sizeof(gs::DrawVertex);
    if (vertexAt + vertexBytes + indexBytes > kFrameBufferBytes) {
        TakeBlock();
        vertexAt = 0;
    }
    std::uint8_t* memory = static_cast<std::uint8_t*>(block_.buffer.contents);
    std::memcpy(memory + vertexAt, v, vertexBytes);
    const std::size_t indexAt = vertexAt + vertexBytes;
    std::memcpy(memory + indexAt, index, indexBytes);
    block_.used = (indexAt + indexBytes + 3) & ~(std::size_t)3;
    runs_++;
    indices_ += indices;

    // SCISSOR's rectangle (inclusive) in the image's pixels
    {
        const float sx = imageWidth_ / fieldWidth_, sy = imageHeight_ / fieldHeight_;
        const std::uint32_t x0 = (std::uint32_t)(s.scissor & 0x7FF), x1 = (std::uint32_t)((s.scissor >> 16) & 0x7FF) + 1,
                            y0 = (std::uint32_t)((s.scissor >> 32) & 0x7FF), y1 = (std::uint32_t)((s.scissor >> 48) & 0x7FF) + 1;
        MTLScissorRect sc;
        sc.x = std::min((std::uint32_t)(x0 * sx), imageWidth_);
        sc.y = std::min((std::uint32_t)(y0 * sy), imageHeight_);
        const std::uint32_t right = std::min((std::uint32_t)(x1 * sx + 0.5f), imageWidth_), bottom = std::min((std::uint32_t)(y1 * sy + 0.5f), imageHeight_);
        if (right <= sc.x || bottom <= sc.y)
            return;
        sc.width = right - sc.x;
        sc.height = bottom - sc.y;
        [encoder_ setScissorRect:sc];
    }
    {
        // An image of the GS's own size is drawn as the GS draws: it takes a pixel's values at the
        // pixel's corner (x, y), the GPU at its centre, so everything is put half a pixel right and
        // down. (A small target always: the game's blur of a shadow's picture depends on it.)
        const float half = imageWidth_ == (std::uint32_t)fieldWidth_ && imageHeight_ == (std::uint32_t)fieldHeight_ ? 0.5f : 0.0f;
        const std::uint32_t zpsm = (std::uint32_t)((s.zbuf >> 24) & 0xF);
        VertexParams vp = {{2.0f / fieldWidth_, -2.0f / fieldHeight_, -1.0f + half * 2.0f / fieldWidth_, 1.0f - half * 2.0f / fieldHeight_},
                           {zpsm == 0 ? 4294967296.0f : zpsm == 1 ? 16777215.0f : 65535.0f, std::max(1.0f, imageWidth_ / fieldWidth_), 0.0f, 0.0f}};
        [encoder_ setVertexBytes:&vp length:sizeof vp atIndex:1];
    }
    FragmentParams fp;
    std::memset(&fp, 0, sizeof fp);
    fp.mode[0] = s.tme;
    fp.mode[1] = (std::int32_t)((s.tex0 >> 35) & 3);
    fp.mode[2] = (std::int32_t)((s.tex0 >> 34) & 1);
    fp.mode[3] = s.fge;
    fp.fog[0] = (float)(s.fogcol & 0xFF);
    fp.fog[1] = (float)((s.fogcol >> 8) & 0xFF);
    fp.fog[2] = (float)((s.fogcol >> 16) & 0xFF);
    fp.test[0] = ate;
    fp.test[1] = (std::int32_t)atst;
    fp.test[2] = (std::int32_t)aref;
    fp.test[3] = (std::int32_t)afail;
    fp.pass[1] = date;
    fp.pass[2] = datm;
    fp.pass[3] = s.abe;
    fp.blend[0] = (std::int32_t)(s.alpha & 3);
    fp.blend[1] = (std::int32_t)((s.alpha >> 2) & 3);
    fp.blend[2] = (std::int32_t)((s.alpha >> 4) & 3);
    fp.blend[3] = (std::int32_t)((s.alpha >> 6) & 3);
    fp.misc[0] = (std::int32_t)((s.alpha >> 32) & 0xFF);
    fp.misc[1] = s.fba;
    const std::uint32_t fpsm = (std::uint32_t)((s.frame >> 24) & 0x3F);
    fp.misc[2] = fpsm == 1;
    if (fpsm > 1 && target < 2)
        lowFrames_++;   // a 16-bit frame: drawn as a 32-bit one
    fp.mask[0] = (std::uint32_t)(s.frame >> 32);
    fp.image[0] = imageScale[0];
    fp.image[1] = imageScale[1];
    fp.image[2] = imageAlpha;
    if (s.tme) {
        const std::uint32_t wms = (std::uint32_t)(s.clamp & 3), wmt = (std::uint32_t)((s.clamp >> 2) & 3);
        const float tw = (float)(1u << std::min<std::uint32_t>((std::uint32_t)((s.tex0 >> 26) & 0xF), 10)),
                    th = (float)(1u << std::min<std::uint32_t>((std::uint32_t)((s.tex0 >> 30) & 0xF), 10));
        if (wms == 2) {   // REGION_CLAMP: the texels MINU to MAXU, as coordinates of their centres
            fp.misc[3] |= 1;
            fp.region[0] = ((float)((s.clamp >> 4) & 0x3FF) + 0.5f) / tw;
            fp.region[1] = ((float)((s.clamp >> 14) & 0x3FF) + 0.5f) / tw;
        }
        if (wmt == 2) {
            fp.misc[3] |= 2;
            fp.region[2] = ((float)((s.clamp >> 24) & 0x3FF) + 0.5f) / th;
            fp.region[3] = ((float)((s.clamp >> 34) & 0x3FF) + 0.5f) / th;
        }
        regionRepeat_ += wms == 3 || wmt == 3;   // REGION_REPEAT: as REPEAT
        const bool linear = !gsFilter_ || ((s.tex1 >> 5) & 1);
        [encoder_ setFragmentSamplerState:sampler_[linear][wms == 1 || wms == 2][wmt == 1 || wmt == 2] atIndex:0];
    } else {
        [encoder_ setFragmentSamplerState:sampler_[0][0][0] atIndex:0];
    }
    [encoder_ setFragmentTexture:texture atIndex:0];

    static const MTLPrimitiveType kPrimitive[3] = {MTLPrimitiveTypePoint, MTLPrimitiveTypeLine, MTLPrimitiveTypeTriangle};
    const MTLPrimitiveType primitive = kPrimitive[kind];
    const int depthTest = !passDepth_ || !zte || ztst == 1 ? 0 : ztst == 2 ? 1 : 2;
    // one pass of the run: which pixels of the alpha test it is for, and whether it writes the depth
    auto pass = [&](int which, bool depthWrite) {
        fp.pass[0] = which;
        [encoder_ setFragmentBytes:&fp length:sizeof fp atIndex:0];
        [encoder_ setDepthStencilState:depth_[depthTest][passDepth_ && zte && !zmsk && depthWrite ? 1 : 0]];
        [encoder_ drawIndexedPrimitives:primitive
                             indexCount:indices
                              indexType:MTLIndexTypeUInt32
                            indexBuffer:block_.buffer
                      indexBufferOffset:indexAt
                          instanceCount:1
                             baseVertex:(NSInteger)(vertexAt / sizeof(gs::DrawVertex))
                           baseInstance:0];
    };
    const bool writesDepth = passDepth_ && zte && !zmsk;
    if (!ate || atst == 1) {   // every pixel passes
        pass(0, true);
        return;
    }
    // The depth is written for a whole pass or not at all, and what a failing pixel writes is not
    // what a passing one does. Where no pixel writes the depth, or (KEEP) a failing pixel writes
    // nothing at all, one pass does, the shader deciding each pixel; else two, the passing pixels'
    // and the failing ones'.
    if (!writesDepth || afail == 0) {
        if (afail == 2 && !writesDepth)
            fp.test[3] = 0;   // (ZB_ONLY with no depth to write: a failing pixel writes nothing)
        pass(0, true);
        return;
    }
    twoPass_++;
    if (atst != 0)
        pass(1, true);
    pass(2, afail == 2);   // FB_ONLY and RGB_ONLY leave the depth; ZB_ONLY writes it alone
}

// The frame shown against the reference drawing's picture of it (--dump-fb's rasteriser, which
// drew the same primitives into the GS's memory): the mean difference of a colour, and how many
// of the pixels are within 2, 8 and 32 of the reference in every colour.
void Renderer::Compare(int target)
{
    std::string ref;
    int rw, rh;
    if (!gs::ReadDisplay(ref, rw, rh) || rw != (int)width_ || (rh != (int)height_ && rh != (int)height_ * 2))
        return;
    const int repeat = rh / (int)height_;
    std::vector<std::uint32_t> mine((std::size_t)width_ * height_);
    [colour_[target] getBytes:mine.data() bytesPerRow:width_ * 4 fromRegion:MTLRegionMake2D(0, 0, width_, height_) mipmapLevel:0];
    double sum = 0;
    std::uint64_t within[3] = {0, 0, 0};
    for (std::uint32_t y = 0; y < height_; y++)
        for (std::uint32_t x = 0; x < width_; x++) {
            const std::uint32_t m = mine[(std::size_t)y * width_ + x];
            const unsigned char* r = reinterpret_cast<const unsigned char*>(&ref[((std::size_t)y * repeat * rw + x) * 3]);
            int worst = 0;
            for (int c = 0; c < 3; c++) {
                const int d = std::abs((int)((m >> (c * 8)) & 0xFF) - (int)r[c]);
                sum += d;
                worst = std::max(worst, d);
            }
            within[0] += worst <= 2;
            within[1] += worst <= 8;
            within[2] += worst <= 32;
        }
    const double pixels = (double)width_ * height_, mean = sum / (pixels * 3);
    cmpFrames_++;
    cmpSum_ += mean;
    for (int k = 0; k < 3; k++)
        cmpWithin_[k] += within[k] / pixels;
    if (mean > cmpWorst_) {
        cmpWorst_ = mean;
        cmpWorstFrame_ = frames_;
    }
    static const long each = std::getenv("SLY1_METAL_COMPARE_EACH") ? std::atol(std::getenv("SLY1_METAL_COMPARE_EACH")) : 60;
    if (each > 0 && frames_ % (std::uint64_t)each == 0)
        std::fprintf(stderr, "metalcmp: frame %llu: mean difference %.3f of 255; within 2: %.2f%%, within 8: %.2f%%, within 32: %.2f%%\n",
                     (unsigned long long)frames_, mean, 100.0 * within[0] / pixels, 100.0 * within[1] / pixels, 100.0 * within[2] / pixels);
}

void Renderer::Present(std::uint64_t dispfb, std::uint64_t)
{
    @autoreleasepool {
        frames_++;
        int target = TargetOf(dispfb & 0x1FF);
        if (target < 0)
            target = 0;
        EndPass();
        const bool dump = !dumpDir_.empty() && frames_ % (std::uint64_t)dumpEvery_ == 0;
        {   // the frame shown onto the screen's image
            CAMetalLayer* layer;
            {
                std::lock_guard<std::mutex> lk(layerLock_);
                layer = layer_;
            }
            id<CAMetalDrawable> drawable = layer ? [layer nextDrawable] : nil;
            if (drawable) {
                EnsureCommands();
                MTLRenderPassDescriptor* d = [MTLRenderPassDescriptor renderPassDescriptor];
                d.colorAttachments[0].texture = drawable.texture;
                d.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                d.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> e = [commands_ renderCommandEncoderWithDescriptor:d];
                [e setRenderPipelineState:copyPipeline_];
                const float params[4] = {VividFactor(), 0.0f, 0.0f, 0.0f};
                [e setFragmentBytes:params length:sizeof params atIndex:0];
                [e setFragmentTexture:colour_[target] atIndex:0];
                [e setFragmentSamplerState:sampler_[1][1][1] atIndex:0];
                [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [e endEncoding];
                [commands_ presentDrawable:drawable];
            }
        }
        if (commands_) {
            std::vector<id<MTLBuffer>> blocks;
            blocks.swap(frameBlocks_);
            Renderer* self = this;
            [commands_ addCompletedHandler:^(id<MTLCommandBuffer>) {
                std::lock_guard<std::mutex> lk(self->poolLock_);
                for (id<MTLBuffer> b : blocks)
                    self->pool_.push_back(b);
            }];
            [commands_ commit];
            if (compare_ || dump)
                [commands_ waitUntilCompleted];
            commands_ = nil;
            block_ = Block();
        }
        if (compare_)
            Compare(target);
        if (dump) {
            std::vector<std::uint32_t> mine((std::size_t)width_ * height_);
            [colour_[target] getBytes:mine.data() bytesPerRow:width_ * 4 fromRegion:MTLRegionMake2D(0, 0, width_, height_) mipmapLevel:0];
            // (an image of the GS's size as the frame dump has it: each line twice)
            const int repeat = height_ == (std::uint32_t)kFieldHeight ? 2 : 1;
            std::string rgb((std::size_t)width_ * height_ * repeat * 3, '\0');
            for (std::uint32_t y = 0; y < height_ * repeat; y++)
                for (std::uint32_t x = 0; x < width_; x++) {
                    const std::uint32_t m = mine[(std::size_t)(y / repeat) * width_ + x];
                    char* p = &rgb[((std::size_t)y * width_ + x) * 3];
                    p[0] = (char)(m & 0xFF);
                    p[1] = (char)((m >> 8) & 0xFF);
                    p[2] = (char)((m >> 16) & 0xFF);
                }
            char name[64];
            std::snprintf(name, sizeof name, "/metal-%05llu.png", (unsigned long long)frames_);
            gs::WritePngFile(dumpDir_ + name, rgb, (int)width_, (int)(height_ * repeat));
        }
    }
}

void Renderer::Report()
{
    std::fprintf(stderr, "gpu: Metal: %llu frames, %llu runs (%llu indices) in %llu passes, %llu textures decoded, %llu runs in two passes for the alpha test\n",
                 (unsigned long long)frames_, (unsigned long long)runs_, (unsigned long long)indices_, (unsigned long long)passes_, (unsigned long long)decodes_,
                 (unsigned long long)twoPass_);
    std::fprintf(stderr, "gpu: Metal: %llu runs into small targets (%llu reading their own), %llu textured from a frame buffer (%s), %llu with REGION_REPEAT, "
                 "%llu into a 16-bit frame\n", (unsigned long long)smallRuns_, (unsigned long long)smallSelf_, (unsigned long long)frameTextured_,
                 blur_ ? "drawn" : "left out", (unsigned long long)regionRepeat_, (unsigned long long)lowFrames_);
    if (otherTargets_) {
        std::fprintf(stderr, "gpu: Metal: %llu runs into buffers not drawn into, by FRAME:", (unsigned long long)otherTargets_);
        for (const auto& f : otherFrames_)
            std::fprintf(stderr, " %08llX x%llu", (unsigned long long)f.first, (unsigned long long)f.second);
        std::fprintf(stderr, "\n");
    }
    if (cmpFrames_)
        std::fprintf(stderr, "metalcmp: %llu frames compared: mean difference %.3f of 255; within 2: %.2f%%, within 8: %.2f%%, within 32: %.2f%%; "
                     "the worst frame %llu at %.3f\n", (unsigned long long)cmpFrames_, cmpSum_ / cmpFrames_, 100.0 * cmpWithin_[0] / cmpFrames_,
                     100.0 * cmpWithin_[1] / cmpFrames_, 100.0 * cmpWithin_[2] / cmpFrames_, (unsigned long long)cmpWorstFrame_, cmpWorst_);
}

void Renderer::SetLayer(CAMetalLayer* layer)
{
    std::lock_guard<std::mutex> lk(layerLock_);
    layer_ = layer;
}

Renderer g_renderer;
bool g_started;

}  // namespace

}  // namespace gpu
}  // namespace port

#if TARGET_OS_OSX

// The Mac's window: a view whose layer is the screen the renderer presents to, and a keyboard
// as the pad. W A S D the left stick, the arrows the right (the camera), I J K L the direction
// buttons; Space cross, F square, E circle, Q triangle; 1 L1, 2 R1, 3 L2, 4 R2; Return Start,
// Tab Select.
namespace {

bool g_keys[128];

void KeysToPad()
{
    auto held = [](int code) { return g_keys[code]; };
    std::uint16_t buttons = 0;
    static const struct { int key; std::uint16_t bit; } kButtons[] = {
        {49, 0x0040}, {3, 0x0080}, {14, 0x0020}, {12, 0x0010},      // Space, F, E, Q: cross, square, circle, triangle
        {18, 0x0004}, {19, 0x0008}, {20, 0x0001}, {21, 0x0002},     // 1, 2, 3, 4: L1, R1, L2, R2
        {36, 0x0800}, {48, 0x0100},                                 // Return, Tab: Start, Select
        {34, 0x1000}, {37, 0x2000}, {40, 0x4000}, {38, 0x8000},     // I, L, K, J: up, right, down, left
    };
    for (const auto& b : kButtons)
        if (held(b.key))
            buttons |= b.bit;
    auto axis = [&](int low, int high) { return (std::uint64_t)(held(low) == held(high) ? 0x80 : held(low) ? 0x00 : 0xFF); };
    const std::uint64_t rx = axis(123, 124), ry = axis(126, 125), lx = axis(0, 2), ly = axis(13, 1);
    std::uint64_t pad = buttons | rx << 16 | ry << 24 | lx << 32 | ly << 40;
    // A controller with the keyboard: the buttons of both, and its sticks where the keys hold none.
    std::uint64_t controller;
    if (port::apple::ControllerPad(&controller)) {
        pad |= controller & 0xFFFF;
        for (int shift = 16; shift <= 40; shift += 8)
            if (((pad >> shift) & 0xFF) == 0x80)
                pad = (pad & ~(0xFFull << shift)) | (controller & (0xFFull << shift));
    }
    port::g_hostPad.store(pad, std::memory_order_relaxed);
}

}  // namespace

@interface SlyView : NSView
@end

@implementation SlyView
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer*)makeBackingLayer { return [CAMetalLayer layer]; }
- (void)keyDown:(NSEvent*)event
{
    if (event.keyCode < 128) {
        g_keys[event.keyCode] = true;
        KeysToPad();
    }
}
- (void)keyUp:(NSEvent*)event
{
    if (event.keyCode < 128) {
        g_keys[event.keyCode] = false;
        KeysToPad();
    }
}
@end

@interface SlyDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@implementation SlyDelegate
- (void)windowWillClose:(NSNotification*)notification
{
    (void)notification;
    port::gpu::Report();
    std::_Exit(0);
}
- (void)windowDidResignKey:(NSNotification*)notification
{
    (void)notification;
    std::memset(g_keys, 0, sizeof g_keys);   // (no key's release is seen while another window has the keyboard)
    KeysToPad();
}
@end

namespace port {
namespace gpu {

bool WindowWanted()
{
    const char* e = std::getenv("SLY1_METAL");
    return g_started && e && !std::strcmp(e, "window");
}

void RunWindow()
{
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        static SlyDelegate* delegate;
        delegate = [SlyDelegate new];
        app.delegate = delegate;
        const NSRect frame = NSMakeRect(0, 0, g_renderer.Width(), g_renderer.Height());
        NSWindow* window = [[NSWindow alloc] initWithContentRect:frame
                                                       styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        window.title = @"sly1-nx (Metal)";
        window.contentAspectRatio = NSMakeSize(g_renderer.Width(), g_renderer.Height());
        window.delegate = delegate;
        window.releasedWhenClosed = NO;
        SlyView* view = [[SlyView alloc] initWithFrame:frame];
        view.wantsLayer = YES;
        CAMetalLayer* layer = (CAMetalLayer*)view.layer;
        layer.device = g_renderer.Device();
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = YES;
        layer.drawableSize = CGSizeMake(g_renderer.Width(), g_renderer.Height());   // the frame's own size: the layer fits it to the window
        window.contentView = view;
        [window center];
        [window makeKeyAndOrderFront:nil];
        [window makeFirstResponder:view];
        [app activateIgnoringOtherApps:YES];
        g_renderer.SetLayer(layer);
        // the pad read 120 times a second: the keys held and a controller's state
        port::apple::WatchControllers();
        [NSTimer scheduledTimerWithTimeInterval:1.0 / 120.0 repeats:YES block:^(NSTimer*) { KeysToPad(); }];
        [app run];
    }
}

}  // namespace gpu
}  // namespace port

#endif

namespace port {
namespace gpu {

// The Mac's build draws with Metal when SLY1_METAL asks (its tests are the reference drawing's);
// the iPhone's always.
bool Start()
{
    if (!std::getenv("SLY1_METAL"))
        return false;
    if (!g_renderer.Create())
        return false;
    gs::SetBackend(&g_renderer);
    g_started = true;
    return true;
}

void Report()
{
    if (g_started)
        g_renderer.Report();
    apple::AudioReport();
}

bool Started() { return g_started; }

void Shutdown() {}

}  // namespace gpu
}  // namespace port
