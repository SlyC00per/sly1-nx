// SPDX-License-Identifier: AGPL-3.0-or-later
// The frame put on the screen with its colours made more vivid (the Options page's "Picture:
// Vivid", port/switch/gpu.cpp's Present): each pixel's colour moved away from its grey by the
// setting's factor. The PS3 release's picture is more saturated than the PS2's on textured surfaces
// (docs/PS3_PARITY.md); this is an approximation of that, not the PS2's picture.
#version 460

layout (location = 0) in vec2 inUv;
layout (location = 0) out vec4 outColour;

layout (binding = 1) uniform sampler2D frame0;

layout (std140, binding = 0) uniform PresentParams {
    vec4 p;   // x the saturation's factor (1: as it is), y and z the part of the image drawn into
} u;

void main()
{
    vec3 rgb = textureLod(frame0, inUv * u.p.yz, 0.0).rgb;
    float grey = dot(rgb, vec3(0.299, 0.587, 0.114));
    outColour = vec4(clamp(grey + (rgb - grey) * u.p.x, 0.0, 1.0), 1.0);
}
