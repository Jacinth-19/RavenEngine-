#version 460
// Bloom stage 2/3: separable 9-tap gaussian blur. Direction passed as a
// fragment push constant in texel units: (1.4, 0) horizontal, (0, 1.4) vertical.
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

layout(push_constant) uniform BlurPush { vec2 direction; } push;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 texel = 1.0 / vec2(textureSize(colorTex, 0));
    vec2 step = push.direction * texel;
    // Weights: gaussian sigma ~2.2, normalized.
    const float w0 = 0.2270270270;
    const float w1 = 0.1945945946;
    const float w2 = 0.1216216216;
    const float w3 = 0.0540540541;
    const float w4 = 0.0162162162;
    vec3 acc = texture(colorTex, vUv).rgb * w0;
    acc += texture(colorTex, vUv + step * 1.0).rgb * w1;
    acc += texture(colorTex, vUv - step * 1.0).rgb * w1;
    acc += texture(colorTex, vUv + step * 2.0).rgb * w2;
    acc += texture(colorTex, vUv - step * 2.0).rgb * w2;
    acc += texture(colorTex, vUv + step * 3.0).rgb * w3;
    acc += texture(colorTex, vUv - step * 3.0).rgb * w3;
    acc += texture(colorTex, vUv + step * 4.0).rgb * w4;
    acc += texture(colorTex, vUv - step * 4.0).rgb * w4;
    outColor = vec4(acc, 1.0);
}
