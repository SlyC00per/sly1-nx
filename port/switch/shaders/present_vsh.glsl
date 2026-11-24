// SPDX-License-Identifier: AGPL-3.0-or-later
// The frame put on the screen by a draw (port/switch/gpu.cpp's Present, the Vivid picture): one
// triangle that covers the viewport, made from the vertex's number, with the frame's coordinates.
#version 460

layout (location = 0) out vec2 outUv;

void main()
{
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));   // (0,0) (2,0) (0,2)
    outUv = p;
    gl_Position = vec4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
}
