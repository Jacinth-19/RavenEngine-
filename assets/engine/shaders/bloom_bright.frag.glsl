#version 460
// Bloom stage 1: soft-knee luminance threshold. Keeps only HDR-ish highlights.
layout(set = 0, binding = 0) uniform PostUbo {
    mat4 viewProj;
    mat4 invViewProj;
    mat4 prevViewProj;
    vec4 cameraPos;
    vec4 params;
    vec4 misc;
    vec4 internalInfo;
    vec4 targetSize;
} post;
layout(set = 0, binding = 1) uniform sampler2D colorTex;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 c = texture(colorTex, vUv).rgb;
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    const float threshold = 0.70;
    const float knee = 0.35;
    float contribution = smoothstep(threshold, threshold + knee, l);
    outColor = vec4(c * contribution, 1.0);
}
