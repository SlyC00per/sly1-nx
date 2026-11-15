// SPDX-License-Identifier: AGPL-3.0-or-later
// The smoke test's fragment shader: the interpolated vertex colour.
#version 450

layout(location = 0) in vec4 inColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = inColor;
}
