// SPDX-License-Identifier: AGPL-3.0-or-later
// gs_fsh.glsl for a batch drawn together with its lights' shares (port/switch/gpu.cpp's Layer):
// the surface's colour, blended over the frame by the shader's coefficients, and the shares
// added to it. Like gs_lite_fsh.glsl it has no discard, so the GPU may test the depth before it
// runs this, which is most of what such a draw saves: port/switch/gpu.cpp chooses it where no
// triangle is to be dropped in the shader and there is no destination alpha test.
#version 460

layout (location = 0) noperspective in vec4 inColour;   // 0 to 255
layout (location = 1) in vec3 inStq;
layout (location = 2) noperspective in float inFog;
layout (location = 5) noperspective in vec4 inAdd;    // the shares (batch_vsh.glsl)
layout (location = 6) noperspective in vec4 inAdd2;

layout (location = 0, index = 0) out vec4 outColour;
layout (location = 0, index = 1) out vec4 outFactor;

layout (binding = 0) uniform sampler2D texture0;
layout (binding = 3) uniform sampler2D texture1;   // the first share's texture, when it is not the batch's (u.mode.x has 2)
layout (binding = 4) uniform sampler2D texture2;   // the second's (u.mode.x has 4)

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
    vec3 add = inAdd.rgb + inAdd2.rgb;
    if (u.mode.x != 0) {   // (the texture function is MODULATE: no other is drawn this way)
        vec2 st = inStq.xy / inStq.z;
        vec4 t = texture(texture0, st) * 255.0;
        vec4 t1 = t, t2 = t;
        if ((u.mode.x & 2) != 0)
            t1 = texture(texture1, st) * 255.0;
        if ((u.mode.x & 4) != 0)
            t2 = texture(texture2, st) * 255.0;
        add = min(t1.rgb * inAdd.rgb / 128.0, 255.0) * (u.mode.z != 0 ? min(t1.a / 128.0, 1.0) : 1.0) +
              min(t2.rgb * inAdd2.rgb / 128.0, 255.0) * (u.mode.z != 0 ? min(t2.a / 128.0, 1.0) : 1.0);
        rgb = min(t.rgb * rgb / 128.0, 255.0);
        if (u.mode.z != 0)
            a = min(t.a * a / 128.0, 255.0);
    }
    if (u.mode.w != 0) {
        rgb = mix(u.fogColour.rgb, rgb, inFog / 255.0);
        add = add * (inFog / 255.0);   // (a share's fog is towards black)
    }
    float c = min(u.blend.x != 0.0 ? a / 128.0 : u.blend.y, 1.0);
    float ks = abs(u.coef.x * c + u.coef.y), kd = abs(u.coef.z * c + u.coef.w);
    outColour = vec4(rgb * min(ks, 1.0) + add, a) / 255.0;
    outFactor = vec4(vec3(1.0 - min(kd, 1.0)), 1.0);
}
