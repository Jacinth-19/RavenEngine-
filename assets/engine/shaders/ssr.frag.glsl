#version 460
// SSR (screen-space reflections, basic but real): world-space ray march along
// the reflection vector, depth-thickness hit test, metallic/roughness masking
// via the forward pass material buffer (r = metallic, g = roughness).
// outColor = sceneColor + reflection * fresnel * roughness fade * edge fade
layout(set = 0, binding = 0) uniform PostUbo {
    mat4 viewProj;
    mat4 invViewProj;
    mat4 prevViewProj;
    vec4 cameraPos;
    vec4 params;       // w = SSR strength
    vec4 misc;
    vec4 internalInfo;
    vec4 targetSize;
} post;
layout(set = 0, binding = 1) uniform sampler2D colorTex;
layout(set = 0, binding = 2) uniform sampler2D depthTex;  // nearest sampler
layout(set = 0, binding = 5) uniform sampler2D materialTex;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

vec3 WorldFromDepth(float depth, vec2 uv) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 world = post.invViewProj * ndc;
    return world.xyz / world.w;
}

void main() {
    vec4 scene = texture(colorTex, vUv);
    float depth = texture(depthTex, vUv).r;
    if (depth >= 0.99999) {
        outColor = scene;
        return;
    }
    vec3 pos = WorldFromDepth(depth, vUv);
    vec3 toCamera = normalize(post.cameraPos.xyz - pos);
    vec3 N = normalize(cross(dFdx(pos), dFdy(pos)));
    if (dot(N, toCamera) < 0.0) N = -N;

    vec2 material = texture(materialTex, vUv).rg;  // metallic, roughness
    float metallic = clamp(material.x, 0.0, 1.0);
    float roughness = clamp(material.y, 0.04, 1.0);

    vec3 reflectDir = reflect(-toCamera, N);
    if (reflectDir.y == 0.0 && abs(reflectDir.x) + abs(reflectDir.z) < 1e-5) {
        outColor = scene;
        return;
    }

    // Reflectance: dielectrics get a small fresnel term, metals full strength.
    float f0 = mix(0.04, 0.6, metallic);
    float cosTheta = max(dot(toCamera, N), 0.0);
    float fresnel = f0 + (1.0 - f0) * pow(1.0 - cosTheta, 5.0);
    float roughFade = 1.0 - roughness * roughness;  // sharp reflections only
    float strength = post.params.w * fresnel * roughFade;
    if (strength <= 0.004) {
        outColor = scene;
        return;
    }

    // World-space exponential march, ~8 world units max.
    const int kSteps = 28;
    const float kMaxDist = 8.0;
    const float kThickness = 0.22;
    float t = 0.06;
    vec2 hitUv = vec2(0.0);
    float hitDist = kMaxDist;
    bool hit = false;
    float prevT = t;
    for (int i = 0; i < kSteps; ++i) {
        vec3 q = pos + reflectDir * t;
        vec4 clip = post.viewProj * vec4(q, 1.0);
        if (clip.w <= 0.0) break;
        vec2 suv = clip.xy / clip.w * 0.5 + 0.5;
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) break;
        float sd = texture(depthTex, suv).r;
        if (sd < 0.99999) {
            vec3 surfaceWorld = WorldFromDepth(sd, suv);
            float dRay = length(q - pos);
            float dSurf = length(surfaceWorld - pos);
            if (dSurf < dRay && (dRay - dSurf) < kThickness) {
                // Binary refine between prevT and t.
                float lo = prevT, hi = t;
                for (int b = 0; b < 4; ++b) {
                    float mid = 0.5 * (lo + hi);
                    vec4 mclip = post.viewProj * vec4(pos + reflectDir * mid, 1.0);
                    vec2 muv = mclip.xy / mclip.w * 0.5 + 0.5;
                    float md = texture(depthTex, muv).r;
                    if (md >= 0.99999) { lo = mid; continue; }
                    vec3 mq = pos + reflectDir * mid;
                    vec3 mSurf = WorldFromDepth(md, muv);
                    float dq = length(mq - pos);
                    float ds = length(mSurf - pos);
                    if (ds < dq) { hi = mid; } else { lo = mid; }
                }
                vec4 hclip = post.viewProj * vec4(pos + reflectDir * hi, 1.0);
                hitUv = hclip.xy / hclip.w * 0.5 + 0.5;
                hitDist = hi;
                hit = true;
                break;
            }
        }
        prevT = t;
        t = min(t * 1.32 + 0.035, kMaxDist);
    }
    if (!hit) {
        outColor = scene;
        return;
    }
    // Fade near screen borders and with distance traveled.
    vec2 centered = abs(hitUv - 0.5) * 2.0;
    float edgeFade = 1.0 - smoothstep(0.72, 1.0, max(centered.x, centered.y));
    float distFade = 1.0 - clamp(hitDist / kMaxDist, 0.0, 1.0);
    vec3 reflection = texture(colorTex, hitUv).rgb;
    outColor = vec4(scene.rgb + reflection * strength * edgeFade * distFade, scene.a);
}
