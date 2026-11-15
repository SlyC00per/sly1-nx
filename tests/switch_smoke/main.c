// SPDX-License-Identifier: AGPL-3.0-or-later
// Switch smoke test: proves the toolchain and the emulator loop before any game code exists.
// It draws one triangle with deko3d (shaders built by uam), presents a few frames, copies the
// last frame back to CPU memory, writes it to sdmc:/sly1/smoke.ppm, reports each step through
// the debug-output channel (svcOutputDebugString, which Ryujinx logs as guest output) and exits
// by itself, so it can run unattended.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <deko3d.h>
#include <switch.h>

enum {
    kWidth = 1280,
    kHeight = 720,
    kImages = 2,
    kFrames = 30,
    kCodeSize = 64 * 1024,
    kCmdSize = 64 * 1024,
};

static void Log(const char* fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof line) n = sizeof line - 1;
    svcOutputDebugString(line, (u64)n);
}

static u32 AlignUp(u32 v, u32 a) { return (v + a - 1) & ~(a - 1); }

// Loads a compiled shader from romfs into the code block at *offset.
static bool LoadShader(DkShader* shader, DkMemBlock code, u32* offset, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        Log("smoke: cannot open %s", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    u32 size = (u32)ftell(f);
    rewind(f);
    if (*offset + size > kCodeSize) {
        fclose(f);
        Log("smoke: shader %s does not fit", path);
        return false;
    }
    size_t got = fread((u8*)dkMemBlockGetCpuAddr(code) + *offset, 1, size, f);
    fclose(f);
    if (got != size) return false;

    DkShaderMaker maker;
    dkShaderMakerDefaults(&maker, code, *offset);
    dkShaderInitialize(shader, &maker);
    *offset = AlignUp(*offset + size, DK_SHADER_CODE_ALIGNMENT);
    return true;
}

// Writes RGBA8 rows as a binary PPM (RGB), the simplest format anything can open.
static bool WritePpm(const char* path, const u8* rgba, u32 w, u32 h, u32 stride) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    static u8 row[kWidth * 3];
    for (u32 y = 0; y < h; y++) {
        const u8* src = rgba + y * stride;
        for (u32 x = 0; x < w; x++) {
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        fwrite(row, 1, w * 3, f);
    }
    return fclose(f) == 0;
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    Log("smoke: start");
    if (R_FAILED(romfsInit())) {
        Log("smoke: romfsInit failed");
        return 1;
    }

    DkDeviceMaker deviceMaker;
    dkDeviceMakerDefaults(&deviceMaker);
    DkDevice device = dkDeviceCreate(&deviceMaker);

    // Swapchain images. No hardware compression, so the last frame can be copied out plainly.
    DkImageLayoutMaker layoutMaker;
    dkImageLayoutMakerDefaults(&layoutMaker, device);
    layoutMaker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent;
    layoutMaker.format = DkImageFormat_RGBA8_Unorm;
    layoutMaker.dimensions[0] = kWidth;
    layoutMaker.dimensions[1] = kHeight;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &layoutMaker);
    u32 imageSize = AlignUp(dkImageLayoutGetSize(&layout), dkImageLayoutGetAlignment(&layout));

    DkMemBlockMaker blockMaker;
    dkMemBlockMakerDefaults(&blockMaker, device, kImages * imageSize);
    blockMaker.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    DkMemBlock imageBlock = dkMemBlockCreate(&blockMaker);

    DkImage images[kImages];
    DkImage const* imagePtrs[kImages];
    for (int i = 0; i < kImages; i++) {
        dkImageInitialize(&images[i], &layout, imageBlock, i * imageSize);
        imagePtrs[i] = &images[i];
    }
    DkSwapchainMaker swapMaker;
    dkSwapchainMakerDefaults(&swapMaker, device, nwindowGetDefault(), imagePtrs, kImages);
    DkSwapchain swapchain = dkSwapchainCreate(&swapMaker);

    // Shaders.
    dkMemBlockMakerDefaults(&blockMaker, device, kCodeSize);
    blockMaker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
    DkMemBlock codeBlock = dkMemBlockCreate(&blockMaker);
    u32 codeOffset = 0;
    DkShader vertex, fragment;
    if (!LoadShader(&vertex, codeBlock, &codeOffset, "romfs:/shaders/smoke_vsh.dksh") ||
        !LoadShader(&fragment, codeBlock, &codeOffset, "romfs:/shaders/smoke_fsh.dksh")) {
        romfsExit();
        return 1;
    }
    Log("smoke: shaders loaded (%u bytes)", codeOffset);

    // Read-back buffer, CPU-uncached so the GPU's writes are visible without a cache flush.
    u32 readStride = kWidth * 4;
    dkMemBlockMakerDefaults(&blockMaker, device, AlignUp(readStride * kHeight, DK_MEMBLOCK_ALIGNMENT));
    blockMaker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    DkMemBlock readBlock = dkMemBlockCreate(&blockMaker);

    dkMemBlockMakerDefaults(&blockMaker, device, kCmdSize);
    blockMaker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    DkMemBlock cmdBlock = dkMemBlockCreate(&blockMaker);
    DkCmdBufMaker cmdMaker;
    dkCmdBufMakerDefaults(&cmdMaker, device);
    DkCmdBuf cmd = dkCmdBufCreate(&cmdMaker);
    dkCmdBufAddMemory(cmd, cmdBlock, 0, kCmdSize);

    DkQueueMaker queueMaker;
    dkQueueMakerDefaults(&queueMaker, device);
    queueMaker.flags = DkQueueFlags_Graphics;
    DkQueue queue = dkQueueCreate(&queueMaker);

    DkViewport viewport = {0.0f, 0.0f, (float)kWidth, (float)kHeight, 0.0f, 1.0f};
    DkScissor scissor = {0, 0, kWidth, kHeight};
    DkShader const* shaders[] = {&vertex, &fragment};
    DkRasterizerState raster;
    DkColorState color;
    DkColorWriteState colorWrite;
    dkRasterizerStateDefaults(&raster);
    raster.cullMode = DkFace_None;
    dkColorStateDefaults(&color);
    dkColorWriteStateDefaults(&colorWrite);

    int lastSlot = -1;
    for (int frame = 0; frame < kFrames && appletMainLoop(); frame++) {
        int slot = dkQueueAcquireImage(queue, swapchain);
        DkImageView view;
        dkImageViewDefaults(&view, &images[slot]);
        dkCmdBufBindRenderTarget(cmd, &view, NULL);
        dkCmdBufSetViewports(cmd, 0, &viewport, 1);
        dkCmdBufSetScissors(cmd, 0, &scissor, 1);
        dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, 0.10f, 0.12f, 0.20f, 1.0f);
        dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, 2);
        dkCmdBufBindRasterizerState(cmd, &raster);
        dkCmdBufBindColorState(cmd, &color);
        dkCmdBufBindColorWriteState(cmd, &colorWrite);
        dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
        // Orientation marker: a white block cleared through a scissor at window rows 0-99,
        // columns 0-199. Scissors are in window coordinates, so it shows where row 0 is.
        DkScissor marker = {0, 0, 200, 100};
        dkCmdBufSetScissors(cmd, 0, &marker, 1);
        dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, 1.0f, 1.0f, 1.0f, 1.0f);
        dkQueueSubmitCommands(queue, dkCmdBufFinishList(cmd));
        dkQueuePresentImage(queue, swapchain, slot);
        dkQueueWaitIdle(queue);
        dkCmdBufClear(cmd);
        lastSlot = slot;
    }
    Log("smoke: presented %d frames", kFrames);

    // Copy the last presented image back and write it out.
    int status = 1;
    if (lastSlot >= 0) {
        DkImageView view;
        dkImageViewDefaults(&view, &images[lastSlot]);
        DkImageRect rect = {0, 0, 0, kWidth, kHeight, 1};
        DkCopyBuf dst = {dkMemBlockGetGpuAddr(readBlock), 0, 0};
        dkCmdBufCopyImageToBuffer(cmd, &view, &rect, &dst, 0);
        dkQueueSubmitCommands(queue, dkCmdBufFinishList(cmd));
        dkQueueWaitIdle(queue);

        const u8* px = (const u8*)dkMemBlockGetCpuAddr(readBlock);
        const u8* c = px + (kHeight / 2) * readStride + (kWidth / 2) * 4;
        const u8* k = px + 8 * readStride + 8 * 4;
        Log("smoke: centre pixel %02x%02x%02x%02x, corner pixel %02x%02x%02x%02x",
            c[0], c[1], c[2], c[3], k[0], k[1], k[2], k[3]);

        mkdir("sdmc:/sly1", 0777);
        if (WritePpm("sdmc:/sly1/smoke.ppm", px, kWidth, kHeight, readStride)) {
            Log("smoke: wrote sdmc:/sly1/smoke.ppm");
            status = 0;
        } else {
            Log("smoke: could not write sdmc:/sly1/smoke.ppm");
        }
    }

    dkQueueWaitIdle(queue);
    dkQueueDestroy(queue);
    dkCmdBufDestroy(cmd);
    dkMemBlockDestroy(cmdBlock);
    dkMemBlockDestroy(readBlock);
    dkMemBlockDestroy(codeBlock);
    dkSwapchainDestroy(swapchain);
    dkMemBlockDestroy(imageBlock);
    dkDeviceDestroy(device);
    romfsExit();
    Log("smoke: done, status %d", status);
    return status;
}
