// SPDX-License-Identifier: AGPL-3.0-or-later
// The GS's vertices as the GPU's (port/switch/gpu.cpp): window pixels to clip space, the 32-bit
// depth as a fraction (bigger is nearer, as the GS has it), and w 1: the GS interpolates colour,
// S, T and Q across the screen and divides by Q at each pixel (gs_fsh.glsl).
#version 460

layout (location = 0) in vec2 inPosition;
layout (location = 1) in uint inDepth;
layout (location = 2) in vec4 inColour;   // 0 to 1 for the GS's 0 to 255
layout (location = 3) in vec3 inStq;
layout (location = 4) in float inFog;

layout (location = 0) out vec4 outColour;   // 0 to 255
layout (location = 1) out vec3 outStq;
layout (location = 2) out float outFog;
layout (location = 3) flat out int outFlags;
layout (location = 4) out float outOutside;
layout (location = 5) out vec4 outAdd;   // (a batch's lights' shares: none here)
layout (location = 6) out vec4 outAdd2;

layout (std140, binding = 0) uniform VertexParams {
    vec4 toClip;   // x and y scales, then the offsets
} u;

void main()
{
    gl_Position = vec4(inPosition * u.toClip.xy + u.toClip.zw, float(inDepth) * (1.0 / 4294967296.0), 1.0);
    outColour = inColour * 255.0;
    outStq = inStq;
    outFog = inFog;
    outFlags = 0;
    outOutside = 0.0;
    outAdd = vec4(0.0);
    outAdd2 = vec4(0.0);
}
