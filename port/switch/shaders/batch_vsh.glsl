// SPDX-License-Identifier: AGPL-3.0-or-later
// A batch of the game's vertices transformed as VU1's program transforms them (port/gs_backend.h's
// Batch; VertexOf there is this arithmetic, checked against the program by SLY1_GPU_BATCH_CHECK):
// the matrix, then the viewport's scale and offset. The position stays in clip coordinates (w
// kept), so that the GPU cuts triangles at the view's edges and at the near and far planes (z
// from -w to w), as the program and the clipper after it do: what lies wholly beyond the far
// plane is not drawn. The viewport's depth range makes the depth the GS's.
// The colour and fog are the GS's, interpolated across the screen (gs_fsh.glsl); S, T and Q go
// undivided, the GPU's perspective giving what the GS's division by Q gives.
#version 460

layout (location = 0) in vec3 inPosition;
layout (location = 1) in uint inControl;   // the control word: its low 12 bits the fog's base
layout (location = 2) in vec3 inStq;
layout (location = 3) in uvec4 inColour;   // R, G, B, A in each word's low byte
layout (location = 4) in uvec4 inShare1;   // the colours of the batch's two lights' shares (u.layers says which are)
layout (location = 5) in uvec4 inShare2;

layout (location = 0) out vec4 outColour;   // 0 to 255
layout (location = 1) out vec3 outStq;
layout (location = 2) out float outFog;
layout (location = 4) out float outOutside;   // 1 at a vertex outside the program's clip volume (SLY1_GPU_CAPTURE's view)
layout (location = 5) out vec4 outAdd;      // the first share's colour times its alpha over 0x80; w: its alpha over 0x80
layout (location = 6) out vec4 outAdd2;     // the second's
layout (location = 3) flat out int outFlags;   // 1: the triangle this vertex closes is dropped when it faces away

layout (std140, binding = 0) uniform BatchParams {
    mat4 matrix;
    vec4 toClip;   // clip x and y scaled (x, y) and w's part added (z, w)
    vec4 depth;    // clip z scaled (x) and w's part added (y): over w, the GS's depth as a fraction of 2 to the 32nd
    vec4 fog;      // x the scale, y the largest w, z 1 when backfaces are dropped, w the alpha's scale
    vec4 layers;   // x, y: 1 for each share drawn with the batch; z, w: added to S and T
    mat4 proj;     // a projected batch's (gs::Batch::proj, gs::ProjectedOf): S, T, Q from the clip position
    vec4 projLimits;
    vec4 projColour;
    vec4 mode;     // x: 1 for a projected batch
} u;

void main()
{
    vec4 clip = u.matrix * vec4(inPosition, 1.0);
    gl_Position = vec4(clip.x * u.toClip.x + clip.w * u.toClip.z, clip.y * u.toClip.y + clip.w * u.toClip.w,
                       (clip.z + clip.w) * 0.5, clip.w);
    float over = clip.w != 0.0 ? 1.0 / clip.w : 0.0;
    outFog = float((int(float(inControl & 0xFFFu) + u.fog.x * min(clip.w, u.fog.y) * over) >> 4) & 0xFF);
    // (a part's own numbers, port/gs_backend.h's Batch: the alpha times its scale, truncated as the
    // program's conversion to an integer truncates it, and the texture slid along)
    outColour = vec4(inColour & 0xFFu);
    outColour.a = floor(outColour.a * u.fog.w);
    outStq = inStq + vec3(u.layers.zw, 0.0);
    if (u.mode.x != 0.0) {   // 0x076's: S, T and Q through proj, Q kept within the limits, one colour
        vec4 c = u.proj * clip;
        float q = c.w, s = c.x, t = c.y;
        outColour = u.projColour;
        bool small = c.w - u.projLimits.x < 0.0;
        if (small || !(c.w - u.projLimits.y < 0.0)) {
            float limit = small ? u.projLimits.x : u.projLimits.y;
            float shift = (limit - c.w) * 0.5;
            q = limit;
            s += shift;
            t += shift;
            if (c.w - u.projLimits.w < 0.0)
                outColour.a = 0.0;
        }
        outStq = vec3(s, t, q);
    }
    vec4 s1 = vec4(inShare1 & 0xFFu) * u.layers.x, s2 = vec4(inShare2 & 0xFFu) * u.layers.y;
    s1.a = floor(s1.a * u.fog.w);
    s2.a = floor(s2.a * u.fog.w);
    outAdd = vec4(s1.rgb * (s1.a / 128.0), s1.a / 128.0);
    outAdd2 = vec4(s2.rgb * (s2.a / 128.0), s2.a / 128.0);
    outOutside = (clip.w <= 0.0 || abs(clip.x) > clip.w || abs(clip.y) > clip.w || abs(clip.z) > clip.w) ? 1.0 : 0.0;
    outFlags = (u.fog.z != 0.0 && (inControl & 0x4000u) != 0u) ? 1 : 0;
}
