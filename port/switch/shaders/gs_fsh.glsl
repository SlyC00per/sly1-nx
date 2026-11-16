// SPDX-License-Identifier: AGPL-3.0-or-later
// The GS's pixel pipeline before its tests and blend (port/render/gs.cpp's Pixel is the
// reference): the texture function, fog, and the blend's coefficient as a second output (the
// blend equation's As or FIX over 0x80, which the blend state reads as the second source).
#version 460

layout (location = 0) in vec4 inColour;   // 0 to 255
layout (location = 1) in vec3 inStq;
layout (location = 2) in float inFog;

layout (location = 0, index = 0) out vec4 outColour;
layout (location = 0, index = 1) out vec4 outFactor;

layout (binding = 0) uniform sampler2D texture0;

layout (std140, binding = 0) uniform FragmentParams {
    ivec4 mode;       // TME, TFX, TCC, FGE
    vec4 fogColour;   // 0 to 255
    vec4 blend;       // x: 1 for As, 0 for FIX; y: FIX over 0x80
} u;

void main()
{
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
