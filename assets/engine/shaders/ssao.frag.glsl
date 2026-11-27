#version 460
// SSAO: reconstructs world-space position from depth, accumulates hemisphere
// occlusion with a hashed random rotation per pixel, and multiplies the
// ambient term of the input color (no separate lighting rig exists yet, so
// AO is applied as a scalar darkening on the lit scene).
// outColor = sceneColor * mix(1.0, ao, 0.75)
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
layout(set = 0, binding = 2) uniform sampler2D depthTex;  // nearest sampler

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

const int kKernelSize = 12;
const vec3 kKernel[kKernelSize] = vec3[kKernelSize](
    vec3( 0.36,  0.21,  0.52), vec3(-0.24,  0.41,  0.33), vec3( 0.18, -0.43,  0.58),
    vec3( 0.62,  0.05,  0.31), vec3(-0.51, -0.27,  0.61), vec3( 0.08,  0.63,  0.44),
    vec3(-0.37,  0.55,  0.24), vec3( 0.55, -0.49,  0.42), vec3(-0.08, -0.62,  0.53),
    vec3( 0.74,  0.32,  0.27), vec3(-0.68,  0.12,  0.48), vec3( 0.27,  0.09,  0.71)
);

float Hash13(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yxz + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec3 WorldFromDepth(float depth, vec2 uv) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 world = post.invViewProj * ndc;
    return world.xyz / world.w;
}

void main() {
    vec4 scene = texture(colorTex, vUv);
    float depth = texture(depthTex, vUv).r;
    if (depth >= 0.99999) {
        outColor = scene;  // background: no geometry to occlude
        return;
    }
    vec3 pos = WorldFromDepth(depth, vUv);
    vec3 toCamera = normalize(post.cameraPos.xyz - pos);

    // Geometric normal from screen-space derivatives of the position.
    vec3 N = normalize(cross(dFdx(pos), dFdy(pos)));
    if (dot(N, toCamera) < 0.0) N = -N;

    // Random tangent rotation per pixel (hides banding without a noise texture).
    float angle = Hash13(gl_FragCoord.xy) * 6.2831853;
    vec3 tangent = vec3(cos(angle), sin(angle), 0.0);
    tangent = normalize(tangent - N * dot(tangent, N));
    vec3 bitangent = cross(N, tangent);
    mat3 TBN = mat3(tangent, bitangent, N);

    const float radius = 0.6;
    const float bias = 0.045;
    float occlusion = 0.0;
    for (int i = 0; i < kKernelSize; ++i) {
        vec3 samplePos = pos + TBN * (kKernel[i] * radius);
        vec4 clip = post.viewProj * vec4(samplePos, 1.0);
        vec2 suv = clip.xy / clip.w * 0.5 + 0.5;
        if (clip.w <= 0.0 || suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) continue;
        float sampleDepth = texture(depthTex, suv).r;
        vec3 sampleWorld = WorldFromDepth(sampleDepth, suv);
        // Occluded when the surface along the tap is closer to the camera than the tap,
        // gated by a range check to avoid halos at depth discontinuities.
        float rangeCheck = smoothstep(0.0, 1.0, radius / max(length(pos - sampleWorld), 1e-4));
        occlusion += (length(pos - sampleWorld) > bias ? 1.0 : 0.0) * rangeCheck;
    }
    float ao = 1.0 - occlusion / float(kKernelSize);
    ao = clamp(ao, 0.0, 1.0);
    outColor = vec4(scene.rgb * mix(1.0, ao, 0.75), scene.a);
}
