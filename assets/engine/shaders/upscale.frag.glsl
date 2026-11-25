#version 460
// Upscale / resolve: when the internal render extent matches the output extent
// this is an exact copy (bilinear tap); otherwise a Catmull-Rom bicubic
// reconstruction expands internal -> target (render-scale upscaler, in-engine;
// no vendored FSR/DLSS).
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
layout(set = 0, binding = 1) uniform sampler2D srcTex;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

vec3 CatmullRom(sampler2D tex, vec2 uv) {
    vec2 texSize = vec2(textureSize(tex, 0));
    vec2 pos = uv * texSize - 0.5;
    vec2 fpos = floor(pos);
    vec2 f = pos - fpos;
    float t0 = f.x, t1 = f.y;

    float wx0 = -0.5 * t0 * t0 * t0 + t0 * t0 - 0.5 * t0;
    float wx1 = 1.5 * t0 * t0 * t0 - 2.5 * t0 * t0 + 1.0;
    float wx2 = -1.5 * t0 * t0 * t0 + 2.0 * t0 * t0 + 0.5 * t0;
    float wx3 = 0.5 * t0 * t0 * t0 - 0.5 * t0 * t0;
    float wy0 = -0.5 * t1 * t1 * t1 + t1 * t1 - 0.5 * t1;
    float wy1 = 1.5 * t1 * t1 * t1 - 2.5 * t1 * t1 + 1.0;
    float wy2 = -1.5 * t1 * t1 * t1 + 2.0 * t1 * t1 + 0.5 * t1;
    float wy3 = 0.5 * t1 * t1 * t1 - 0.5 * t1 * t1;

    float wx[4] = float[4](wx0, wx1, wx2, wx3);
    float wy[4] = float[4](wy0, wy1, wy2, wy3);

    vec3 result = vec3(0.0);
    float total = 0.0;
    for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < 4; ++i) {
            vec2 samplePos = (fpos + vec2(float(i) - 1.0, float(j) - 1.0) + 0.5) / texSize;
            float w = wx[i] * wy[j];
            result += texture(tex, samplePos).rgb * w;
            total += w;
        }
    }
    return result / max(total, 1e-5);
}

void main() {
    bool native = abs(post.internalInfo.x - post.targetSize.x) < 0.5 &&
                  abs(post.internalInfo.y - post.targetSize.y) < 0.5;
    vec3 color = native ? texture(srcTex, vUv).rgb : CatmullRom(srcTex, vUv);
    outColor = vec4(color, 1.0);
}
