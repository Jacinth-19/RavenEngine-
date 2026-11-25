#version 460
// Fullscreen triangle vertex shader for post-process passes.
// gl_VertexIndex 0..2 expands to a triangle covering the whole viewport.
layout(location = 0) out vec2 vUv;

void main() {
    vUv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(vUv * 2.0 - 1.0, 0.0, 1.0);
}
