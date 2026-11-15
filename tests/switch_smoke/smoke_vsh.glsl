// SPDX-License-Identifier: AGPL-3.0-or-later
// The smoke test's vertex shader: one triangle from the vertex index, no vertex buffer.
#version 450

layout(location = 0) out vec4 outColor;

void main() {
    const vec2 pos[3] = vec2[](vec2(0.0, -0.8), vec2(-0.7, 0.7), vec2(0.7, 0.7));
    const vec3 col[3] = vec3[](vec3(1.0, 0.25, 0.1), vec3(0.1, 0.9, 0.3), vec3(0.2, 0.4, 1.0));
    gl_Position = vec4(pos[gl_VertexID], 0.0, 1.0);
    outColor = vec4(col[gl_VertexID], 1.0);
}
