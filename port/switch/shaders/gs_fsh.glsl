// SPDX-License-Identifier: AGPL-3.0-or-later
// The GS's pixel pipeline before its tests and blend (port/render/gs.cpp's Pixel is the
// reference): the texture function, fog, and the blend's coefficient as a second output (the
// blend equation's As or FIX over 0x80, which the blend state reads as the second source).
#version 460

// Colour and fog go across the screen as the GS takes them, whatever w the vertices have (a
// batch's are in clip coordinates, batch_vsh.glsl); S, T and Q with the GPU's perspective.
layout (location = 0) noperspective in vec4 inColour;   // 0 to 255
layout (location = 1) in vec3 inStq;
layout (location = 2) noperspective in float inFog;
layout (location = 4) noperspective in float inOutside;   // above 0 in a triangle with a vertex outside the clip volume
layout (location = 3) flat in int inFlags;   // 1: dropped when the triangle faces away (a batch's backfaces)

layout (location = 0, index = 0) out vec4 outColour;
layout (location = 0, index = 1) out vec4 outFactor;

layout (binding = 0) uniform sampler2D texture0;
layout (binding = 1) uniform sampler2D frame0;   // the frame buffer being drawn into, for the destination alpha test

layout (std140, binding = 0) uniform FragmentParams {
    ivec4 mode;       // TME, TFX, TCC, FGE
    vec4 fogColour;   // 0 to 255; w: the destination alpha test (0 none, 1 drawn where the frame's alpha bit is set, 2 where it is clear)
    vec4 blend;       // x: 1 for As, 0 for FIX; y: FIX over 0x80; z: 1 when the front faces are the ones facing away;
                      // w: SLY1_GPU_CAPTURE's views (6 a flat colour, 7 the depth)
} u;

void main()
{
    int view = int(u.blend.w);
    if (u.fogColour.w != 0.0) {   // DATE: the frame's alpha under the pixel, its top bit against DATM
        bool set = texelFetch(frame0, ivec2(gl_FragCoord.xy), 0).a >= 127.5 / 255.0;
        if (set != (u.fogColour.w == 1.0))
            discard;
    }
    if (inFlags != 0 && gl_FrontFacing == (u.blend.z != 0.0))
        discard;
    if (view == 8 && inOutside > 0.0)   // triangles reaching outside the clip volume left out
        discard;
    if (view == 9) {   // or shown
        outColour = vec4(inOutside > 0.0 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0), 0.5);
        outFactor = vec4(1.0);
        return;
    }
    if (view == 6 || view == 7) {   // a flat colour, or the depth (the whole of a batch's range white)
        outColour = view == 6 ? vec4(1.0, 0.0, 1.0, 0.5) : vec4(vec3(gl_FragCoord.z * 8192.0), 0.5);
        outFactor = vec4(1.0);
        return;
    }
    vec3 rgb = inColour.rgb;
    float a = inColour.a;
    if (u.mode.x != 0) {
        vec4 t = texture(texture0, inStq.xy / inStq.z) * 255.0;
        if (u.mode.y == 0) {          // MODULATE
            rgb = min(t.rgb * rgb / 128.0, 255.0);
            if (u.mode.z != 0)
                a = min(t.a * a / 128.0, 255.0);
        } else if (u.mode.y == 1) {   // DECAL
            rgb = t.rgb;
            if (u.mode.z != 0)
                a = t.a;
        } else {                      // HIGHLIGHT, HIGHLIGHT2
            rgb = min(t.rgb * rgb / 128.0 + a, 255.0);
            if (u.mode.z != 0)
                a = u.mode.y == 2 ? min(t.a + a, 255.0) : t.a;
        }
    }
    if (u.mode.w != 0)
        rgb = mix(u.fogColour.rgb, rgb, inFog / 255.0);
    outColour = vec4(rgb, a) / 255.0;
    float factor = u.blend.x != 0.0 ? a / 128.0 : u.blend.y;
    outFactor = vec4(min(factor, 1.0));
}
