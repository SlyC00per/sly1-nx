// SPDX-License-Identifier: AGPL-3.0-or-later
// gs_lite_fsh.glsl for a run drawn into the half-size picture of the translucent effects
// (port/switch/gpu.cpp): the same arithmetic, and the depth test done here, against the frame's
// depth buffer read as a texture (the half-size picture has none): a pixel of it stands for four
// of the frame's and is drawn where the run is in front of the nearest of the four.
#version 460

layout (location = 0) noperspective in vec4 inColour;   // 0 to 255
layout (location = 1) in vec3 inStq;
layout (location = 2) noperspective in float inFog;

layout (location = 0, index = 0) out vec4 outColour;
layout (location = 0, index = 1) out vec4 outFactor;

layout (binding = 0) uniform sampler2D texture0;
layout (binding = 2) uniform sampler2D depth0;   // the frame's depth buffer (bigger is nearer)

layout (std140, binding = 0) uniform FragmentParams {   // as gs_fsh.glsl has it, but
    ivec4 mode;
    vec4 fogColour;   // w: the depth test (0 none, 2 GEQUAL, 3 GREATER)
    vec4 coef;
    vec4 blend;
} u;

void main()
{
    // (w: the test, and beside it half the scale the frame is drawn at: 0.5 for the whole image)
    float test = floor(u.fogColour.w), scale = fract(u.fogColour.w) * 2.0;
    if (test != 0.0) {
        ivec2 p = ivec2(floor(gl_FragCoord.xy) * 2.0 * scale);
        // the four in one fetch: the 2x2 texels a filtered read at the corner between them would use
        // (four texelFetch calls each pixel were the most of this shader's time on the Switch)
        vec4 four = textureGather(depth0, (vec2(p) + 1.0) / vec2(textureSize(depth0, 0)), 0);
        float d = max(max(four.x, four.y), max(four.z, four.w));
        if (test == 2.0 ? gl_FragCoord.z < d : gl_FragCoord.z <= d)
            discard;
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
    outFactor = vec4(min(u.blend.x != 0.0 ? a / 128.0 : u.blend.y, 1.0));
}
