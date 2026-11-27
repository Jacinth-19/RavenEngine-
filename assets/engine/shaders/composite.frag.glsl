#version 460
// Final composite (internal resolution):
//   1. add bloom (params.x)
//   2. optional TAA resolve (params.y = history retention, params.z = history valid)
//   3. optional camera-motion radial blur (misc.x)
// Writes the internal color that the upscale pass then expands to the target extent.
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
layout(set = 0, binding = 1) uniform sampler2D colorTex;    // lit (AO + SSR applied)
layout(set = 0, binding = 2) uniform sampler2D depthTex;    // nearest sampler
layout(set = 0, binding = 3) uniform sampler2D bloomTex;    // half internal res
layout(set = 0, binding = 4) uniform sampler2D historyTex;  // previous composite (internal res)

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 color = texture(colorTex, vUv).rgb;

    // --- bloom add ---
    if (post.params.x > 0.0) {
        color += texture(bloomTex, vUv).rgb * post.params.x;
    }

    // --- TAA resolve ---
    if (post.params.y > 0.0 && post.params.z > 0.5) {
        float depth = texture(depthTex, vUv).r;
        if (depth < 0.99999) {
            vec4 ndc = vec4(vUv * 2.0 - 1.0, depth, 1.0);
            vec4 world4 = post.invViewProj * ndc;
            vec3 worldPos = world4.xyz / world4.w;
            vec4 pc = post.prevViewProj * vec4(worldPos, 1.0);
            vec2 prevUv = pc.xy / pc.w * 0.5 + 0.5;
            bool valid = pc.w > 0.0 && pc.z >= 0.0 &&
                         prevUv.x > 0.001 && prevUv.x < 0.999 &&
                         prevUv.y > 0.001 && prevUv.y < 0.999;
            if (valid) {
                // Neighborhood AABB (5-tap) clamps history to the current frame's range.
                vec2 texel = 1.0 / vec2(textureSize(colorTex, 0));
                vec3 c0 = texture(colorTex, vUv).rgb;
                vec3 c1 = texture(colorTex, vUv + vec2( texel.x, 0.0)).rgb;
                vec3 c2 = texture(colorTex, vUv - vec2( texel.x, 0.0)).rgb;
                vec3 c3 = texture(colorTex, vUv + vec2(0.0,  texel.y)).rgb;
                vec3 c4 = texture(colorTex, vUv - vec2(0.0,  texel.y)).rgb;
                vec3 minC = min(c0, min(min(c1, c2), min(c3, c4)));
                vec3 maxC = max(c0, max(max(c1, c2), max(c3, c4)));
                vec3 hist = clamp(texture(historyTex, prevUv).rgb, minC, maxC);
                vec3 blended = mix(color, hist, post.params.y);
                // If bloom was added on top of this frame, fold it in after the blend
                // so the glow stays crisp instead of ghosting.
                vec3 bloomTerm = (post.params.x > 0.0) ? (color - texture(colorTex, vUv).rgb) : vec3(0.0);
                color = blended + bloomTerm;
            }
        }
    }

    // --- camera-motion radial blur (subtle, camera-translation driven) ---
    float mb = post.misc.x;
    if (mb > 0.001) {
        vec2 dir = vUv - 0.5;
        vec3 acc = color;
        const int kTaps = 6;
        for (int i = 1; i <= kTaps; ++i) {
            float t = float(i) / float(kTaps);
            acc += texture(colorTex, vUv - dir * t * mb * 0.09).rgb;
        }
        color = acc / float(kTaps + 1);
    }

    outColor = vec4(color, 1.0);
}
