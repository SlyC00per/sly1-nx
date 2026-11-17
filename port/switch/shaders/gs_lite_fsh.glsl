// SPDX-License-Identifier: AGPL-3.0-or-later
// gs_fsh.glsl for the runs that need the least of it (port/switch/gpu.cpp chooses): the GS's
// vertices, no destination alpha test, the blend's factors in the blend state, no capture view.
// The same arithmetic, without a discard (so the GPU may test the depth before it runs this) and
// without the tests of what such a run never has. A particle effect's sprites are these: a scene
// with steam covers the picture twenty times over with them.
#version 460

layout (location = 0) noperspective in vec4 inColour;   // 0 to 255
layout (location = 1) in vec3 inStq;
layout (location = 2) noperspective in float inFog;

layout (location = 0, index = 0) out vec4 outColour;
layout (location = 0, index = 1) out vec4 outFactor;

layout (binding = 0) uniform sampler2D texture0;

layout (std140, binding = 0) uniform FragmentParams {   // as gs_fsh.glsl has it
    ivec4 mode;
    vec4 fogColour;
    vec4 coef;
    vec4 blend;
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
    outFactor = vec4(min(u.blend.x != 0.0 ? a / 128.0 : u.blend.y, 1.0));
}
